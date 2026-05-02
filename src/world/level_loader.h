#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

namespace LevelLoader {
    static constexpr u32 MAX_ENEMY_SPAWNS = 64;

    struct EnemySpawn {
        char type[32] = {};
        f32  x = 0, z = 0;
    };

    // Load a level from JSON. Fills grid, returns player spawn position.
    // Also populates enemy spawn list.
    // Returns {0,0,0} on failure (grid left unmodified).
    Vec3 loadFromJson(const char* path, LevelGrid& grid,
                      EnemySpawn* outSpawns, u32& outSpawnCount, u32 maxSpawns);
}
