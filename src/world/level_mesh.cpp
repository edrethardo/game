#include "world/level_mesh.h"
#include "world/tile_noise.h"
#include "renderer/renderer.h"
#include "renderer/material.h"
#include "core/log.h"
#include "core/assert.h"

#include <cstdlib>
#include <cstring>
#include <vector>

// -------------------------------------------------------------------------
// Scratch buffer per material bucket
// -------------------------------------------------------------------------
static constexpr u32 SCRATCH_VERTS   = 12288;
static constexpr u32 SCRATCH_INDICES = 16384;

struct MaterialBucket {
    Vertex verts[SCRATCH_VERTS];
    u32    indices[SCRATCH_INDICES];
    u32    vertCount  = 0;
    u32    indexCount = 0;
    u8     materialId = 0;
    bool   used       = false;
};

// Heap-allocated scratch buckets — only used during buildAll(), freed after.
// Avoids 3.6MB of BSS which crashes the Switch at static init.
static MaterialBucket* s_buckets = nullptr;

static void resetBuckets() {
    for (u32 i = 0; i < MAX_SUBMESHES_PER_SECTION; i++) {
        s_buckets[i].vertCount  = 0;
        s_buckets[i].indexCount = 0;
        s_buckets[i].materialId = 0;
        s_buckets[i].used       = false;
    }
}

static MaterialBucket* getBucket(u8 matId) {
    // Find existing bucket for this material
    for (u32 i = 0; i < MAX_SUBMESHES_PER_SECTION; i++) {
        if (s_buckets[i].used && s_buckets[i].materialId == matId)
            return &s_buckets[i];
    }
    // Allocate new bucket
    for (u32 i = 0; i < MAX_SUBMESHES_PER_SECTION; i++) {
        if (!s_buckets[i].used) {
            s_buckets[i].used = true;
            s_buckets[i].materialId = matId;
            return &s_buckets[i];
        }
    }
    // All buckets full — use first bucket as fallback
    return &s_buckets[0];
}

static void pushQuad(MaterialBucket& bkt,
                     Vertex v0, Vertex v1, Vertex v2, Vertex v3)
{
    if (bkt.vertCount + 4 > SCRATCH_VERTS || bkt.indexCount + 6 > SCRATCH_INDICES) return;

    u32 base = bkt.vertCount;
    bkt.verts[base+0] = v0;
    bkt.verts[base+1] = v1;
    bkt.verts[base+2] = v2;
    bkt.verts[base+3] = v3;
    bkt.vertCount += 4;

    bkt.indices[bkt.indexCount++] = base+0;
    bkt.indices[bkt.indexCount++] = base+1;
    bkt.indices[bkt.indexCount++] = base+2;
    bkt.indices[bkt.indexCount++] = base+0;
    bkt.indices[bkt.indexCount++] = base+2;
    bkt.indices[bkt.indexCount++] = base+3;
}

// -------------------------------------------------------------------------
// Decoration props — registered CPU geometry baked into floor sections
// -------------------------------------------------------------------------
static constexpr u32 MAX_PROP_MESHES = 8;

// One registered prop's CPU geometry (owned copy). Scattered onto floors and appended into the
// section's material bucket in buildSection, so props add no per-frame draw calls of their own.
struct PropMeshData {
    std::vector<Vertex> verts;
    std::vector<u32>    indices;
    u8   materialId = 0;
    f32  radius     = 0.3f;
};
static PropMeshData s_propMeshes[MAX_PROP_MESHES];
static u32          s_propMeshCount = 0;

// Fraction of eligible (open, wall-clear) floor cells that get a prop. Kept low so decorations
// read as occasional accents, not clutter. Deterministic per cell via the floor seed.
static constexpr f32 PROP_DENSITY = 0.10f;

void LevelMeshSystem::addPropMesh(const Vertex* verts, u32 vertCount,
                                  const u32* indices, u32 indexCount,
                                  u8 materialId, f32 radius)
{
    if (s_propMeshCount >= MAX_PROP_MESHES || vertCount == 0 || indexCount == 0) return;
    PropMeshData& p = s_propMeshes[s_propMeshCount++];
    p.verts.assign(verts, verts + vertCount);
    p.indices.assign(indices, indices + indexCount);
    p.materialId = materialId;
    p.radius     = radius;
}

void LevelMeshSystem::clearPropMeshes() {
    for (u32 i = 0; i < MAX_PROP_MESHES; i++) {
        s_propMeshes[i].verts.clear();
        s_propMeshes[i].indices.clear();
    }
    s_propMeshCount = 0;
}

// Transform a prop's local geometry by `xf` (translate * rotateY only — no scale, so the normal
// transform is just the rotation) and append it into `bkt`, remapping indices to the bucket's
// current vertex base. Props keep white vertex color (they aren't tile-shaded). Silently skips
// if the bucket lacks scratch room, so a crowded section just drops the last few props.
static void appendProp(MaterialBucket& bkt, const PropMeshData& prop, const Mat4& xf) {
    const u32 vc = (u32)prop.verts.size();
    const u32 ic = (u32)prop.indices.size();
    if (bkt.vertCount + vc > SCRATCH_VERTS || bkt.indexCount + ic > SCRATCH_INDICES) return;

    const u32 base = bkt.vertCount;
    for (u32 i = 0; i < vc; i++) {
        Vertex v = prop.verts[i];
        Vec4 wp = xf * vec4(v.position, 1.0f);   // point: w=1 applies translation
        Vec4 wn = xf * vec4(v.normal,   0.0f);   // direction: w=0 drops translation
        v.position = {wp.x, wp.y, wp.z};
        v.normal   = normalize(Vec3{wn.x, wn.y, wn.z});
        v.color    = {1.0f, 1.0f, 1.0f};
        bkt.verts[base + i] = v;
    }
    bkt.vertCount += vc;
    for (u32 i = 0; i < ic; i++)
        bkt.indices[bkt.indexCount++] = base + prop.indices[i];
}

// -------------------------------------------------------------------------
// Per-section mesh build
// -------------------------------------------------------------------------
static void buildSection(const LevelGrid& grid, u32 seed,
                         u32 startX, u32 startZ,
                         u32 endX,   u32 endZ,
                         LevelSection& out)
{
    resetBuckets();

    f32 cs = grid.cellSize;

    // AABB tracking
    f32 minX =  1e30f, minY =  1e30f, minZ =  1e30f;
    f32 maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;

    auto expand = [&](f32 x, f32 y, f32 z) {
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
        if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
    };

    for (u32 z = startZ; z < endZ; z++) {
        for (u32 x = startX; x < endX; x++) {
            f32 wx = x * cs;
            f32 wz = z * cs;

            // Subtle baked per-tile shade (soft ±8% blobs, ~3-cell frequency) — breaks up the
            // otherwise-identical tiling. Baked into vertex color, so it's free per-frame and
            // deterministic per floor (seed = levelSeed → host/clients match).
            f32 shade = 0.92f + TileNoise::value(wx * 0.35f, wz * 0.35f, seed) * 0.16f;
            Vec3 tileTint{shade, shade, shade};

            const GridCell& cell = LevelGridSystem::getCell(grid, x, z);
            bool solid = (cell.flags & CELL_SOLID) != 0;

            if (solid) {
                // Emit wall quads facing empty neighbours
                struct WallNeighbour { s32 dx, dz; Vec3 normal; };
                static constexpr WallNeighbour kNeighbours[4] = {
                    {  0, -1, { 0, 0, -1} },
                    {  0,  1, { 0, 0,  1} },
                    { -1,  0, {-1, 0,  0} },
                    {  1,  0, { 1, 0,  0} },
                };

                for (auto& nb : kNeighbours) {
                    s32 nx = (s32)x + nb.dx;
                    s32 nz = (s32)z + nb.dz;
                    if (!LevelGridSystem::isInBounds(grid, (u32)nx, (u32)nz)) continue;
                    if (LevelGridSystem::isSolid(grid, (u32)nx, (u32)nz)) continue;

                    const GridCell& nbCell = LevelGridSystem::getCell(grid, (u32)nx, (u32)nz);
                    f32 nbFloor = LevelGridSystem::getFloorHeight(grid, (u32)nx, (u32)nz);
                    f32 nbCeil  = LevelGridSystem::getCeilingHeight(grid, (u32)nx, (u32)nz);

                    f32 wallBot = nbFloor;
                    f32 wallTop = nbCeil;
                    if (wallTop <= wallBot) continue;

                    // Use the wall material from the neighbour's open cell
                    u8 wallMat = nbCell.wallMaterialId;
                    MaterialBucket* bkt = getBucket(wallMat);

                    Vec3 n = nb.normal;
                    Vec3 p0, p1, p2, p3;
                    if (nb.dz == -1) {
                        p0 = {wx + cs, wallBot, wz};
                        p1 = {wx,      wallBot, wz};
                        p2 = {wx,      wallTop, wz};
                        p3 = {wx + cs, wallTop, wz};
                    } else if (nb.dz == 1) {
                        p0 = {wx,      wallBot, wz + cs};
                        p1 = {wx + cs, wallBot, wz + cs};
                        p2 = {wx + cs, wallTop, wz + cs};
                        p3 = {wx,      wallTop, wz + cs};
                    } else if (nb.dx == -1) {
                        p0 = {wx, wallBot, wz};
                        p1 = {wx, wallBot, wz + cs};
                        p2 = {wx, wallTop, wz + cs};
                        p3 = {wx, wallTop, wz};
                    } else {
                        p0 = {wx + cs, wallBot, wz + cs};
                        p1 = {wx + cs, wallBot, wz};
                        p2 = {wx + cs, wallTop, wz};
                        p3 = {wx + cs, wallTop, wz + cs};
                    }

                    f32 uSpan = cs;
                    f32 vSpan = wallTop - wallBot;
                    Vertex v0{p0, n, {0.0f,  0.0f  }};
                    Vertex v1{p1, n, {uSpan, 0.0f  }};
                    Vertex v2{p2, n, {uSpan, vSpan }};
                    Vertex v3{p3, n, {0.0f,  vSpan }};
                    v0.color = v1.color = v2.color = v3.color = tileTint;

                    pushQuad(*bkt, v0, v1, v2, v3);
                    expand(p0.x, p0.y, p0.z); expand(p1.x, p1.y, p1.z);
                    expand(p2.x, p2.y, p2.z); expand(p3.x, p3.y, p3.z);
                }
            } else {
                f32 floorH = LevelGridSystem::getFloorHeight(grid, x, z);
                f32 ceilH  = LevelGridSystem::getCeilingHeight(grid, x, z);

                // Floor quad
                if (cell.flags & CELL_FLOOR) {
                    MaterialBucket* bkt = getBucket(cell.floorMaterialId);
                    Vec3 n{0.0f, 1.0f, 0.0f};
                    Vec3 p0{wx,      floorH, wz + cs};
                    Vec3 p1{wx + cs, floorH, wz + cs};
                    Vec3 p2{wx + cs, floorH, wz};
                    Vec3 p3{wx,      floorH, wz};
                    Vertex v0{p0, n, {0.0f, cs  }};
                    Vertex v1{p1, n, {cs,   cs  }};
                    Vertex v2{p2, n, {cs,   0.0f}};
                    Vertex v3{p3, n, {0.0f, 0.0f}};
                    v0.color = v1.color = v2.color = v3.color = tileTint;
                    pushQuad(*bkt, v0, v1, v2, v3);
                    expand(wx, floorH, wz); expand(wx+cs, floorH, wz+cs);

                    // --- Floor-height riser faces (vertical variety) -----------------------------
                    // Where an OPEN neighbour sits lower than this cell, emit the vertical step face
                    // between them (neighbour's floor up to ours). Without this a raised platform or
                    // tier renders as a floating quad with a gap at its edge. Drawn only toward LOWER
                    // neighbours, so the HIGHER cell owns each shared riser and it is emitted exactly
                    // once; same material + winding convention as the solid-cell wall faces above.
                    {
                        MaterialBucket* rbkt = getBucket(cell.wallMaterialId);
                        static const s32 kdx[4] = {1, -1, 0, 0};
                        static const s32 kdz[4] = {0, 0, 1, -1};
                        for (int ei = 0; ei < 4; ei++) {
                            s32 nx = (s32)x + kdx[ei], nz = (s32)z + kdz[ei];
                            if (!LevelGridSystem::isInBounds(grid, (u32)nx, (u32)nz)) continue;
                            if (LevelGridSystem::isSolid(grid, (u32)nx, (u32)nz)) continue; // solid edge already walls
                            f32 nbF = LevelGridSystem::getFloorHeight(grid, (u32)nx, (u32)nz);
                            if (nbF >= floorH - 0.001f) continue;   // neighbour not lower — no riser
                            const f32 B = nbF, T = floorH, vSpan = T - B;
                            Vec3 rn, rp0, rp1, rp2, rp3;
                            if (kdz[ei] == -1)      { rn = {0,0,-1}; rp0={wx+cs,B,wz};    rp1={wx,B,wz};       rp2={wx,T,wz};       rp3={wx+cs,T,wz}; }
                            else if (kdz[ei] == 1)  { rn = {0,0, 1}; rp0={wx,B,wz+cs};    rp1={wx+cs,B,wz+cs}; rp2={wx+cs,T,wz+cs}; rp3={wx,T,wz+cs}; }
                            else if (kdx[ei] == -1) { rn = {-1,0,0}; rp0={wx,B,wz};       rp1={wx,B,wz+cs};    rp2={wx,T,wz+cs};    rp3={wx,T,wz}; }
                            else                    { rn = { 1,0,0}; rp0={wx+cs,B,wz+cs}; rp1={wx+cs,B,wz};    rp2={wx+cs,T,wz};    rp3={wx+cs,T,wz+cs}; }
                            Vertex rv0{rp0, rn, {0.0f, 0.0f }};
                            Vertex rv1{rp1, rn, {cs,   0.0f }};
                            Vertex rv2{rp2, rn, {cs,   vSpan}};
                            Vertex rv3{rp3, rn, {0.0f, vSpan}};
                            rv0.color = rv1.color = rv2.color = rv3.color = tileTint;
                            pushQuad(*rbkt, rv0, rv1, rv2, rv3);
                            expand(rp0.x, B, rp0.z); expand(rp2.x, T, rp2.z);
                        }
                    }

                    // Scatter a decoration prop onto some open floor cells and bake it in.
                    // Deterministic in (cell, floor seed) so host + clients build identical
                    // floors with zero netcode, and it costs no extra draw calls.
                    if (s_propMeshCount > 0 &&
                        TileNoise::hash2((s32)x, (s32)z, seed ^ 0x9E3779B9u) < PROP_DENSITY) {
                        // Only place where all 4 orthogonal neighbours are open, so props sit in
                        // room/corridor interiors and never clip a wall base or a doorway.
                        bool clear = true;
                        static constexpr s32 kOff[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                        for (auto& o : kOff) {
                            u32 nnx = (u32)((s32)x + o[0]);
                            u32 nnz = (u32)((s32)z + o[1]);
                            if (!LevelGridSystem::isInBounds(grid, nnx, nnz) ||
                                 LevelGridSystem::isSolid(grid, nnx, nnz)) { clear = false; break; }
                        }
                        if (clear) {
                            // Independent hashes (distinct seed salts) → uncorrelated pick /
                            // jitter / rotation, so props look randomly placed, not gridded.
                            u32 pick = (u32)(TileNoise::hash2((s32)x, (s32)z, seed ^ 0x85EBCA6Bu)
                                             * (f32)s_propMeshCount);
                            if (pick >= s_propMeshCount) pick = s_propMeshCount - 1;
                            f32 jx  = (TileNoise::hash2((s32)x, (s32)z, seed ^ 0xC2B2AE35u) - 0.5f) * cs * 0.4f;
                            f32 jz  = (TileNoise::hash2((s32)x, (s32)z, seed ^ 0x27D4EB2Fu) - 0.5f) * cs * 0.4f;
                            f32 rot =  TileNoise::hash2((s32)x, (s32)z, seed ^ 0x165667B1u) * 6.2831853f;
                            Vec3 pos{wx + cs * 0.5f + jx, floorH, wz + cs * 0.5f + jz};
                            Mat4 xf = Mat4::translate(pos) * Mat4::rotateY(rot);
                            const PropMeshData& prop = s_propMeshes[pick];
                            appendProp(*getBucket(prop.materialId), prop, xf);
                            expand(pos.x, floorH + 0.6f, pos.z);  // cover prop height in bounds
                        }
                    }
                }

                // Ceiling quad
                if (cell.flags & CELL_CEILING) {
                    MaterialBucket* bkt = getBucket(cell.ceilMaterialId);
                    Vec3 n{0.0f, -1.0f, 0.0f};
                    Vec3 p0{wx,      ceilH, wz};
                    Vec3 p1{wx + cs, ceilH, wz};
                    Vec3 p2{wx + cs, ceilH, wz + cs};
                    Vec3 p3{wx,      ceilH, wz + cs};
                    Vertex v0{p0, n, {0.0f, 0.0f}};
                    Vertex v1{p1, n, {cs,   0.0f}};
                    Vertex v2{p2, n, {cs,   cs  }};
                    Vertex v3{p3, n, {0.0f, cs  }};
                    v0.color = v1.color = v2.color = v3.color = tileTint;
                    pushQuad(*bkt, v0, v1, v2, v3);
                    expand(wx, ceilH, wz); expand(wx+cs, ceilH, wz+cs);
                }

                // Platform slab (CELL_PLATFORM): the second story. Top drawn like a floor quad at
                // the slab top, underside like a ceiling quad at the underside (skipped when the
                // clamp left no under-space — low stair steps), rim faces toward every neighbour
                // that doesn't cover our band. Ownership mirrors the riser faces: each cell draws
                // only the part of its own rim a neighbour leaves exposed, so shared faces emit
                // exactly once and stair runs show just their 0.25 m step slivers.
                if (cell.flags & CELL_PLATFORM) {
                    const f32 topH = LevelGridSystem::getPlatformTop(grid, x, z);
                    const f32 undH = LevelGridSystem::getPlatformUnderside(grid, x, z);
                    MaterialBucket* pbkt = getBucket(cell.platMaterialId[0]);   // Phase 0: single-slab read
                    {   // top (+Y) — same layout/winding as the floor quad
                        Vec3 n{0.0f, 1.0f, 0.0f};
                        Vec3 p0{wx,      topH, wz + cs};
                        Vec3 p1{wx + cs, topH, wz + cs};
                        Vec3 p2{wx + cs, topH, wz};
                        Vec3 p3{wx,      topH, wz};
                        Vertex v0{p0, n, {0.0f, cs  }};
                        Vertex v1{p1, n, {cs,   cs  }};
                        Vertex v2{p2, n, {cs,   0.0f}};
                        Vertex v3{p3, n, {0.0f, 0.0f}};
                        v0.color = v1.color = v2.color = v3.color = tileTint;
                        pushQuad(*pbkt, v0, v1, v2, v3);
                        expand(wx, topH, wz); expand(wx + cs, topH, wz + cs);
                    }
                    if (undH > floorH + 0.001f) {   // underside (−Y) — same winding as the ceiling quad
                        Vec3 n{0.0f, -1.0f, 0.0f};
                        Vec3 p0{wx,      undH, wz};
                        Vec3 p1{wx + cs, undH, wz};
                        Vec3 p2{wx + cs, undH, wz + cs};
                        Vec3 p3{wx,      undH, wz + cs};
                        Vertex v0{p0, n, {0.0f, 0.0f}};
                        Vertex v1{p1, n, {cs,   0.0f}};
                        Vertex v2{p2, n, {cs,   cs  }};
                        Vertex v3{p3, n, {0.0f, cs  }};
                        v0.color = v1.color = v2.color = v3.color = tileTint;
                        pushQuad(*pbkt, v0, v1, v2, v3);
                        expand(wx, undH, wz); expand(wx + cs, undH, wz + cs);
                    }
                    {   // rim faces. A rim spans the part of OUR band [undH, topH] a neighbour leaves
                        // exposed. A platform neighbour covers its OWN band [nbUnd, nbTop]; subtracting
                        // that from ours can leave a LOWER strip (below the neighbour's underside) and/or
                        // an UPPER strip (above its top) — a stepped slab staircase exposes a thin strip
                        // on BOTH the taller side (upper) and the shorter side (lower), so both must be
                        // emitted or the underside of a stair develops a see-through gap. Equal-thickness
                        // slabs make only one strip non-empty per neighbour, so a shared edge still emits
                        // exactly one face; an equal-height neighbour emits none (interior edge). Winding
                        // mirrors the riser faces above.
                        MaterialBucket* rbkt = getBucket(cell.wallMaterialId);
                        static const s32 kpdx[4] = {1, -1, 0, 0};
                        static const s32 kpdz[4] = {0, 0, 1, -1};
                        // Emit one vertical rim quad spanning [B,T] on edge ei; self-guards degenerate spans.
                        auto emitRim = [&](int ei, f32 B, f32 T) {
                            if (T <= B + 0.001f) return;
                            const f32 vSpan = T - B;
                            Vec3 rn, rp0, rp1, rp2, rp3;
                            if (kpdz[ei] == -1)      { rn = {0,0,-1}; rp0={wx+cs,B,wz};    rp1={wx,B,wz};       rp2={wx,T,wz};       rp3={wx+cs,T,wz}; }
                            else if (kpdz[ei] == 1)  { rn = {0,0, 1}; rp0={wx,B,wz+cs};    rp1={wx+cs,B,wz+cs}; rp2={wx+cs,T,wz+cs}; rp3={wx,T,wz+cs}; }
                            else if (kpdx[ei] == -1) { rn = {-1,0,0}; rp0={wx,B,wz};       rp1={wx,B,wz+cs};    rp2={wx,T,wz+cs};    rp3={wx,T,wz}; }
                            else                     { rn = { 1,0,0}; rp0={wx+cs,B,wz+cs}; rp1={wx+cs,B,wz};    rp2={wx+cs,T,wz};    rp3={wx+cs,T,wz+cs}; }
                            Vertex rv0{rp0, rn, {0.0f, 0.0f }};
                            Vertex rv1{rp1, rn, {cs,   0.0f }};
                            Vertex rv2{rp2, rn, {cs,   vSpan}};
                            Vertex rv3{rp3, rn, {0.0f, vSpan}};
                            rv0.color = rv1.color = rv2.color = rv3.color = tileTint;
                            pushQuad(*rbkt, rv0, rv1, rv2, rv3);
                            expand(rp0.x, B, rp0.z); expand(rp2.x, T, rp2.z);
                        };
                        for (int ei = 0; ei < 4; ei++) {
                            s32 nx = (s32)x + kpdx[ei], nz = (s32)z + kpdz[ei];
                            if (!LevelGridSystem::isInBounds(grid, (u32)nx, (u32)nz)) continue;
                            if (LevelGridSystem::isSolid(grid, (u32)nx, (u32)nz)) continue; // wall face covers it
                            if (LevelGridSystem::hasPlatform(grid, (u32)nx, (u32)nz)) {
                                const f32 nbTop = LevelGridSystem::getPlatformTop(grid, (u32)nx, (u32)nz);
                                const f32 nbUnd = LevelGridSystem::getPlatformUnderside(grid, (u32)nx, (u32)nz);
                                // exposed = our band minus the neighbour's band → up to two strips (each self-guards).
                                emitRim(ei, undH, nbUnd < topH ? nbUnd : topH);          // lower strip
                                emitRim(ei, nbTop > undH ? nbTop : undH, topH);          // upper strip
                            } else {
                                emitRim(ei, undH, topH);                                 // full exposed band
                            }
                        }
                    }
                }
            }
        }
    }

    // Build submeshes from used buckets
    out.submeshCount = 0;
    bool anyGeometry = false;

    for (u32 i = 0; i < MAX_SUBMESHES_PER_SECTION; i++) {
        if (!s_buckets[i].used || s_buckets[i].indexCount == 0) continue;

        SectionSubmesh& sm = out.submeshes[out.submeshCount];
        sm.mesh = MeshSystem::create(s_buckets[i].verts, s_buckets[i].vertCount,
                                     s_buckets[i].indices, s_buckets[i].indexCount);
        sm.materialId = s_buckets[i].materialId;
        out.submeshCount++;
        anyGeometry = true;
    }

    if (anyGeometry) {
        out.bounds = {{minX, minY, minZ}, {maxX, maxY, maxZ}};
    } else {
        out.bounds = {{0,0,0},{0,0,0}};
    }
    out.model = Mat4::identity();
    out.dirty = false;
}

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------
u32 LevelMeshSystem::buildAll(const LevelGrid& grid, u32 seed,
                               LevelSection* outSections, u32 maxSects)
{
    // Allocate scratch buckets on heap (3.6MB — too large for BSS on Switch)
    if (!s_buckets) s_buckets = new MaterialBucket[MAX_SUBMESHES_PER_SECTION];

    u32 sx = (grid.width + SECTION_SIZE - 1) / SECTION_SIZE;
    u32 sz = (grid.depth + SECTION_SIZE - 1) / SECTION_SIZE;
    u32 total = sx * sz;
    ENGINE_ASSERT(total <= maxSects, "Not enough section slots for this grid");

    u32 count = 0;
    for (u32 gz = 0; gz < sz; gz++) {
        for (u32 gx = 0; gx < sx; gx++) {
            u32 x0 = gx * SECTION_SIZE;
            u32 z0 = gz * SECTION_SIZE;
            u32 x1 = (x0 + SECTION_SIZE < grid.width)  ? x0 + SECTION_SIZE : grid.width;
            u32 z1 = (z0 + SECTION_SIZE < grid.depth)  ? z0 + SECTION_SIZE : grid.depth;
            buildSection(grid, seed, x0, z0, x1, z1, outSections[count]);
            count++;
        }
    }

    // Free scratch buckets after building (reclaim 3.6MB)
    delete[] s_buckets;
    s_buckets = nullptr;

    LOG_INFO("LevelMesh: built %u sections", count);
    return count;
}

void LevelMeshSystem::submitAll(const LevelSection* sections, u32 count,
                                 const Shader& shader)
{
    for (u32 i = 0; i < count; i++) {
        for (u32 j = 0; j < sections[i].submeshCount; j++) {
            const SectionSubmesh& sm = sections[i].submeshes[j];
            if (sm.mesh.indexCount == 0) continue;
            const Material* mat = MaterialSystem::get(sm.materialId);
            Renderer::submit(shader, mat->texture, sm.mesh,
                             sections[i].model, sections[i].bounds, mat->tint);
        }
    }
}

void LevelMeshSystem::destroyAll(LevelSection* sections, u32 count) {
    for (u32 i = 0; i < count; i++) {
        for (u32 j = 0; j < sections[i].submeshCount; j++) {
            if (sections[i].submeshes[j].mesh.vao) {
                MeshSystem::destroy(sections[i].submeshes[j].mesh);
            }
        }
        sections[i].submeshCount = 0;
    }
}
