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

TEST_CASE("ClockSync: bootstrap uses the median of the handshake pongs (rejects an outlier)") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    ClockSyncOps::onPongReceived(cs, 100, 200, 0.130);   // RTT 30  → OWT 15  → median(15)        = 15
    CHECK(cs.oneWayTripMs == doctest::Approx(15.0f));
    ClockSyncOps::onPongReceived(cs, 200, 210, 0.250);   // RTT 50  → OWT 25  → median(15,25)      = 20
    CHECK(cs.oneWayTripMs == doctest::Approx(20.0f));
    ClockSyncOps::onPongReceived(cs, 300, 220, 0.540);   // RTT 240 → OWT 120 → median(15,25,120)  = 25
    // The 120 ms outlier is REJECTED — the session clock keeps the median 25, not an EMA that
    // would have been dragged toward 120.
    CHECK(cs.oneWayTripMs == doctest::Approx(25.0f));
    CHECK(cs.pongsReceived == 3);
}

TEST_CASE("ClockSync: medianOwt handles 1/2/3 samples") {
    f32 one[1]   = {30.0f};
    f32 two[2]   = {30.0f, 40.0f};
    f32 three[3] = {30.0f, 31.0f, 120.0f};   // 120 is the outlier
    CHECK(ClockSyncOps::medianOwt(one,   1) == doctest::Approx(30.0f));
    CHECK(ClockSyncOps::medianOwt(two,   2) == doctest::Approx(35.0f));  // average of the two
    CHECK(ClockSyncOps::medianOwt(three, 3) == doctest::Approx(31.0f));  // middle — rejects 120
    // Order-independence of the n=3 median.
    f32 shuffled[3] = {120.0f, 30.0f, 31.0f};
    CHECK(ClockSyncOps::medianOwt(shuffled, 3) == doctest::Approx(31.0f));
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

// --- SV_TIME_PONG byte-buffer decode tests ---
// These exercise the wire-decode side: a SV_TIME_PONG payload as bytes goes through
// Client::handleTimePong, which unpacks the fields and feeds them to ClockSyncOps.

#include "net/client.h"   // for Client::handleTimePong

TEST_CASE("Client::handleTimePong decodes payload and seeds ClockSync") {
    // Build a 12-byte SV_TIME_PONG payload by hand.
    u8 payload[12];
    // clientTimeMs = 100 (little-endian u32 — matches PacketReader::readU32)
    payload[0] = 100; payload[1] = 0; payload[2] = 0; payload[3] = 0;
    // serverTick = 200
    payload[4] = 200; payload[5] = 0; payload[6] = 0; payload[7] = 0;
    // serverTimeMs = 9999 (ignored by handleTimePong but must be in payload to read)
    payload[8] = 0x0F; payload[9] = 0x27; payload[10] = 0; payload[11] = 0;

    ClockSync cs;
    ClockSyncOps::reset(cs);
    // Pretend we received this payload at pongRecvNowSec=0.130 — same numbers as
    // the "first pong bootstraps" test above, exercising the dispatch + decode path.
    Client::handleTimePong(payload, sizeof(payload), cs, /*pongRecvNowSec=*/0.130);
    CHECK(cs.bootstrapped == true);
    CHECK(cs.pongsReceived == 1);
    CHECK(cs.oneWayTripMs == doctest::Approx(15.0f));
    CHECK(cs.serverTickEst == doctest::Approx(200.9));
}

TEST_CASE("Client::handleTimePong ignores too-short payloads") {
    u8 payload[4] = { 100, 0, 0, 0 };   // 4 bytes — way too small
    ClockSync cs;
    ClockSyncOps::reset(cs);
    Client::handleTimePong(payload, sizeof(payload), cs, 0.130);
    CHECK(cs.bootstrapped == false);
    CHECK(cs.pongsReceived == 0);
}
