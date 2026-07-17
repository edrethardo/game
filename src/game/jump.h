#pragma once

#include "core/types.h"

// game/jump.h — pure jump-assist logic (coyote time + jump buffering), no engine/grid deps so it is
// unit-testable in isolation (same pattern as crowd_control.h / arena.h / stash.h). The physics —
// the launch impulse and gravity — lives with the collider in world/collision.h (JUMP_SPEED /
// GRAVITY); this header only decides WHEN a jump fires, given the one-tick jump-press edge and the
// current ground state.
//
// Two forgiveness mechanics, both ~0.1 s, that turn "I pressed jump and nothing happened" into a
// jump:
//   * Coyote time — you can still jump for a short grace window AFTER walking off a ledge, so a
//                   fractionally-late press at an edge isn't silently eaten.
//   * Jump buffer — a press made just BEFORE landing is remembered and fires on touchdown, so a
//                   fractionally-early press isn't silently eaten either.
//
// The state (two decaying timers) is per-player and lives on BOTH Player (client/host local
// prediction, evolved by PlayerController::update) and NetPlayer (server-authoritative remote sim,
// evolved by updateNetPlayerFromInput), mirrored through syncLocalPlayerToNetPlayer /
// syncNetPlayerToLocalPlayer so a co-op guest predicts its own ledge jumps and the server agrees —
// no rubber-band. It is deliberately NOT on the wire (no SnapPlayer field, no PROTOCOL bump): the
// jump RESULT (velocity.y) is already replicated as core transform state, and each side derives the
// timers deterministically from the same input stream. Because a jump is an IMPULSE (once applied
// it lives in velocity, which is authoritative and snapshotted) rather than a continuous buff, a
// rare timer disagreement at a reconcile boundary self-corrects within the window instead of
// persisting — unlike the speed-buff class of bug that MUST ride NetPlayer + the snapshot.
namespace JumpAssist {

// Grace windows, seconds. 0.1 s ≈ 6 ticks at 60 Hz — the genre-standard feel: forgiving enough to
// erase edge-of-ledge misses, short enough to never read as a deliberate mid-air jump.
constexpr f32 COYOTE_TIME = 0.1f;
constexpr f32 BUFFER_TIME = 0.1f;

struct JumpState {
    f32 coyoteTimer = 0.0f;  // >0 = still within the post-ledge grace window
    f32 bufferTimer = 0.0f;  // >0 = a recent jump press is still "remembered"
};

// Advance the grace timers one tick and decide whether the jump impulse fires THIS tick.
//   jumpEdge : the one-tick jump PRESS (isActionPressed / INPUT_JUMP), not the held state.
//   onGround : standing on the ground this tick (as seen at the start of the movement step).
// Returns true when a (possibly buffered) press meets (possibly coyote) ground — the caller then
// sets velocity.y = JUMP_SPEED. On a fire, BOTH timers are consumed so a single press can never
// yield two jumps: no coyote-fed air double-jump, no buffered re-fire on the next grounded tick.
inline bool resolve(JumpState& st, bool jumpEdge, bool onGround, f32 dt) {
    // Coyote: refill to full while grounded, bleed off in the air.
    if (onGround) {
        st.coyoteTimer = COYOTE_TIME;
    } else if (st.coyoteTimer > 0.0f) {
        st.coyoteTimer -= dt;
        if (st.coyoteTimer < 0.0f) st.coyoteTimer = 0.0f;
    }
    // Buffer: (re)arm on a fresh press, bleed off otherwise.
    if (jumpEdge) {
        st.bufferTimer = BUFFER_TIME;
    } else if (st.bufferTimer > 0.0f) {
        st.bufferTimer -= dt;
        if (st.bufferTimer < 0.0f) st.bufferTimer = 0.0f;
    }

    const bool wantJump = st.bufferTimer > 0.0f;             // a press within the last BUFFER_TIME
    const bool canJump  = onGround || st.coyoteTimer > 0.0f; // grounded, or within coyote grace
    if (wantJump && canJump) {
        st.coyoteTimer = 0.0f;   // consume both → one press yields exactly one jump
        st.bufferTimer = 0.0f;
        return true;
    }
    return false;
}

} // namespace JumpAssist
