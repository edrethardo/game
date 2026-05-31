// tests/net/test_render_offset.cpp
//
// Unit tests for RenderOffset — the smooth prediction-correction layer (M4).
// Verifies: exponential decay over time, delta accumulation, dt=0 identity,
// and convergence to near-zero over 1 second at 60 Hz.

#include <doctest/doctest.h>
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
    RenderOffset r;
    RenderOffsetOps::accumulate(r, {0.5f, 0.0f, 0.0f});
    RenderOffsetOps::accumulate(r, {0.5f, 0.0f, 0.0f});
    CHECK(r.offset.x == doctest::Approx(1.0f));
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
