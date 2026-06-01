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
    // R10: values stay under MAX_OFFSET_M=0.35 m so the cap doesn't intervene. The
    // "two sub-cap corrections compose linearly" property is what this case guards;
    // cap-clamping is exercised by the two dedicated cases below.
    RenderOffset r;
    RenderOffsetOps::accumulate(r, {0.1f, 0.0f, 0.0f});
    RenderOffsetOps::accumulate(r, {0.1f, 0.0f, 0.0f});
    CHECK(r.offset.x == doctest::Approx(0.2f));
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
