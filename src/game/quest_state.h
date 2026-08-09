// quest_state.h — every mutable fact about a character's quest progress, and the rules that move it.
//
// Header-only and engine-free (the zone_route.h / free_play.h pattern) so the act's rules unit-test
// with no GL and no live level.
//
// STATUS — this is the FOUNDATION, not yet the live path. `Progress` WILL BE what the save file
// holds at SAVE_VERSION 7 (a later commit in this series). TODAY SAVE_VERSION is 6, nothing in the
// engine constructs a `Progress` at all, and quest completion still lives in `Engine::m_questMask`
// — a plain u64, written by engine_persist.cpp and read by every live consumer. Until that
// migration lands, changing this struct alone changes NOTHING a player can observe: m_questMask is
// what actually gates the acts.
//
// THE DERIVED MASK is the compatibility hinge. ZoneRoute, the gate refusals and the whole autoplay
// act branch consume a plain u64 of completed quests. They keep doing so — completionMask() builds
// it from `state` on demand, so the mask is a CACHE with one home rather than a second copy of a
// fact that can drift from the first. That drift is the single most common bug shape in this
// codebase's history; see CLAUDE.md.
#pragma once

#include "core/types.h"
#include "game/quest_def.h"

namespace Quest {

// How far along one quest is.
//   LOCKED   — the character has not met it yet (not entered its zone, not spoken to its giver)
//   OFFERED  — known, no objective progress yet
//   ACTIVE   — at least one objective advanced
//   COMPLETE — every objective satisfied
enum struct State : u8 { LOCKED, OFFERED, ACTIVE, COMPLETE, COUNT };

// Per character. This WILL BE the save payload at SAVE_VERSION 7; engine_persist.cpp still writes
// the u64 m_questMask today (see STATUS above).
//
// `obj` holds STORED progress only. A CLEAR_ZONE objective deliberately has no stored counterpart:
// "how many hostiles are left" is a property of the entity pool, which is already polled every
// frame, and keeping a copy here would be a second home for a fact that already has one.
struct Progress {
    u8 state[MAX_QUESTS] = {};
    u8 obj[MAX_QUESTS][MAX_OBJ] = {};
};

// Bit i set iff quest i is COMPLETE. The u64 every existing consumer already takes.
inline u64 completionMask(const Progress& p) {
    u64 m = 0;
    for (u32 i = 0; i < MAX_QUESTS && i < 64; i++)
        if (p.state[i] == static_cast<u8>(State::COMPLETE)) m |= (1ull << i);
    return m;
}

// v6 -> v7. A pre-v7 save stores completions as one u64 bit per quest and nothing else, so every
// set bit becomes a COMPLETE quest with no objective detail. Objectives of an already-finished
// quest are never read, so leaving `obj` zeroed loses nothing a player can see.
//
// Called on exactly one path (the legacy load). Getting it wrong un-completes both acts for every
// hero who already played them, and the next autosave writes that loss back permanently.
inline void migrateFromMask(Progress& p, u64 legacyMask) {
    for (u32 i = 0; i < MAX_QUESTS && i < 64; i++)
        if (legacyMask & (1ull << i)) p.state[i] = static_cast<u8>(State::COMPLETE);
}

} // namespace Quest
