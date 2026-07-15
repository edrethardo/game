// tests/net/test_prediction_ring.cpp
//
// Unit tests for PredictionRing — the client-side circular buffer that stores
// (input, sim-state) pairs keyed by clientTick for prediction replay (M3).
// Covers: empty find, push+find, oldest-entry eviction, collectInputsAfter ordering.

#include <doctest/doctest.h>
#include "net/prediction_ring.h"

TEST_CASE("PredictionRing: empty ring returns null") {
    PredictionRing r;
    PredictionRingOps::reset(r);
    CHECK(PredictionRingOps::find(r, 100) == nullptr);
}

TEST_CASE("PredictionRing: push + find returns same entry") {
    PredictionRing r;
    PredictionRingOps::reset(r);
    PredictedState s; s.position = {1.0f, 2.0f, 3.0f}; s.health = 90.0f;
    NetInput in{}; in.clientTick = 42;
    PredictionRingOps::push(r, 42, in, s);
    const PredictionEntry* e = PredictionRingOps::find(r, 42);
    REQUIRE(e != nullptr);
    CHECK(e->state.position.x == doctest::Approx(1.0f));
    CHECK(e->state.health == doctest::Approx(90.0f));
}

TEST_CASE("PredictionRing: oldest entries evicted past capacity") {
    PredictionRing r;
    PredictionRingOps::reset(r);
    for (u32 t = 0; t < PREDICTION_RING_CAPACITY + 10; t++) {
        PredictedState s; s.position = {(f32)t, 0, 0};
        NetInput in{}; in.clientTick = t;
        PredictionRingOps::push(r, t, in, s);
    }
    // Oldest 10 should have been overwritten; entry at tick 0 gone.
    CHECK(PredictionRingOps::find(r, 0) == nullptr);
    // Entry at tick PREDICTION_RING_CAPACITY+9 (newest) should be present.
    const PredictionEntry* newest = PredictionRingOps::find(r, PREDICTION_RING_CAPACITY + 9);
    REQUIRE(newest != nullptr);
    CHECK(newest->state.position.x == doctest::Approx((f32)(PREDICTION_RING_CAPACITY + 9)));
}

TEST_CASE("PredictionRing: replayInputsAfter returns inputs in clientTick order") {
    PredictionRing r;
    PredictionRingOps::reset(r);
    for (u32 t = 100; t < 105; t++) {
        PredictedState s; NetInput in{}; in.clientTick = t;
        PredictionRingOps::push(r, t, in, s);
    }
    NetInput out[16] = {};
    u32 n = PredictionRingOps::collectInputsAfter(r, 102, out, 16);
    REQUIRE(n == 2);
    CHECK(out[0].clientTick == 103);
    CHECK(out[1].clientTick == 104);
}

TEST_CASE("PredictionRing: findMut returns a WRITABLE entry that find() then observes") {
    // The replay path rewrites each replayed tick's predicted state so the NEXT ack compares
    // the server against the corrected history. Without that rewrite, a single real mispredict
    // re-fires a "divergence" on every subsequent ack (60/s) until the stale entries age out of
    // the 256-tick ring — 4 seconds of phantom corrections from one event.
    PredictionRing r;
    NetInput in = {};
    PredictedState s;
    s.position = {1.0f, 0.0f, 1.0f};
    PredictionRingOps::push(r, 100, in, s);
    PredictionRingOps::push(r, 101, in, s);

    PredictionEntry* e = PredictionRingOps::findMut(r, 101);
    REQUIRE(e != nullptr);
    e->state.position = {9.0f, 0.0f, 9.0f};              // the corrected replay result

    const PredictionEntry* seen = PredictionRingOps::find(r, 101);
    REQUIRE(seen != nullptr);
    CHECK(seen->state.position.x == doctest::Approx(9.0f));  // correction visible to the next ack
    CHECK(PredictionRingOps::find(r, 100)->state.position.x == doctest::Approx(1.0f)); // neighbour untouched
    CHECK(PredictionRingOps::findMut(r, 999) == nullptr);    // miss behaves like find()
}
