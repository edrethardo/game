// test_story_nav.cpp — the pure cross-story navigation helpers the enemy chase uses on VERTICAL_HALL
// floors. onUpperStory answers "which story are these feet on?"; nearestPortalGoal answers "which
// ramp end do I walk to in order to change story?". Both are pure (grid/dungeon in, XZ/bool out).

#include <doctest/doctest.h>
#include "world/story_nav.h"
#include "world/level_grid.h"

TEST_CASE("nearestPortalGoal routes to the near ramp end on my own story") {
    DungeonResult d = {};
    d.portalCount = 2;
    d.portals[0].lowPos  = {2.5f, 0.0f, 2.5f};   // near ramp
    d.portals[0].highPos = {2.5f, 2.5f, 4.5f};
    d.portals[1].lowPos  = {30.5f, 0.0f, 2.5f};  // far ramp
    d.portals[1].highPos = {30.5f, 2.5f, 4.5f};

    SUBCASE("ground → up: walk to the nearest ramp FOOT (lowPos)") {
        Vec3 g = StoryNav::nearestPortalGoal(d, {8.0f, 0.0f, 8.0f}, /*fromUpper=*/false, /*toUpper=*/true);
        CHECK(g.x == doctest::Approx(2.5f));   // the near ramp, not the far one
        CHECK(g.z == doctest::Approx(2.5f));
    }
    SUBCASE("balcony → down: walk to the nearest ramp TOP (highPos)") {
        Vec3 h = StoryNav::nearestPortalGoal(d, {8.0f, 2.5f, 8.0f}, /*fromUpper=*/true, /*toUpper=*/false);
        CHECK(h.x == doctest::Approx(2.5f));
        CHECK(h.z == doctest::Approx(4.5f));
    }
    SUBCASE("same story: no redirect (returns the origin)") {
        Vec3 s = StoryNav::nearestPortalGoal(d, {8.0f, 0.0f, 8.0f}, false, false);
        CHECK(s.x == doctest::Approx(8.0f));
        CHECK(s.z == doctest::Approx(8.0f));
    }
    SUBCASE("no portals: no redirect") {
        DungeonResult empty = {};
        Vec3 s = StoryNav::nearestPortalGoal(empty, {8.0f, 0.0f, 8.0f}, false, true);
        CHECK(s.x == doctest::Approx(8.0f));
    }
}

TEST_CASE("onUpperStory reads the slab-top vs ground from the body's feet") {
    LevelGrid g;
    LevelGridSystem::init(g, 6, 6, 1.0f);
    for (u32 i = 0; i < 36; i++) { g.cells[i].flags = CELL_FLOOR; g.cells[i].ceilingHeight = 20; }
    GridCell& c = LevelGridSystem::getCell(g, 3, 3);
    c.flags = static_cast<u8>(CELL_FLOOR);
    LevelGridSystem::setPlatform(c, 10, 0);              // slab top 2.5 m
    Vec3 at = {3.5f, 0.0f, 3.5f};                        // over the slab cell

    CHECK(StoryNav::onUpperStory(g, at, /*feetY=*/2.5f) == true);   // feet at slab top → upper
    CHECK(StoryNav::onUpperStory(g, at, /*feetY=*/0.0f) == false);  // feet on the ground → lower
    CHECK(StoryNav::onUpperStory(g, {1.5f, 0.0f, 1.5f}, 0.0f) == false); // non-slab cell → lower
    LevelGridSystem::shutdown(g);
}

TEST_CASE("nearestPadGoal: walkers route to the closest jump pad, and it's inert without pads") {
    // A four-story Descent floor has NO ramps (portalCount == 0), so a jump pad is the only way up.
    // Without pad routing an enemy simply loses anyone who drops a level.
    DungeonResult d{};
    const Vec3 from{10.0f, 0.0f, 10.0f};

    // No pads recorded (every style that has none) → unchanged goal, so the caller falls through to
    // its normal chase behaviour rather than walking to the origin.
    CHECK(StoryNav::nearestPadGoal(d, from).x == doctest::Approx(from.x));
    CHECK(StoryNav::nearestPadGoal(d, from).z == doctest::Approx(from.z));

    d.jumpPadCount = 3;
    d.jumpPads[0] = {40.0f, 0.0f, 40.0f};   // far
    d.jumpPads[1] = {13.0f, 3.0f, 14.0f};   // nearest in XZ (5 m away)
    d.jumpPads[2] = {10.0f, 6.0f, 25.0f};   // 15 m away
    const Vec3 goal = StoryNav::nearestPadGoal(d, from);
    CHECK(goal.x == doctest::Approx(13.0f));
    CHECK(goal.z == doctest::Approx(14.0f));
    // Y is left as the seeker's own: the pad supplies the vertical move, the AI only has to arrive.
    CHECK(goal.y == doctest::Approx(from.y));
}

TEST_CASE("targetIsAbove distinguishes a real storey from a step") {
    // Stories are 3 m apart; stairs, ledges and raised room floors are well under a metre. The gate
    // has to catch the former without firing on the latter, or enemies would abandon a chase to go
    // hunt a pad every time you stood on a crate.
    CHECK(StoryNav::targetIsAbove(0.0f, 3.0f));     // one storey up
    CHECK(StoryNav::targetIsAbove(3.0f, 9.0f));     // two up
    CHECK_FALSE(StoryNav::targetIsAbove(0.0f, 0.5f));   // a step
    CHECK_FALSE(StoryNav::targetIsAbove(0.0f, 0.0f));   // level
    CHECK_FALSE(StoryNav::targetIsAbove(3.0f, 0.0f));   // target BELOW — pads only solve "up"
}
