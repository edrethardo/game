// clock_sync.cpp — TDD red phase: stubs that compile but do nothing useful. The
// actual math lands in Task 2 to make the test_clock_sync.cpp expectations pass.

#include "net/clock_sync.h"
#include "core/types.h"

void ClockSyncOps::reset(ClockSync& /*cs*/) {
    // Intentional no-op — tests will fail until Task 2 lands the real reset.
}

void ClockSyncOps::onPongReceived(ClockSync& /*cs*/,
                                  u32 /*clientSentMs*/,
                                  u32 /*serverTickAtRecv*/,
                                  f64 /*pongRecvNowSec*/) {
    // Intentional no-op — Task 2 implements.
}

void ClockSyncOps::onSnapshotReceived(ClockSync& /*cs*/,
                                      u32 /*snapServerTick*/,
                                      f64 /*recvNowSec*/) {
    // Intentional no-op — Task 2 implements.
}

f64 ClockSyncOps::currentServerTickEst(const ClockSync& /*cs*/, f64 /*nowSec*/) {
    return 0.0;  // wrong — Task 2 implements.
}

u32 ClockSyncOps::currentServerTickEstU32(const ClockSync& /*cs*/, f64 /*nowSec*/) {
    return 0u;  // wrong — Task 2 implements.
}
