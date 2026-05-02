#include "world/level_mesh.h"
#include "renderer/renderer.h"
#include "core/log.h"
#include "core/assert.h"

#include <cstdlib>
#include <cstring>

// -------------------------------------------------------------------------
// Scratch buffer — allocated once, reused each buildAll call.
// Worst case for one 16x16 section:
//   Each cell can emit at most 5 quads (4 walls + floor or ceiling).
//   Each quad = 4 vertices + 6 indices.
//   16*16 = 256 cells * 5 quads * 4 verts = 5120 verts; * 6 idx = 7680 idx.
//   Use 2x headroom: 12288 verts, 16384 indices.
// -------------------------------------------------------------------------
static constexpr u32 SCRATCH_VERTS   = 12288;
static constexpr u32 SCRATCH_INDICES = 16384;

static Vertex* s_verts   = nullptr;
static u32*    s_indices = nullptr;
static bool    s_scratchInit = false;

static void ensureScratch() {
    if (s_scratchInit) return;
    s_verts   = static_cast<Vertex*>(std::malloc(SCRATCH_VERTS   * sizeof(Vertex)));
    s_indices = static_cast<u32*>   (std::malloc(SCRATCH_INDICES * sizeof(u32)));
    ENGINE_ASSERT(s_verts && s_indices, "Level mesh scratch allocation failed");
    s_scratchInit = true;
}

// -------------------------------------------------------------------------
// Helpers to push quads into scratch buffers
// -------------------------------------------------------------------------
struct QuadContext {
    Vertex* verts;
    u32*    indices;
    u32     vertCount;
    u32     indexCount;
};

// Emit a single quad: four vertices (already filled) + 6 indices (two triangles).
static void pushQuad(QuadContext& ctx,
                     Vertex v0, Vertex v1, Vertex v2, Vertex v3)
{
    ENGINE_ASSERT(ctx.vertCount  + 4 <= SCRATCH_VERTS,   "Vertex scratch overflow");
    ENGINE_ASSERT(ctx.indexCount + 6 <= SCRATCH_INDICES, "Index scratch overflow");

    u32 base = ctx.vertCount;
    ctx.verts[base+0] = v0;
    ctx.verts[base+1] = v1;
    ctx.verts[base+2] = v2;
    ctx.verts[base+3] = v3;
    ctx.vertCount += 4;

    // CCW winding when viewed from outside
    ctx.indices[ctx.indexCount++] = base+0;
    ctx.indices[ctx.indexCount++] = base+1;
    ctx.indices[ctx.indexCount++] = base+2;
    ctx.indices[ctx.indexCount++] = base+0;
    ctx.indices[ctx.indexCount++] = base+2;
    ctx.indices[ctx.indexCount++] = base+3;
}

// -------------------------------------------------------------------------
// Per-section mesh build
// -------------------------------------------------------------------------
static void buildSection(const LevelGrid& grid,
                         u32 startX, u32 startZ,
                         u32 endX,   u32 endZ,
                         LevelSection& out)
{
    ensureScratch();

    QuadContext ctx;
    ctx.verts      = s_verts;
    ctx.indices    = s_indices;
    ctx.vertCount  = 0;
    ctx.indexCount = 0;

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
            f32 wx = x * cs;  // world X of cell origin (min corner)
            f32 wz = z * cs;  // world Z of cell origin (min corner)

            bool solid = LevelGridSystem::isSolid(grid, x, z);

            if (solid) {
                // Emit wall quads facing empty neighbours
                f32 floorH   = LevelGridSystem::getFloorHeight(grid,   x, z);
                f32 ceilH    = LevelGridSystem::getCeilingHeight(grid,  x, z);
                // For solid cells treat floor=0, ceiling=ceilH (wall height)
                // We emit visible wall faces outward toward empty neighbours.

                // North face (toward -Z): visible if cell at (x, z-1) is empty
                // South face (toward +Z): visible if cell at (x, z+1) is empty
                // West  face (toward -X): visible if cell at (x-1, z) is empty
                // East  face (toward +X): visible if cell at (x+1, z) is empty
                //
                // Wall extends from floor to ceiling of the solid cell.
                // If the neighbour is an open cell we use the higher of the two
                // floor heights as the wall bottom, and the lower ceiling as top.

                struct WallNeighbour { s32 dx, dz; Vec3 normal; };
                static constexpr WallNeighbour kNeighbours[4] = {
                    {  0, -1, { 0, 0, -1} }, // North
                    {  0,  1, { 0, 0,  1} }, // South
                    { -1,  0, {-1, 0,  0} }, // West
                    {  1,  0, { 1, 0,  0} }, // East
                };

                for (auto& nb : kNeighbours) {
                    s32 nx = (s32)x + nb.dx;
                    s32 nz = (s32)z + nb.dz;
                    // Skip if out of bounds (OOB treated as solid — no face needed)
                    if (!LevelGridSystem::isInBounds(grid, (u32)nx, (u32)nz)) continue;
                    if (LevelGridSystem::isSolid(grid, (u32)nx, (u32)nz)) continue;

                    // Open neighbour: compute wall extent
                    f32 nbFloor = LevelGridSystem::getFloorHeight(grid,   (u32)nx, (u32)nz);
                    f32 nbCeil  = LevelGridSystem::getCeilingHeight(grid,  (u32)nx, (u32)nz);

                    f32 wallBot = nbFloor;  // bottom of visible wall
                    f32 wallTop = nbCeil;   // top of visible wall
                    if (wallTop <= wallBot) continue; // degenerate

                    // Four corners of the wall quad (viewed from outside the solid cell)
                    Vec3 n = nb.normal;
                    // Build two axes in the wall plane
                    // For N/S walls: right axis = +X; for E/W walls: right axis = +Z
                    Vec3 right = (nb.dx == 0)
                        ? Vec3{cs, 0.0f, 0.0f}
                        : Vec3{0.0f, 0.0f, cs};

                    // Origin of the face (corner of the solid cell on the side facing nb)
                    Vec3 faceOrigin;
                    if (nb.dx ==  1) faceOrigin = {wx + cs, 0.0f, wz};        // East face
                    else if (nb.dx == -1) faceOrigin = {wx,       0.0f, wz};  // West face (right = +Z from this side, need to flip to face outward)
                    else if (nb.dz ==  1) faceOrigin = {wx,       0.0f, wz + cs}; // South face
                    else                  faceOrigin = {wx,       0.0f, wz};       // North face

                    // West and North faces need right axis reversed so winding faces outward
                    if (nb.dx == -1) right = {0.0f, 0.0f, cs};  // already correct
                    if (nb.dz == -1) right = {cs,   0.0f, 0.0f}; // already correct

                    // Winding: CCW when seen from outside (away from solid cell)
                    // For +X normal: right=+Z, from (face, bot, wz) going to (face, top, wz+cs)
                    // Simplify: build 4 corner positions then choose winding per face direction.

                    Vec3 p0, p1, p2, p3;
                    if (nb.dz == -1) { // North: normal -Z, face at wz
                        p0 = {wx + cs, wallBot, wz};
                        p1 = {wx,      wallBot, wz};
                        p2 = {wx,      wallTop, wz};
                        p3 = {wx + cs, wallTop, wz};
                    } else if (nb.dz == 1) { // South: normal +Z, face at wz+cs
                        p0 = {wx,      wallBot, wz + cs};
                        p1 = {wx + cs, wallBot, wz + cs};
                        p2 = {wx + cs, wallTop, wz + cs};
                        p3 = {wx,      wallTop, wz + cs};
                    } else if (nb.dx == -1) { // West: normal -X, face at wx
                        p0 = {wx, wallBot, wz};
                        p1 = {wx, wallBot, wz + cs};
                        p2 = {wx, wallTop, wz + cs};
                        p3 = {wx, wallTop, wz};
                    } else { // East: normal +X, face at wx+cs
                        p0 = {wx + cs, wallBot, wz + cs};
                        p1 = {wx + cs, wallBot, wz};
                        p2 = {wx + cs, wallTop, wz};
                        p3 = {wx + cs, wallTop, wz + cs};
                    }

                    f32 uSpan = (nb.dz == 0) ? cs : cs; // texture repeat per cell
                    f32 vSpan = wallTop - wallBot;
                    Vertex v0{p0, n, {0.0f,    0.0f   }};
                    Vertex v1{p1, n, {uSpan,   0.0f   }};
                    Vertex v2{p2, n, {uSpan,   vSpan  }};
                    Vertex v3{p3, n, {0.0f,    vSpan  }};

                    pushQuad(ctx, v0, v1, v2, v3);
                    expand(p0.x, p0.y, p0.z); expand(p1.x, p1.y, p1.z);
                    expand(p2.x, p2.y, p2.z); expand(p3.x, p3.y, p3.z);
                }

                (void)floorH; // used indirectly via nbFloor/nbCeil
            } else {
                // Empty cell: emit floor quad and ceiling quad
                f32 floorH = LevelGridSystem::getFloorHeight(grid,   x, z);
                f32 ceilH  = LevelGridSystem::getCeilingHeight(grid,  x, z);

                // Floor quad (normal = +Y)
                if (LevelGridSystem::getCell(grid, x, z).flags & CELL_FLOOR) {
                    Vec3 n{0.0f, 1.0f, 0.0f};
                    Vec3 p0{wx,      floorH, wz + cs};
                    Vec3 p1{wx + cs, floorH, wz + cs};
                    Vec3 p2{wx + cs, floorH, wz};
                    Vec3 p3{wx,      floorH, wz};
                    Vertex v0{p0, n, {0.0f, cs  }};
                    Vertex v1{p1, n, {cs,   cs  }};
                    Vertex v2{p2, n, {cs,   0.0f}};
                    Vertex v3{p3, n, {0.0f, 0.0f}};
                    pushQuad(ctx, v0, v1, v2, v3);
                    expand(wx, floorH, wz); expand(wx+cs, floorH, wz+cs);
                }

                // Ceiling quad (normal = -Y, flipped winding)
                if (LevelGridSystem::getCell(grid, x, z).flags & CELL_CEILING) {
                    Vec3 n{0.0f, -1.0f, 0.0f};
                    Vec3 p0{wx,      ceilH, wz};
                    Vec3 p1{wx + cs, ceilH, wz};
                    Vec3 p2{wx + cs, ceilH, wz + cs};
                    Vec3 p3{wx,      ceilH, wz + cs};
                    Vertex v0{p0, n, {0.0f, 0.0f}};
                    Vertex v1{p1, n, {cs,   0.0f}};
                    Vertex v2{p2, n, {cs,   cs  }};
                    Vertex v3{p3, n, {0.0f, cs  }};
                    pushQuad(ctx, v0, v1, v2, v3);
                    expand(wx, ceilH, wz); expand(wx+cs, ceilH, wz+cs);
                }
            }
        }
    }

    if (ctx.indexCount == 0) {
        out.mesh   = {};
        out.bounds = {{0,0,0},{0,0,0}};
        out.model  = Mat4::identity();
        return;
    }

    out.mesh   = MeshSystem::create(ctx.verts, ctx.vertCount,
                                    ctx.indices, ctx.indexCount);
    out.bounds = {{minX, minY, minZ}, {maxX, maxY, maxZ}};
    out.model  = Mat4::identity();
    out.dirty  = false;
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
                                 const Shader& shader, const Texture& texture)
{
    for (u32 i = 0; i < count; i++) {
        if (sections[i].mesh.indexCount == 0) continue;
        Renderer::submit(shader, texture, sections[i].mesh,
                         sections[i].model, sections[i].bounds);
    }
}

void LevelMeshSystem::destroyAll(LevelSection* sections, u32 count) {
    for (u32 i = 0; i < count; i++) {
        if (sections[i].mesh.vao) {
            MeshSystem::destroy(sections[i].mesh);
        }
    }
}
