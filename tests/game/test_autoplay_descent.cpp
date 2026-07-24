// test_autoplay_descent.cpp — the FOUR_STORY "Descent" travel field (game/autoplay_descent.h).
//
// The field replaced a straight-line bearing and then an A* leg, both of which failed live for the
// same reason: on a braided maze they handed back headings that pointed into walls, and the bot
// scraped along them ("the time goes right now looking and hugging the walls and corners"). The
// properties pinned here are the ones that make a field the right answer — it always points along a
// route that exists, it is defined everywhere reachable so there is no "no plan" tick, and it never
// routes over a jump pad, which on this floor is the one piece of terrain that undoes a descent.
//
// Built on synthetic LevelGrids like the other nav tests (LevelGridSystem::init/shutdown, cells
// indexed z*width+x).

#include <doctest/doctest.h>
#include "game/autoplay_descent.h"
#include "game/autoplay_nav.h"   // Autoplay::onJumpPad
#include "world/level_grid.h"

namespace {
// A one-story test maze: open floor with the L3 slab over it, so a body at y=9 is on a real storey.
LevelGrid makeStoryGrid(u32 w, u32 d) {
    LevelGrid g;
    LevelGridSystem::init(g, w, d, 1.0f);
    for (u32 z = 0; z < d; z++)
        for (u32 x = 0; x < w; x++) {
            GridCell& c = g.cells[z * w + x];
            c.flags = CELL_FLOOR;
            c.floorHeight = 0; c.ceilingHeight = 48;
            LevelGridSystem::addPlatform(c, 36, 0);   // L3 @ 9 m
        }
    return g;
}
void setSolid(LevelGrid& g, u32 x, u32 z) { g.cells[z * g.width + x].flags = CELL_SOLID; }
void setPad  (LevelGrid& g, u32 x, u32 z) { g.cells[z * g.width + x].flags |= CELL_JUMPPAD; }
DropHole holeAt(f32 x, f32 z, f32 y) { DropHole h; h.pos = {x, y, z}; h.surfaceY = y; return h; }

// Follow the field from `start`, one cell per step, and report whether it arrives at a hole.
// This is the property that matters: not the direction of any single step, but that the chain of
// them terminates at a way down.
bool fieldReachesAHole(const Autoplay::DescentField& f, const LevelGrid& g, Vec3 start, u32 maxSteps) {
    Vec3 p = start;
    for (u32 i = 0; i < maxSteps; i++) {
        if (Autoplay::atDescentGoal(f, g, p)) return true;
        const Vec3 dir = Autoplay::descentDirection(f, g, p);
        if (lengthSq(dir) < 1e-6f) return false;
        p = p + dir * g.cellSize;      // one cell along the field
        p.y = start.y;                  // stay on the storey (the field is 2D)
    }
    return false;
}
} // namespace

TEST_CASE("descent field: routes around a wall instead of pointing through it") {
    // A full-height barrier splits the grid; the only hole is on the far side, straight through the
    // wall from the bot. A bearing would aim into the wall forever — the field must walk the gap.
    LevelGrid g = makeStoryGrid(16, 16);
    for (u32 z = 0; z < 12; z++) setSolid(g, 8, z);      // wall with a gap along the +z edge
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(12.5f, 3.5f, 9.0f);

    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));
    const Vec3 start{3.5f, 9.0f, 3.5f};
    // The straight-line bearing at the hole is roughly +X — the wall. The field must not send us there.
    const Vec3 dir = Autoplay::descentDirection(f, g, start);
    CHECK(lengthSq(dir) > 1e-6f);                        // it always has an answer
    CHECK(fieldReachesAHole(f, g, start, 200));          // and the answer gets there
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: every reachable cell has a heading (no stand-and-stare)") {
    LevelGrid g = makeStoryGrid(12, 12);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(1.5f, 1.5f, 9.0f);
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));
    for (u32 z = 0; z < 12; z++)
        for (u32 x = 0; x < 12; x++) {
            const Vec3 p{x + 0.5f, 9.0f, z + 0.5f};
            const bool ok = Autoplay::atDescentGoal(f, g, p) ||
                            lengthSq(Autoplay::descentDirection(f, g, p)) > 1e-6f;
            CHECK(ok);
        }
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: never routes over a jump pad") {
    // Pads throw the bot ~two storeys back up, so a route across one throws away the descent. The
    // field leaves them out entirely rather than steering onto one and being vetoed off it per tick.
    LevelGrid g = makeStoryGrid(16, 16);
    for (u32 z = 6; z < 10; z++) for (u32 x = 6; x < 10; x++) setPad(g, x, z);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(13.5f, 13.5f, 9.0f);
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));
    // Walk the field from the far corner and assert it never sets foot in the pad block.
    Vec3 p{2.5f, 9.0f, 2.5f};
    for (u32 i = 0; i < 200 && !Autoplay::atDescentGoal(f, g, p); i++) {
        const Vec3 dir = Autoplay::descentDirection(f, g, p);
        REQUIRE(lengthSq(dir) > 1e-6f);
        p = p + dir * g.cellSize; p.y = 9.0f;
        CHECK_FALSE(Autoplay::onJumpPad(g, p));
    }
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: a storey with no holes reports invalid (L0 keeps the exit flow field)") {
    LevelGrid g = makeStoryGrid(10, 10);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(5.5f, 5.5f, 9.0f);   // upstairs only
    Autoplay::DescentField f;
    CHECK_FALSE(Autoplay::ensureDescentField(f, g, d, 0.0f, 1)); // asking about the ground storey
    CHECK(lengthSq(Autoplay::descentDirection(f, g, Vec3{3.5f, 0.0f, 3.5f})) < 1e-6f);
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: a padded hole is used only when it is the ONLY way down") {
    // Hole density thins to 7% on the deepest storey, so "every hole here is a return lift" is a
    // real state. A bounce still beats having no descent plan at all.
    LevelGrid g = makeStoryGrid(12, 12);
    setPad(g, 5, 5);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(5.5f, 5.5f, 9.0f);   // padded, and the only one
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d, 9.0f, 1));
    CHECK(fieldReachesAHole(f, g, Vec3{2.5f, 9.0f, 2.5f}, 100));
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent field: a new floor invalidates a stale field") {
    // The stamp is the floor identity. Without it a Descent floor would inherit the previous one's
    // routes — the grid is the same size and the storey height is identical, so nothing else differs.
    LevelGrid g = makeStoryGrid(10, 10);
    DungeonResult d1{}; d1.dropHoles[d1.dropHoleCount++] = holeAt(1.5f, 1.5f, 9.0f);
    DungeonResult d2{}; d2.dropHoles[d2.dropHoleCount++] = holeAt(8.5f, 8.5f, 9.0f);
    Autoplay::DescentField f;
    REQUIRE(Autoplay::ensureDescentField(f, g, d1, 9.0f, /*floor=*/1));
    const Vec3 mid{5.5f, 9.0f, 5.5f};
    const Vec3 toFirst = Autoplay::descentDirection(f, g, mid);
    REQUIRE(Autoplay::ensureDescentField(f, g, d2, 9.0f, /*floor=*/2));
    const Vec3 toSecond = Autoplay::descentDirection(f, g, mid);
    CHECK(dot(toFirst, toSecond) < 0.9f);    // the routes genuinely differ (opposite corners)
    Autoplay::freeDescentField(f);
    LevelGridSystem::shutdown(g);
}
