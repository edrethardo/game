// test_pathfinder.cpp — clearance field + clearance-aware A* (Phase 1 nav rework).
//
// Covers the foundation that stops enemies wedging in corners: the per-cell
// clearance field, 8-connected A* with corner-cut prevention, and string-pulled
// waypoints. Grids are built from ASCII maps ('#' = wall, '.' = floor; row = z).

#include "doctest/doctest.h"
#include "world/level_grid.h"
#include "world/pathfinder.h"
#include <initializer_list>
#include <string>
#include <vector>
#include <cmath>

namespace {

// Build a LevelGrid from ASCII rows. row index = z, column index = x.
LevelGrid makeGrid(std::initializer_list<const char*> rows) {
    std::vector<std::string> r(rows.begin(), rows.end());
    u32 depth = static_cast<u32>(r.size());
    u32 width = static_cast<u32>(r[0].size());
    LevelGrid g;
    LevelGridSystem::init(g, width, depth, 1.0f);
    for (u32 z = 0; z < depth; z++)
        for (u32 x = 0; x < width; x++) {
            GridCell& c = LevelGridSystem::getCell(g, x, z);
            c.flags = (r[z][x] == '#') ? CELL_SOLID : CELL_FLOOR;
        }
    LevelGridSystem::buildClearanceField(g);
    return g;
}

Vec3 center(u32 x, u32 z) { return {x + 0.5f, 0.0f, z + 0.5f}; }

bool nearGoal(Vec3 p, Vec3 goal) {
    return std::fabs(p.x - goal.x) < 0.01f && std::fabs(p.z - goal.z) < 0.01f;
}

// True if every returned waypoint lies in a non-solid cell.
bool waypointsClear(const LevelGrid& g, const Vec3* wp, u8 n) {
    for (u8 i = 0; i < n; i++) {
        u32 gx, gz;
        if (!LevelGridSystem::worldToGrid(g, wp[i], gx, gz)) return false;
        if (LevelGridSystem::isSolid(g, gx, gz)) return false;
    }
    return true;
}

} // namespace

TEST_CASE("clearance field measures distance to nearest wall") {
    LevelGrid g = makeGrid({
        "#####",
        "#...#",
        "#...#",
        "#...#",
        "#####",
    });
    CHECK(LevelGridSystem::clearanceAt(g, 0, 0) == 0); // solid
    CHECK(LevelGridSystem::clearanceAt(g, 1, 1) == 1); // touches walls (diag+orth)
    CHECK(LevelGridSystem::clearanceAt(g, 2, 2) == 2); // open centre, 2 steps to wall
    LevelGridSystem::shutdown(g);
}

TEST_CASE("findPath: open straight line string-pulls to a single leg") {
    LevelGrid g = makeGrid({
        "#######",
        "#.....#",
        "#######",
    });
    Vec3 path[MAX_PATH_WAYPOINTS];
    u8 n = Pathfinder::findPath(g, center(1, 1), center(5, 1), path,
                                MAX_PATH_WAYPOINTS, 0.4f);
    REQUIRE(n >= 1);
    CHECK(n == 1);                          // no cell-center staircase
    CHECK(nearGoal(path[n - 1], center(5, 1)));
    LevelGridSystem::shutdown(g);
}

TEST_CASE("findPath: routes around a wall and keeps waypoints off solids") {
    LevelGrid g = makeGrid({
        "#######",
        "#.....#",
        "#.###.#",
        "#.....#",
        "#######",
    });
    Vec3 path[MAX_PATH_WAYPOINTS];
    u8 n = Pathfinder::findPath(g, center(1, 2), center(5, 2), path,
                                MAX_PATH_WAYPOINTS, 0.4f);
    REQUIRE(n >= 1);
    CHECK(nearGoal(path[n - 1], center(5, 2)));
    CHECK(waypointsClear(g, path, n));
    CHECK(n <= 4);                          // string-pulled, not one-per-cell
    LevelGridSystem::shutdown(g);
}

TEST_CASE("findPath: diagonal across an open room is a direct line") {
    LevelGrid g = makeGrid({
        "#####",
        "#...#",
        "#...#",
        "#...#",
        "#####",
    });
    Vec3 path[MAX_PATH_WAYPOINTS];
    u8 n = Pathfinder::findPath(g, center(1, 1), center(3, 3), path,
                                MAX_PATH_WAYPOINTS, 0.0f);
    REQUIRE(n >= 1);
    CHECK(nearGoal(path[n - 1], center(3, 3)));
    LevelGridSystem::shutdown(g);
}

TEST_CASE("findPath: diagonals never cut between two corner-touching walls") {
    // (1,1) and (2,2) are the only floor cells, touching only diagonally; the
    // two shared orthogonal cells are solid, so the squeeze must be rejected.
    LevelGrid blocked = makeGrid({
        "####",
        "#.##",
        "##.#",
        "####",
    });
    Vec3 path[MAX_PATH_WAYPOINTS];
    u8 n = Pathfinder::findPath(blocked, center(1, 1), center(2, 2), path,
                                MAX_PATH_WAYPOINTS, 0.0f);
    CHECK(n == 0);                          // corner-cut refused → no path
    LevelGridSystem::shutdown(blocked);

    // Open one orthogonal cell and the same trip is now reachable via cardinals.
    LevelGrid openMap = makeGrid({
        "####",
        "#..#",
        "##.#",
        "####",
    });
    u8 n2 = Pathfinder::findPath(openMap, center(1, 1), center(2, 2), path,
                                 MAX_PATH_WAYPOINTS, 0.0f);
    CHECK(n2 >= 1);
    CHECK(nearGoal(path[n2 - 1], center(2, 2)));
    LevelGridSystem::shutdown(openMap);
}
