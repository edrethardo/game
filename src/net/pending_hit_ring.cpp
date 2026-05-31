// pending_hit_ring.cpp — implementation for PendingHitRing ops.
// See pending_hit_ring.h for design notes.

#include "net/pending_hit_ring.h"

void PendingHitRingOps::reset(PendingHitRing& r) {
    for (u32 i = 0; i < PENDING_HIT_RING_CAPACITY; i++) r.entries[i] = {};
    r.count = 0;
}

void PendingHitRingOps::record(PendingHitRing& r, u32 clientTick, u16 targetIdx, u8 isPlayer) {
    if (r.count >= PENDING_HIT_RING_CAPACITY) {
        // Ring full: evict oldest entry by shifting everything down one slot.
        // Oldest is entries[0] since we always append at entries[count].
        for (u32 i = 1; i < PENDING_HIT_RING_CAPACITY; i++) r.entries[i - 1] = r.entries[i];
        r.entries[PENDING_HIT_RING_CAPACITY - 1] = {};
        r.count = PENDING_HIT_RING_CAPACITY - 1;
    }
    PendingHit& e = r.entries[r.count];
    e.clientTick = clientTick;
    e.targetIdx  = targetIdx;
    e.isPlayer   = isPlayer;
    e.acked      = 0;
    r.count++;
}

bool PendingHitRingOps::ack(PendingHitRing& r, u32 clientTick, u16 targetIdx) {
    for (u32 i = 0; i < r.count; i++) {
        PendingHit& e = r.entries[i];
        if (e.clientTick == clientTick && e.targetIdx == targetIdx) {
            // Compact: shift all later entries down to fill the gap.
            for (u32 j = i + 1; j < r.count; j++) r.entries[j - 1] = r.entries[j];
            r.entries[r.count - 1] = {};
            r.count--;
            return true;
        }
    }
    return false;
}

void PendingHitRingOps::expireOlderThan(PendingHitRing& r, u32 cutoffClientTick) {
    // Partition in-place: keep entries with clientTick >= cutoff at the front.
    u32 write = 0;
    for (u32 read = 0; read < r.count; read++) {
        if (r.entries[read].clientTick >= cutoffClientTick) {
            if (write != read) r.entries[write] = r.entries[read];
            write++;
        }
    }
    // Zero out the now-unused tail slots for clean state.
    for (u32 i = write; i < r.count; i++) r.entries[i] = {};
    r.count = write;
}
