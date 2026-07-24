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

TEST_CASE("town portal: beeline heading points at the portal on XZ, ignoring height") {
    // The town's flow field targets the plaza CENTRE, so the driver steers at the portal directly.
    // Height must not tilt the heading — it is fed to a flat MOVE_FORWARD, not to an aim.
    const Autoplay::TownPortalPlan p =
        Autoplay::planTownPortal(Vec3{22.0f, 0.0f, 36.0f},        // the south-gate arrival spot
                                 Vec3{22.0f, 3.0f, 32.0f});       // portal, deliberately 3 m higher
    CHECK(p.heading.y == doctest::Approx(0.0f));
    CHECK(p.heading.z == doctest::Approx(-1.0f));                 // straight up the plaza (-Z)
    CHECK(p.heading.x == doctest::Approx(0.0f));
    CHECK(p.walk);                                                // 4 m out: keep walking
}

TEST_CASE("town portal: stops short of the centre but inside the trigger, so the hold can land") {
    // The portal is an EXIT-class HOLD target: the bot has to STAND inside the 2 m trigger for
    // INTERACT_HOLD_SEC. Walking to the exact centre would carry it straight back out the far side
    // at 6 m/s — the floor-door bug the exit bull was written to fix.
    const Vec3 portal{10.0f, 0.0f, 10.0f};

    const Autoplay::TownPortalPlan far = Autoplay::planTownPortal(Vec3{10.0f, 0.0f, 18.0f}, portal);
    CHECK(far.walk);
    CHECK_FALSE(far.take);                                        // 8 m: nothing to press yet

    // The stop band and the trigger band OVERLAP on purpose: from 2.0 m in, the bot is already
    // pressing WHILE it closes the last half-metre, so the hold is mid-cycle by the time it halts.
    // A stop distance at or beyond the trigger would instead park it outside the radius pressing a
    // button that can never fire.
    CHECK(Autoplay::TOWN_PORTAL_STOP < Autoplay::TOWN_PORTAL_RADIUS);
    const Autoplay::TownPortalPlan closing = Autoplay::planTownPortal(Vec3{10.0f, 0.0f, 11.8f}, portal);
    CHECK(closing.walk);                                          // 1.8 m: still closing to the stop
    CHECK(closing.take);                                          // ...but already inside the trigger

    const Autoplay::TownPortalPlan parked = Autoplay::planTownPortal(Vec3{10.0f, 0.0f, 11.4f}, portal);
    CHECK_FALSE(parked.walk);                                     // 1.4 m: inside the stop, hold still
    CHECK(parked.take);
}

TEST_CASE("town portal: standing exactly on it yields no heading but still presses") {
    // Degenerate case: a zero heading must not produce a NaN direction, and the press must survive
    // it (the bot may well be shoved onto the portal's own cell).
    const Autoplay::TownPortalPlan p =
        Autoplay::planTownPortal(Vec3{10.0f, 0.0f, 10.0f}, Vec3{10.0f, 0.0f, 10.0f});
    CHECK(lengthSq(p.heading) == doctest::Approx(0.0f));
    CHECK_FALSE(p.walk);
    CHECK(p.take);
}

// --- FOUR_STORY "Descent": drop-hole choice ------------------------------------------------------
// The measured floor-1 livelock (marksman: 150 s, never descended, 8 unplanned climbs back up) came
// down to the old "nearest same-story hole" rule walking straight into the return-lift pads.

namespace {
// A hole record on the given story at a grid cell. surfaceY is the pierced slab's TOP — the height
// the bot's feet are at when it can enter, which is exactly what pickDropHole matches on.
DropHole holeAt(f32 x, f32 z, f32 surfaceY) { DropHole h; h.pos = {x, surfaceY, z}; h.surfaceY = surfaceY; return h; }
void setPad(LevelGrid& g, u32 x, u32 z) { g.cells[z * g.width + x].flags |= CELL_JUMPPAD; }
} // namespace

TEST_CASE("descent: a hole on ANOTHER story is never chosen") {
    LevelGrid g = makeFlatGrid(40, 40);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(5.5f, 5.5f, 6.0f);    // one story below us
    d.dropHoles[d.dropHoleCount++] = holeAt(20.5f, 20.5f, 9.0f);  // ours, but far
    const s32 i = Autoplay::pickDropHole(g, d, Vec3{6.0f, 9.0f, 6.0f});
    CHECK(i == 1);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent: no hole on this story => -1 (fall back to the flat exit flow, e.g. on L0)") {
    LevelGrid g = makeFlatGrid(40, 40);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(5.5f, 5.5f, 9.0f);
    CHECK(Autoplay::pickDropHole(g, d, Vec3{6.0f, 0.0f, 6.0f}) == -1);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent: a RETURN-LIFT hole is refused in favour of a clean one") {
    // The bug: a pad one story under the hole fires the instant the bot lands, throwing it back up
    // through the hole it just took — and from up there that same hole is again the nearest. The
    // grid flag is the ground truth (jumpPads[] is capped; the flag is not), so it is what we read.
    LevelGrid g = makeFlatGrid(40, 40);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(6.5f, 6.5f, 9.0f);    // RIGHT NEXT to us — but padded
    d.dropHoles[d.dropHoleCount++] = holeAt(14.5f, 14.5f, 9.0f);  // farther, clean
    setPad(g, 6, 6);
    const s32 i = Autoplay::pickDropHole(g, d, Vec3{6.0f, 9.0f, 6.0f});
    CHECK(i == 1);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent: a padded hole is still taken when it is the ONLY way down") {
    // Hole density thins to 7% on the deepest story, so "no clean hole" is a real state. A bounce
    // at least relocates the bot; standing still on a floor whose exit is downstairs never ends.
    LevelGrid g = makeFlatGrid(40, 40);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(6.5f, 6.5f, 9.0f);
    setPad(g, 6, 6);
    CHECK(Autoplay::pickDropHole(g, d, Vec3{6.0f, 9.0f, 6.0f}) == 0);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent: among clean holes the NEAREST wins — the goal must stay steerable") {
    // Deliberately not "the hole that also advances toward the exit". That variant was built, shipped
    // to a live run and measured: it chose holes 15-22 m off, and since the travel heading is a
    // straight line with a small detour fan (not a path), the bot beelined into a maze wall and never
    // left the top story. A LOCAL goal is the only kind this steering can reach on a labyrinth.
    LevelGrid g = makeFlatGrid(40, 40);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(4.5f,  4.5f,  9.0f);  // 5 m away
    d.dropHoles[d.dropHoleCount++] = holeAt(14.5f, 14.5f, 9.0f);  // 9 m away, nearer the far exit
    const s32 i = Autoplay::pickDropHole(g, d, Vec3{8.0f, 9.0f, 8.0f});
    CHECK(i == 0);
    LevelGridSystem::shutdown(g);
}
