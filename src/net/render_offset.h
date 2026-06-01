#pragma once
// render_offset.h — Smooth prediction-correction layer (M4).
//
// When prediction reconcile detects a divergence (server disagrees with our simulated
// position), we don't want a visible teleport snap. Instead we store the delta between
// where we THOUGHT we were and where the server says we are in a RenderOffset, and decay
// it each frame. The sim position (m_localPlayer.position) is immediately corrected to
// the server's authoritative value; only the RENDERED camera/eye position is offset by
// the decaying remainder so the player smoothly slides toward the corrected sim position.
//
// Math: offset decays multiplicatively each tick: offset *= exp(-DECAY_RATE * dt).
// DECAY_RATE = 13.0 produces ~14% remaining after 150 ms, giving a fast-but-smooth feel.

#include "core/types.h"
#include "core/math.h"

struct RenderOffset {
    Vec3 offset = {0, 0, 0};
};

namespace RenderOffsetOps {
    // Tunable: how quickly the offset decays toward zero.
    // exp(-DECAY_RATE * dt) per tick. DECAY_RATE = ~13 produces a "1 m correction
    // visually mostly closed in ~150 ms" feel (10% remaining at t = 0.150 s).
    static constexpr f32 DECAY_RATE = 13.0f;

    // R10: cap the accumulated offset magnitude to keep the visible correction layer
    // from running away under bursts of small reconcile snaps in high-action combat.
    // Solo walking accumulates ~0 offset; a sustained per-tick ~13 cm delta (e.g.
    // dodge-roll, where movement is client-predicted-only) would otherwise drive
    // steady-state offset to ~65 cm of visible camera lag, perceived as the "movement
    // gets shaky" symptom when those corrections fire in varying directions. Cap
    // clamps the magnitude (direction preserved) so the rendered camera never appears
    // more than this far behind the authoritative sim position.
    static constexpr f32 MAX_OFFSET_M = 0.35f;

    // Accumulate a new correction delta into the offset. If a prior correction is still
    // decaying, the two sum together so compounding errors don't snap more than necessary.
    void accumulate(RenderOffset& r, Vec3 delta);

    // Decay the offset for one timestep (call once per CLIENT frame before rendering).
    // No-op when dt <= 0 so the identity case is exact.
    void tick(RenderOffset& r, f32 dt);

    // Return the visually-smoothed position: simPos minus the current offset.
    // The caller renders from this rather than the raw sim position.
    Vec3 apply(const RenderOffset& r, Vec3 simPos);
}
