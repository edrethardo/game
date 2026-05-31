// tests/net/test_pending_damage_ring.cpp — TDD tests for PendingDamageRing (M7.1).
// Exercises: empty init, record, ack (match + compact), ack miss, expireOlderThan.

#include <doctest/doctest.h>
#include "net/pending_damage_ring.h"

TEST_CASE("PendingDamageRing: empty has zero pending") {
    PendingDamageRing r;
    PendingDamageRingOps::reset(r);
    CHECK(r.count == 0);
}

TEST_CASE("PendingDamageRing: record adds entry") {
    PendingDamageRing r;
    PendingDamageRingOps::reset(r);
    PendingDamageRingOps::record(r, 100, 42);  // clientTick=100, projectileSrcKey=42
    CHECK(r.count == 1);
    CHECK(r.entries[0].clientTick == 100);
    CHECK(r.entries[0].projectileSrcKey == 42);
}

TEST_CASE("PendingDamageRing: ack matches and clears") {
    PendingDamageRing r;
    PendingDamageRingOps::reset(r);
    PendingDamageRingOps::record(r, 100, 42);
    PendingDamageRingOps::record(r, 101, 43);
    bool acked = PendingDamageRingOps::ack(r, 42);
    CHECK(acked == true);
    CHECK(r.count == 1);
    CHECK(r.entries[0].clientTick == 101);
}

TEST_CASE("PendingDamageRing: ack misses return false") {
    PendingDamageRing r;
    PendingDamageRingOps::reset(r);
    PendingDamageRingOps::record(r, 100, 42);
    CHECK(PendingDamageRingOps::ack(r, 999) == false);
    CHECK(r.count == 1);
}

TEST_CASE("PendingDamageRing: expireOlderThan removes stale entries") {
    PendingDamageRing r;
    PendingDamageRingOps::reset(r);
    PendingDamageRingOps::record(r, 50, 1);
    PendingDamageRingOps::record(r, 100, 2);
    PendingDamageRingOps::record(r, 150, 3);
    PendingDamageRingOps::expireOlderThan(r, 100);
    CHECK(r.count == 2);
}
