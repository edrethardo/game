#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

// Room data exposed for enemy/item spawning after generation
struct DungeonRoom {
    u32 x, z, w, d;     // grid cell position and dimensions
    f32 floorHeight;     // floor height in meters
    u8  wallMat;         // wall material ID
};

static constexpr u32 MAX_DUNGEON_ROOMS = 32;

struct DungeonResult {
    Vec3 spawnPos;                         // player spawn (world coords)
    DungeonRoom rooms[MAX_DUNGEON_ROOMS];  // generated rooms
    u32 roomCount;                         // number of rooms created
};

namespace LevelGen {
    // Legacy test dungeon (kept for fallback)
    Vec3 generateTestDungeon(LevelGrid& grid);

    // Procedural BSP dungeon generation
    // seed: RNG seed for deterministic generation
    // gridWidth/gridDepth: grid dimensions to use (will init the grid)
    DungeonResult generate(LevelGrid& grid, u32 seed, u32 gridWidth = 48, u32 gridDepth = 48);
}
