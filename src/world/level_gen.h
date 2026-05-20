#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

// Room data exposed for enemy/item spawning after generation
struct DungeonRoom {
    u32 x, z, w, d;     // grid cell position and dimensions
    f32 floorHeight;     // floor height in meters
    u8  wallMat;         // wall material ID

    // Rooms connected via corridors (indices into DungeonResult::rooms).
    // Populated by LevelGen::generate after all rooms and corridors are carved.
    u16 adjacentRooms[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    u8  adjacentCount = 0;
};

static constexpr u32 MAX_DUNGEON_ROOMS = 32;

struct DungeonResult {
    Vec3 spawnPos;                         // player spawn (world coords)
    DungeonRoom rooms[MAX_DUNGEON_ROOMS];  // generated rooms
    u32 roomCount;                         // number of rooms created
    u32 spawnRoomIdx;                      // dead-end room chosen for player spawn
    u32 exitRoomIdx;                       // farthest room from spawn for exit portal
};

namespace LevelGen {
    // Legacy test dungeon (kept for fallback)
    Vec3 generateTestDungeon(LevelGrid& grid);

    // Procedural BSP dungeon generation. Always produces a valid result —
    // spawn is the room with fewest corridor exits, exit is the BFS-farthest room.
    DungeonResult generate(LevelGrid& grid, u32 seed, u32 gridWidth = 48, u32 gridDepth = 48);
}
