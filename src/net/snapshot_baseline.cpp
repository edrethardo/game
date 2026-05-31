// snapshot_baseline.cpp — per-client delta-compression baseline tracker.
//
// Tracks which snapshot tick each remote client has last applied. The server
// consults this before each snapshot broadcast to decide whether a full snapshot
// is required (no baseline yet, or ACK doesn't match the stored tick) or whether
// a delta vs. the stored baseline could be sent (future per-slot hint encoding).
//
// V1 ships the infrastructure only: shouldSendFullSnapshot drives log output and
// stores the baseline, but the snapshot encoder always sends full for now.
//
// Fits into the architecture as a pure-data helper (no engine, no SDL, no GL) so
// it can be included in both the engine and the unit-test binary without drag.

#include "net/snapshot_baseline.h"

void BaselineTrackerOps::reset(BaselineTracker& t) {
    t.baselineTick = 0;
}

void BaselineTrackerOps::store(BaselineTracker& t, u32 serverTick) {
    t.baselineTick = serverTick;
}

bool BaselineTrackerOps::shouldSendFullSnapshot(const BaselineTracker& t, u32 clientAckedTick) {
    if (t.baselineTick == 0) return true;   // no baseline yet — must send full
    return clientAckedTick != t.baselineTick;
}
