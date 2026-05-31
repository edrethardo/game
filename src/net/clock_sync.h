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
// Threading: single-threaded — the net layer drives this from the same callback
// that delivers packets, on the main thread. No locking.
struct ClockSync {
    bool   bootstrapped       = false;
    f64    serverTickEst      = 0.0;
    f64    lastUpdateClockSec = 0.0;
    f32    oneWayTripMs       = 30.0f;
    u32    pongsReceived      = 0;
    u32    snapshotsApplied   = 0;

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
}
