#pragma once
// crowd_control.h — pure CC math shared by the choke helper (Combat::applyCCToPlayer) and the
// unit tests. No engine deps (the arena.h / stash.h pattern). Holds the tenacity duration
// scale, the 60% resist cap, and the PvP stun diminishing-returns ladder so PvP can't perma-lock.
#include "core/types.h"

namespace CrowdControl {

constexpr f32 RESIST_CAP   = 0.60f;   // max CC Resistance from all gear combined
constexpr f32 DR_WINDOW    = 8.0f;    // seconds; repeated stuns inside this window diminish

// Tenacity: a CC's duration after % resistance. resist is assumed already capped (capResist).
inline f32 scaleDuration(f32 duration, f32 resist) { return duration * (1.0f - resist); }

// Clamp a summed gear resistance to the hard cap. Negative guarded to 0.
inline f32 capResist(f32 resist) {
    if (resist < 0.0f)        return 0.0f;
    if (resist > RESIST_CAP)  return RESIST_CAP;
    return resist;
}

// Per-victim stun diminishing-returns state (PvP victims only). Lives on Player/NetPlayer.
struct StunDr {
    f32 timer = 0.0f;   // seconds remaining in the current DR window (0 = window closed)
    u8  count = 0;      // stuns landed in the window so far
};

// Decay the DR window; when it lapses, the count resets so the next stun is full again.
inline void tickStunDr(StunDr& dr, f32 dt) {
    if (dr.timer > 0.0f) {
        dr.timer -= dt;
        if (dr.timer <= 0.0f) { dr.timer = 0.0f; dr.count = 0; }
    }
}

// Returns the duration multiplier for the NEXT stun AND advances the ladder as a side effect:
// count 0->1 returns 1.0, 1->2 returns 0.5, 2->3 returns 0.25, 3->4 returns 0.0 (immune).
// Refreshes the window to `window` seconds on every stun.
inline f32 advanceStunDr(StunDr& dr, f32 window) {
    f32 mult;
    switch (dr.count) {
        case 0:  mult = 1.0f;  break;
        case 1:  mult = 0.5f;  break;
        case 2:  mult = 0.25f; break;
        default: mult = 0.0f;  break;   // 4th+ stun in the window: immune
    }
    if (dr.count < 255) dr.count++;
    dr.timer = window;
    return mult;
}

} // namespace CrowdControl
