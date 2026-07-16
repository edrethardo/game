#pragma once
// pending_pickup_ring.h — client-side ring buffer of unacknowledged predicted pickup requests.
//
// When the CLIENT sends CL_PICKUP_ITEM (engine_update.cpp::sendPickupRequest), it records a
// (clientTick, itemUid, predictedSlot) entry here and immediately deactivates the world item
// AND adds the item to the local inventory (predicted=true) so the bag slot appears without
// waiting ~RTT/2 for the server. On SV_PICKUP_RESULT accept the predicted flag is cleared;
// on reject, removeFromBackpack(slot) rolls back the inventory add and mirrorWorldItems
// restores the world item from the next authoritative snapshot.
//
// Design mirrors PendingDamageRing: fixed-capacity linear array, compacted on removal (no
// holes). Capacity 16 is sufficient because pickups are user-initiated at human pace.

#include "core/types.h"

static constexpr u32 PENDING_PICKUP_RING_CAPACITY = 16;

struct PendingPickup {
    u32 clientTick    = 0;   // local client tick at which the CL_PICKUP_ITEM was sent
    u32 itemUid       = 0;   // uid of the world item being picked up
    s8  predictedSlot = -1;  // backpack slot the predicted item occupies; -1 if not predicted
    u8  lane          = 0;   // LOCAL lane that predicted it (online couch co-op has two) — a
                             // rejected pickup must roll back THAT lane's backpack, not whichever
                             // lane happened to be swapped in when SV_PICKUP_RESULT arrived
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
    // predictedSlot is the backpack slot returned by addToBackpack (-1 if not inventory-predicted);
    // lane is the local player that predicted it (0 outside couch co-op).
    void record(PendingPickupRing& r, u32 clientTick, u32 itemUid, s8 predictedSlot = -1,
                u8 lane = 0);

    // Returns predictedSlot for the given itemUid, or -1 if the uid is not pending.
    s8 findSlotByUid(const PendingPickupRing& r, u32 itemUid);

    // Returns the local lane that predicted the given itemUid, or 0 if the uid is not pending
    // (safe default: lane 0 is the only lane outside couch co-op).
    u8 findLaneByUid(const PendingPickupRing& r, u32 itemUid);

    // Return true if itemUid is currently listed as a pending predicted pickup.
    bool isPending(const PendingPickupRing& r, u32 itemUid);

    // Remove the first entry matching itemUid (compact in-place). Returns true if found.
    // Called when the snapshot confirms the item is gone (server accepted the pickup).
    bool ack(PendingPickupRing& r, u32 itemUid);

    // Remove all entries with clientTick < cutoffClientTick (compact in-place).
    // Called in clientNetPost at ~60 Hz to bound the ring if acks never arrive (UDP loss).
    void expireOlderThan(PendingPickupRing& r, u32 cutoffClientTick);
}
