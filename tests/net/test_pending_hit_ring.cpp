// tests/net/test_pending_hit_ring.cpp — TDD tests for PendingHitRing (M6.1).
// Exercises: empty init, record, ack (match + compact), ack miss, expireOlderThan.

#include <doctest/doctest.h>
#include "net/pending_hit_ring.h"

TEST_CASE("PendingHitRing: empty ring has zero pending") {
    PendingHitRing r;
    PendingHitRingOps::reset(r);
    CHECK(r.count == 0);
}

TEST_CASE("PendingHitRing: record adds an entry") {
    PendingHitRing r;
    PendingHitRingOps::reset(r);
    PendingHitRingOps::record(r, 100, 5, 0);  // clientTick=100, entityIdx=5, isPlayer=0
    CHECK(r.count == 1);
    CHECK(r.entries[0].clientTick == 100);
    CHECK(r.entries[0].targetIdx == 5);
}

TEST_CASE("PendingHitRing: ack matches and clears entry") {
    PendingHitRing r;
    PendingHitRingOps::reset(r);
    PendingHitRingOps::record(r, 100, 5, 0);
    PendingHitRingOps::record(r, 101, 7, 0);
    bool acked = PendingHitRingOps::ack(r, 100, 5);
    CHECK(acked == true);
    // entry 0 removed, entry 1 compacted to slot 0
    CHECK(r.count == 1);
    CHECK(r.entries[0].clientTick == 101);
}

TEST_CASE("PendingHitRing: ack returns false on miss") {
    PendingHitRing r;
    PendingHitRingOps::reset(r);
    PendingHitRingOps::record(r, 100, 5, 0);
    CHECK(PendingHitRingOps::ack(r, 999, 5) == false);
    CHECK(r.count == 1);
}

TEST_CASE("PendingHitRing: expireOlderThan removes stale entries") {
    PendingHitRing r;
    PendingHitRingOps::reset(r);
    PendingHitRingOps::record(r, 50,  1, 0);
    PendingHitRingOps::record(r, 100, 2, 0);
    PendingHitRingOps::record(r, 150, 3, 0);
    PendingHitRingOps::expireOlderThan(r, 100);
    CHECK(r.count == 2);  // tick 50 removed; 100 and 150 remain
}
