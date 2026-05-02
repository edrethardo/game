#include "world/level_mesh.h"
#include "renderer/renderer.h"
#include "renderer/material.h"
#include "core/log.h"
#include "core/assert.h"

#include <cstdlib>
#include <cstring>

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

// We keep a small static pool of buckets
static MaterialBucket s_buckets[MAX_SUBMESHES_PER_SECTION];

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
// Per-section mesh build
// -------------------------------------------------------------------------
static void buildSection(const LevelGrid& grid,
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
                    pushQuad(*bkt, v0, v1, v2, v3);
                    expand(wx, floorH, wz); expand(wx+cs, floorH, wz+cs);
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
                    pushQuad(*bkt, v0, v1, v2, v3);
                    expand(wx, ceilH, wz); expand(wx+cs, ceilH, wz+cs);
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
u32 LevelMeshSystem::buildAll(const LevelGrid& grid,
                               LevelSection* outSections, u32 maxSects)
{
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
            buildSection(grid, x0, z0, x1, z1, outSections[count]);
            count++;
        }
    }

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
