// pending_damage_ring.cpp — implementation for PendingDamageRing ops.
// See pending_damage_ring.h for design notes and key-encoding rationale.

#include "net/pending_damage_ring.h"

void PendingDamageRingOps::reset(PendingDamageRing& r) {
    for (u32 i = 0; i < PENDING_DAMAGE_RING_CAPACITY; i++) r.entries[i] = {};
    r.count = 0;
}

void PendingDamageRingOps::record(PendingDamageRing& r, u32 clientTick, u32 projectileSrcKey) {
    if (r.count >= PENDING_DAMAGE_RING_CAPACITY) {
        // Ring full: evict oldest entry by shifting everything down one slot.
        // Oldest is entries[0] since we always append at entries[count].
        for (u32 i = 1; i < PENDING_DAMAGE_RING_CAPACITY; i++) r.entries[i-1] = r.entries[i];
        r.count = PENDING_DAMAGE_RING_CAPACITY - 1;
    }
    PendingDamage& e = r.entries[r.count];
    e.clientTick       = clientTick;
    e.projectileSrcKey = projectileSrcKey;
    r.count++;
}

bool PendingDamageRingOps::ack(PendingDamageRing& r, u32 projectileSrcKey) {
    for (u32 i = 0; i < r.count; i++) {
        if (r.entries[i].projectileSrcKey == projectileSrcKey) {
            // Compact: shift all later entries down to fill the gap.
            for (u32 j = i + 1; j < r.count; j++) r.entries[j-1] = r.entries[j];
            r.entries[r.count - 1] = {};
            r.count--;
            return true;
        }
    }
    return false;
}

void PendingDamageRingOps::expireOlderThan(PendingDamageRing& r, u32 cutoffClientTick) {
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
