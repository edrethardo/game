#pragma once

#include "core/types.h"

// Client-side estimate of the server's current simulation tick + wall-clock time.
// Foundation for the netplay rewrite (M1) — the prediction + replay reconciliation
// work in M3 needs to know *when* a snapshot represents, not just *what it carries*.
//
// Bootstrapped at connect by 3 CL_TIME_PING ↔ SV_TIME_PONG round-trips; refined every
// time a WorldSnapshot arrives by feeding `snap.serverTick` through a P controller
// (gain ~0.1) so transient jitter doesn't phase-jump the estimate.
//
// All times are doubles internally because we accumulate over a session.
//
// STATUS — DIAGNOSTIC-ONLY (as of the 2026-07 audit): the estimate feeds only the net-graph /
// logs; `currentServerTickEst*` has NO functional caller (prediction/reconcile derive timing from
// acked snapshot ticks, not from here). Corollary: `oneWayTripMs` is FROZEN after the 3-pong
// bootstrap — there is no periodic re-ping, so on a link whose latency drifts the OWT goes stale
// and never recovers. That is harmless WHILE this is diagnostic. Before wiring ClockSync into any
// real logic (e.g. driving a predicted server-tick), FIRST add periodic pings to keep OWT live —
// otherwise a stale OWT silently becomes a functional bug.
//
// Threading: single-threaded — the net layer drives this from the same callback
// that delivers packets, on the main thread. No locking.
struct ClockSync {
    bool   bootstrapped       = false;
    f64    serverTickEst      = 0.0;
    f64    lastUpdateClockSec = 0.0;
    f32    oneWayTripMs       = 30.0f;
    u32    pongsReceived      = 0;
    u32    snapshotsApplied   = 0;

    // Bootstrap OWT outlier rejection. Only ~3 handshake pongs are ever sent (RTT is
    // frozen after — runtime refinement is snapshot-driven), so a single bad handshake
    // sample would otherwise skew the entire session's clock. Collect the samples and use
    // their MEDIAN instead of seeding from the first and EMA-ing.
    f32    owtSamples[3]      = {0.0f, 0.0f, 0.0f};
    u8     owtSampleCount     = 0;

    static constexpr f32 SNAP_GAIN   = 0.1f;
    static constexpr f32 OWT_GAIN    = 0.2f;
    static constexpr f64 LARGE_DELTA = 6.0;
};

namespace ClockSyncOps {
    void reset(ClockSync& cs);

    void onPongReceived(ClockSync& cs,
                        u32 clientSentMs,
                        u32 serverTickAtRecv,
                        f64 pongRecvNowSec);

    void onSnapshotReceived(ClockSync& cs, u32 snapServerTick, f64 recvNowSec);

    f64 currentServerTickEst(const ClockSync& cs, f64 nowSec);
    u32 currentServerTickEstU32(const ClockSync& cs, f64 nowSec);

    // Median of up to 3 one-way-trip samples (pure; used for robust bootstrap). Odd count
    // returns the middle element (rejects one outlier); even count averages the two middle.
    f32 medianOwt(const f32* samples, u8 n);
}
