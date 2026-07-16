#pragma once
// static_charge.h — Capacitor Mail (STATIC_CHARGE armor passive) stack logic. Pure so the
// call sites (host in tickArmorRingPassives, remotes in serverNetPost) cannot drift and
// tests/game/test_static_charge.cpp can pin it without linking the engine.

#include "core/types.h"

namespace StaticCharge {
    constexpr u8  MAX_STACKS = 5;
    constexpr f32 WINDOW_SEC = 10.0f;   // stacks drop this long after the last hit

    // Advance the window and absorb this tick's "was struck" signal. Returns true exactly when
    // the new hit fills the 5th stack — the caller discharges (chain lightning at the attacker)
    // and the stacks are already reset here.
    inline bool accumulate(u8& stacks, f32& timer, bool struck, f32 dt) {
        if (timer > 0.0f) {
            timer -= dt;
            if (timer <= 0.0f) { timer = 0.0f; stacks = 0; }
        }
        if (!struck) return false;
        stacks++;
        timer = WINDOW_SEC;
        if (stacks >= MAX_STACKS) { stacks = 0; timer = 0.0f; return true; }
        return false;
    }
}
