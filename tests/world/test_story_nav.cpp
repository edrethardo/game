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

TEST_CASE("planVault: a chasing mob vaults a jumpable gap and refuses a lake") {
    // A 12x12 stacked room whose interior all carries the L1 slab @ 3 m, with a 1-cell gap punched
    // at x=6 and a 3-cell lake at x∈[8..10] on row z=3 — the two shapes a Descent chase meets.
    LevelGrid g;
    LevelGridSystem::init(g, 12, 12, 1.0f);
    for (u32 z = 0; z < 12; z++)
        for (u32 x = 0; x < 12; x++) {
            GridCell& c = g.cells[z * 12 + x];
            const bool border = (x == 0 || z == 0 || x == 11 || z == 11);
            c.flags         = border ? CELL_SOLID : CELL_FLOOR;
            c.floorHeight   = 0;
            c.ceilingHeight = 48;
            if (!border) LevelGridSystem::addPlatform(c, 12, 1);   // L1 top 3 m
        }
    LevelGridSystem::removePlatform(g.cells[3 * 12 + 6], 12);                       // 1-cell gap
    for (u32 x = 8; x <= 10; x++) LevelGridSystem::removePlatform(g.cells[5 * 12 + x], 12); // lake row z=5

    const f32 feet = 3.0f;   // standing on L1

    SUBCASE("1-cell gap: viable, lands on the far cell centre") {
        StoryNav::VaultPlan p = StoryNav::planVault(g, {5.5f, feet, 3.5f}, feet, {1, 0, 0});
        CHECK(p.gapAhead);
        CHECK(p.viable);
        CHECK(p.landing.x == doctest::Approx(7.5f));   // cell 7 centre — the far lip
        CHECK(p.landing.y == doctest::Approx(3.0f));
    }
    SUBCASE("lake wider than the probe: gapAhead but NOT viable — do not leap in") {
        StoryNav::VaultPlan p = StoryNav::planVault(g, {7.5f, feet, 5.5f}, feet, {1, 0, 0});
        CHECK(p.gapAhead);
        CHECK_FALSE(p.viable);
    }
    SUBCASE("2-cell hole: still viable (within VAULT_MAX_CELLS)") {
        LevelGridSystem::removePlatform(g.cells[7 * 12 + 5], 12);
        LevelGridSystem::removePlatform(g.cells[7 * 12 + 6], 12);
        StoryNav::VaultPlan p = StoryNav::planVault(g, {4.5f, feet, 7.5f}, feet, {1, 0, 0});
        CHECK(p.viable);
        CHECK(p.landing.x == doctest::Approx(7.5f));
    }
    SUBCASE("flat ground ahead: no gap, common-case early exit") {
        StoryNav::VaultPlan p = StoryNav::planVault(g, {2.5f, feet, 8.5f}, feet, {1, 0, 0});
        CHECK_FALSE(p.gapAhead);
        CHECK_FALSE(p.viable);
    }
    SUBCASE("a wall ahead is a wall, not a gap") {
        StoryNav::VaultPlan p = StoryNav::planVault(g, {1.5f, feet, 8.5f}, feet, {-1, 0, 0});
        CHECK_FALSE(p.gapAhead);
        CHECK_FALSE(p.viable);
    }
    SUBCASE("no heading, no probe") {
        StoryNav::VaultPlan p = StoryNav::planVault(g, {5.5f, feet, 3.5f}, feet, {0, 0, 0});
        CHECK_FALSE(p.viable);
    }
    LevelGridSystem::shutdown(g);
}

// --- COMMITTED ramp routing (the Autoplay stair climb) ------------------------------------------
// A VERTICAL_HALL ramp is a GRADUATED SLAB, which is what made climbing one fail: onUpperStory tests
// the feet against the slab top of the cell underfoot, so a body one riser up already reads "upper".
// A caller routing on `botUpper != exitUpper` therefore stopped routing the instant the climb began
// and fell back to the flat ground field, which pulled the bot off the ramp — land, re-read "lower",
// walk back to the foot, repeat. These pin the two pieces that replaced it.

TEST_CASE("story nav: feetOnStory judges by height, not by the slab underfoot") {
    // The whole point: mid-ramp the body is metres below the balcony even though the cell it stands
    // on reports a slab. Height is the only honest test, and 1.5 m cleanly halves the 3 m pitch.
    CHECK(StoryNav::feetOnStory(0.0f, 0.0f));      // on the ground, ground exit
    CHECK(StoryNav::feetOnStory(3.0f, 3.0f));      // on the balcony, balcony exit
    CHECK_FALSE(StoryNav::feetOnStory(0.0f, 3.0f));// on the ground, balcony exit: not there yet
    CHECK_FALSE(StoryNav::feetOnStory(1.4f, 3.0f));// HALFWAY UP THE RAMP: still not there
    CHECK(StoryNav::feetOnStory(2.9f, 3.0f));      // last riser: close enough to count as arrived
}

TEST_CASE("story nav: portalRouteGoal aims at the far end once the near end is reached") {
    // Phase 1 walks the bot to the foot; phase 2 walks it UP. Without phase 2 the goal is the foot,
    // so arriving there collapses the heading and the climb never starts.
    StoryPortal p{};
    p.lowPos  = Vec3{10.0f, 0.0f, 10.0f};
    p.highPos = Vec3{16.0f, 3.0f, 10.0f};

    // Far from the foot: aim at the foot.
    const Vec3 g1 = StoryNav::portalRouteGoal(p, Vec3{2.0f, 0.0f, 10.0f}, /*climbing=*/true);
    CHECK(g1.x == doctest::Approx(p.lowPos.x));

    // Standing at the foot: aim at the top, which is what walks the slab.
    const Vec3 g2 = StoryNav::portalRouteGoal(p, Vec3{10.0f, 0.0f, 10.0f}, /*climbing=*/true);
    CHECK(g2.x == doctest::Approx(p.highPos.x));

    // Partway up, still committed to the top rather than turning back to the foot.
    const Vec3 g3 = StoryNav::portalRouteGoal(p, Vec3{13.0f, 1.5f, 10.0f}, /*climbing=*/true);
    CHECK(g3.x == doctest::Approx(p.highPos.x));

    // AT THE TOP, the goal must STILL be the top — not flip back to the foot. The driver keeps
    // `climbing` fixed to the exit story exactly so it does not invert here at the crest: passing
    // climbing=true at the top height must still return highPos. (Passing climbing based on
    // `exitY > feetY` would go false here and send the bot back down — the bug this pins against.)
    const Vec3 g3b = StoryNav::portalRouteGoal(p, Vec3{16.0f, 3.0f, 10.0f}, /*climbing=*/true);
    CHECK(g3b.x == doctest::Approx(p.highPos.x));

    // Descending mirrors it: the top is the near end, the foot the far one.
    const Vec3 g4 = StoryNav::portalRouteGoal(p, Vec3{16.0f, 3.0f, 10.0f}, /*climbing=*/false);
    CHECK(g4.x == doctest::Approx(p.lowPos.x));
}

TEST_CASE("story nav: nearestPortalIdx picks by the end on the body's OWN story") {
    DungeonResult d{};
    d.portals[d.portalCount].lowPos  = Vec3{ 5.0f, 0.0f, 5.0f};
    d.portals[d.portalCount].highPos = Vec3{30.0f, 3.0f, 30.0f};
    d.portalCount++;
    d.portals[d.portalCount].lowPos  = Vec3{40.0f, 0.0f, 40.0f};
    d.portals[d.portalCount].highPos = Vec3{ 8.0f, 3.0f, 8.0f};
    d.portalCount++;

    // On the GROUND, ramp 0's foot is closest.
    CHECK(StoryNav::nearestPortalIdx(d, Vec3{6.0f, 0.0f, 6.0f}, /*fromUpper=*/false) == 0);
    // On the BALCONY at the same XZ, ramp 1's TOP is closest — the near end is the other one.
    CHECK(StoryNav::nearestPortalIdx(d, Vec3{6.0f, 3.0f, 6.0f}, /*fromUpper=*/true) == 1);
    // No ramps at all: -1, so the caller keeps its flat heading.
    DungeonResult empty{};
    CHECK(StoryNav::nearestPortalIdx(empty, Vec3{0, 0, 0}, false) == -1);
}
