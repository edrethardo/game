#pragma once
// pending_hit_ring.h — client-side ring buffer of unacknowledged predicted hits.
//
// When the CLIENT predicts a melee or hitscan hit (Phase 2 paths in engine_combat.cpp),
// it records a (clientTick, targetEntitySlot) tuple here. M10's SV_DAMAGE_DONE handler
// will call ack() to confirm or clear mismatches. Until M10 lands, entries accumulate
// (expireOlderThan bounds growth at ~1 Hz once M10 calls it).
//
// Design: fixed-capacity linear array, compacted on removal (no holes). Capacity 64
// covers ~1 s of 60 Hz fire with no acks — comfortably more than any realistic RTT.

#include "core/types.h"

static constexpr u32 PENDING_HIT_RING_CAPACITY = 64;

struct PendingHit {
    u32 clientTick = 0;     // the client tick on which the predicted hit fired
    u16 targetIdx  = 0;     // entity slot index (or player slot when isPlayer=1)
    u8  isPlayer   = 0;     // 0 = entity, 1 = remote player target
    u8  acked      = 0;     // 1 if server confirmed — marked for cleanup by M10
};

struct PendingHitRing {
    PendingHit entries[PENDING_HIT_RING_CAPACITY] = {};
    u32        count = 0;
};

namespace PendingHitRingOps {
    // Clear all entries and reset count to zero.
    void reset(PendingHitRing& r);

    // Append a new predicted-hit entry. If the ring is full, the oldest entry is
    // evicted (the client can't ack what it never recorded; graceful degradation).
    void record(PendingHitRing& r, u32 clientTick, u16 targetIdx, u8 isPlayer);

    // Find the first entry matching (clientTick, targetIdx), remove it (compacting
    // the array), and return true. Returns false if no match — the hit was either
    // never predicted or already acked/expired.
    // M10 calls this on SV_DAMAGE_DONE arrival.
    bool ack(PendingHitRing& r, u32 clientTick, u16 targetIdx);

    // Remove all entries with clientTick < cutoffClientTick (compact in-place).
    // Call at ~1 Hz once a server stops sending acks to bound memory growth.
    // M10 drives this; kept here so the ring is self-contained.
    void expireOlderThan(PendingHitRing& r, u32 cutoffClientTick);
}
