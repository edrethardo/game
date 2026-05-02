#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

namespace LevelGen {
    // Fill grid with a procedural test dungeon:
    //   - Solid everywhere by default
    //   - 3-5 rectangular rooms carved out
    //   - Rooms connected with L-shaped corridors
    //   - One room has raised floor (0.5m) to test height variation
    //   - Sets floor/ceiling flags and heights on empty cells
    // Returns the player spawn position (feet, centre of first room).
    Vec3 generateTestDungeon(LevelGrid& grid);
}
