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

// --- CORNER-CUT PREVENTION -------------------------------------------------------------------
// The veto used to point-sample ONLY the destination cell, so a DIAGONAL heading squeezed past a
// wall corner the bot's ~0.3 m body actually clips — it pressed into the corner and wedged ("it
// tries to cut corners too often and gets stuck in the corner"). A diagonal step now needs the
// diagonal cell AND both orthogonal component cells, exactly like Pathfinder's own rule.
TEST_CASE("hazard veto: a diagonal step past a wall CORNER is rejected") {
    // From (4,4) heading NE toward (5,5). The diagonal cell itself is open, but the +X orthogonal
    // (5,4) is solid — that is the corner the body scrapes. This is the wedge regression.
    LevelGrid g = makeFlatGrid(8, 8);
    setSolid(g, 5, 4);
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK_FALSE(Autoplay::stepAllowed(g, from, 0.0f, Vec3{1, 0, 1}, false));
    // ...and symmetrically when it is the +Z orthogonal (4,5) that is solid.
    LevelGridSystem::shutdown(g);
    LevelGrid g2 = makeFlatGrid(8, 8);
    setSolid(g2, 4, 5);
    const Vec3 f2 = LevelGridSystem::gridToWorld(g2, 4, 4);
    CHECK_FALSE(Autoplay::stepAllowed(g2, f2, 0.0f, Vec3{1, 0, 1}, false));
    LevelGridSystem::shutdown(g2);
}

TEST_CASE("hazard veto: a diagonal step with all three cells open is allowed") {
    LevelGrid g = makeFlatGrid(8, 8);
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK(Autoplay::stepAllowed(g, from, 0.0f, Vec3{1, 0, 1}, false));    // NE, open field
    CHECK(Autoplay::stepAllowed(g, from, 0.0f, Vec3{-1, 0, -1}, false));  // SW
    LevelGridSystem::shutdown(g);
}

TEST_CASE("hazard veto: the corner rule never fires on a CARDINAL step") {
    // A cardinal crosses ONE grid axis, so there is no shared corner to clip — walls flanking the
    // corridor must not veto walking down it (that would freeze the bot in every hallway).
    LevelGrid g = makeFlatGrid(8, 8);
    setSolid(g, 5, 5); setSolid(g, 5, 3);              // both diagonal neighbours of the +X step
    setSolid(g, 4, 5); setSolid(g, 4, 3);              // and both walls flanking the bot itself
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK(Autoplay::stepAllowed(g, from, 0.0f, Vec3{1, 0, 0}, false));
    LevelGridSystem::shutdown(g);
}

TEST_CASE("hazard veto: a diagonal past a LAVA corner is rejected too") {
    // Same body-clip geometry, molten: the orthogonal cell burns even though the destination is dry.
    LevelGrid g = makeFlatGrid(8, 8);
    setLava(g, 5, 4);
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK_FALSE(Autoplay::stepAllowed(g, from, /*feetY=*/0.0f, Vec3{1, 0, 1}, /*lavaFloor=*/true));
    CHECK(Autoplay::stepAllowed(g, from, /*feetY=*/1.2f, Vec3{1, 0, 1}, true));   // airborne: free
    LevelGridSystem::shutdown(g);
}

TEST_CASE("escapeHeading: a cell boxed on all sides but one returns that one opening") {
    // (4,4) is walled on all 8 neighbours except the +X (east) cell (5,4). The 8-dir search must
    // find that single opening whatever the anchor is.
    LevelGrid g = makeFlatGrid(8, 8);
    setSolid(g, 4, 5); setSolid(g, 4, 3);                       // N, S
    setSolid(g, 5, 5); setSolid(g, 5, 3);                       // NE, SE
    setSolid(g, 3, 5); setSolid(g, 3, 3);                       // NW, SW
    setSolid(g, 3, 4);                                          // W  (leaves only E = (5,4) open)
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    // Anchor sits on the bot itself (curD2 == 0): any safe step counts as "away".
    const Vec3 esc = Autoplay::escapeHeading(g, from, /*feetY=*/0.0f, from, /*lavaFloor=*/false);
    CHECK(lengthSq(esc) > 1e-6f);      // found an escape
    CHECK(esc.x > 0.5f);               // it is the +X opening
    CHECK(esc.z == doctest::Approx(0.0f));
    LevelGridSystem::shutdown(g);
}

TEST_CASE("escapeHeading: a fully-walled cell returns zero (nothing is safe)") {
    LevelGrid g = makeFlatGrid(8, 8);
    setSolid(g, 5, 4); setSolid(g, 3, 4); setSolid(g, 4, 5); setSolid(g, 4, 3);   // E W N S
    setSolid(g, 5, 5); setSolid(g, 5, 3); setSolid(g, 3, 5); setSolid(g, 3, 3);   // diagonals
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    const Vec3 esc = Autoplay::escapeHeading(g, from, 0.0f, from, false);
    CHECK(lengthSq(esc) < 1e-6f);      // boxed in: no heading
    LevelGridSystem::shutdown(g);
}

TEST_CASE("escapeHeading: with two openings it prefers the one AWAY from the wedge anchor") {
    // (4,4) is open only E (5,4) and W (3,4); the anchor is far to the +X (east) side, so a +X step
    // moves TOWARD it and a -X step moves AWAY. Even though E is scanned before W, the search must
    // skip the toward-opening and return the away-opening.
    LevelGrid g = makeFlatGrid(8, 8);
    setSolid(g, 4, 5); setSolid(g, 4, 3);                       // N, S
    setSolid(g, 5, 5); setSolid(g, 5, 3);                       // NE, SE
    setSolid(g, 3, 5); setSolid(g, 3, 3);                       // NW, SW  (E and W stay open)
    const Vec3 from   = LevelGridSystem::gridToWorld(g, 4, 4);
    const Vec3 anchor = LevelGridSystem::gridToWorld(g, 6, 4);  // wedge point off to the east (+X)
    const Vec3 esc = Autoplay::escapeHeading(g, from, 0.0f, anchor, false);
    CHECK(lengthSq(esc) > 1e-6f);
    CHECK(esc.x < -0.5f);              // walked WEST, away from the eastern anchor
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

TEST_CASE("combat break-off: fires only past the threshold, with an in-band target, and no damage") {
    // The unified stuck detector holds noProgressTimer at zero while the bot MOVES or DEALS DAMAGE;
    // combatStalled is the break-off trigger for the remaining case — the timer has climbed (no move,
    // no damage) WHILE an in-band target is present (FIGHT is active, firing in place at cover/angle).
    // Below threshold: not yet a standoff.
    CHECK_FALSE(Autoplay::combatStalled(/*timer=*/1.0f, /*inBandTarget=*/true,  /*combatProgress=*/false));
    CHECK_FALSE(Autoplay::combatStalled(2.9f, true, false));
    // Past threshold, in-band, no damage: the livelock — break off.
    CHECK(Autoplay::combatStalled(3.1f, true, false));
    CHECK(Autoplay::combatStalled(10.0f, true, false));
    // Dealing damage right now is a real fight, never a standoff (the driver also zeroes the timer, but
    // the helper is honest on its own).
    CHECK_FALSE(Autoplay::combatStalled(10.0f, true, /*combatProgress=*/true));
    // No in-band target = a plain travel wedge, not a combat standoff (the escape ladder handles it).
    CHECK_FALSE(Autoplay::combatStalled(10.0f, /*inBandTarget=*/false, false));
}
