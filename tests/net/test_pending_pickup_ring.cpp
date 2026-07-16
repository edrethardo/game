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

TEST_CASE("PendingPickupRing: lane roundtrip (online couch co-op)") {
    // A rejected pickup must roll back the LANE that predicted it. SV_PICKUP_RESULT arrives
    // during Net::poll — outside the per-lane swap loop — so the handler cannot use
    // m_localPlayerIndex; the lane has to ride the ring entry itself.
    PendingPickupRing r;
    PendingPickupRingOps::reset(r);
    PendingPickupRingOps::record(r, 100, 43, /*predictedSlot=*/5);   // default lane = 0
    PendingPickupRingOps::record(r, 101, 42, /*predictedSlot=*/3, /*lane=*/1);
    CHECK(PendingPickupRingOps::findLaneByUid(r, 42) == 1);
    CHECK(PendingPickupRingOps::findLaneByUid(r, 43) == 0);
    CHECK(PendingPickupRingOps::findLaneByUid(r, 999) == 0);         // absent uid → safe lane 0
    // The lane must survive ack()'s in-place compaction when an EARLIER entry is removed.
    CHECK(PendingPickupRingOps::ack(r, 43) == true);
    CHECK(PendingPickupRingOps::findLaneByUid(r, 42) == 1);
}
