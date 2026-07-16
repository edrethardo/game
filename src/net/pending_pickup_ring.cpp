// pending_pickup_ring.cpp — implementation of the client-side predicted pickup ring.
//
// See pending_pickup_ring.h for design rationale. This file only contains the five operations
// declared there; all ring mutation is in-place array compaction (no allocations).

#include "net/pending_pickup_ring.h"

void PendingPickupRingOps::reset(PendingPickupRing& r) {
    for (u32 i = 0; i < PENDING_PICKUP_RING_CAPACITY; i++) r.entries[i] = {};
    r.count = 0;
}

void PendingPickupRingOps::record(PendingPickupRing& r, u32 clientTick, u32 itemUid,
                                   s8 predictedSlot, u8 lane) {
    // If full, evict the oldest (index 0) entry by shifting everything left one slot, then
    // decrement count so we write into the last slot below. Graceful degradation: the visual
    // mis-prediction is corrected on the next mirrorWorldItems pass anyway.
    if (r.count >= PENDING_PICKUP_RING_CAPACITY) {
        for (u32 i = 1; i < PENDING_PICKUP_RING_CAPACITY; i++) r.entries[i-1] = r.entries[i];
        r.count = PENDING_PICKUP_RING_CAPACITY - 1;
    }
    PendingPickup& e = r.entries[r.count];
    e.clientTick    = clientTick;
    e.itemUid       = itemUid;
    e.predictedSlot = predictedSlot;  // -1 if no inventory prediction (legacy callers)
    e.lane          = lane;
    r.count++;
}

s8 PendingPickupRingOps::findSlotByUid(const PendingPickupRing& r, u32 itemUid) {
    for (u32 i = 0; i < r.count; i++) {
        if (r.entries[i].itemUid == itemUid) return r.entries[i].predictedSlot;
    }
    return -1;
}

u8 PendingPickupRingOps::findLaneByUid(const PendingPickupRing& r, u32 itemUid) {
    for (u32 i = 0; i < r.count; i++) {
        if (r.entries[i].itemUid == itemUid) return r.entries[i].lane;
    }
    return 0;
}

bool PendingPickupRingOps::isPending(const PendingPickupRing& r, u32 itemUid) {
    for (u32 i = 0; i < r.count; i++) {
        if (r.entries[i].itemUid == itemUid) return true;
    }
    return false;
}

bool PendingPickupRingOps::ack(PendingPickupRing& r, u32 itemUid) {
    for (u32 i = 0; i < r.count; i++) {
        if (r.entries[i].itemUid == itemUid) {
            // Compact: shift entries after i left by one.
            for (u32 j = i + 1; j < r.count; j++) r.entries[j-1] = r.entries[j];
            r.entries[r.count - 1] = {};  // zero the vacated tail slot
            r.count--;
            return true;
        }
    }
    return false;
}

void PendingPickupRingOps::expireOlderThan(PendingPickupRing& r, u32 cutoffClientTick) {
    // Compact in-place: keep only entries with clientTick >= cutoffClientTick.
    u32 write = 0;
    for (u32 read = 0; read < r.count; read++) {
        if (r.entries[read].clientTick >= cutoffClientTick) {
            if (write != read) r.entries[write] = r.entries[read];
            write++;
        }
    }
    // Zero out vacated tail slots for cleanliness.
    for (u32 i = write; i < r.count; i++) r.entries[i] = {};
    r.count = write;
}
