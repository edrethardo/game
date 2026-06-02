// Tests for the network-metrics windowing helpers (net/net_metrics.h). These convert raw
// per-window byte/packet/snapshot counters into the per-second figures the F9 net-graph and
// the 1 Hz [NET-GRAPH] log show, used to verify the M12 bandwidth target. Pure math — no
// ENet/GL — so they pin the conversions, ratios, and guards exactly.

#include "doctest/doctest.h"
#include "net/net_metrics.h"

TEST_CASE("NetMetrics bytesToKBPerSec") {
    CHECK(NetMetricsOps::bytesToKBPerSec(1024, 1.0f) == doctest::Approx(1.0f));
    CHECK(NetMetricsOps::bytesToKBPerSec(1024, 0.5f) == doctest::Approx(2.0f));
    CHECK(NetMetricsOps::bytesToKBPerSec(0,    1.0f) == doctest::Approx(0.0f));
    CHECK(NetMetricsOps::bytesToKBPerSec(1024, 0.0f) == doctest::Approx(0.0f)); // first-window guard
    CHECK(NetMetricsOps::bytesToKBPerSec(1024, -1.0f) == doctest::Approx(0.0f)); // degenerate guard
}

TEST_CASE("NetMetrics countToHz") {
    CHECK(NetMetricsOps::countToHz(60, 1.0f) == doctest::Approx(60.0f));
    CHECK(NetMetricsOps::countToHz(30, 0.5f) == doctest::Approx(60.0f));
    CHECK(NetMetricsOps::countToHz(60, 0.0f) == doctest::Approx(0.0f)); // guard
}

TEST_CASE("NetMetrics deltaRatio") {
    CHECK(NetMetricsOps::deltaRatio(59, 1) == doctest::Approx(59.0f / 60.0f));
    CHECK(NetMetricsOps::deltaRatio(0,  0) == doctest::Approx(0.0f)); // no snapshots -> 0, no div0
    CHECK(NetMetricsOps::deltaRatio(10, 0) == doctest::Approx(1.0f)); // all deltas
}

TEST_CASE("NetMetrics baselineAgeTicks wrap/future guard") {
    CHECK(NetMetricsOps::baselineAgeTicks(100, 97)  == 3u);
    CHECK(NetMetricsOps::baselineAgeTicks(100, 100) == 0u);
    CHECK(NetMetricsOps::baselineAgeTicks(97,  100) == 0u); // ack ahead of current -> 0
}

TEST_CASE("NetMetrics wireBytesEstimate") {
    CHECK(NetMetricsOps::wireBytesEstimate(1000, 10, 36) == 1360u);
    CHECK(NetMetricsOps::wireBytesEstimate(0,    0,  36) == 0u);
    CHECK(NetMetricsOps::wireBytesEstimate(500,  1,  EST_PACKET_OVERHEAD_BYTES) == 536u);
}

TEST_CASE("NetMetrics compute integration") {
    NetCounters c;
    // Server view, slot 2: 60 unreliable snapshot packets of 1024 B each on ch1 = 61440 B;
    // 59 deltas + 1 full.
    c.bytesOut[2][1] = 61440; c.pktsOut[2][1] = 60;
    c.snapsOut[2] = 60; c.deltaSnaps[2] = 59; c.fullSnaps[2] = 1;
    // Inbound (client-style) ch1: 30720 B over 30 packets; nothing on ch0.
    c.bytesIn[1] = 30720; c.pktsIn[1] = 30; c.snapsIn = 30;

    NetMetrics m = NetMetricsOps::compute(c, /*slot=*/2, /*elapsedSec=*/1.0f,
                                          /*packetLoss=*/0.05f, /*baselineAge=*/4);

    CHECK(m.kbOutPerSec[1] == doctest::Approx(60.0f));   // 61440 / 1024
    CHECK(m.kbOutPerSec[0] == doctest::Approx(0.0f));
    CHECK(m.kbOutTotal     == doctest::Approx(60.0f));
    CHECK(m.kbInPerSec[1]  == doctest::Approx(30.0f));   // 30720 / 1024
    CHECK(m.kbInTotal      == doctest::Approx(30.0f));
    CHECK(m.snapsOutPerSec == doctest::Approx(60.0f));
    CHECK(m.snapsInPerSec  == doctest::Approx(30.0f));
    CHECK(m.deltaFullRatio == doctest::Approx(59.0f / 60.0f));
    CHECK(m.packetLoss     == doctest::Approx(0.05f));
    CHECK(m.baselineAge    == 4u);
    // Wire estimate adds 36 B/pkt: out (61440 + 36*60)=63600 B /1024; in (30720 + 36*30)=31800 /1024.
    CHECK(m.wireKbOut == doctest::Approx((61440.0f + 36.0f * 60.0f) / 1024.0f));
    CHECK(m.wireKbIn  == doctest::Approx((30720.0f + 36.0f * 30.0f) / 1024.0f));
}

TEST_CASE("NetMetrics compute out-of-range slot is inert") {
    NetCounters c;
    c.bytesIn[1] = 1024; c.pktsIn[1] = 1; c.snapsIn = 1;
    // slot == NET_METRICS_SLOTS is out of range: OUT side reads as 0, IN side still works.
    NetMetrics m = NetMetricsOps::compute(c, NET_METRICS_SLOTS, 1.0f, 0.0f, 0);
    CHECK(m.kbOutTotal    == doctest::Approx(0.0f));
    CHECK(m.kbInTotal     == doctest::Approx(1.0f));
    CHECK(m.snapsInPerSec == doctest::Approx(1.0f));
}
