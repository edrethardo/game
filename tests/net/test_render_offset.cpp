// tests/net/test_render_offset.cpp
//
// Unit tests for RenderOffset — the smooth prediction-correction layer (M4).
// Verifies: exponential decay over time, delta accumulation, dt=0 identity,
// and convergence to near-zero over 1 second at 60 Hz.

#include <doctest/doctest.h>
#include <math.h>   // sqrtf — used by the R10 cap-magnitude checks
#include "net/render_offset.h"

TEST_CASE("RenderOffset: decay reaches near zero over expected timeframe") {
    RenderOffset r;
    r.offset = {1.0f, 0.0f, 0.0f};
    // Decay rate tuned so a 1 m correction is ~10% remaining after 150 ms.
    RenderOffsetOps::tick(r, 0.150f);  // simulate one 150 ms tick (one-shot)
    CHECK(r.offset.x < 0.2f);
    CHECK(r.offset.x > 0.0f);  // not fully zero
}

TEST_CASE("RenderOffset: accumulating two corrections sums into offset") {
    // R10: values stay under MAX_OFFSET_M (0.15 m) so the cap doesn't intervene. The
    // "two sub-cap corrections compose linearly" property is what this case guards;
    // cap-clamping is exercised by the two dedicated cases below. (Deltas were 0.1+0.1
    // pre-dampener; 0.2 now exceeds the tightened cap, so use 0.05+0.05 to stay sub-cap
    // and keep testing the linear-composition property rather than the clamp.)
    RenderOffset r;
    RenderOffsetOps::accumulate(r, {0.05f, 0.0f, 0.0f});
    RenderOffsetOps::accumulate(r, {0.05f, 0.0f, 0.0f});
    CHECK(r.offset.x == doctest::Approx(0.1f));
}

TEST_CASE("RenderOffset: accumulate clamps magnitude at MAX_OFFSET_M") {
    // R10: a single large delta saturates the cap. Direction (positive x) preserved.
    RenderOffset r;
    RenderOffsetOps::accumulate(r, {5.0f, 0.0f, 0.0f});
    f32 mag = sqrtf(r.offset.x*r.offset.x + r.offset.y*r.offset.y + r.offset.z*r.offset.z);
    CHECK(mag == doctest::Approx(RenderOffsetOps::MAX_OFFSET_M).epsilon(0.001f));
    CHECK(r.offset.x > 0.0f);
    CHECK(r.offset.y == doctest::Approx(0.0f));
    CHECK(r.offset.z == doctest::Approx(0.0f));
}

TEST_CASE("RenderOffset: repeated small accumulates plateau at cap, never exceed") {
    // R10: 1 s of per-tick 13 cm corrections in the same direction — worst case
    // (dodge-roll, server doesn't move, every frame snaps ~13 cm back). Pre-cap,
    // offset would steady-state at ~65 cm. Capped, it plateaus at MAX_OFFSET_M.
    RenderOffset r;
    for (u32 i = 0; i < 60; i++) {
        RenderOffsetOps::accumulate(r, {0.13f, 0.0f, 0.0f});
        RenderOffsetOps::tick(r, 1.0f / 60.0f);
    }
    f32 mag = sqrtf(r.offset.x*r.offset.x + r.offset.y*r.offset.y + r.offset.z*r.offset.z);
    CHECK(mag <= RenderOffsetOps::MAX_OFFSET_M + 0.001f);
    CHECK(r.offset.x > 0.0f);
    CHECK(r.offset.y == doctest::Approx(0.0f));
    CHECK(r.offset.z == doctest::Approx(0.0f));
}

TEST_CASE("RenderOffset: apply ADDS the offset so the camera holds its pre-snap position") {
    // Regression guard for the shaky-client-FOV sign bug. At a reconcile snap the sim jumps
    // to the server position and the offset stores (pre-snap camera pos − server pos), fed as
    // `m_localPlayer.position - serverPos`. apply(offset, sim=serverPos) must return the
    // pre-snap camera position = sim + offset, so the eye does NOT teleport and then decays
    // onto sim. The old `sim - offset` returned 2·sim − camera (mirror to the far side,
    // doubling the error) — that inverted sign was the shake. Verify apply adds.
    RenderOffset r;
    r.offset = {0.2f, 0.0f, -0.1f};        // camera was +0.2 x / −0.1 z of the snapped sim
    Vec3 sim = {5.0f, 1.0f, -3.0f};        // sim after the snap = server position
    Vec3 got = RenderOffsetOps::apply(r, sim);
    CHECK(got.x == doctest::Approx(5.2f)); // sim + offset — holds the pre-snap camera pos
    CHECK(got.y == doctest::Approx(1.0f));
    CHECK(got.z == doctest::Approx(-3.1f));
    // Fully-decayed offset ⇒ apply is the identity on sim (no residual shift).
    RenderOffset zero;
    Vec3 same = RenderOffsetOps::apply(zero, sim);
    CHECK(same.x == doctest::Approx(5.0f));
    CHECK(same.y == doctest::Approx(1.0f));
    CHECK(same.z == doctest::Approx(-3.0f));
}

TEST_CASE("RenderOffset: tick with dt=0 is identity") {
    RenderOffset r;
    r.offset = {1.0f, 2.0f, 3.0f};
    RenderOffsetOps::tick(r, 0.0f);
    CHECK(r.offset.x == doctest::Approx(1.0f));
    CHECK(r.offset.y == doctest::Approx(2.0f));
    CHECK(r.offset.z == doctest::Approx(3.0f));
}

TEST_CASE("RenderOffset: many small ticks converge to near zero") {
    RenderOffset r;
    r.offset = {2.0f, 0.0f, 0.0f};
    for (u32 i = 0; i < 60; i++) RenderOffsetOps::tick(r, 1.0f / 60.0f); // 1 second of 60 Hz
    CHECK(r.offset.x < 0.01f);  // <1 cm residual after 1 s
}
