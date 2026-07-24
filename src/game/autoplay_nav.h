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

// True if stepping one cell along `dir` (XZ heading, need not be unit) from `from` at feet height
// `feetY` is safe: the destination cell is in-bounds, not solid, and not lava-at-feet-height. The
// lava test reuses LevelGridSystem::feetInLava — the SINGLE-SOURCE rule the burn tick uses (level_grid.h:
// "the burn tick and any future lava consumer share one rule"), so the bot vetoes exactly the cells
// that would burn it and NOT the ones it can clear airborne. `lavaFloor` short-circuits the lava
// test entirely on non-Hellforge floors (no lava cells exist there, so the query is pure waste).
//
// SCOPE — this is a POINT sample of the ONE cell normalize(dir)*cellSize ahead, NOT a swept body
// AABB: a diagonal heading probes only the diagonal cell and skips the two orthogonally-adjacent
// cells the body actually clips at the shared corner. It therefore assumes sub-cell per-tick steps
// (fine for the ~0.17 m/tick driver at 6 m/s over 60 Hz); a dash/teleport that crosses a whole cell
// in one tick is the driver's responsibility to gate, not this veto's.
//
// It also does NOT cover stepping off an interior CELL_PLATFORM balcony edge into a drop — those
// falls are INTENTIONAL traversal on VERTICAL_HALL / FOUR_STORY (a balcony overlook, a drop-hole
// descent), decided by the driver's story-navigation layer, not a hazard. The veto covers only the
// three things that are never wanted: off-map, a solid wall, and grounded-in-lava.
inline bool stepAllowed(const LevelGrid& g, Vec3 from, f32 feetY, Vec3 dir, bool lavaFloor) {
    Vec3 flat{dir.x, 0.0f, dir.z};
    if (lengthSq(flat) < 1e-6f) return true;                 // no heading: nothing to veto
    Vec3 to = from + normalize(flat) * g.cellSize;           // one cell ahead along the heading
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, to, gx, gz)) return false;   // off the map edge
    if (LevelGridSystem::isSolid(g, gx, gz)) return false;           // into a wall
    // Airborne over lava is free (feet above the molten surface) — exactly what makes a 1-cell
    // vein jumpable — so probe feetInLava at the DESTINATION with the bot's own feet height.
    if (lavaFloor && LevelGridSystem::feetInLava(g, Vec3{to.x, feetY, to.z})) return false;
    return true;
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
