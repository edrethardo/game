// test_clock_sync.cpp — TDD spec for the ClockSync state machine. Each TEST_CASE
// describes one behavior the type must exhibit; tests are written before the
// implementation. Pure math + state — no network, no engine globals.

#include <doctest/doctest.h>
#include "net/clock_sync.h"

TEST_CASE("ClockSync: reset returns to pristine state") {
    ClockSync cs;
    cs.bootstrapped = true;
    cs.serverTickEst = 12345.0;
    cs.oneWayTripMs = 99.0f;
    cs.pongsReceived = 7;
    ClockSyncOps::reset(cs);
    CHECK(cs.bootstrapped == false);
    CHECK(cs.serverTickEst == 0.0);
    CHECK(cs.oneWayTripMs == doctest::Approx(30.0f));
    CHECK(cs.pongsReceived == 0);
    CHECK(cs.snapshotsApplied == 0);
}

TEST_CASE("ClockSync: first pong bootstraps the estimate") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    // We sent the ping at 100 ms (clientSentMs=100), server stamped its tick at
    // recv = 200, the pong arrived back at 130 ms (pongRecvNowSec=0.130). RTT is
    // 30 ms, so oneWayTripMs is 15 ms.
    ClockSyncOps::onPongReceived(cs, /*clientSentMs=*/100,
                                     /*serverTickAtRecv=*/200,
                                     /*pongRecvNowSec=*/0.130);
    CHECK(cs.bootstrapped == true);
    CHECK(cs.pongsReceived == 1);
    CHECK(cs.oneWayTripMs == doctest::Approx(15.0f));
    // serverTickAtPongArrival = 200 + (15/1000)*60 = 200.9 ticks.
    CHECK(cs.serverTickEst == doctest::Approx(200.9));
    CHECK(cs.lastUpdateClockSec == doctest::Approx(0.130));
}

TEST_CASE("ClockSync: subsequent pongs EMA-smooth oneWayTripMs") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    ClockSyncOps::onPongReceived(cs, 100, 200, 0.130);   // RTT 30 → OWT 15
    ClockSyncOps::onPongReceived(cs, 200, 210, 0.250);   // sent@200ms, recv@250ms, RTT 50 → OWT 25
    // After EMA(OWT_GAIN=0.2): OWT = 15 + 0.2*(25 - 15) = 17.0
    CHECK(cs.oneWayTripMs == doctest::Approx(17.0f));
    CHECK(cs.pongsReceived == 2);
}

TEST_CASE("ClockSync: currentServerTickEst projects forward by wall time") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    ClockSyncOps::onPongReceived(cs, 100, 600, 0.130);
    // Right at recv time: estimate equals serverTickEst (≈600.9 — recomputed for OWT 15).
    CHECK(ClockSyncOps::currentServerTickEst(cs, 0.130) == doctest::Approx(600.9));
    // 100 ms later (0.230 s): 0.1 s * 60 = 6 ticks ahead → 606.9.
    CHECK(ClockSyncOps::currentServerTickEst(cs, 0.230) == doctest::Approx(606.9));
    // 1 second later: +60 ticks → 660.9.
    CHECK(ClockSyncOps::currentServerTickEst(cs, 1.130) == doctest::Approx(660.9));
}

TEST_CASE("ClockSync: currentServerTickEst returns 0 before bootstrap") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    CHECK(ClockSyncOps::currentServerTickEst(cs, 0.0) == 0.0);
    CHECK(ClockSyncOps::currentServerTickEstU32(cs, 99.0) == 0u);
}

TEST_CASE("ClockSync: currentServerTickEstU32 floors fractional ticks") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    ClockSyncOps::onPongReceived(cs, 100, 500, 0.130);
    // est ≈ 500.9 at recv time → U32 floor is 500.
    CHECK(ClockSyncOps::currentServerTickEstU32(cs, 0.130) == 500u);
}

TEST_CASE("ClockSync: onSnapshotReceived seeds when not bootstrapped") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    // Snapshot arrives before any pong — should bootstrap from snapshot's serverTick.
    // OWT is still the default 30 ms, so estimate becomes 1000 + (30/1000)*60 = 1001.8.
    ClockSyncOps::onSnapshotReceived(cs, /*snapServerTick=*/1000, /*recvNowSec=*/0.500);
    CHECK(cs.bootstrapped == true);
    CHECK(cs.serverTickEst == doctest::Approx(1001.8));
    CHECK(cs.snapshotsApplied == 1);
}

TEST_CASE("ClockSync: onSnapshotReceived smooths small drift via P controller") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    ClockSyncOps::onPongReceived(cs, 100, 1000, 0.130);  // serverTickEst ≈ 1000.9 at t=0.130
    // 100 ms later (recvNowSec=0.230), a snapshot arrives claiming serverTick=1007.
    // Predicted now: 1000.9 + 0.1*60 = 1006.9. Observed: 1007 + 0.015*60 = 1007.9. Delta: 1.0.
    // After P controller (gain 0.1): est = 1006.9 + 0.1 * 1.0 = 1007.0.
    ClockSyncOps::onSnapshotReceived(cs, /*snapServerTick=*/1007, /*recvNowSec=*/0.230);
    CHECK(cs.serverTickEst == doctest::Approx(1007.0));
    CHECK(cs.snapshotsApplied == 1);
}

TEST_CASE("ClockSync: onSnapshotReceived snaps on large delta (host floor reset)") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    ClockSyncOps::onPongReceived(cs, 100, 5000, 0.130);   // serverTickEst ≈ 5000.9
    // 50 ms later, host has just descended and reset m_serverTick = 0. Next snapshot
    // carries snapServerTick = 3 (a few ticks past reset). Delta vs predicted is
    // huge → should SNAP rather than smooth.
    ClockSyncOps::onSnapshotReceived(cs, /*snapServerTick=*/3, /*recvNowSec=*/0.180);
    // After snap: estimate = observed = 3 + (15/1000)*60 = 3.9
    CHECK(cs.serverTickEst == doctest::Approx(3.9));
}
