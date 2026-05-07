#pragma once
// pathfinder.h — Grid A* pathfinder for enemy tactical navigation.
// Converts world positions to grid coords, searches for a walkable path,
// and returns a compressed list of world-space waypoints.
// All working memory is stack/static — no heap allocation.

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

static constexpr u8  MAX_PATH_WAYPOINTS = 6;
static constexpr u16 MAX_ASTAR_SEARCH   = 256;

namespace Pathfinder {
    // Find path from start to goal. Returns number of waypoints written (0 = no path).
    // outPath receives world-space positions. Searches up to maxSearch cells.
    u8 findPath(const LevelGrid& grid, Vec3 start, Vec3 goal,
                Vec3* outPath, u8 maxWaypoints = MAX_PATH_WAYPOINTS,
                u16 maxSearch = MAX_ASTAR_SEARCH);
}
