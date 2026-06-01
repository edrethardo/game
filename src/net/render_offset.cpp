// render_offset.cpp — Smooth prediction-correction layer implementation (M4).
// See render_offset.h for design rationale and math.

#include "net/render_offset.h"
#include <math.h>

void RenderOffsetOps::accumulate(RenderOffset& r, Vec3 delta) {
    // Sum into any existing offset so back-to-back corrections don't reset
    // partial decays — they compound smoothly instead of snapping.
    r.offset = r.offset + delta;
    // R10: clamp magnitude to MAX_OFFSET_M. Direction is preserved so the smoothing
    // still trends correctly toward the sim position; only magnitude is bounded.
    // Without this, repeated small reconcile snaps in high-action combat random-walk
    // the offset past any perceptual ceiling — visible as shaky camera motion even
    // though each individual snap is sub-threshold tiny.
    f32 magSq = r.offset.x * r.offset.x
              + r.offset.y * r.offset.y
              + r.offset.z * r.offset.z;
    const f32 capSq = RenderOffsetOps::MAX_OFFSET_M
                    * RenderOffsetOps::MAX_OFFSET_M;
    if (magSq > capSq) {
        f32 scale = RenderOffsetOps::MAX_OFFSET_M / sqrtf(magSq);
        r.offset = r.offset * scale;
    }
}

void RenderOffsetOps::tick(RenderOffset& r, f32 dt) {
    // No-op on dt=0 so the identity property holds exactly.
    if (dt <= 0.0f) return;
    // Exponential decay: offset shrinks by factor exp(-DECAY_RATE * dt) each tick.
    f32 k = expf(-DECAY_RATE * dt);
    r.offset = r.offset * k;
}

Vec3 RenderOffsetOps::apply(const RenderOffset& r, Vec3 simPos) {
    // Visual position = sim position minus the current offset remainder.
    // When offset is zero (fully decayed) this returns simPos unchanged.
    return simPos - r.offset;
}
