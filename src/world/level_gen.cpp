#include "world/level_gen.h"
#include "core/log.h"

// Hardcoded rooms (x, z, w, d in cells) — works for a 32×32 grid
struct Room { u32 x, z, w, d; };

static constexpr Room k_rooms[] = {
    { 2,  2,  8,  6},  // room 0 — spawn room
    {13,  2,  7,  7},  // room 1
    {22,  2,  8,  6},  // room 2
    { 2, 12,  6,  8},  // room 3
    {14, 13,  8,  6},  // room 4 — raised floor
};
static constexpr u32 k_roomCount = 5;

// Carve a rectangular room into the grid
static void carveRoom(LevelGrid& grid, const Room& r,
                      f32 floorH, f32 ceilH,
                      u8 wallMat = 0, u8 floorMat = 1, u8 ceilMat = 2)
{
    u8 floorEnc = static_cast<u8>(floorH / 0.25f);
    u8 ceilEnc  = static_cast<u8>(ceilH  / 0.25f);

    for (u32 z = r.z; z < r.z + r.d; z++) {
        for (u32 x = r.x; x < r.x + r.w; x++) {
            if (!LevelGridSystem::isInBounds(grid, x, z)) continue;
            GridCell& cell = LevelGridSystem::getCell(grid, x, z);
            cell.flags           = CELL_FLOOR | CELL_CEILING;
            cell.floorHeight     = floorEnc;
            cell.ceilingHeight   = ceilEnc;
            cell.wallMaterialId  = wallMat;
            cell.floorMaterialId = floorMat;
            cell.ceilMaterialId  = ceilMat;
        }
    }
}

// Carve an L-shaped corridor between two room centre points
static void carveCorridor(LevelGrid& grid,
                           u32 x0, u32 z0, u32 x1, u32 z1,
                           f32 floorH, f32 ceilH)
{
    u8 floorEnc = static_cast<u8>(floorH / 0.25f);
    u8 ceilEnc  = static_cast<u8>(ceilH  / 0.25f);

    // Horizontal leg first (x0→x1 at z0), then vertical (z0→z1 at x1)
    u32 xMin = (x0 < x1) ? x0 : x1;
    u32 xMax = (x0 > x1) ? x0 : x1;
    for (u32 x = xMin; x <= xMax; x++) {
        // 2 cells wide corridor
        for (s32 dz = -1; dz <= 0; dz++) {
            s32 cz = (s32)z0 + dz;
            if (cz < 0) continue;
            if (!LevelGridSystem::isInBounds(grid, x, (u32)cz)) continue;
            GridCell& cell = LevelGridSystem::getCell(grid, x, (u32)cz);
            if (cell.flags & CELL_SOLID) {
                cell.flags           = CELL_FLOOR | CELL_CEILING;
                cell.floorHeight     = floorEnc;
                cell.ceilingHeight   = ceilEnc;
                cell.wallMaterialId  = 0;
                cell.floorMaterialId = 1;
                cell.ceilMaterialId  = 2;
            }
        }
    }

    u32 zMin = (z0 < z1) ? z0 : z1;
    u32 zMax = (z0 > z1) ? z0 : z1;
    for (u32 z = zMin; z <= zMax; z++) {
        for (s32 dx = -1; dx <= 0; dx++) {
            s32 cx = (s32)x1 + dx;
            if (cx < 0) continue;
            if (!LevelGridSystem::isInBounds(grid, (u32)cx, z)) continue;
            GridCell& cell = LevelGridSystem::getCell(grid, (u32)cx, z);
            if (cell.flags & CELL_SOLID) {
                cell.flags           = CELL_FLOOR | CELL_CEILING;
                cell.floorHeight     = floorEnc;
                cell.ceilingHeight   = ceilEnc;
                cell.wallMaterialId  = 0;
                cell.floorMaterialId = 1;
                cell.ceilMaterialId  = 2;
            }
        }
    }
}

Vec3 LevelGen::generateTestDungeon(LevelGrid& grid) {
    // Fill everything as solid
    for (u32 z = 0; z < grid.depth; z++) {
        for (u32 x = 0; x < grid.width; x++) {
            GridCell& c = LevelGridSystem::getCell(grid, x, z);
            c.flags           = CELL_SOLID;
            c.floorHeight     = 0;
            c.ceilingHeight   = 0;
            c.wallMaterialId  = 0;
            c.floorMaterialId = 0;
            c.ceilMaterialId  = 0;
        }
    }

    constexpr f32 FLOOR  = 0.0f;
    constexpr f32 CEIL   = 3.0f;   // 3m ceiling (12 quarter-units)

    // Carve rooms (room 4 has raised floor + brick walls)
    for (u32 i = 0; i < k_roomCount; i++) {
        f32 floorH = (i == 4) ? 0.5f : FLOOR;
        u8 wallMat  = (i == 4) ? 3 : 0; // brick_wall for room 4
        u8 floorMat = 1;
        u8 ceilMat  = 2;
        carveRoom(grid, k_rooms[i], floorH, CEIL, wallMat, floorMat, ceilMat);
    }

    // Connect rooms with L-corridors (centre-to-centre)
    auto centre = [](const Room& r, u32& ox, u32& oz) {
        ox = r.x + r.w / 2;
        oz = r.z + r.d / 2;
    };

    // Connect 0→1, 1→2, 0→3, 3→4, 1→4
    constexpr u32 kPairs[][2] = {{0,1},{1,2},{0,3},{3,4},{1,4}};
    for (auto& p : kPairs) {
        u32 ax, az, bx, bz;
        centre(k_rooms[p[0]], ax, az);
        centre(k_rooms[p[1]], bx, bz);
        carveCorridor(grid, ax, az, bx, bz, FLOOR, CEIL);
    }

    // Player spawns in the centre of room 0
    const Room& spawn = k_rooms[0];
    f32 sx = (spawn.x + spawn.w * 0.5f) * grid.cellSize;
    f32 sz = (spawn.z + spawn.d * 0.5f) * grid.cellSize;

    LOG_INFO("LevelGen: dungeon generated (%ux%u), spawn (%.1f, %.1f)",
             grid.width, grid.depth, sx, sz);

    return {sx, 0.0f, sz};
}
