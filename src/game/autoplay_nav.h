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

namespace Autoplay {

// One cell's worth of the veto, at a WORLD POINT (so the caller controls the feet height the lava
// test reads). In-bounds, not solid, and not lava-at-feet-height. The lava test reuses
// LevelGridSystem::feetInLava — the SINGLE-SOURCE rule the burn tick uses (level_grid.h: "the burn
// tick and any future lava consumer share one rule"), so the bot vetoes exactly the cells that would
// burn it and NOT the ones it can clear airborne. `lavaFloor` short-circuits the lava test entirely
// on non-Hellforge floors (no lava cells exist there, so the query is pure waste).
inline bool cellPassable(const LevelGrid& g, Vec3 at, f32 feetY, bool lavaFloor) {
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, at, gx, gz)) return false;   // off the map edge
    if (LevelGridSystem::isSolid(g, gx, gz))          return false;   // into a wall
    if (lavaFloor && LevelGridSystem::feetInLava(g, Vec3{at.x, feetY, at.z})) return false;
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
inline bool stepAllowed(const LevelGrid& g, Vec3 from, f32 feetY, Vec3 dir, bool lavaFloor) {
    Vec3 flat{dir.x, 0.0f, dir.z};
    if (lengthSq(flat) < 1e-6f) return true;                 // no heading: nothing to veto
    const Vec3 to = from + normalize(flat) * g.cellSize;     // one cell ahead along the heading
    if (!cellPassable(g, to, feetY, lavaFloor)) return false;

    // Diagonal? Compare the grid cells rather than the heading angle — what matters is whether the
    // step actually crosses BOTH grid axes (a shallow heading from near a cell edge can, a 45-degree
    // one from mid-cell might not), which is exactly when a shared corner exists to clip.
    u32 fx, fz, tx, tz;
    if (!LevelGridSystem::worldToGrid(g, from, fx, fz)) return true;   // off-grid origin: nothing to compare
    if (!LevelGridSystem::worldToGrid(g, to,   tx, tz)) return false;  // (already covered, kept explicit)
    if (fx == tx || fz == tz) return true;                             // cardinal (or same cell): done
    // Both orthogonal component cells must be passable too. Built as world points on the SAME axes
    // as `to`, so they resolve to (tx,fz) and (fx,tz) without a grid->world round trip.
    if (!cellPassable(g, Vec3{to.x,   feetY, from.z}, feetY, lavaFloor)) return false;
    if (!cellPassable(g, Vec3{from.x, feetY, to.z},   feetY, lavaFloor)) return false;
    return true;
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
// Mirror of updateFloorDoor's gate: within 2 m of an active door (the real code tests squared
// distance < 4.0 — the strict-vs-inclusive boundary at exactly 2.0 m is unreachable in practice)
// and not blocked by a live floor boss. The Source portal is a SEPARATE trigger the brain simply
// never requests, so it needs no branch here.
inline bool mayDescend(const DescendCtx& c) {
    if (!c.doorActive) return false;
    if (c.distToDoor > 2.0f) return false;
    if (c.hasBoss && c.bossAlive) return false;
    return true;
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

} // namespace Autoplay
