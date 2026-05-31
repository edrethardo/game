#pragma once
// pending_skill_ring.h — client-side ring buffer of unacknowledged predicted skill activations.
//
// When the CLIENT successfully activates a skill via SkillSystem::tryActivate (class/boot/helm),
// it records a (clientTick, skillSlot) entry here. M10 will consume this ring to match
// server-side SV_SKILL_RESULT acks and detect server rejections (insufficient energy,
// cooldown not ready). Until M10 lands, entries accumulate and are bounded by expireOlderThan.
//
// skillSlot values: 0–3 = class skill slots (active slot index), 0xFE = boot skill,
// 0xFF = helmet skill. These sentinel values are chosen to be outside the valid class slot
// range (0–3) so M10 can distinguish the three activation paths.
//
// Design mirrors PendingPickupRing: fixed-capacity linear array, compacted on removal
// (no holes). Capacity 16 is generous for human-paced skill presses.
//
// Reset on every CLIENT connect (engine_startgame.cpp).

#include "core/types.h"

static constexpr u32 PENDING_SKILL_RING_CAPACITY = 16;

// Slot sentinel constants for equipment-slot skills (no integer slot index exists for those).
static constexpr u8 PENDING_SKILL_SLOT_BOOT   = 0xFE;
static constexpr u8 PENDING_SKILL_SLOT_HELMET = 0xFF;

struct PendingSkill {
    u32 clientTick = 0;
    u8  skillSlot  = 0;  // 0–3 = class skill slot index; 0xFE = boot; 0xFF = helmet
};

struct PendingSkillRing {
    PendingSkill entries[PENDING_SKILL_RING_CAPACITY] = {};
    u32          count = 0;
};

namespace PendingSkillRingOps {
    // Clear all entries and reset count to zero.
    void reset(PendingSkillRing& r);

    // Append a new predicted-skill entry. If full, evicts the oldest entry (ring semantics —
    // graceful degradation; a missed ack is recovered when the snapshot mirrors the server state).
    void record(PendingSkillRing& r, u32 clientTick, u8 skillSlot);

    // Remove the first entry matching (clientTick, skillSlot). Returns true if found.
    // Called when the server confirms the skill activation via SV_SKILL_RESULT (M10).
    bool ack(PendingSkillRing& r, u32 clientTick, u8 skillSlot);

    // Remove all entries with clientTick < cutoffClientTick (compact in-place).
    // Called to bound ring growth in the absence of M10 acks (e.g. UDP loss).
    void expireOlderThan(PendingSkillRing& r, u32 cutoffClientTick);
}
