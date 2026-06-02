#pragma once

#include "core/types.h"

// net_metrics.h — network bandwidth/health metrics aggregation (pure, header-only).
//
// The F9 net-graph overlay and the 1 Hz [NET-GRAPH] log read these to report real
// bytes-on-wire, snapshot rate, delta/full ratio, and baseline age so the M12 bandwidth
// target (<=25 KB/s per client at 60 Hz delta-compressed) can be verified with the M14
// fake-loss harness. Raw counters live as Net:: statics (net.cpp), incremented on the
// single net thread (Net::poll inbound, sendImmediate_* outbound) — so plain u32, no
// atomics. Once per second they are folded into NetMetrics by compute() and zeroed.
//
// Header-only + dependency-free (only core/types.h) so the windowing math unit-tests
// without the heavy net.cpp/ENet translation unit — same pattern as interp_delay.h.

// These mirror MAX_PLAYERS / NUM_CHANNELS in net.h. Kept local (we deliberately do NOT
// #include "net/net.h") so there's no include cycle — net.h includes THIS header for the
// accessor signatures. A static_assert in net.cpp pins them to the real values, so any
// drift is a compile error rather than a silent mismatch.
static constexpr u32 NET_METRICS_SLOTS    = 4;   // == MAX_PLAYERS
static constexpr u32 NET_METRICS_CHANNELS = 2;   // [0]=reliable (events), [1]=unreliable (snapshots+input)

// Per-packet wire-overhead estimate: UDP(8) + IPv4(20) + ~ENet command framing(8). The
// counters track PAYLOAD bytes only (matching the doc's ~24 KB/s framing, net.h:11); the
// estimate column adds this per packet so the overlay can also show a closer-to-real wire
// figure. It is an estimate — ENet's reliable retransmits under loss are not visible here.
static constexpr u32 EST_PACKET_OVERHEAD_BYTES = 36;

// Raw tallies accumulated over one 1 s window, then zeroed. The out-side is per-slot because
// a broadcast fans out to every peer — the M12 read-off needs each client's stream, not the
// aggregate. Inbound is a single stream per role (one server peer on the client; on the
// server the per-peer split isn't needed for the read-off).
struct NetCounters {
    u32 bytesOut[NET_METRICS_SLOTS][NET_METRICS_CHANNELS] = {};
    u32 pktsOut [NET_METRICS_SLOTS][NET_METRICS_CHANNELS] = {};
    u32 snapsOut [NET_METRICS_SLOTS] = {};  // server: snapshot sends to this slot
    u32 fullSnaps[NET_METRICS_SLOTS] = {};  // server: full-snapshot sends to this slot
    u32 deltaSnaps[NET_METRICS_SLOTS] = {}; // server: delta-snapshot sends to this slot
    u32 bytesIn[NET_METRICS_CHANNELS] = {};
    u32 pktsIn [NET_METRICS_CHANNELS] = {};
    u32 snapsIn = 0;                        // client: SV_SNAPSHOT packets received
};

// Computed read-side metrics for one slot/role over the last window.
struct NetMetrics {
    f32 kbInPerSec[NET_METRICS_CHANNELS]  = {};
    f32 kbOutPerSec[NET_METRICS_CHANNELS] = {};
    f32 kbInTotal  = 0.0f;
    f32 kbOutTotal = 0.0f;
    f32 wireKbIn   = 0.0f;   // payload + EST_PACKET_OVERHEAD_BYTES per packet
    f32 wireKbOut  = 0.0f;
    f32 snapsInPerSec  = 0.0f;
    f32 snapsOutPerSec = 0.0f;
    f32 deltaFullRatio = 0.0f; // delta / (delta+full), 0..1
    f32 packetLoss     = 0.0f; // 0..1, sourced from the ENet peer (passthrough, not a counter)
    u32 baselineAge    = 0;    // ticks the delta baseline is stale (passthrough)
};

namespace NetMetricsOps {
    // bytes over elapsedSec -> KB/s. elapsedSec <= 0 returns 0 (first-window / degenerate guard).
    inline f32 bytesToKBPerSec(u32 bytes, f32 elapsedSec) {
        if (elapsedSec <= 0.0f) return 0.0f;
        return (static_cast<f32>(bytes) / 1024.0f) / elapsedSec;
    }
    // count over elapsedSec -> Hz. Same zero guard.
    inline f32 countToHz(u32 count, f32 elapsedSec) {
        if (elapsedSec <= 0.0f) return 0.0f;
        return static_cast<f32>(count) / elapsedSec;
    }
    // delta / (delta+full); 0 when no snapshots were sent this window.
    inline f32 deltaRatio(u32 deltaSnaps, u32 fullSnaps) {
        u32 total = deltaSnaps + fullSnaps;
        return (total == 0) ? 0.0f : static_cast<f32>(deltaSnaps) / static_cast<f32>(total);
    }
    // Baseline age in ticks. Operands are already-reconstructed full u32 ticks (never raw u16
    // wire values), so a plain subtraction is wrap-safe; the future-guard returns 0 when the
    // ack appears ahead of the current tick (clock skew / not-yet-acked).
    inline u32 baselineAgeTicks(u32 currentTick, u32 ackedFullTick) {
        return (currentTick >= ackedFullTick) ? (currentTick - ackedFullTick) : 0;
    }
    // Estimated wire bytes = payload + a fixed per-packet header estimate.
    inline u32 wireBytesEstimate(u32 payloadBytes, u32 pktCount, u32 overheadPerPkt) {
        return payloadBytes + overheadPerPkt * pktCount;
    }

    // Fold one slot's raw counters into per-second metrics. packetLoss (0..1) and baselineAge
    // are sourced outside (ENet peer / tick math) and passed through. Inbound fields are
    // role-global (the slot arg selects only the OUT side); snapsInPerSec uses c.snapsIn.
    inline NetMetrics compute(const NetCounters& c, u8 slot, f32 elapsedSec,
                              f32 packetLoss, u32 baselineAge) {
        NetMetrics m;
        u32 totalOutBytes = 0, totalOutPkts = 0;
        u32 totalInBytes  = 0, totalInPkts  = 0;
        for (u32 ch = 0; ch < NET_METRICS_CHANNELS; ch++) {
            u32 outB = (slot < NET_METRICS_SLOTS) ? c.bytesOut[slot][ch] : 0;
            u32 outP = (slot < NET_METRICS_SLOTS) ? c.pktsOut[slot][ch]  : 0;
            m.kbOutPerSec[ch] = bytesToKBPerSec(outB, elapsedSec);
            m.kbInPerSec[ch]  = bytesToKBPerSec(c.bytesIn[ch], elapsedSec);
            totalOutBytes += outB;            totalOutPkts += outP;
            totalInBytes  += c.bytesIn[ch];   totalInPkts  += c.pktsIn[ch];
        }
        m.kbOutTotal = bytesToKBPerSec(totalOutBytes, elapsedSec);
        m.kbInTotal  = bytesToKBPerSec(totalInBytes,  elapsedSec);
        m.wireKbOut  = bytesToKBPerSec(
            wireBytesEstimate(totalOutBytes, totalOutPkts, EST_PACKET_OVERHEAD_BYTES), elapsedSec);
        m.wireKbIn   = bytesToKBPerSec(
            wireBytesEstimate(totalInBytes,  totalInPkts,  EST_PACKET_OVERHEAD_BYTES), elapsedSec);
        u32 snapsOut = (slot < NET_METRICS_SLOTS) ? c.snapsOut[slot] : 0;
        m.snapsOutPerSec = countToHz(snapsOut, elapsedSec);
        m.snapsInPerSec  = countToHz(c.snapsIn, elapsedSec);
        u32 dsnap = (slot < NET_METRICS_SLOTS) ? c.deltaSnaps[slot] : 0;
        u32 fsnap = (slot < NET_METRICS_SLOTS) ? c.fullSnaps[slot]  : 0;
        m.deltaFullRatio = deltaRatio(dsnap, fsnap);
        m.packetLoss  = packetLoss;
        m.baselineAge = baselineAge;
        return m;
    }
}
