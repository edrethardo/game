#pragma once
// story_nav.h — pure two-story navigation helpers for VERTICAL_HALL floors. The enemy CHASE uses
// these to decide whether its target is on the OTHER story and, if so, which ramp end to walk to.
// Kept pure (grid/dungeon in, XZ/bool out) so they're unit-testable independent of the AI (tests/
// world/test_story_nav.cpp) — the crowd_control.h / arena.h / lead_assist.h pattern.

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"
#include "world/level_gen.h"

namespace StoryNav {

// Which story a body's FEET are on in the cell under xzPos: true = upper (standing on the slab top),
// false = ground. Mirrors the collision story selector (effectiveFloorHeight) — a body is "upper"
// when its feet are within a step of the slab top. A cell with no slab is always the ground story.
inline bool onUpperStory(const LevelGrid& g, Vec3 xzPos, f32 feetY) {
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, xzPos, gx, gz)) return false;
    if (!LevelGridSystem::hasPlatform(g, gx, gz))         return false;
    return feetY >= LevelGridSystem::getPlatformTop(g, gx, gz) - PLATFORM_STEP_TOLERANCE;
}

// The XZ goal a body on story `fromUpper` should walk toward to reach story `toUpper`: the nearest
// ramp END on its OWN story (the foot if climbing up, the top if heading down). Crossing that end
// carries it via the ramp (the entity story-snap does the actual vertical move). Returns `from`
// unchanged when already on the target story or when the floor has no ramps.
inline Vec3 nearestPortalGoal(const DungeonResult& d, Vec3 from, bool fromUpper, bool toUpper) {
    if (fromUpper == toUpper || d.portalCount == 0) return from;
    Vec3 best = from;
    f32  bestD2 = 1e30f;
    for (u8 i = 0; i < d.portalCount; i++) {
        const Vec3 nearEnd = fromUpper ? d.portals[i].highPos : d.portals[i].lowPos;
        const f32 dx = nearEnd.x - from.x, dz = nearEnd.z - from.z;
        const f32 d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = nearEnd; }
    }
    return best;
}

// --- COMMITTED ramp routing (the Autoplay bot's stair climb) ------------------------------------
//
// nearestPortalGoal above answers "which ramp END is nearest" and is re-asked every tick, which is
// wrong for a body that is actually CLIMBING, for two compounding reasons:
//
//  1. A VERTICAL_HALL ramp is a GRADUATED SLAB — a run of cells whose platform tops rise from the
//     ground to the balcony. onUpperStory tests the feet against THIS CELL'S slab top, so a body one
//     riser up the ramp already reports "upper". A caller that routes on `botUpper != exitUpper`
//     therefore stops routing the moment the climb BEGINS, drops back to the flat ground flow field,
//     and gets pulled off the ramp it just stepped onto — then lands, reads "lower" again, walks back
//     to the foot, and repeats. That is a climb that never completes, and it is what the bot did
//     ("climbing stairs is also somehow difficult for the auto play bot").
//  2. Re-picking the nearest end each tick lets the goal flip between two ramps as the body moves,
//     swinging the heading (the documented "VH balcony story-routing oscillates on some seeds").
//
// So: pick a ramp ONCE (nearestPortalIdx), and steer with portalRouteGoal, which aims at the near
// end until the body has arrived at it and at the FAR end thereafter — walking the graduated slab
// end to end instead of treating arrival at the foot as arrival at the story. The caller decides
// when the crossing is done, and must do so by comparing FEET HEIGHT to the destination story rather
// than by asking onUpperStory, for reason (1) above.

// Index of the ramp whose end on the body's own story is nearest, or -1 when the floor has none.
inline s32 nearestPortalIdx(const DungeonResult& d, Vec3 from, bool fromUpper) {
    s32 best = -1;
    f32 bestD2 = 1e30f;
    for (u8 i = 0; i < d.portalCount; i++) {
        const Vec3 nearEnd = fromUpper ? d.portals[i].highPos : d.portals[i].lowPos;
        const f32 dx = nearEnd.x - from.x, dz = nearEnd.z - from.z;
        const f32 d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = (s32)i; }
    }
    return best;
}

// Where to walk next to cross this ramp. `climbing` = heading from the ground to the balcony.
// Two phases: MOUNT (aim at the near end, so the body arrives at the foot/top squarely rather than
// at the ramp's side, which the graduated slab would refuse to step up) and TRAVERSE (aim at the far
// end, which walks the slab).
//
// The phase is decided by HEIGHT PROGRESS along the ramp, not by distance to the near end. Distance
// is the obvious choice and it is wrong in the one place it matters: partway up, the body is FAR
// from the foot again, so a proximity rule re-selects the foot and marches it back down — the very
// flip-flop this function exists to remove. Once the feet have left the near end's height the body
// is committed to the run, however far along it is.
inline Vec3 portalRouteGoal(const StoryPortal& p, Vec3 from, bool climbing) {
    constexpr f32 kMount  = 2.0f;   // metres from the near end that count as "on the approach"
    constexpr f32 kOnRamp = 0.5f;   // metres of height gained/lost that count as "already on it"
    const Vec3 nearEnd = climbing ? p.lowPos  : p.highPos;
    const Vec3 farEnd  = climbing ? p.highPos : p.lowPos;
    // How far the feet have travelled vertically away from the near end (positive once on the slab).
    const f32 progressed = climbing ? (from.y - p.lowPos.y) : (p.highPos.y - from.y);
    if (progressed > kOnRamp) return farEnd;                      // mid-run: never turn back
    const f32 dx = nearEnd.x - from.x, dz = nearEnd.z - from.z;
    return (dx * dx + dz * dz > kMount * kMount) ? nearEnd : farEnd;
}

// True when a body's FEET are on the story that `targetY` belongs to. Deliberately a height
// comparison and NOT onUpperStory: mid-ramp the per-cell slab test reports the upper story while the
// body is still metres below it (see the note above). The two stories of a VERTICAL_HALL are 3 m
// apart, so half that separates them cleanly whatever the ramp gradient is doing underfoot.
inline bool feetOnStory(f32 feetY, f32 targetY) {
    constexpr f32 kHalfStory = 1.5f;
    const f32 d = feetY - targetY;
    return (d < 0.0f ? -d : d) < kHalfStory;
}

// The XZ goal for a body that wants to get UP but has no ramp: the nearest recorded JUMP PAD.
// Four-story Descent floors have portalCount==0 — no ramps, no stairs — so a pad is the only way up
// and without this an enemy simply cannot follow a player who dropped a level. Returns `from`
// unchanged when the floor has no pads, which makes it inert on every style that has none.
//
// Nearest in XZ only, and deliberately so: the pads worth walking to are the ones on your own
// level, and a pad column is a pad on every story it carries, so the horizontal choice is the
// meaningful one. The launch itself is physics (entityMoveAndSlide), not navigation — arriving is
// all the AI has to do.
inline Vec3 nearestPadGoal(const DungeonResult& d, Vec3 from) {
    if (d.jumpPadCount == 0) return from;
    Vec3 best = from;
    f32  bestD2 = 1e30f;
    for (u8 i = 0; i < d.jumpPadCount; i++) {
        const f32 dx = d.jumpPads[i].x - from.x, dz = d.jumpPads[i].z - from.z;
        const f32 d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = d.jumpPads[i]; }
    }
    return { best.x, from.y, best.z };   // XZ goal; the pad does the vertical part
}

// True when `targetFeetY` is at least a story above `myFeetY`. Stories are 3 m apart, so 1.5 m is
// comfortably clear of stairs, ledges and raised room floors while catching a real level change.
inline bool targetIsAbove(f32 myFeetY, f32 targetFeetY) {
    return targetFeetY - myFeetY > 1.5f;
}

// --- Gap vaulting -------------------------------------------------------------------------------
// How far ahead (in cells) a vault probe looks for a landing. 3 cells: a 1-cell jump gap and a
// 2-cell hole both land within it, a wider lake correctly reads as un-vaultable.
static constexpr u32 VAULT_MAX_CELLS = 3;

// The forward impulse a vaulting enemy gets, and the speed floor held while airborne over a gap.
// 6 m/s x 0.40 s airtime = 2.4 m of reach against the ~1.6 m a 1-cell gap plus a body needs — the
// median enemy walks at 3.5 m/s (1.4 m reach), so without the lunge most of the roster would leap in
// and fall short. If this is ever lowered below ~5 m/s the landing check will still pass while the
// arc fails: keep it and the airborne floor in entityMoveAndSlide in sync (they are the same number).
static constexpr f32 VAULT_SPEED = 6.0f;

struct VaultPlan {
    bool viable;    // a gap starts one cell ahead AND a same-height landing exists within range
    bool gapAhead;  // a gap starts one cell ahead at all (viable or not) — the "do not walk in" bit
    Vec3 landing;   // centre of the landing cell (only meaningful when viable)
};

// Probe for a vaultable GAP along dirXZ: cell 1 must be a drop (its story-selected floor more than a
// step below the feet), and some cell within VAULT_MAX_CELLS must come back up to feet height. Pure
// and grid-only, so it is unit-tested; the launch itself lives in entityMoveAndSlide. Uses
// effectiveFloorHeight — the same story selector collision uses — so it is correct on every stacked
// style and inert on flat floors, where cell 1 is never a drop and the probe exits on one lookup.
inline VaultPlan planVault(const LevelGrid& g, Vec3 from, f32 feetY, Vec3 dirXZ) {
    VaultPlan plan{false, false, from};
    const f32 len = sqrtf(dirXZ.x * dirXZ.x + dirXZ.z * dirXZ.z);
    if (len < 1e-4f) return plan;                       // no heading — nothing to probe
    const f32 nx = dirXZ.x / len, nz = dirXZ.z / len;
    const f32 cs = g.cellSize;

    for (u32 step = 1; step <= VAULT_MAX_CELLS; step++) {
        const Vec3 probe{from.x + nx * cs * step, from.y, from.z + nz * cs * step};
        u32 gx, gz;
        if (!LevelGridSystem::worldToGrid(g, probe, gx, gz)) return plan;   // off the grid
        if (LevelGridSystem::isSolid(g, gx, gz))             return plan;   // a wall, not a gap
        const f32 h = LevelGridSystem::effectiveFloorHeight(g, gx, gz, feetY);
        if (step == 1) {
            if (h >= feetY - PLATFORM_STEP_TOLERANCE) return plan;          // no drop — common case
            plan.gapAhead = true;
        } else if (h >= feetY - PLATFORM_STEP_TOLERANCE &&
                   h <= feetY + PLATFORM_STEP_TOLERANCE) {
            // The far side: back at (or within a step of) our height. Land on the cell CENTRE.
            plan.viable  = true;
            plan.landing = { (gx + 0.5f) * cs, h, (gz + 0.5f) * cs };
            return plan;
        }
        // still over the gap — keep looking
    }
    return plan;   // gapAhead but no landing in range: a lake or a real drop — do not jump
}

} // namespace StoryNav
