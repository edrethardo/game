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

TEST_CASE("hazard veto: avoidPads refuses a step whose BODY clips an adjacent pad, not just its centre") {
    // The launch (Collision::jumpPadSpeed) fires when the body's ~0.35 m halfWidth footprint overlaps
    // ANY CELL_JUMPPAD — not only when the centre sits on one — so a centre-cell-only veto let the bot
    // clip a return lift with its side and get flung a storey up (the FOUR_STORY descent bounce). The
    // veto tests the footprint corners to match the launch.
    LevelGrid g = makeFlatGrid(10, 10);              // cellSize 1.0
    g.cells[5 * g.width + 5].flags |= CELL_JUMPPAD;  // a pad at (5,5)
    // Step +X to a destination whose CENTRE lands on the safe cell (4,5) but whose body overlaps (5,5).
    const Vec3 from{3.65f, 0.0f, 5.5f};              // to = (4.65,5.5): centre cell (4,5), body reaches x>=5
    CHECK_FALSE(Autoplay::stepAllowed(g, from, 0.0f, Vec3{1, 0, 0}, false, /*avoidPads=*/true));
    CHECK      (Autoplay::stepAllowed(g, from, 0.0f, Vec3{1, 0, 0}, false, /*avoidPads=*/false));
    // A step nowhere near the pad is still fine with avoidPads on (the footprint must not over-reject).
    CHECK(Autoplay::stepAllowed(g, Vec3{1.5f, 0.0f, 1.5f}, 0.0f, Vec3{1, 0, 0}, false, /*avoidPads=*/true));
    LevelGridSystem::shutdown(g);
}

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

TEST_CASE("a remedy may only STAND STILL where the descend can actually fire") {
    // The exit-wedge remedy engaged at 2.5 m while updateFloorDoor descends at 2.0 m, so between the
    // two the bot stood perfectly still holding a button that could never fire — and standing still
    // IS "no progress", so the remedy re-armed itself forever (73 consecutive seconds frozen beside
    // an open exit, measured live). The stop distance must therefore sit strictly INSIDE the radius.
    CHECK(Autoplay::DESCEND_STOP_M < Autoplay::DESCEND_RADIUS);
    Autoplay::DescendCtx ctx;
    ctx.doorActive = true; ctx.hasBoss = false; ctx.bossAlive = false;
    ctx.distToDoor = Autoplay::DESCEND_STOP_M;
    CHECK(Autoplay::mayDescend(ctx));            // parked at the stop distance, the hold really fires
    ctx.distToDoor = 2.4f;
    CHECK_FALSE(Autoplay::mayDescend(ctx));      // ...and the old 2.5 m trigger band never could
}

// LOOK BEHIND — the dormant-ambusher trigger. enemy_ai_states.cpp springs a DORMANT AMBUSH enemy
// only when a player is in range AND `!watched`, and Combat::applyDamage returns early on one, so a
// gargoyle the bot is staring at is an unkillable solid body that can never wake. Turning around is
// the only move that clears that wedge.
TEST_CASE("look behind: due once the stuck timer passes the threshold, and only once") {
    CHECK_FALSE(Autoplay::lookBehindDue(Autoplay::LOOK_BEHIND_AT - 0.1f, false));  // not stuck long enough
    CHECK(Autoplay::lookBehindDue(Autoplay::LOOK_BEHIND_AT, false));               // armed
    CHECK_FALSE(Autoplay::lookBehindDue(Autoplay::LOOK_BEHIND_AT + 5.0f, true));   // already spent: no spinning
    // It fires BEFORE the 4 s geometry ladder — looking is cheaper than walking, and a woken
    // gargoyle usually unblocks the very cell the escape would have been searching around.
    CHECK(Autoplay::LOOK_BEHIND_AT < 4.0f);
    // The turn must outlast the aim smoother's own half-turn (~0.9 s) or the bot never actually
    // faces away and the wake condition is never met.
    CHECK(Autoplay::LOOK_BEHIND_HOLD > 0.9f);
}

TEST_CASE("look behind: the reversed yaw is a real 180 deg, folded to the short arc") {
    constexpr f32 kPi = 3.14159265358979f;
    auto opposite = [](f32 a, f32 b) {   // |shortest arc| between them must be pi
        f32 d = a - b;
        while (d >  kPi) d -= 2.0f * kPi;
        while (d < -kPi) d += 2.0f * kPi;
        return (d < 0.0f ? -d : d);
    };
    for (f32 y = -9.0f; y < 9.0f; y += 0.37f) {
        const f32 b = Autoplay::lookBehindYaw(y);
        CHECK(opposite(b, y) == doctest::Approx(kPi).epsilon(0.001));
        CHECK(b <=  kPi + 0.001f);       // folded: the engine never re-wraps Player::yaw, and an
        CHECK(b >= -kPi - 0.001f);       // unfolded target would make the smoother turn the long way
    }
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

// A Descent-shaped grid: the flat ground plus the three stacked slabs carveFourStory lays at
// 3/6/9 m over every open cell. The slabs are not decoration here — botStoryY identifies the bot's
// story by asking effectiveFloorHeight which SURFACE it is standing on, so a grid without them puts
// every body on the ground floor no matter what its feet say, and no L3 hole would ever match.
LevelGrid makeDescentGrid(u32 w, u32 d) {
    LevelGrid g = makeFlatGrid(w, d);
    for (u32 z = 0; z < d; z++)
        for (u32 x = 0; x < w; x++) {
            GridCell& c = g.cells[z * w + x];
            LevelGridSystem::addPlatform(c, 12, 0);   // L1 @ 3 m  (quarter-units)
            LevelGridSystem::addPlatform(c, 24, 0);   // L2 @ 6 m
            LevelGridSystem::addPlatform(c, 36, 0);   // L3 @ 9 m
        }
    return g;
}
} // namespace

TEST_CASE("descent: the story reference is the slab underfoot, not the raw feet height") {
    // The bot JUMPS constantly (the kite/strafe pulse, the unstick ladder), and a jump carries the
    // feet 2.4 m over a storey pitch of 3 m for well over a second. Matching holes on raw feet-Y
    // rejected every hole on the bot's own storey for the whole flight — measured at 21-27% of all
    // ticks, 100% of them airborne — so the router went dark and the bot fell back to a heading
    // aimed three floors below. The slab underfoot does not move while you are above it.
    LevelGrid g = makeDescentGrid(20, 20);
    const Vec3 at{6.0f, 9.0f, 6.0f};
    CHECK(Autoplay::botStoryY(g, at) == doctest::Approx(9.0f));            // standing on L3
    CHECK(Autoplay::botStoryY(g, Vec3{6.0f, 11.4f, 6.0f}) == doctest::Approx(9.0f)); // mid-jump off L3
    CHECK(Autoplay::botStoryY(g, Vec3{6.0f,  0.0f, 6.0f}) == doctest::Approx(0.0f));  // on the ground
    // The one that the old PLATFORM_STEP_TOLERANCE window got wrong: a jump from L0 that comes
    // within 0.4 m of the L1 slab must still read as L0 — it is passing under it, not standing on it.
    CHECK(Autoplay::botStoryY(g, Vec3{6.0f, 2.7f, 6.0f}) == doctest::Approx(0.0f));
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent: the committed story holds through a hole-lip flicker, moves only on a real landing") {
    // At a drop hole's LIP the raw storey reference is a knife-edge: a 0.2 m dip in feet-Y (or a few cm
    // of XZ drift onto the hole cell) makes botStoryY fall a full storey to the ground below, because
    // the storey's own slab then sits just outside effectiveFloorHeight's tolerance window. The descent
    // field reseeds per storey and the two seedings point OPPOSITE ways, so the raw reading alone froze
    // the bot oscillating at the very hole it should drop into (measured: three builds each stuck
    // 19-37 min on a FOUR_STORY floor). commitBotStory is the hysteresis that fixes it: it only moves
    // the storey once the bot is SOLIDLY standing on a different one.
    LevelGrid g = makeDescentGrid(20, 20);
    const f32 x = 6.0f, z = 6.0f;
    // First call of a floor (committed == the 1e9 sentinel): adopt whatever storey the bot is on.
    CHECK(Autoplay::commitBotStory(g, Vec3{x, 6.0f, z}, 1e9f) == doctest::Approx(6.0f));
    // Solidly on L2 (feet at the 6 m slab): commit to it.
    CHECK(Autoplay::commitBotStory(g, Vec3{x, 6.0f, z}, 6.0f) == doctest::Approx(6.0f));
    // THE FLICKER: feet dip to 5.8 m at the lip, so raw botStoryY reads the 3 m slab below — but the
    // feet are nowhere near 3 m, so the commit must HOLD L2 rather than reseed the field to L1.
    REQUIRE(Autoplay::botStoryY(g, Vec3{x, 5.8f, z}) == doctest::Approx(3.0f));   // raw really does flip
    CHECK(Autoplay::commitBotStory(g, Vec3{x, 5.8f, z}, 6.0f) == doctest::Approx(6.0f)); // …but commit holds
    // Mid-fall (feet at 4.5 m, between storeys): still not landed, still hold L2.
    CHECK(Autoplay::commitBotStory(g, Vec3{x, 4.5f, z}, 6.0f) == doctest::Approx(6.0f));
    // Landed on L1 (feet at the 3 m slab): NOW the bot is solidly a storey down — commit to it.
    CHECK(Autoplay::commitBotStory(g, Vec3{x, 3.1f, z}, 6.0f) == doctest::Approx(3.0f));
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent: candidates are nearest-first with every clean hole ahead of any padded one") {
    // The driver walks this order handing each hole to its router, so the ORDER is the contract:
    // padded holes (return lifts) must sort behind every clean one however close they are.
    LevelGrid g = makeDescentGrid(40, 40);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(7.5f,  7.5f,  9.0f);   // nearest, but padded
    d.dropHoles[d.dropHoleCount++] = holeAt(20.5f, 20.5f, 9.0f);   // farthest clean
    d.dropHoles[d.dropHoleCount++] = holeAt(12.5f, 12.5f, 9.0f);   // middle clean
    d.dropHoles[d.dropHoleCount++] = holeAt(5.5f,  5.5f,  6.0f);   // wrong storey: never listed
    setPad(g, 7, 7);
    s32 out[4];
    const u8 n = Autoplay::dropHoleCandidates(g, d, Vec3{6.0f, 9.0f, 6.0f}, out, 4);
    REQUIRE(n == 3);
    CHECK(out[0] == 2);   // clean, nearer
    CHECK(out[1] == 1);   // clean, farther
    CHECK(out[2] == 0);   // padded last, despite being closest of all
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent: a hole on ANOTHER story is never chosen") {
    LevelGrid g = makeDescentGrid(40, 40);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(5.5f, 5.5f, 6.0f);    // one story below us
    d.dropHoles[d.dropHoleCount++] = holeAt(20.5f, 20.5f, 9.0f);  // ours, but far
    const s32 i = Autoplay::pickDropHole(g, d, Vec3{6.0f, 9.0f, 6.0f});
    CHECK(i == 1);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent: no hole on this story => -1 (fall back to the flat exit flow, e.g. on L0)") {
    LevelGrid g = makeDescentGrid(40, 40);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(5.5f, 5.5f, 9.0f);
    CHECK(Autoplay::pickDropHole(g, d, Vec3{6.0f, 0.0f, 6.0f}) == -1);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("descent: a RETURN-LIFT hole is refused in favour of a clean one") {
    // The bug: a pad one story under the hole fires the instant the bot lands, throwing it back up
    // through the hole it just took — and from up there that same hole is again the nearest. The
    // grid flag is the ground truth (jumpPads[] is capped; the flag is not), so it is what we read.
    LevelGrid g = makeDescentGrid(40, 40);
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
    LevelGrid g = makeDescentGrid(40, 40);
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
    LevelGrid g = makeDescentGrid(40, 40);
    DungeonResult d{};
    d.dropHoles[d.dropHoleCount++] = holeAt(4.5f,  4.5f,  9.0f);  // 5 m away
    d.dropHoles[d.dropHoleCount++] = holeAt(14.5f, 14.5f, 9.0f);  // 9 m away, nearer the far exit
    const s32 i = Autoplay::pickDropHole(g, d, Vec3{8.0f, 9.0f, 8.0f});
    CHECK(i == 0);
    LevelGridSystem::shutdown(g);
}

// --- wouldFall: the off-a-ledge test (Autoplay anti-fall on a VHALL climb) -----------------------
TEST_CASE("wouldFall: stepping off a balcony edge onto the ground is a fall") {
    // Cells (4,4) and (5,4) carry a 3 m slab (a balcony); (6,4) does not (open ground below).
    // A body standing on the balcony (feetY = 3) that steps +X from (5,4) to (6,4) drops 3 m.
    LevelGrid g = makeFlatGrid(10, 10);
    for (u32 x = 4; x <= 5; x++) LevelGridSystem::addPlatform(g.cells[4 * g.width + x], 12, 0); // 12 q = 3 m
    const Vec3 onBalcony = LevelGridSystem::gridToWorld(g, 5, 4);   // XZ of (5,4)
    CHECK(Autoplay::wouldFall(g, onBalcony, /*feetY=*/3.0f, Vec3{ 1, 0, 0}));  // +X onto bare ground (6,4)
    CHECK_FALSE(Autoplay::wouldFall(g, onBalcony, 3.0f, Vec3{-1, 0, 0}));      // -X onto the slab (4,4)
    LevelGridSystem::shutdown(g);
}

TEST_CASE("wouldFall: no drop on flat ground, and a zero heading never falls") {
    LevelGrid g = makeFlatGrid(10, 10);
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK_FALSE(Autoplay::wouldFall(g, from, /*feetY=*/0.0f, Vec3{1, 0, 0}));  // flat: not a fall
    CHECK_FALSE(Autoplay::wouldFall(g, from, 0.0f, Vec3{0, 0, 0}));            // no heading: never a fall
    LevelGridSystem::shutdown(g);
}

TEST_CASE("wouldFall: a small step-up ledge (<= tolerance) is NOT a fall") {
    // A body on the ground next to a 0.25 m slab is not "falling" onto it — that is a walkable stair.
    LevelGrid g = makeFlatGrid(10, 10);
    LevelGridSystem::addPlatform(g.cells[4 * g.width + 5], 1, 0);   // 1 q = 0.25 m, under the 0.4 tolerance
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK_FALSE(Autoplay::wouldFall(g, from, 0.0f, Vec3{1, 0, 0}));
    LevelGridSystem::shutdown(g);
}

// ── VHALL ramp APPROACH steering (rampSegDistXZ / rampApproachDir) ─────────────────────────────
// Pure XZ geometry (Y ignored — the ramp is a graduated slab, we only steer the ground plane). Ramp
// foot `low` at the origin, top `high` 10 m along +X and 3 m up. These pin the centreline-approach that
// replaced the old "bounce at the ramp foot forever" climb.

TEST_CASE("rampSegDistXZ: distance to the ramp segment, projected onto XZ") {
    const Vec3 low{0, 0, 0}, high{10, 3, 0};                // axis is +X in XZ
    CHECK(Autoplay::rampSegDistXZ(low, high, Vec3{5, 1.5f, 0}) == doctest::Approx(0.0f));  // ON the segment
    CHECK(Autoplay::rampSegDistXZ(low, high, Vec3{5, 9,   2}) == doctest::Approx(2.0f));   // 2 m lateral (Y ignored)
    CHECK(Autoplay::rampSegDistXZ(low, high, Vec3{13, 3,  0}) == doctest::Approx(3.0f));   // 3 m past the TOP (t clamps to 1)
    CHECK(Autoplay::rampSegDistXZ(low, high, Vec3{-2, 0,  0}) == doctest::Approx(2.0f));   // 2 m before the FOOT (t clamps to 0)
}

TEST_CASE("rampApproachDir: on the centreline below the top, steer straight UP the ramp") {
    const Vec3 low{0, 0, 0}, high{10, 3, 0};
    const Vec3 d = Autoplay::rampApproachDir(low, high, Vec3{2, 0, 0});   // on centreline, 2 m up
    CHECK(d.x == doctest::Approx(1.0f));                                  // pure +X (up-ramp), unit length
    CHECK(d.z == doctest::Approx(0.0f));
}

TEST_CASE("rampApproachDir: laterally offset, keep an UP component AND correct toward the centreline") {
    const Vec3 low{0, 0, 0}, high{10, 3, 0};
    const Vec3 d = Autoplay::rampApproachDir(low, high, Vec3{2, 0, 2});   // 2 m off the centreline (+Z)
    CHECK(d.x > 0.0f);                                                    // still climbing (never steers purely sideways)
    CHECK(d.z < 0.0f);                                                    // pulled back toward the centreline (−Z, opposite the +Z offset)
    CHECK(std::sqrt(d.x * d.x + d.z * d.z) == doctest::Approx(1.0f));     // normalized
}

TEST_CASE("rampApproachDir: past the top returns zero (caller crosses to the door)") {
    const Vec3 low{0, 0, 0}, high{10, 3, 0};
    const Vec3 d = Autoplay::rampApproachDir(low, high, Vec3{12, 3, 0});  // 2 m beyond the top (> L + 1)
    CHECK(lengthSq(d) == doctest::Approx(0.0f));
}

TEST_CASE("rampApproachDir: a degenerate (zero-length) ramp returns zero") {
    const Vec3 p{0, 0, 0};                                                // low == high — nothing to climb
    CHECK(lengthSq(Autoplay::rampApproachDir(p, p, Vec3{1, 0, 1})) == doctest::Approx(0.0f));
}

// --- UNDER-RAMP PINCH --------------------------------------------------------------------------
// A VERTICAL_HALL ramp is a graduated slab whose low end descends to head height and below. The
// ground under it is a trap, and VHallField has always excluded it — but the travel VETO did not, so
// every producer that steers by stepAllowed instead of by the field (the escape ladder's 8-direction
// search, the detour fan, the pad beeline) could still walk the bot under a ramp. Aaron, watching a
// run: "trying to walk under the stairs is bad."
TEST_CASE("Autoplay veto: refuses ground under a low slab, allows it on top and under a high one") {
    LevelGrid g{};
    LevelGridSystem::init(g, 8, 8, 1.0f);
    for (u32 z = 0; z < 8; z++)
        for (u32 x = 0; x < 8; x++) {
            GridCell& c = g.cells[z * 8 + x];
            c.flags = (x == 0 || z == 0 || x == 7 || z == 7) ? CELL_SOLID : (CELL_FLOOR | CELL_CEILING);
            c.floorHeight = 0; c.ceilingHeight = 20;
        }

    // A ramp's LOW end at cell (4,4): a slab whose underside is well below body height.
    GridCell& low = g.cells[4 * 8 + 4];
    low.flags |= CELL_PLATFORM;
    // Slab TOP 1.25 m => underside 0.75 m (PLATFORM_THICKNESS_Q = 2 => 0.5 m). That is the real
    // ramp-low-end profile: too HIGH to step onto (PLATFORM_STEP_TOLERANCE = 0.4 m) and too LOW to
    // fit under (BODY_CLEARANCE = 0.8 m). A 0.5 m slab would simply be stepped ON, which is correct
    // behaviour and not the trap being tested.
    low.platCount = 1;
    low.platHeight[0] = 5;

    // A BALCONY arcade at (2,4): slab at 3 m, plenty of headroom — must stay walkable.
    GridCell& high = g.cells[4 * 8 + 2];
    high.flags |= CELL_PLATFORM;
    high.platCount = 1;
    high.platHeight[0] = 12;   // 3.0 m

    const Vec3 underLow { 4.5f, 0.0f, 4.5f };
    const Vec3 underHigh{ 2.5f, 0.0f, 4.5f };

    // Walking UNDER the ramp's low end: refused.
    CHECK_FALSE(Autoplay::cellPassable(g, underLow, /*feetY=*/0.0f, /*lavaFloor=*/false));
    // Standing ON that same slab: allowed — the rule is story-aware, not a blanket cell ban.
    CHECK(Autoplay::cellPassable(g, underLow, /*feetY=*/1.25f, false));
    // The balcony arcade is fine to walk under; only a LOW slab pinches.
    CHECK(Autoplay::cellPassable(g, underHigh, 0.0f, false));
    // Plain open floor is unaffected.
    CHECK(Autoplay::cellPassable(g, Vec3{5.5f, 0.0f, 5.5f}, 0.0f, false));

    LevelGridSystem::shutdown(g);
}

// A body that is ALREADY pinned under a ramp needs a way OUT. The veto only prevents ENTERING the
// trap, and it cannot help here — from underneath, every direction is equally refused. This is the
// counterpart, and it is needed because the trap is genuinely reachable: the FIGHT branch's
// close/kite movement is deliberately unvetoed, so a melee build chasing a hostile can walk itself
// under the stairs. Reported twice live: "paladin is stuck again under the stairs".
TEST_CASE("Autoplay: unpinDirection walks a trapped body out from under a low slab") {
    LevelGrid g{};
    LevelGridSystem::init(g, 12, 12, 1.0f);
    for (u32 z = 0; z < 12; z++)
        for (u32 x = 0; x < 12; x++) {
            GridCell& c = g.cells[z * 12 + x];
            c.flags = (x == 0 || z == 0 || x == 11 || z == 11) ? CELL_SOLID : (CELL_FLOOR | CELL_CEILING);
            c.floorHeight = 0; c.ceilingHeight = 20;
        }
    // A ramp's low end covering x = 4..6 (top 1.25 m => underside 0.75 m: the pinch profile).
    for (u32 x = 4; x <= 6; x++)
        for (u32 z = 1; z <= 10; z++) {
            GridCell& c = g.cells[z * 12 + x];
            c.flags |= CELL_PLATFORM; c.platCount = 1; c.platHeight[0] = 5;
        }

    const Vec3 under{5.5f, 0.0f, 5.5f};                 // squarely beneath the slab
    const Vec3 out = Autoplay::unpinDirection(g, under, 0.0f);
    REQUIRE(lengthSq(out) > 1e-6f);                     // it must produce a heading at all

    // Following the heading must actually LEAVE the band. It cannot escape in a single cell — the
    // band is 3 wide and the body starts in the middle — so walk it and require an exit within the
    // band's own width, which is what "the shortest way out" means here.
    bool escaped = false;
    for (u32 step = 1; step <= 3 && !escaped; step++) {
        const Vec3 at{under.x + out.x * g.cellSize * (f32)step, 0.0f,
                      under.z + out.z * g.cellSize * (f32)step};
        u32 sx, sz;
        if (LevelGridSystem::worldToGrid(g, at, sx, sz))
            escaped = !LevelGridSystem::bodyPinnedUnderSlab(g, sx, sz, 0.0f);
    }
    CHECK(escaped);
    // ...and it leaves ACROSS the band (which runs in Z), not along its diagonal — the shorter exit.
    CHECK(fabsf(out.x) > fabsf(out.z));

    // A body NOT pinned gets no heading — this must never perturb ordinary movement.
    CHECK(lengthSq(Autoplay::unpinDirection(g, Vec3{2.5f, 0.0f, 5.5f}, 0.0f)) == doctest::Approx(0.0f));
    // ...nor one standing ON the slab.
    CHECK(lengthSq(Autoplay::unpinDirection(g, under, 1.25f)) == doctest::Approx(0.0f));

    LevelGridSystem::shutdown(g);
}
