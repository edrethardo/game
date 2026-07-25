// test_autoplay_route.cpp — the general wall-aware goal flow field (game/autoplay_route.h).
//
// This field is the conceptual fix for "the bot walks into a wall trying to reach the boss": it
// replaced a straight bearing (and then a capped A* leg), both of which pointed into any wall between
// the bot and the goal. The properties pinned here are the ones that make a field the right answer —
// it always points along a route that exists (never into a wall), it is defined on every reachable
// cell, it rebuilds when the goal moves, and following it terminates AT the goal.
//
// Built on synthetic LevelGrids like the other nav tests (cells indexed z*width+x).
#include <doctest/doctest.h>
#include "game/autoplay_route.h"
#include "world/level_grid.h"

namespace {
LevelGrid makeFlatGrid(u32 w, u32 d) {
    LevelGrid g;
    LevelGridSystem::init(g, w, d, 1.0f);
    for (u32 z = 0; z < d; z++)
        for (u32 x = 0; x < w; x++) {
            GridCell& c = g.cells[z * w + x];
            c.flags = CELL_FLOOR; c.floorHeight = 0; c.ceilingHeight = 12;
        }
    return g;
}
void setSolid(LevelGrid& g, u32 x, u32 z) { g.cells[z * g.width + x].flags = CELL_SOLID; }
Vec3 cell(u32 x, u32 z) { return {(x + 0.5f), 0.0f, (z + 0.5f)}; }   // cellSize 1.0

// Follow the field from `start` to the goal; report arrival AND that no step ever lands in a wall.
bool fieldReachesGoal(const Autoplay::RouteField& f, const LevelGrid& g, Vec3 start, u32 maxSteps) {
    Vec3 p = start;
    for (u32 i = 0; i < maxSteps; i++) {
        u32 gx, gz;
        if (!LevelGridSystem::worldToGrid(g, p, gx, gz) || LevelGridSystem::isSolid(g, gx, gz))
            return false;                                  // a step landed in a wall — the bug this prevents
        if (f.dir[gz * f.width + gx] == 0xFE) return true; // arrived at the goal cell
        const Vec3 dir = Autoplay::routeDirection(f, g, p);
        if (lengthSq(dir) < 1e-6f) return false;
        p = p + dir * (g.cellSize * 0.5f);                 // half-cell steps: exercise the readout more
    }
    return false;
}
} // namespace

TEST_CASE("route field: routes AROUND a wall to the goal, never through it") {
    // A full-height barrier splits the grid with a gap along the top; the goal is straight through the
    // wall from the start. A bearing aims into the wall forever — the field must walk the gap.
    LevelGrid g = makeFlatGrid(16, 16);
    for (u32 z = 0; z < 13; z++) setSolid(g, 8, z);        // wall x=8, gap at z=13..15

    Autoplay::RouteField f;
    REQUIRE(Autoplay::ensureRouteField(f, g, cell(13, 8), 1u));   // goal on the far side
    CHECK(f.valid);
    CHECK(fieldReachesGoal(f, g, cell(2, 8), 200));               // start on the near side, same row (through the wall)
    Autoplay::freeRouteField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("route field: defined on every reachable cell; unreachable pockets read 0xFF") {
    LevelGrid g = makeFlatGrid(10, 10);
    // Seal a 1-cell pocket at (0,0) off from the rest.
    setSolid(g, 1, 0); setSolid(g, 0, 1); setSolid(g, 1, 1);
    Autoplay::RouteField f;
    REQUIRE(Autoplay::ensureRouteField(f, g, cell(8, 8), 2u));
    // The open cell (5,5) routes; the sealed pocket (0,0) is unreachable (0xFF => zero heading).
    CHECK(lengthSq(Autoplay::routeDirection(f, g, cell(5, 5))) > 1e-6f);
    CHECK(lengthSq(Autoplay::routeDirection(f, g, cell(0, 0))) == doctest::Approx(0.0f));
    Autoplay::freeRouteField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("route field: rebuilds when the goal moves to a new cell, no-op when it stays") {
    LevelGrid g = makeFlatGrid(10, 10);
    Autoplay::RouteField f;
    REQUIRE(Autoplay::ensureRouteField(f, g, cell(8, 8), 3u));
    const s32 gx0 = f.goalX, gz0 = f.goalZ;
    // Same cell (goal drifted within it): no rebuild — goal cell unchanged.
    CHECK(Autoplay::ensureRouteField(f, g, Vec3{8.2f, 0.0f, 8.7f}, 3u));
    CHECK(f.goalX == gx0); CHECK(f.goalZ == gz0);
    // Moved a cell: the seed follows.
    CHECK(Autoplay::ensureRouteField(f, g, cell(2, 2), 3u));
    CHECK(f.goalX == 2); CHECK(f.goalZ == 2);
    // The goal cell itself reads 0xFE ("at goal").
    u32 gx, gz; LevelGridSystem::worldToGrid(g, cell(2, 2), gx, gz);
    CHECK(f.dir[gz * f.width + gx] == 0xFE);
    Autoplay::freeRouteField(f);
    LevelGridSystem::shutdown(g);
}
