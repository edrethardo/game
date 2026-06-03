#pragma once
// pathfinder.h — Grid A* pathfinder for enemy tactical navigation.
// Converts world positions to grid coords, searches for a walkable path,
// and returns a compressed list of world-space waypoints.
// All working memory is stack/static — no heap allocation.

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

static constexpr u8  MAX_PATH_WAYPOINTS = 16;
static constexpr u16 MAX_ASTAR_SEARCH   = 256;

namespace Pathfinder {
    // Find path from start to goal. Returns number of waypoints written (0 = no path).
    // outPath receives world-space positions; outPath[n-1] is always the exact goal.
    //
    // bodyRadius (metres) makes the search clearance-aware: the path keeps that
    // much slack from walls where space allows, diagonals never cut wall corners,
    // and the raw cell path is string-pulled into long straight runs the AABB can
    // actually traverse — so a wide entity stops clipping inside corners. Pass 0
    // for a point-sized agent (legacy behaviour, any non-solid cell is walkable).
    //
    // Searches up to maxSearch cells. All working memory is stack-only.
    u8 findPath(const LevelGrid& grid, Vec3 start, Vec3 goal,
                Vec3* outPath, u8 maxWaypoints = MAX_PATH_WAYPOINTS,
                f32 bodyRadius = 0.0f,
                u16 maxSearch = MAX_ASTAR_SEARCH);
}
