// Procedural dungeon generation via recursive BSP partitioning.
// LevelGen::generate(grid, seed, w, d) initializes the grid, splits the area
// into rooms, carves corridors between sibling partitions, writes cell flags +
// floor/ceiling heights + material IDs into the grid, and returns the player
// spawn (room 0 centre) plus the room list (used by Engine::startGame to place
// enemies). RNG is a local LCG (GenRNG) seeded by the caller — independent of
// the item-roll RNG so loot is not coupled to map layout.

#include "world/level_gen.h"
#include "core/log.h"
#include <cstring>

// --- BSP dungeon generator ---

// Simple LCG RNG for generation (independent from item RNG)
struct GenRNG {
    u32 state;
    u32 next() { state = state * 1664525u + 1013904223u; return state; }
    u32 range(u32 min, u32 max) {
        if (min >= max) return min;
        return min + (next() >> 8) % (max - min);
    }
    f32 f01() { return static_cast<f32>(next() >> 8) / static_cast<f32>(1u << 24); }
};

struct BSPNode {
    u32 x, z, w, d;          // partition bounds in grid cells
    s32 left  = -1;          // child indices (-1 = leaf)
    s32 right = -1;
    s32 roomIndex = -1;      // index into DungeonResult::rooms (-1 = no room)
};

static constexpr u32 MAX_BSP_NODES = 128;
static constexpr u32 MIN_PARTITION_SIZE = 7;  // minimum partition dimension
static constexpr u32 MIN_ROOM_SIZE = 4;       // minimum room dimension
static constexpr u32 ROOM_MARGIN = 1;         // margin between room and partition edge

// Carve a rectangular room into the grid
static void carveArea(LevelGrid& grid, u32 rx, u32 rz, u32 rw, u32 rd,
                      f32 floorH, f32 ceilH,
                      u8 wallMat, u8 floorMat, u8 ceilMat)
{
    u8 floorEnc = static_cast<u8>(floorH / 0.25f);
    u8 ceilEnc  = static_cast<u8>(ceilH  / 0.25f);

    for (u32 z = rz; z < rz + rd; z++) {
        for (u32 x = rx; x < rx + rw; x++) {
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

// Carve a 2-wide L-shaped corridor between two points
static void carveLCorridor(LevelGrid& grid, u32 x0, u32 z0, u32 x1, u32 z1,
                           f32 floorH, f32 ceilH)
{
    u8 floorEnc = static_cast<u8>(floorH / 0.25f);
    u8 ceilEnc  = static_cast<u8>(ceilH  / 0.25f);

    auto carveCell = [&](u32 x, u32 z) {
        if (!LevelGridSystem::isInBounds(grid, x, z)) return;
        GridCell& cell = LevelGridSystem::getCell(grid, x, z);
        if (cell.flags & CELL_SOLID) {
            cell.flags           = CELL_FLOOR | CELL_CEILING;
            cell.floorHeight     = floorEnc;
            cell.ceilingHeight   = ceilEnc;
            cell.wallMaterialId  = 0;
            cell.floorMaterialId = 1;
            cell.ceilMaterialId  = 2;
        }
    };

    // Horizontal leg — 2 cells wide in Z
    u32 xMin = (x0 < x1) ? x0 : x1;
    u32 xMax = (x0 > x1) ? x0 : x1;
    for (u32 x = xMin; x <= xMax; x++) {
        for (s32 dz = -1; dz <= 0; dz++) {
            s32 cz = static_cast<s32>(z0) + dz;
            if (cz < 0) continue;
            carveCell(x, static_cast<u32>(cz));
        }
    }

    // Vertical leg — 2 cells wide in X
    u32 zMin = (z0 < z1) ? z0 : z1;
    u32 zMax = (z0 > z1) ? z0 : z1;
    for (u32 z = zMin; z <= zMax; z++) {
        for (s32 dx = -1; dx <= 0; dx++) {
            s32 cx = static_cast<s32>(x1) + dx;
            if (cx < 0) continue;
            carveCell(static_cast<u32>(cx), z);
        }
    }
}

// Recursively split BSP
static void splitBSP(BSPNode* nodes, u32& nodeCount, u32 nodeIdx, GenRNG& rng, u32 depth) {
    BSPNode& node = nodes[nodeIdx];

    // Stop if too small or too deep
    if (depth > 5 || node.w < MIN_PARTITION_SIZE * 2 || node.d < MIN_PARTITION_SIZE * 2) {
        // Try splitting on the larger axis only
        if (node.w >= MIN_PARTITION_SIZE * 2 && depth <= 5) {
            // Can still split horizontally
        } else if (node.d >= MIN_PARTITION_SIZE * 2 && depth <= 5) {
            // Can still split vertically
        } else {
            return; // Leaf node
        }
    }

    if (nodeCount + 2 >= MAX_BSP_NODES) return;

    // Decide split direction: prefer splitting the longer axis
    bool splitHorizontal;
    if (node.w >= MIN_PARTITION_SIZE * 2 && node.d >= MIN_PARTITION_SIZE * 2) {
        // Both axes large enough, prefer longer axis with some randomness
        if (node.w > node.d + 2) splitHorizontal = true;
        else if (node.d > node.w + 2) splitHorizontal = false;
        else splitHorizontal = (rng.next() & 1) == 0;
    } else if (node.w >= MIN_PARTITION_SIZE * 2) {
        splitHorizontal = true;
    } else if (node.d >= MIN_PARTITION_SIZE * 2) {
        splitHorizontal = false;
    } else {
        return; // Can't split
    }

    u32 leftIdx  = nodeCount++;
    u32 rightIdx = nodeCount++;
    node.left  = static_cast<s32>(leftIdx);
    node.right = static_cast<s32>(rightIdx);

    if (splitHorizontal) {
        // Split along X axis
        u32 splitX = rng.range(node.x + MIN_PARTITION_SIZE, node.x + node.w - MIN_PARTITION_SIZE);
        nodes[leftIdx]  = {node.x, node.z, splitX - node.x, node.d, -1, -1, -1};
        nodes[rightIdx] = {splitX, node.z, node.x + node.w - splitX, node.d, -1, -1, -1};
    } else {
        // Split along Z axis
        u32 splitZ = rng.range(node.z + MIN_PARTITION_SIZE, node.z + node.d - MIN_PARTITION_SIZE);
        nodes[leftIdx]  = {node.x, node.z, node.w, splitZ - node.z, -1, -1, -1};
        nodes[rightIdx] = {node.x, splitZ, node.w, node.z + node.d - splitZ, -1, -1, -1};
    }

    splitBSP(nodes, nodeCount, leftIdx, rng, depth + 1);
    splitBSP(nodes, nodeCount, rightIdx, rng, depth + 1);
}

// Find a room center in a subtree (for corridor connections)
static bool findRoomCenter(const BSPNode* nodes, s32 nodeIdx, const DungeonRoom* rooms,
                           u32& outCX, u32& outCZ) {
    if (nodeIdx < 0) return false;
    const BSPNode& n = nodes[nodeIdx];
    if (n.roomIndex >= 0) {
        const DungeonRoom& r = rooms[n.roomIndex];
        outCX = r.x + r.w / 2;
        outCZ = r.z + r.d / 2;
        return true;
    }
    // Try left subtree first, then right
    if (findRoomCenter(nodes, n.left, rooms, outCX, outCZ)) return true;
    return findRoomCenter(nodes, n.right, rooms, outCX, outCZ);
}

DungeonResult LevelGen::generate(LevelGrid& grid, u32 seed, u32 gridWidth, u32 gridDepth) {
    DungeonResult result = {};
    GenRNG rng = {seed};

    // Fill everything solid
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

    constexpr f32 CEIL = 3.0f;

    // Build BSP tree
    BSPNode nodes[MAX_BSP_NODES] = {};
    u32 nodeCount = 1;
    // Root node covers the usable area (1-cell border around edges)
    nodes[0] = {1, 1, grid.width - 2, grid.depth - 2, -1, -1, -1};

    splitBSP(nodes, nodeCount, 0, rng, 0);

    // Place rooms in leaf nodes
    for (u32 i = 0; i < nodeCount; i++) {
        BSPNode& node = nodes[i];
        if (node.left >= 0 || node.right >= 0) continue; // Not a leaf
        if (result.roomCount >= MAX_DUNGEON_ROOMS) break;

        // Random room size within partition (with margin)
        u32 maxW = node.w - ROOM_MARGIN * 2;
        u32 maxD = node.d - ROOM_MARGIN * 2;
        if (maxW < MIN_ROOM_SIZE || maxD < MIN_ROOM_SIZE) continue;

        u32 rw = rng.range(MIN_ROOM_SIZE, maxW + 1);
        u32 rd = rng.range(MIN_ROOM_SIZE, maxD + 1);
        u32 rx = rng.range(node.x + ROOM_MARGIN, node.x + node.w - ROOM_MARGIN - rw + 1);
        u32 rz = rng.range(node.z + ROOM_MARGIN, node.z + node.d - ROOM_MARGIN - rd + 1);

        // ~20% chance of raised floor, ~30% chance of brick walls
        f32 floorH = (rng.f01() < 0.2f) ? 0.5f : 0.0f;
        u8 wallMat = (rng.f01() < 0.3f) ? 3 : 0;

        carveArea(grid, rx, rz, rw, rd, floorH, CEIL, wallMat, 1, 2);

        DungeonRoom& room = result.rooms[result.roomCount];
        room.x = rx;
        room.z = rz;
        room.w = rw;
        room.d = rd;
        room.floorHeight = floorH;
        room.wallMat = wallMat;

        node.roomIndex = static_cast<s32>(result.roomCount);
        result.roomCount++;
    }

    // Connect sibling rooms with corridors
    for (u32 i = 0; i < nodeCount; i++) {
        BSPNode& node = nodes[i];
        if (node.left < 0 || node.right < 0) continue;

        u32 lx, lz, rx, rz;
        if (findRoomCenter(nodes, node.left, result.rooms, lx, lz) &&
            findRoomCenter(nodes, node.right, result.rooms, rx, rz)) {
            carveLCorridor(grid, lx, lz, rx, rz, 0.0f, CEIL);
        }
    }

    // Compute room adjacency — two rooms are adjacent when their bounding boxes,
    // expanded by 3 cells to account for corridor width, overlap in both axes.
    // This drives the wave-propagation alert system in squad.cpp.
    for (u32 i = 0; i < result.roomCount; i++) {
        DungeonRoom& ri = result.rooms[i];
        for (u32 j = i + 1; j < result.roomCount; j++) {
            DungeonRoom& rj = result.rooms[j];
            bool xOverlap = (ri.x < rj.x + rj.w + 3) && (rj.x < ri.x + ri.w + 3);
            bool zOverlap = (ri.z < rj.z + rj.d + 3) && (rj.z < ri.z + ri.d + 3);
            if (xOverlap && zOverlap) {
                if (ri.adjacentCount < 4) ri.adjacentRooms[ri.adjacentCount++] = static_cast<u16>(j);
                if (rj.adjacentCount < 4) rj.adjacentRooms[rj.adjacentCount++] = static_cast<u16>(i);
            }
        }
    }

    // Player spawns in center of first room
    if (result.roomCount > 0) {
        const DungeonRoom& r = result.rooms[0];
        result.spawnPos.x = (r.x + r.w * 0.5f) * grid.cellSize;
        result.spawnPos.y = r.floorHeight;
        result.spawnPos.z = (r.z + r.d * 0.5f) * grid.cellSize;
    }

    LOG_INFO("LevelGen: BSP dungeon generated (%ux%u), %u rooms, spawn (%.1f, %.1f, %.1f)",
             grid.width, grid.depth, result.roomCount,
             result.spawnPos.x, result.spawnPos.y, result.spawnPos.z);

    return result;
}

// --- Legacy test dungeon (kept for fallback) ---

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
// 2-wide corridor variant used by BSP connector
static void carveCorridor(LevelGrid& grid,
                           u32 x0, u32 z0, u32 x1, u32 z1,
                           f32 floorH, f32 ceilH)
{
    u8 floorEnc = static_cast<u8>(floorH / 0.25f);
    u8 ceilEnc  = static_cast<u8>(ceilH  / 0.25f);

    auto carveCell = [&](u32 x, u32 z) {
        if (!LevelGridSystem::isInBounds(grid, x, z)) return;
        GridCell& cell = LevelGridSystem::getCell(grid, x, z);
        if (cell.flags & CELL_SOLID) {
            cell.flags           = CELL_FLOOR | CELL_CEILING;
            cell.floorHeight     = floorEnc;
            cell.ceilingHeight   = ceilEnc;
            cell.wallMaterialId  = 0;
            cell.floorMaterialId = 1;
            cell.ceilMaterialId  = 2;
        }
    };

    // Horizontal leg — 2 cells wide in Z
    u32 xMin = (x0 < x1) ? x0 : x1;
    u32 xMax = (x0 > x1) ? x0 : x1;
    for (u32 x = xMin; x <= xMax; x++) {
        for (s32 dz = -1; dz <= 0; dz++) {
            s32 cz = (s32)z0 + dz;
            if (cz < 0) continue;
            carveCell(x, (u32)cz);
        }
    }

    // Vertical leg — 2 cells wide in X
    u32 zMin = (z0 < z1) ? z0 : z1;
    u32 zMax = (z0 > z1) ? z0 : z1;
    for (u32 z = zMin; z <= zMax; z++) {
        for (s32 dx = -1; dx <= 0; dx++) {
            s32 cx = (s32)x1 + dx;
            if (cx < 0) continue;
            carveCell((u32)cx, z);
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
