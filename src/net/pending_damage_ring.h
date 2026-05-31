#pragma once
// pending_damage_ring.h — client-side ring buffer of unacknowledged predicted damage events.
//
// When the CLIENT broadphase-detects an incoming enemy projectile that is about to hit
// the local player (Phase 2.3 extension in engine_update.cpp tickSharedSystems), it records
// a (clientTick, projectileSrcKey) tuple here and immediately fires visual feedback
// (damageFlashTimer + hurtVignette). M10's SV_DAMAGE_TO_ME reliable event will call ack()
// to confirm or clear mismatches. Until M10 lands, entries accumulate; expireOlderThan
// bounds growth at ~1 Hz once M10 calls it.
//
// Key encoding: (ownerSlot << 24) | (proj.clientTick & 0xFFFFFF) — unique enough for the
// short prediction window while fitting in a u32. HP is NOT modified by M7; only visuals.
//
// Design: fixed-capacity linear array, compacted on removal (no holes). Capacity 32
// covers ~0.5 s of predicted hits at 60 Hz with no acks.

#include "core/types.h"

static constexpr u32 PENDING_DAMAGE_RING_CAPACITY = 32;

struct PendingDamage {
    u32 clientTick       = 0;   // when the predicted hit fired locally
    u32 projectileSrcKey = 0;   // identifier for the inbound projectile (slot index
                                // or clientTickLow — whatever uniquely keys it)
};

struct PendingDamageRing {
    PendingDamage entries[PENDING_DAMAGE_RING_CAPACITY] = {};
    u32           count = 0;
};

namespace PendingDamageRingOps {
    // Clear all entries and reset count to zero.
    void reset(PendingDamageRing& r);

    // Append a new predicted-damage entry. If the ring is full, the oldest entry is
    // evicted (graceful degradation — the client can't ack what it never recorded).
    void record(PendingDamageRing& r, u32 clientTick, u32 projectileSrcKey);

    // Find the first entry matching projectileSrcKey, remove it (compacting the array),
    // and return true. Returns false if no match.
    // M10 calls this on SV_DAMAGE_TO_ME arrival.
    bool ack(PendingDamageRing& r, u32 projectileSrcKey);

    // Remove all entries with clientTick < cutoffClientTick (compact in-place).
    // Call at ~1 Hz once a server stops sending acks to bound memory growth.
    // M10 drives this; kept here so the ring is self-contained.
    void expireOlderThan(PendingDamageRing& r, u32 cutoffClientTick);
}
