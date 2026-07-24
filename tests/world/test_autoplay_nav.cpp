// test_autoplay_nav.cpp — pure Autoplay navigation policy: the hazard veto (never steer into lava,
// a solid wall, or off the map — balcony-edge drops are intentional traversal the veto does NOT
// cover) and descend eligibility (never while the boss lives; only inside the door radius). Built
// on synthetic LevelGrids so it needs no engine —
// the same construction pattern as test_lava.cpp / test_platform.cpp (LevelGridSystem::init/shutdown,
// cells indexed z*width+x, floor height in quarter-units).

#include <doctest/doctest.h>
#include "game/autoplay_nav.h"
#include "world/level_grid.h"

namespace {
// All-floor grid, floor height 0, given width/depth. Mirrors the lava/platform test setup: init
// allocates cells+flowDir+clearance, every cell is CELL_FLOOR at height 0. Caller frees with
// LevelGridSystem::shutdown.
LevelGrid makeFlatGrid(u32 w, u32 d) {
    LevelGrid g;
    LevelGridSystem::init(g, w, d, 1.0f);
    for (u32 z = 0; z < d; z++)
        for (u32 x = 0; x < w; x++) {
            GridCell& c = g.cells[z * w + x];
            c.flags         = CELL_FLOOR;
            c.floorHeight   = 0;
            c.ceilingHeight = 20;
        }
    return g;
}
void setLava(LevelGrid& g, u32 x, u32 z)  { g.cells[z * g.width + x].flags = CELL_FLOOR | CELL_LAVA; }
void setSolid(LevelGrid& g, u32 x, u32 z) { g.cells[z * g.width + x].flags = CELL_SOLID; }
} // namespace

TEST_CASE("hazard veto: a heading into a lava cell one step ahead is rejected") {
    LevelGrid g = makeFlatGrid(8, 8);
    setLava(g, 5, 4);
    // Standing at cell (4,4), a +X heading steps onto the lava cell (5,4): vetoed.
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK_FALSE(Autoplay::stepAllowed(g, from, /*feetY=*/0.0f, Vec3{1, 0, 0}, /*lavaFloor=*/true));
    CHECK(Autoplay::stepAllowed(g, from, 0.0f, Vec3{-1, 0, 0}, true));   // away from lava: fine
    LevelGridSystem::shutdown(g);
}

TEST_CASE("hazard veto: airborne over lava is allowed (feet above the surface)") {
    LevelGrid g = makeFlatGrid(8, 8);
    setLava(g, 5, 4);
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK(Autoplay::stepAllowed(g, from, /*feetY=*/1.2f, Vec3{1, 0, 0}, true));  // jumping the vein
    LevelGridSystem::shutdown(g);
}

TEST_CASE("hazard veto: a solid wall one step ahead is rejected") {
    LevelGrid g = makeFlatGrid(8, 8);
    setSolid(g, 5, 4);
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK_FALSE(Autoplay::stepAllowed(g, from, 0.0f, Vec3{1, 0, 0}, false));
    LevelGridSystem::shutdown(g);
}

TEST_CASE("hazard veto: stepping off the map edge is rejected") {
    LevelGrid g = makeFlatGrid(8, 8);
    const Vec3 from = LevelGridSystem::gridToWorld(g, 7, 4);      // last column
    CHECK_FALSE(Autoplay::stepAllowed(g, from, 0.0f, Vec3{1, 0, 0}, false));  // +X leaves the grid
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descend eligibility: never while a boss is alive") {
    Autoplay::DescendCtx ctx;
    ctx.doorActive = true; ctx.distToDoor = 1.0f; ctx.hasBoss = true; ctx.bossAlive = true;
    CHECK_FALSE(Autoplay::mayDescend(ctx));
    ctx.bossAlive = false;
    CHECK(Autoplay::mayDescend(ctx));
}

TEST_CASE("descend eligibility: only inside the 2 m door radius, only when active") {
    Autoplay::DescendCtx ctx;
    ctx.doorActive = true; ctx.hasBoss = false; ctx.bossAlive = false;
    ctx.distToDoor = 3.0f; CHECK_FALSE(Autoplay::mayDescend(ctx));   // too far (>2 m)
    ctx.distToDoor = 1.5f; CHECK(Autoplay::mayDescend(ctx));
    ctx.doorActive = false; CHECK_FALSE(Autoplay::mayDescend(ctx));  // no door (town/arena)
}

TEST_CASE("descend pulse: holds then releases so the hold can re-fire past a shrine") {
    // The button must be HELD past the 0.35 s hold threshold (so the hold fires at all) and then
    // RELEASED within one cycle (so Interact::poll's `consumed` latch clears and the NEXT hold can
    // reach the exit past a just-spent shrine). A continuous hold would fire exactly once.
    CHECK(Autoplay::descendPulseHeld(0.00f));   // press edge: held
    CHECK(Autoplay::descendPulseHeld(0.35f));   // still held AT the hold threshold -> the hold fires
    CHECK(Autoplay::descendPulseHeld(0.49f));   // still held just before release
    CHECK_FALSE(Autoplay::descendPulseHeld(0.55f));   // release window: clears `consumed`
    CHECK_FALSE(Autoplay::descendPulseHeld(0.64f));   // still releasing
    CHECK(Autoplay::descendPulseHeld(0.66f));   // next cycle: held again -> re-fires (now the exit)
    CHECK(Autoplay::descendPulseHeld(0.66f + 0.35f));  // and re-fires past the 0.35 s threshold
}
