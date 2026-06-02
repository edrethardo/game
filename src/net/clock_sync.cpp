// clock_sync.cpp — client-side estimate of server simulation tick + wall time.
// See clock_sync.h for the contract; this file is the math. Pinned to 60 Hz tick
// (TICK_DT = 1/60 s) because the project's simulation runs at that rate — if the
// tick rate ever changes, surface it as a parameter rather than hardcoding.
//
// All four operations (reset, onPongReceived, onSnapshotReceived, currentServerTickEst)
// are O(1). No allocation, no logging in hot paths — caller can log diagnostics at
// will from outside.

#include "net/clock_sync.h"
#include "core/types.h"

// Tick rate is shared with the rest of the engine (NET_TICK_RATE in net.h is 60).
// Duplicating the constant here keeps clock_sync.cpp independent of net.h's broader
// surface — only the type aliases are needed.
static constexpr f32 TICKS_PER_SEC = 60.0f;

void ClockSyncOps::reset(ClockSync& cs) {
    cs.bootstrapped       = false;
    cs.serverTickEst      = 0.0;
    cs.lastUpdateClockSec = 0.0;
    cs.oneWayTripMs       = 30.0f;
    cs.pongsReceived      = 0;
    cs.snapshotsApplied   = 0;
    cs.owtSampleCount     = 0;
}

// Median of up to 3 OWT samples (pure). Insertion-sorts a local copy — n is tiny.
f32 ClockSyncOps::medianOwt(const f32* samples, u8 n) {
    if (n == 0) return 0.0f;
    if (n > 3) n = 3;
    f32 s[3] = {0.0f, 0.0f, 0.0f};
    for (u8 i = 0; i < n; i++) s[i] = samples[i];
    for (u8 i = 1; i < n; i++) {
        f32 v = s[i];
        s32 j = static_cast<s32>(i) - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
    }
    if (n & 1u) return s[n / 2];                       // odd: middle element (n=3 rejects outlier)
    return 0.5f * (s[n / 2 - 1] + s[n / 2]);           // even: average the two middle (n=2)
}

void ClockSyncOps::onPongReceived(ClockSync& cs,
                                  u32 clientSentMs,
                                  u32 serverTickAtRecv,
                                  f64 pongRecvNowSec) {
    const u32 nowMs = static_cast<u32>(pongRecvNowSec * 1000.0);
    const u32 rttMs = nowMs - clientSentMs;
    const f32 newOwt = static_cast<f32>(rttMs) * 0.5f;

    // Collect the handshake samples and use their median (robust to one bad RTT). Once the
    // 3-sample bootstrap window is full, any late pong falls back to EMA smoothing.
    if (cs.owtSampleCount < 3) {
        cs.owtSamples[cs.owtSampleCount++] = newOwt;
        cs.oneWayTripMs = medianOwt(cs.owtSamples, cs.owtSampleCount);
        cs.bootstrapped = true;
    } else {
        // EMA smoothing: blend new OWT measurement toward current estimate (gain 0.2).
        cs.oneWayTripMs += ClockSync::OWT_GAIN * (newOwt - cs.oneWayTripMs);
    }

    // Project the server's tick forward by the one-way trip time to get the
    // server tick at the moment the pong arrived on this side.
    const f64 serverTickAtPongArrival =
        static_cast<f64>(serverTickAtRecv) + (cs.oneWayTripMs / 1000.0) * TICKS_PER_SEC;

    cs.serverTickEst      = serverTickAtPongArrival;
    cs.lastUpdateClockSec = pongRecvNowSec;
    cs.pongsReceived++;
}

void ClockSyncOps::onSnapshotReceived(ClockSync& cs, u32 snapServerTick, f64 recvNowSec) {
    if (!cs.bootstrapped) {
        // No pong data yet — bootstrap directly from the snapshot's server tick,
        // projecting forward by the default OWT (30 ms).
        cs.serverTickEst      = static_cast<f64>(snapServerTick) +
                                (cs.oneWayTripMs / 1000.0) * TICKS_PER_SEC;
        cs.lastUpdateClockSec = recvNowSec;
        cs.bootstrapped       = true;
        cs.snapshotsApplied++;
        return;
    }

    // Predict what the server tick should be right now based on our stored estimate
    // and elapsed wall time since the last update.
    const f64 elapsedSinceUpdate = recvNowSec - cs.lastUpdateClockSec;
    const f64 predictedNow       = cs.serverTickEst + elapsedSinceUpdate * TICKS_PER_SEC;

    // Observed server tick at arrival: snap's wire tick + OWT projection.
    const f64 observed = static_cast<f64>(snapServerTick) +
                         (cs.oneWayTripMs / 1000.0) * TICKS_PER_SEC;

    const f64 delta = observed - predictedNow;
    if (delta > ClockSync::LARGE_DELTA || delta < -ClockSync::LARGE_DELTA) {
        // Big jump — phase change (host restarted floor → m_serverTick=0, network
        // hiccup, etc.). Snap rather than smooth so we don't lag the estimate for
        // seconds waiting for the P controller to catch up.
        cs.serverTickEst = observed;
    } else {
        // P controller: nudge the running estimate toward the observed value (gain 0.1).
        cs.serverTickEst = predictedNow + ClockSync::SNAP_GAIN * delta;
    }
    cs.lastUpdateClockSec = recvNowSec;
    cs.snapshotsApplied++;
}

f64 ClockSyncOps::currentServerTickEst(const ClockSync& cs, f64 nowSec) {
    if (!cs.bootstrapped) return 0.0;
    // Project the stored estimate forward by how much wall time has elapsed since
    // the last pong or snapshot update.
    const f64 elapsed = nowSec - cs.lastUpdateClockSec;
    return cs.serverTickEst + elapsed * TICKS_PER_SEC;
}

u32 ClockSyncOps::currentServerTickEstU32(const ClockSync& cs, f64 nowSec) {
    const f64 est = currentServerTickEst(cs, nowSec);
    if (est <= 0.0) return 0;
    return static_cast<u32>(est);
}
