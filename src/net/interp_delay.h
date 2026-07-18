#pragma once

#include "core/types.h"

// Adaptive interpolation-delay helpers (consumed by client.cpp). Pure + header-only so they
// can be unit-tested without the heavy Client translation unit (SDL/GL/etc.).
//
// Remote players/entities/projectiles render at a delayed "render time" = now - delay, so the
// snapshot ring acts as a jitter buffer. With a FIXED delay, a single late snapshot pushes the
// render time past the newest sample → freeze/extrapolation/stutter. Widening the delay with
// observed snapshot-arrival jitter rides spikes out; it must shrink back SLOWLY when the link
// calms, because shrinking fast jumps the render clock forward and skips a snapshot (a visible
// stutter — the opposite of what we want).

// RFC 3550-style smoothed jitter: EMA (gain 1/16) of the absolute deviation of the latest
// inter-arrival interval from the nominal send interval. All times in seconds.
inline f32 updateArrivalJitter(f32 prevJitter, f32 deltaSec, f32 nominalSec) {
    // OUTAGE GUARD: a burst outage (a gap several times the send interval) is a LOSS event, not
    // jitter — feeding it raw would inflate the smoothed jitter (and thus the interp delay) for
    // hundreds of ms after the link already recovered, making every wifi hiccup permanently tax
    // remote-render latency. Clamp the sample at 3x nominal: real jitter passes untouched
    // (spikes < 2 intervals), anything larger contributes at most a bounded nudge.
    //
    // How the constants tie together (why the delay never reaches the 250 ms ceiling): with the
    // 3x clamp the largest deviation any sample carries is (3-1)=2x nominal, so the EMA-smoothed
    // jitter SATURATES at 2x nominal. computeInterpDelay's target is base + JITTER_K*jitter, so the
    // jitter-driven delay tops out at base + K*2*nominal ≈ 33 + 2.5*2*16.7 ≈ 116 ms. That is the
    // realistic steady ceiling of THIS estimator; LagComp::MAX_INTERP_DELAY_MS (250) is the separate
    // wire/rewind TRUST ceiling (how far back the server will honor a rewind), NOT a target this
    // estimator is meant to reach. A future high-VARIANCE-link tuner must raise the 3x outage clamp
    // (to let genuine high jitter through), not the 250 cap — raising 250 alone changes nothing here.
    const f32 outageClamp = 3.0f * nominalSec;
    if (deltaSec > outageClamp) deltaSec = outageClamp;
    f32 dev = deltaSec - nominalSec;
    if (dev < 0.0f) dev = -dev;
    return prevJitter + (dev - prevJitter) * (1.0f / 16.0f);
}

// New render delay from the smoothed jitter. Target = base + K*jitter, clamped to
// [base, maxd]; approached asymmetrically — grow fast toward a larger target (cover the
// spike now), shrink slow toward a smaller one (never jump the render clock forward).
inline f32 computeInterpDelay(f32 prevDelay, f32 jitterSec, f32 baseSec, f32 maxSec) {
    constexpr f32 JITTER_K    = 2.5f;   // headroom = 2.5× smoothed jitter
    constexpr f32 GROW_RATE   = 0.5f;   // approach a larger target fast
    constexpr f32 SHRINK_RATE = 0.05f;  // approach a smaller target slowly
    f32 target = baseSec + JITTER_K * jitterSec;
    if (target < baseSec) target = baseSec;
    if (target > maxSec)  target = maxSec;
    const f32 rate = (target > prevDelay) ? GROW_RATE : SHRINK_RATE;
    f32 next = prevDelay + rate * (target - prevDelay);
    if (next < baseSec) next = baseSec;
    if (next > maxSec)  next = maxSec;
    return next;
}
