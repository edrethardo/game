#pragma once

#include "core/types.h"

// interact.h — the tap/hold rule for the interact button (E / gamepad X), as PURE functions.
//
// Three things compete for one button: picking up loot, activating a shrine, and taking the floor
// exit. Loot is what the hand reaches for a hundred times a floor, so an item ALWAYS wins a tap — a
// shrine or an exit you happen to be standing on must never eat the grab. Holding is the deliberate
// override for the two things you do once.
//
// This lives apart from Engine because the two rules below are exactly the kind of logic that breaks
// silently: a wrong priority row does not crash, it just quietly steals a player's item, and a wrong
// edge in the hold machine either fires twice or never fires. Both are pinned by
// tests/game/test_interact.cpp.

namespace Interact {

// What the button produced this tick.
enum struct Intent : u8 { NONE, TAP, HOLD };

// What that intent should act on, given what is in reach.
enum struct Target : u8 { NONE, ITEM, SHRINE, EXIT };

// Per-player button state. Lives in the caller; poll() owns it.
struct HoldState {
    f32  held     = 0.0f;    // seconds the button has been down
    bool consumed = false;   // this press already produced an intent; ignore until release
};

// Tap-vs-hold arbitration.
//
// A tap CANNOT fire on the press edge whenever a hold is possible: if it did, press-and-hold would
// already have taken the item on frame 1 and "hold for the shrine" could never work. So when there
// IS something to disambiguate the tap fires on RELEASE. When there is not — the overwhelmingly
// common case, plain loot on the floor — it still fires instantly on the press edge, so ordinary
// looting keeps its snap and pays nothing for a feature it isn't using.
inline Intent poll(HoldState& st, bool down, bool hasHoldTarget, f32 dt, f32 holdSec) {
    if (!down) {
        const bool fired = st.consumed;
        const f32  held  = st.held;
        st.held = 0.0f;
        st.consumed = false;
        // Released before the threshold, and nothing fired on the way down → it was a tap.
        if (!fired && held > 0.0f && held < holdSec) return Intent::TAP;
        return Intent::NONE;
    }

    const bool pressEdge = (st.held == 0.0f);
    st.held += dt;
    if (st.consumed) return Intent::NONE;   // already acted on this press; wait for release

    if (!hasHoldTarget) {
        // Nothing to disambiguate — act on the press edge, exactly as a plain pickup always did.
        if (pressEdge) { st.consumed = true; return Intent::TAP; }
        return Intent::NONE;
    }
    if (st.held >= holdSec) {
        st.consumed = true;
        return Intent::HOLD;   // fires ONCE; consumed blocks a repeat while the button stays down
    }
    return Intent::NONE;
}

// The priority table. An item outranks a shrine outranks the exit on a TAP; a HOLD reaches past the
// item to the shrine, and past the shrine to the exit only when there is no shrine.
inline Target choose(Intent intent, bool hasItem, bool hasShrine, bool hasExit) {
    if (intent == Intent::HOLD) {
        if (hasShrine) return Target::SHRINE;
        if (hasExit)   return Target::EXIT;
        return Target::NONE;   // holding with only an item in reach does nothing — the tap took it
    }
    if (intent == Intent::TAP) {
        if (hasItem)   return Target::ITEM;
        if (hasShrine) return Target::SHRINE;   // no loot competing → a tap is enough
        if (hasExit)   return Target::EXIT;
    }
    return Target::NONE;
}

} // namespace Interact
