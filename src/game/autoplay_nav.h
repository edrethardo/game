// autoplay_nav.h — pure navigation POLICY for the Autoplay bot (no player mutation here).
//
// Two jobs: (1) the hazard veto — is a one-cell XZ step in `dir` safe given the bot's feet
// height, so combat kiting and travel can never back it into lava, a wall, or off the map; (2)
// descend eligibility — the exact gate updateFloorDoor enforces (engine_update.cpp:2760-2801),
// mirrored so the brain only asks to descend when it actually can. The per-style TRAVEL goal
// (ramp climb / drop-hole pick / causeway) is computed in the engine driver from StoryNav +
// DungeonResult; this file holds the safety check every steering intent passes through.
#pragma once
#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"
#include "world/level_gen.h"    // DungeonResult::dropHoles — the Descent's ways down

namespace Autoplay {

// One cell's worth of the veto, at a WORLD POINT (so the caller controls the feet height the lava
// test reads). In-bounds, not solid, and not lava-at-feet-height. The lava test reuses
// LevelGridSystem::feetInLava — the SINGLE-SOURCE rule the burn tick uses (level_grid.h: "the burn
// tick and any future lava consumer share one rule"), so the bot vetoes exactly the cells that would
// burn it and NOT the ones it can clear airborne. `lavaFloor` short-circuits the lava test entirely
// on non-Hellforge floors (no lava cells exist there, so the query is pure waste).
// `avoidPads` additionally refuses CELL_JUMPPAD cells. Off by default because a pad is normally a
// GIFT — free height, a shortcut off the Stacked Loop's ramps, the recovery lift after a bad fall.
// It is switched on for TRAVEL on a FOUR_STORY "Descent" floor, where the whole objective is to get
// DOWN and a pad is the one piece of terrain that undoes the floor: the maze seeds them in every
// dead-end node (a full 3x3 glowing slab) plus under a third of the drop holes as return lifts, they
// fire the instant you are grounded, and they lift ~two stories. A bot wandering over one loses its
// entire descent. Measured on a 150 s marksman trace before this: 25-27 unplanned climbs per run,
// clustering at single XZ spots where it bounced repeatedly, with the bot spending 63% of its time
// on ONE story instead of descending. See the driver's `avoidPads` for the standing-on-one carve-out.
inline bool cellPassable(const LevelGrid& g, Vec3 at, f32 feetY, bool lavaFloor,
                         bool avoidPads = false) {
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, at, gx, gz)) return false;   // off the map edge
    if (LevelGridSystem::isSolid(g, gx, gz))          return false;   // into a wall
    if (lavaFloor && LevelGridSystem::feetInLava(g, Vec3{at.x, feetY, at.z})) return false;
    if (avoidPads && (LevelGridSystem::getCell(g, gx, gz).flags & CELL_JUMPPAD)) return false;
    return true;
}

// True if stepping one cell along `dir` (XZ heading, need not be unit) from `from` at feet height
// `feetY` is safe.
//
// NO CORNER CUTTING. A DIAGONAL step requires the destination cell AND both orthogonal component
// cells — the same rule Pathfinder::findPath enforces on its 8-connected expansion (pathfinder.cpp:
// "a diagonal step is only legal if BOTH shared orthogonal cells are also walkable"). The original
// version point-sampled ONLY the destination, so on a diagonal heading it happily approved squeezing
// through the shared corner of a wall — which the bot's ~0.3 m body cannot fit through. Live, that
// is the bot pressing itself into wall corners and wedging there ("it tries to cut corners too often
// and gets stuck in the corner"). Cardinal steps cross one grid axis and have no shared corner, so
// they are unchanged (a corridor's flanking walls must never veto walking down it). When a diagonal
// is refused the callers already rotate to +-45 degrees, which lands exactly on the two cardinals
// bracketing it — so the practical effect is that the bot rounds a corner squarely instead of
// clipping it.
//
// It does NOT cover stepping off an interior CELL_PLATFORM balcony edge into a drop — those falls
// are INTENTIONAL traversal on VERTICAL_HALL / FOUR_STORY (a balcony overlook, a drop-hole descent),
// decided by the driver's story-navigation layer, not a hazard. The veto covers only the three
// things that are never wanted: off-map, a solid wall, and grounded-in-lava.
//
// Still a per-cell test rather than a swept body AABB, so it assumes sub-cell per-tick steps (fine
// for the ~0.17 m/tick driver at 6 m/s over 60 Hz); a dash/teleport that crosses a whole cell in one
// tick is the driver's responsibility to gate, not this veto's.
inline bool stepAllowed(const LevelGrid& g, Vec3 from, f32 feetY, Vec3 dir, bool lavaFloor,
                        bool avoidPads = false) {
    Vec3 flat{dir.x, 0.0f, dir.z};
    if (lengthSq(flat) < 1e-6f) return true;                 // no heading: nothing to veto
    const Vec3 to = from + normalize(flat) * g.cellSize;     // one cell ahead along the heading
    if (!cellPassable(g, to, feetY, lavaFloor, avoidPads)) return false;

    // Diagonal? Compare the grid cells rather than the heading angle — what matters is whether the
    // step actually crosses BOTH grid axes (a shallow heading from near a cell edge can, a 45-degree
    // one from mid-cell might not), which is exactly when a shared corner exists to clip.
    u32 fx, fz, tx, tz;
    if (!LevelGridSystem::worldToGrid(g, from, fx, fz)) return true;   // off-grid origin: nothing to compare
    if (!LevelGridSystem::worldToGrid(g, to,   tx, tz)) return false;  // (already covered, kept explicit)
    if (fx == tx || fz == tz) return true;                             // cardinal (or same cell): done
    // Both orthogonal component cells must be passable too. Built as world points on the SAME axes
    // as `to`, so they resolve to (tx,fz) and (fx,tz) without a grid->world round trip.
    if (!cellPassable(g, Vec3{to.x,   feetY, from.z}, feetY, lavaFloor, avoidPads)) return false;
    if (!cellPassable(g, Vec3{from.x, feetY, to.z},   feetY, lavaFloor, avoidPads)) return false;
    return true;
}

// True when the body's own cell is a jump pad. The `avoidPads` veto tests the DESTINATION cell, so a
// bot that has already landed in the middle of a 3x3 pad node would find every neighbour refused and
// box itself in; the driver therefore drops the veto while standing on one, so it can walk out.
inline bool onJumpPad(const LevelGrid& g, Vec3 pos) {
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) return false;
    return (LevelGridSystem::getCell(g, gx, gz).flags & CELL_JUMPPAD) != 0;
}

// Just the pad clause of the veto: is the cell one step along `dir` a jump pad? Split out because
// the driver applies pad-avoidance to COMBAT movement on a Descent floor, where borrowing the whole
// of stepAllowed would also cancel a kite step at a wall — a much wider behaviour change than
// intended, and one the FIGHT branch is deliberately exempt from.
inline bool padAhead(const LevelGrid& g, Vec3 from, Vec3 dir) {
    Vec3 flat{dir.x, 0.0f, dir.z};
    if (lengthSq(flat) < 1e-6f) return false;
    return onJumpPad(g, from + normalize(flat) * g.cellSize);
}

// --- FOUR_STORY "Descent": which story am I on, and which drop hole do I take? -------------------

// The bot's STORY REFERENCE: the surface it is standing on, or — while airborne — the one it took
// off from and will land back on. This is `effectiveFloorHeight`, the same story selector collision
// uses, NOT the raw feet height.
//
// The distinction is the whole ballgame on a Descent floor, because the bot JUMPS constantly: the
// kite/strafe pulse fires one every ~2.2 s and the unstick ladder adds more, and a jump carries the
// feet 2.4 m over the slab for well over a second. Filtering holes on raw feet-Y (`|surfaceY - pos.y|
// <= PLATFORM_STEP_TOLERANCE`, a 0.4 m window) therefore rejects EVERY hole on the bot's own story
// for the entire flight — the drop-hole router silently goes dark and the heading falls back to the
// flat exit flow field, which on the wrong story points at a spot three floors below. Measured on a
// 150 s marksman trace: 21-27% of all ticks had no hole pick at all, and 100% of those were airborne
// (log line "holesRaw=0 holesEff=18" — eighteen holes on the story, none visible to the filter).
// Reading the slab instead makes the story stable across a jump, and it is exactly right while
// FALLING too: over a punched hole there is no slab at the old height, so the effective floor
// immediately resolves to the surface one story down and the bot is already routing on the story it
// is committed to landing on.
// The feet offset is what makes this the story BELOW rather than the story being jumped toward.
// effectiveFloorHeight accepts any slab whose top is within PLATFORM_STEP_TOLERANCE (0.4 m) ABOVE
// the feet — right for a body stepping up onto a stair, wrong for identifying which storey a body
// belongs to, because a jump that passes within 0.4 m of the next slab would report that slab. The
// Descent's storeys are 3 m apart and the base jump reaches ~2.4-2.7 m, so a plain jump comes close
// enough to flip the answer near its apex. Subtracting the tolerance back off cancels it exactly:
// the selector then returns the highest slab at or BELOW the feet, which is the one the bot took
// off from and will land back on. kSettle is a hair of slack so a body resting on a slab at
// 8.99999 m still reads as being on the 9 m storey rather than flickering to the one beneath.
inline f32 botStoryY(const LevelGrid& g, Vec3 pos) {
    constexpr f32 kSettle = 0.05f;
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) return pos.y;   // off-grid: nothing better
    return LevelGridSystem::effectiveFloorHeight(g, gx, gz,
                                                 pos.y - PLATFORM_STEP_TOLERANCE + kSettle);
}

// The exit is always DOWN, so the bot's whole travel plan on a Descent floor is "find a hole in my
// own story's slab and walk into it". The first version simply took the NEAREST same-story hole,
// and MEASURED live that never gets off floor 1 in 150 s (marksman: closest approach to the L0 exit
// 13.9 m, 8 unplanned climbs back up; warrior: never reached L0 at all).
//
// The cause is RETURN-LIFT PADS. About one hole in three has a CELL_JUMPPAD on the surface one story
// below it (level_gen.cpp `PAD_HOLE_ONE_IN`), deliberately, so a player can climb back up the way
// they came. A pad fires the INSTANT you are grounded, so dropping through such a hole is undone
// before the bot can decide anything — and, standing back on the story it started on, its nearest
// hole is that same one. That is a closed loop the bot can neither see nor escape, and it is exactly
// what the live trace shows it doing. So they are SKIPPED, reading the pad flag straight off the
// GRID at the hole's own XZ (a pad cell is a pad on every story it carries, and the recorded
// jumpPads[] array is capped at MAX_JUMP_PADS while a floor can hold more), which makes the test
// exact rather than approximate.
//
// The ORDER among the survivors is nearest-first, and the driver walks that order handing each
// candidate to A* until one is actually routable — which is why this returns a LIST rather than a
// single pick. Euclidean distance alone is a poor guide on a labyrinth: the nearest hole by metres
// is regularly on the far side of a wall and reachable only by a long way round, and the bot used to
// beeline at it with a ±45/±90 detour fan and simply scrape along the wall (measured: over a 150 s
// trace the distance to the chosen hole GREW on 61 samples and shrank on 53 — a random walk, not an
// approach). An earlier attempt to fix that by SCORING holes on "walk to it plus its walk to the
// exit" made things worse still, because it chose holes 15-22 m off and a straight-line heading
// cannot deliver the bot to a goal that far away through a maze. The answer is not a cleverer score
// but a real route: keep the goal local and nearest-first, and let the driver's A* decide which of
// them it can genuinely get to. Landing XZ needs no managing — once the bot is on L0 the ordinary
// flat exit flow field routes it to the door properly, walls and all.
//
// Writes at most `maxOut` indices into `out`, nearest-first, all CLEAN holes before any padded one
// (the padded ones are the return lifts described above — a last resort, never a first choice).
// Returns how many were written; 0 means this story has no way down (the flat exit flow field is
// then the right heading — that IS the case on L0, where the bot simply walks to the door).
inline u8 dropHoleCandidates(const LevelGrid& g, const DungeonResult& d, Vec3 pos,
                             s32* out, u8 maxOut) {
    if (maxOut == 0) return 0;
    const f32 storyY = botStoryY(g, pos);
    // Sort key: distance², with padded holes pushed behind every clean one by a constant so large no
    // real squared distance can cross it (a 64-cell map's diagonal² is ~8k m²).
    constexpr f32 kPadPenalty = 1e9f;
    f32 key[16];
    u8  n = 0;
    if (maxOut > 16) maxOut = 16;   // the scratch bound; callers want 1-4
    for (u8 i = 0; i < d.dropHoleCount; i++) {
        const DropHole& h = d.dropHoles[i];
        if (fabsf(h.surfaceY - storyY) > PLATFORM_STEP_TOLERANCE) continue;   // not our story
        const f32 dx = h.pos.x - pos.x, dz = h.pos.z - pos.z;
        // Padded? The pad lives one story DOWN at the same XZ, but a pad cell carries the flag for
        // the whole column, so the flag at the hole's own cell answers the question.
        u32 gx, gz;
        const bool padded = LevelGridSystem::worldToGrid(g, h.pos, gx, gz) &&
                            (LevelGridSystem::getCell(g, gx, gz).flags & CELL_JUMPPAD) != 0;
        const f32 k = dx * dx + dz * dz + (padded ? kPadPenalty : 0.0f);
        if (n == maxOut && k >= key[n - 1]) continue;      // worse than everything we're keeping
        u8 slot = (n < maxOut) ? n : (u8)(maxOut - 1);
        if (n < maxOut) n++;
        while (slot > 0 && key[slot - 1] > k) { key[slot] = key[slot - 1]; out[slot] = out[slot - 1]; slot--; }
        key[slot] = k; out[slot] = (s32)i;
    }
    return n;
}

// Single best candidate, or -1 when this story has no way down. Thin wrapper over the list above so
// both share one story/pad rule.
inline s32 pickDropHole(const LevelGrid& g, const DungeonResult& d, Vec3 pos) {
    s32 one[1];
    return dropHoleCandidates(g, d, pos, one, 1) ? one[0] : -1;
}

// combatStalled — the combat-standoff break-off test (engine_autoplay.cpp). The FIGHT branch fires at
// any in-band LOS target, but LOS is a raycast to the target's CENTRE: cover, a doorway, or an
// elevation edge can make every shot miss or be blocked while the ray still reads clear, so the bot
// fires forever, kills nothing, and — firing in place is zero XZ movement — never trips the plain
// travel stuck-detector. The unified detector (driver) holds `noProgressTimer` at zero while the bot
// either MOVES (>0.5 m) or deals damage (`combatProgressThisTick` — a nearby hostile's HP fell or one
// died); so a timer that has climbed past the break-off threshold WITH an in-band target present means
// the bot is locked in a fight it is making no progress on. That is the livelock: break it by forcing a
// travel leg (change position + firing angle). Pure so the rule unit-tests without the Engine.
inline bool combatStalled(f32 noProgressTimer, bool inBandTarget, bool combatProgressThisTick) {
    constexpr f32 kBreakoffSec = 3.0f;   // fire-in-place with no damage this long = a standoff to break
    return noProgressTimer > kBreakoffSec && inBandTarget && !combatProgressThisTick;
}

// escapeHeading — the escalating stuck-override's Stage-2 fallback (engine_autoplay.cpp). When the
// flow field yields no usable heading and the lateral ±90/180 nudge finds nothing, scan all 8 compass
// headings and return the FIRST hazard-safe one (stepAllowed) that ALSO moves the bot away from
// `awayFrom` — the position where it wedged — so it walks OUT of the pocket rather than back into it.
// Cardinals (N/S/E/W) are tried before diagonals: a cardinal step aligns with the grid and with
// stepAllowed's single-cell point sample, so it's the more reliable escape. If no safe heading
// increases the XZ distance from the wedge (the bot is standing on the anchor, or every opening faces
// it) it falls back to the FIRST hazard-safe heading so the bot still MOVES; only a fully-walled cell
// (all 8 vetoed) returns {0,0,0}. Pure — no engine state — so it unit-tests on a synthetic LevelGrid.
inline Vec3 escapeHeading(const LevelGrid& g, Vec3 from, f32 feetY, Vec3 awayFrom, bool lavaFloor) {
    constexpr f32 kD = 0.70710678f;   // 1/sqrt(2): the unit component of a 45° diagonal heading
    const Vec3 dirs[8] = {
        { 0, 0,  1}, { 0, 0, -1}, {  1, 0,  0}, { -1, 0,  0},         // N, S, E, W (cardinals first)
        { kD, 0, kD}, { kD, 0, -kD}, { -kD, 0, kD}, { -kD, 0, -kD},   // NE, SE, NW, SW (last resort)
    };
    // XZ distance² from the wedge anchor at the CURRENT cell — a candidate step must beat this to
    // count as "moving away".
    const f32 ax = from.x - awayFrom.x, az = from.z - awayFrom.z;
    const f32 curD2 = ax * ax + az * az;
    Vec3 firstSafe{0, 0, 0};
    bool haveFirstSafe = false;
    for (u32 i = 0; i < 8; i++) {
        if (!stepAllowed(g, from, feetY, dirs[i], lavaFloor)) continue;      // hazard: never step here
        if (!haveFirstSafe) { firstSafe = dirs[i]; haveFirstSafe = true; }   // remembered for fallback
        const Vec3 to = from + dirs[i] * g.cellSize;                         // dirs are already unit
        const f32 tx = to.x - awayFrom.x, tz = to.z - awayFrom.z;
        if (tx * tx + tz * tz > curD2 + 1e-4f) return dirs[i];              // safe AND away: take it
    }
    return firstSafe;   // {0,0,0} only when every one of the 8 neighbour cells is a hazard (boxed in)
}

// Inputs the descend gate reasons over, gathered by the engine driver from m_level + boss state.
struct DescendCtx {
    bool doorActive = false;   // m_level.floorDoorActive (false in town / arena / The Source)
    f32  distToDoor = 1e9f;    // metres from the bot to m_level.floorDoorPos
    bool hasBoss    = false;   // m_level.floorHasBoss (exit is sealed on boss floors)
    bool bossAlive  = false;   // floorBossAlive()
};
// The radius updateFloorDoor actually descends in (engine_update.cpp tests squared distance < 4.0 —
// the strict-vs-inclusive boundary at exactly 2.0 m is unreachable in practice). SINGLE-SOURCED,
// because every anti-wedge remedy that decides to STAND STILL and hold the interact button has to
// be sure the hold can really fire from where it stopped.
inline constexpr f32 DESCEND_RADIUS = 2.0f;
// Where a remedy may plant the bot and hold interact. Strictly INSIDE DESCEND_RADIUS: the exit-wedge
// remedy used to engage at 2.5 m, so between 2.0 and 2.5 m it stood the bot perfectly still holding
// a button that could never fire — and standing still is itself "no progress", so the remedy re-armed
// forever (measured live: 73 consecutive seconds frozen beside an open exit). Outside this the bot
// must keep WALKING IN, never park.
inline constexpr f32 DESCEND_STOP_M = 1.9f;
static_assert(DESCEND_STOP_M < DESCEND_RADIUS, "a remedy must only stand still where the descend can fire");

// Mirror of updateFloorDoor's gate: within DESCEND_RADIUS of an active door and not blocked by a
// live floor boss. The Source portal is a SEPARATE trigger the brain simply never requests, so it
// needs no branch here.
inline bool mayDescend(const DescendCtx& c) {
    if (!c.doorActive) return false;
    if (c.distToDoor > DESCEND_RADIUS) return false;
    if (c.hasBoss && c.bossAlive) return false;
    return true;
}

// --- LOOK BEHIND (dormant-ambusher trigger) -----------------------------------------------------
// The dungeon's stone gargoyles (EnemyRole::AMBUSH) sit in AIState::DORMANT posing as statues under
// a WEEPING-ANGEL rule: enemy_ai_states.cpp wakes one only when a player is inside its detection
// range AND **nobody is watching it** (`provoked && !watched`), and Combat::applyDamage returns
// early on a dormant AMBUSH enemy, so a statue cannot be shot awake either.
//
// That is a perfect trap for the bot. A gargoyle is a solid body, so one standing in a doorway
// blocks the way; it is also an ordinary hostile in the target list, so the bot AIMS AT IT — which
// is precisely what pins it asleep — and then fires at it forever for zero damage. Staring is the
// one thing that guarantees the wedge never clears.
//
// So when the bot has made no progress for LOOK_BEHIND_AT seconds it deliberately TURNS AROUND for
// LOOK_BEHIND_HOLD seconds. Turning 180 deg un-watches whatever it was facing, which is exactly the
// condition a dormant ambusher needs to spring — and once awake it moves, becomes damageable, and
// stops being a wall. (It also reveals anything that crept up behind, which is the same wake rule
// running in the bot's favour.) ONE-SHOT per stuck episode: a bot that keeps spinning is neither
// useful nor watchable, so the latch only re-arms after real progress.
inline constexpr f32 LOOK_BEHIND_AT   = 3.0f;   // s of no progress before the look-behind (Aaron's number)
// Long enough for the aim smoother to actually complete the half-turn (a 180 deg sweep takes ~0.9 s
// at the shipped flick rate + ease-out) and still face away for a few ticks — the wake test runs
// every tick, so a few is plenty.
inline constexpr f32 LOOK_BEHIND_HOLD = 1.2f;   // s spent turned around

// True when a stuck episode has run long enough to spend its one look-behind.
inline bool lookBehindDue(f32 noProgressTimer, bool alreadySpent) {
    return !alreadySpent && noProgressTimer >= LOOK_BEHIND_AT;
}
// The yaw directly behind `yaw`, folded into [-pi, pi] so the aim smoother turns the SHORT way
// (either way is 180 deg, but an unfolded value could sit revolutions out — the engine never
// re-wraps Player::yaw).
inline f32 lookBehindYaw(f32 yaw) {
    constexpr f32 kPi = 3.14159265358979f, kTwoPi = 6.28318530717959f;
    f32 y = yaw + kPi;
    y = y - kTwoPi * floorf((y + kPi) / kTwoPi);
    return y;
}

// PULSE the descend interact-hold rather than holding it continuously. The floor exit is taken by
// HOLDING the interact button (Interact::poll / interact.h) — but a HOLD reaches the SHRINE first
// when one shares the exit's interact range (choose(): shrine outranks exit on a hold). A human
// activates the shrine, RELEASES, then holds again to descend. The bot holds forever, so poll fires
// its one hold (on the shrine), latches `consumed`, and — never releasing — can never re-fire to
// reach the exit: a permanent wedge next to an already-used shrine (observed live on flat floors
// whenever a shrine rolled onto the exit). So the driver pulses the button: hold long enough to fire
// (> INTERACT_HOLD_SEC = 0.35 s), then release a beat to clear `consumed`, and repeat — one cycle
// spends the shrine, the next reaches the exit. `phase` is the seconds the bot has continuously
// WANTED to descend (reset to 0 when it stops). Returns whether PICKUP should be held THIS tick.
inline bool descendPulseHeld(f32 phase) {
    constexpr f32 kHoldOn = 0.50f;   // hold window — comfortably past the 0.35 s hold threshold
    constexpr f32 kCycle  = 0.65f;   // + a 0.15 s release window that clears poll's `consumed` latch
    const f32 t = phase - kCycle * floorf(phase / kCycle);   // phase mod kCycle (no <cmath> fmodf dep)
    return t < kHoldOn;
}

// --- TOWN PORTAL -------------------------------------------------------------------------------
// The town is the one world the exit-flow travel policy cannot express at all: it has NO floor door
// (`floorDoorActive` false, so `onNormalFloor` is false and the brain returns an empty intent) and
// its flow field is built toward the PLAZA CENTRE, not toward the to-dungeon portal — so an armed
// AFK run used to park at the hub forever. The whole town policy is below: beeline at the portal on
// XZ, stop just inside its trigger, press interact. It lives here rather than in the driver so the
// two radii are pinned by a test against the engine's own numbers instead of drifting silently.

// The portal's trigger radius — mirrors resolveInteractTargets' `lengthSq(townPortalPos - pos) < 4.0f`.
inline constexpr f32 TOWN_PORTAL_RADIUS = 2.0f;
// Stop SHORT of the portal's centre instead of walking onto it. The portal is a HOLD target
// (Interact::choose: the exit class is only reachable by holding), so the bot has to STAND inside the
// trigger for INTERACT_HOLD_SEC — the same lesson the exit bull learned when it blasted straight
// through the floor door's 2 m window at 6-16 m/s and never descended.
inline constexpr f32 TOWN_PORTAL_STOP = 1.5f;

// What the bot should do about the town portal from where it is standing.
struct TownPortalPlan {
    Vec3 heading{0.0f, 0.0f, 0.0f};   // unit XZ direction to the portal ({0,0,0} = standing on it)
    bool walk = false;                // hold MOVE_FORWARD along `heading`
    bool take = false;                // inside the trigger: press (pulse) the interact button
};

// Pure: positions in, plan out. `walk` and `take` overlap on purpose — between STOP and RADIUS the
// bot is already close enough to trigger, so it stops moving and starts pressing in the same tick.
inline TownPortalPlan planTownPortal(Vec3 pos, Vec3 portal) {
    TownPortalPlan p{};
    const Vec3 to{portal.x - pos.x, 0.0f, portal.z - pos.z};
    const f32  d2 = lengthSq(to);
    if (d2 > 1e-6f) {
        p.heading = normalize(to);
        p.walk    = d2 > TOWN_PORTAL_STOP * TOWN_PORTAL_STOP;
    }
    // The engine's own proximity test is 3D. The town is flat, so this is the same number today —
    // mirroring it rather than assuming keeps the bot's idea of "in range" from drifting from the
    // arbitration's if the hub ever gains a step.
    p.take = lengthSq(portal - pos) < TOWN_PORTAL_RADIUS * TOWN_PORTAL_RADIUS;
    return p;
}

} // namespace Autoplay
