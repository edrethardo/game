#include <doctest/doctest.h>
#include "net/pending_pickup_ring.h"

TEST_CASE("PendingPickupRing: empty") {
    PendingPickupRing r;
    PendingPickupRingOps::reset(r);
    CHECK(r.count == 0);
    CHECK(PendingPickupRingOps::isPending(r, 42) == false);
}

TEST_CASE("PendingPickupRing: record and isPending") {
    PendingPickupRing r;
    PendingPickupRingOps::reset(r);
    PendingPickupRingOps::record(r, 100, 42);
    CHECK(r.count == 1);
    CHECK(PendingPickupRingOps::isPending(r, 42) == true);
    CHECK(PendingPickupRingOps::isPending(r, 999) == false);
}

TEST_CASE("PendingPickupRing: ack removes entry") {
    PendingPickupRing r;
    PendingPickupRingOps::reset(r);
    PendingPickupRingOps::record(r, 100, 42);
    PendingPickupRingOps::record(r, 101, 43);
    CHECK(PendingPickupRingOps::ack(r, 42) == true);
    CHECK(r.count == 1);
    CHECK(PendingPickupRingOps::isPending(r, 42) == false);
    CHECK(PendingPickupRingOps::isPending(r, 43) == true);
}

TEST_CASE("PendingPickupRing: expireOlderThan") {
    PendingPickupRing r;
    PendingPickupRingOps::reset(r);
    PendingPickupRingOps::record(r, 50, 1);
    PendingPickupRingOps::record(r, 150, 2);
    PendingPickupRingOps::expireOlderThan(r, 100);
    CHECK(r.count == 1);
    CHECK(PendingPickupRingOps::isPending(r, 1) == false);
    CHECK(PendingPickupRingOps::isPending(r, 2) == true);
}
