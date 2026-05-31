// pending_skill_ring.cpp — implementation of the client-side predicted skill activation ring.
//
// See pending_skill_ring.h for design rationale. All ring mutation is in-place array
// compaction; no heap allocations. The (clientTick, skillSlot) pair uniquely identifies
// a prediction — two different skills activated on the same tick will have distinct skillSlot
// values, and the same skill cannot be activated twice on the same tick (cooldown prevents it).

#include "net/pending_skill_ring.h"

void PendingSkillRingOps::reset(PendingSkillRing& r) {
    for (u32 i = 0; i < PENDING_SKILL_RING_CAPACITY; i++) r.entries[i] = {};
    r.count = 0;
}

void PendingSkillRingOps::record(PendingSkillRing& r, u32 clientTick, u8 skillSlot) {
    // If full, evict the oldest (index 0) entry by shifting everything left one slot, then
    // decrement count so we write into the last slot below. Graceful degradation: the visual
    // mis-prediction (cooldown bar staying predicted vs. server state) is corrected by the
    // next snapshot mirror anyway.
    if (r.count >= PENDING_SKILL_RING_CAPACITY) {
        for (u32 i = 1; i < PENDING_SKILL_RING_CAPACITY; i++) r.entries[i-1] = r.entries[i];
        r.count = PENDING_SKILL_RING_CAPACITY - 1;
    }
    PendingSkill& e = r.entries[r.count];
    e.clientTick = clientTick;
    e.skillSlot  = skillSlot;
    r.count++;
}

bool PendingSkillRingOps::ack(PendingSkillRing& r, u32 clientTick, u8 skillSlot) {
    // Linear scan: ring is small (16 entries max) and ack is not a hot path.
    for (u32 i = 0; i < r.count; i++) {
        if (r.entries[i].clientTick == clientTick && r.entries[i].skillSlot == skillSlot) {
            // Compact: shift entries after i left by one.
            for (u32 j = i + 1; j < r.count; j++) r.entries[j-1] = r.entries[j];
            r.entries[r.count - 1] = {};  // zero the vacated tail slot
            r.count--;
            return true;
        }
    }
    return false;
}

void PendingSkillRingOps::expireOlderThan(PendingSkillRing& r, u32 cutoffClientTick) {
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
