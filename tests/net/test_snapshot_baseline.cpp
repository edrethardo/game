#include <doctest/doctest.h>
#include "net/snapshot_baseline.h"

TEST_CASE("BaselineTracker: empty baseline returns 0 as baselineTick") {
    BaselineTracker t;
    BaselineTrackerOps::reset(t);
    CHECK(BaselineTrackerOps::shouldSendFullSnapshot(t, /*clientAckedTick=*/100) == true);
    CHECK(t.baselineTick == 0);
}

TEST_CASE("BaselineTracker: storing a baseline updates baselineTick") {
    BaselineTracker t;
    BaselineTrackerOps::reset(t);
    BaselineTrackerOps::store(t, /*serverTick=*/42);
    CHECK(t.baselineTick == 42);
}

TEST_CASE("BaselineTracker: ackedTick == baselineTick → no full snapshot needed") {
    BaselineTracker t;
    BaselineTrackerOps::reset(t);
    BaselineTrackerOps::store(t, /*serverTick=*/100);
    CHECK(BaselineTrackerOps::shouldSendFullSnapshot(t, /*clientAckedTick=*/100) == false);
}

TEST_CASE("BaselineTracker: stale ackedTick (older than baseline) → send full") {
    BaselineTracker t;
    BaselineTrackerOps::reset(t);
    BaselineTrackerOps::store(t, 100);
    // Client says it has applied tick 50 — older than our baseline of 100.
    CHECK(BaselineTrackerOps::shouldSendFullSnapshot(t, 50) == true);
}

TEST_CASE("BaselineTracker: ackedTick newer than baseline (shouldn't happen) → send full") {
    BaselineTracker t;
    BaselineTrackerOps::reset(t);
    BaselineTrackerOps::store(t, 100);
    CHECK(BaselineTrackerOps::shouldSendFullSnapshot(t, 200) == true);
}
