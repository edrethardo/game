#pragma once
// pending_pickup_ring.h — client-side ring buffer of unacknowledged predicted pickup requests.
//
// When the CLIENT sends CL_PICKUP_ITEM (engine_update.cpp::sendPickupRequest), it records a
// (clientTick, itemUid) entry here and immediately deactivates the world item locally so it
// disappears without waiting ~RTT/2 for the server's next snapshot mirror. If the server
// rejects the pickup, the next snapshot still contains the item and mirrorWorldItems brings it
// back — the ring entry is just a bound on how long we wait before giving up.
//
// Design mirrors PendingDamageRing: fixed-capacity linear array, compacted on removal (no
// holes). Capacity 16 is sufficient because pickups are user-initiated at human pace.
//
// NOTE: ONLY the inventory add stays snapshot-driven in v1. The ring predicts the world-item
// DISAPPEARANCE only, not the bag slot appearing.

#include "core/types.h"

static constexpr u32 PENDING_PICKUP_RING_CAPACITY = 16;

struct PendingPickup {
    u32 clientTick = 0;   // local client tick at which the CL_PICKUP_ITEM was sent
    u32 itemUid    = 0;   // uid of the world item being picked up
};

struct PendingPickupRing {
    PendingPickup entries[PENDING_PICKUP_RING_CAPACITY] = {};
    u32           count = 0;
};

namespace PendingPickupRingOps {
    // Clear all entries and reset count to zero.
    void reset(PendingPickupRing& r);

    // Append a new predicted-pickup entry. If full, evicts the oldest entry (ring semantics —
    // graceful degradation; mis-predicted disappearance is recovered by mirrorWorldItems).
    void record(PendingPickupRing& r, u32 clientTick, u32 itemUid);

    // Return true if itemUid is currently listed as a pending predicted pickup.
    bool isPending(const PendingPickupRing& r, u32 itemUid);

    // Remove the first entry matching itemUid (compact in-place). Returns true if found.
    // Called when the snapshot confirms the item is gone (server accepted the pickup).
    bool ack(PendingPickupRing& r, u32 itemUid);

    // Remove all entries with clientTick < cutoffClientTick (compact in-place).
    // Called in clientNetPost at ~60 Hz to bound the ring if acks never arrive (UDP loss).
    void expireOlderThan(PendingPickupRing& r, u32 cutoffClientTick);
}
