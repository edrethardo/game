// test_lag_comp_ring.cpp — Unit tests for the standalone lag-comp ring module.
// Verifies push/lookup contract independently of the full Engine object graph.
// These tests lock the ring semantics so engine_combat.cpp and future callers
// (M9 AOE lag-comp) can rely on consistent behaviour.

#include <doctest/doctest.h>
#include "engine/lag_comp_ring.h"

TEST_CASE("LagCompRing: empty ring returns null") {
    LagCompRing r;
    LagCompRingOps::reset(r);
    CHECK(LagCompRingOps::findByTickStamp(r, 100) == nullptr);
    CHECK(LagCompRingOps::atTicksAgo(r, 1) == nullptr);
}

TEST_CASE("LagCompRing: push and lookup by tickStamp") {
    LagCompRing r;
    LagCompRingOps::reset(r);
    LagCompPose p; p.position = {1.0f, 2.0f, 3.0f}; p.tickStamp = 42;
    LagCompRingOps::push(r, p);
    const LagCompPose* found = LagCompRingOps::findByTickStamp(r, 42);
    REQUIRE(found != nullptr);
    CHECK(found->position.x == doctest::Approx(1.0f));
}

TEST_CASE("LagCompRing: atTicksAgo returns newest when ticksAgo=1") {
    LagCompRing r;
    LagCompRingOps::reset(r);
    LagCompPose a; a.tickStamp = 100; a.position = {10.0f,0,0};
    LagCompPose b; b.tickStamp = 101; b.position = {20.0f,0,0};
    LagCompRingOps::push(r, a);
    LagCompRingOps::push(r, b);
    const LagCompPose* newest = LagCompRingOps::atTicksAgo(r, 1);
    REQUIRE(newest != nullptr);
    CHECK(newest->tickStamp == 101);
    CHECK(newest->position.x == doctest::Approx(20.0f));
    const LagCompPose* prior = LagCompRingOps::atTicksAgo(r, 2);
    REQUIRE(prior != nullptr);
    CHECK(prior->tickStamp == 100);
}

TEST_CASE("LagCompRing: oldest evicted past capacity") {
    LagCompRing r;
    LagCompRingOps::reset(r);
    for (u32 t = 1; t <= LAG_COMP_HISTORY_TICKS_MAX + 5; t++) {
        LagCompPose p; p.tickStamp = t;
        LagCompRingOps::push(r, p);
    }
    // tick 1..5 evicted — the ring can only hold LAG_COMP_HISTORY_TICKS_MAX entries.
    // tick 6 should be the oldest still present.
    CHECK(LagCompRingOps::findByTickStamp(r, 1) == nullptr);
    CHECK(LagCompRingOps::findByTickStamp(r, 5) == nullptr);
    REQUIRE(LagCompRingOps::findByTickStamp(r, 6) != nullptr);
    REQUIRE(LagCompRingOps::findByTickStamp(r, LAG_COMP_HISTORY_TICKS_MAX + 5) != nullptr);
}
