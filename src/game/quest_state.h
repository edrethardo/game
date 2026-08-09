// quest_state.h — every mutable fact about a character's quest progress, and the rules that move it.
//
// Header-only and engine-free (the zone_route.h / free_play.h pattern) so the act's rules unit-test
// with no GL and no live level.
//
// `Progress` IS the save payload at SAVE_VERSION 7, and it is the AUTHORITY: `Engine::m_questProgress`
// holds it per lane, engine_persist.cpp writes it, and a v6 file is reconstructed into it by
// migrateFromMask on load. `Engine::m_questMask` survives only as a CACHE refreshed from here.
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

// Per character. This IS the save payload at SAVE_VERSION 7 (engine_persist.cpp, the per-player tail).
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


// ---- Reading progress -------------------------------------------------------------------------

// Stored progress for one objective.
//
// A CLEAR_ZONE objective returns 0 here by design: it has no stored counterpart, and the Journal
// supplies the live remaining-hostile count itself. An ACTIVATE objective returns the POPCOUNT of
// its bitmask, since the byte records which fixtures were used rather than how many.
inline u8 objectiveProgress(const Progress& p, u8 questIdx, u8 objIdx) {
    if (questIdx >= COUNT || objIdx >= QUESTS[questIdx].objectiveCount) return 0;
    const u8 raw = p.obj[questIdx][objIdx];
    if (QUESTS[questIdx].objectives[objIdx].trigger != Trigger::ACTIVATE) return raw;
    u8 n = 0;
    for (u8 b = 0; b < 8; b++) if (raw & (1u << b)) n++;
    return n;
}

inline bool objectiveDone(const Progress& p, u8 questIdx, u8 objIdx) {
    if (questIdx >= COUNT || objIdx >= QUESTS[questIdx].objectiveCount) return false;
    return objectiveProgress(p, questIdx, objIdx) >= QUESTS[questIdx].objectives[objIdx].required;
}

// ---- Advancing --------------------------------------------------------------------------------

// Re-derive a quest's state from its objectives. Called after every mutation so `state` can never
// disagree with `obj` — the same single-source discipline the derived mask follows.
//
// TALK is EXCLUDED from the completion test on purpose. A quest completes on its deed alone; the
// conversation is narration, not permission. See the design doc's "TALK is never a prerequisite":
// this is where that rule actually lives, and test_quest_state pins it by name.
inline void reevaluate(Progress& p, u8 questIdx) {
    if (questIdx >= COUNT) return;
    if (p.state[questIdx] == static_cast<u8>(State::LOCKED)) return;
    // COMPLETE is terminal, and that is not tidiness. A v6 hero migrates in COMPLETE with `obj`
    // zeroed — migrateFromMask has no detail to restore — so re-deriving would un-complete every
    // quest they had already finished the first time they greeted its giver, re-sealing a road
    // they had already walked.
    if (p.state[questIdx] == static_cast<u8>(State::COMPLETE)) return;

    const QuestDef& q = QUESTS[questIdx];
    bool allDeedsDone = true;
    bool anyProgress  = false;
    for (u32 o = 0; o < q.objectiveCount; o++) {
        const bool done = objectiveDone(p, questIdx, static_cast<u8>(o));
        if (done) anyProgress = true;
        if (q.objectives[o].trigger == Trigger::TALK) continue;
        if (!done) allDeedsDone = false;
    }

    if (allDeedsDone)      p.state[questIdx] = static_cast<u8>(State::COMPLETE);
    else if (anyProgress)  p.state[questIdx] = static_cast<u8>(State::ACTIVE);
}

// Make a quest known. Never demotes: a COMPLETE quest re-offered by walking back into its zone
// must stay complete.
inline void offer(Progress& p, u8 questIdx) {
    if (questIdx >= COUNT) return;
    if (p.state[questIdx] == static_cast<u8>(State::LOCKED))
        p.state[questIdx] = static_cast<u8>(State::OFFERED);
}

// Set the first objective of `trigger` kind whose target matches, then re-derive the state.
// Internal; the named hooks below are what callers use.
inline void satisfy(Progress& p, u8 questIdx, Trigger trigger, const char* target) {
    if (questIdx >= COUNT) return;
    offer(p, questIdx);                                   // reaching a deed implies knowing of it
    const QuestDef& q = QUESTS[questIdx];
    for (u32 o = 0; o < q.objectiveCount; o++) {
        if (q.objectives[o].trigger != trigger) continue;
        if (trigger == Trigger::SLAY) {
            if (!target || !q.objectives[o].target) continue;
            // Hand-rolled compare: <cstring> would be the only include this header needs, and the
            // point of it being engine-free is that it drags nothing in.
            const char* a = target; const char* b = q.objectives[o].target;
            while (*a && *a == *b) { a++; b++; }
            if (*a != *b) continue;
        }
        p.obj[questIdx][o] = q.objectives[o].required;
        break;
    }
    reevaluate(p, questIdx);
}

inline void noteTalk   (Progress& p, u8 questIdx)                     { satisfy(p, questIdx, Trigger::TALK,       nullptr); }
inline void noteReached(Progress& p, u8 questIdx)                     { satisfy(p, questIdx, Trigger::REACH,      nullptr); }
inline void noteCleared(Progress& p, u8 questIdx)                     { satisfy(p, questIdx, Trigger::CLEAR_ZONE, nullptr); }
inline void noteKill   (Progress& p, u8 questIdx, const char* enemy)  { satisfy(p, questIdx, Trigger::SLAY,       enemy);   }

// One fixture of an ACTIVATE objective. `fixtureIdx` is the fixture's ordinal within the quest
// (0..required-1) and becomes a bit, so re-touching a stone cannot double-count it.
inline void noteActivate(Progress& p, u8 questIdx, u8 fixtureIdx) {
    if (questIdx >= COUNT || fixtureIdx >= 8) return;
    offer(p, questIdx);
    const QuestDef& q = QUESTS[questIdx];
    for (u32 o = 0; o < q.objectiveCount; o++) {
        if (q.objectives[o].trigger != Trigger::ACTIVATE) continue;
        if (fixtureIdx >= q.objectives[o].required) return;   // not a fixture this quest owns
        p.obj[questIdx][o] |= static_cast<u8>(1u << fixtureIdx);
        break;
    }
    reevaluate(p, questIdx);
}

// ---- Quest givers -------------------------------------------------------------------------------

// The quest this giver currently has to hand out: their first non-COMPLETE quest in table order,
// which is the road's walking order. 0xFF = they have nothing left.
//
// One quest at a time on purpose. A giver who dumps their whole act at once turns the Journal into
// a wall of text on the first conversation and removes any sense of the chain advancing.
//
// LOCKED counts as outstanding, not as skippable: a quest the character has never met is exactly
// what a giver exists to hand over. So does ACTIVE — a quest in progress stays the one that giver
// talks about until its deed is done.
inline u8 giverOutstanding(const Progress& p, u8 giverIdx) {
    // A contract, not a memory fix: the scan below never indexes GIVERS[], so an unknown giver
    // already falls through to 0xFF. Keep the guard anyway — the day a caller-facing field of
    // GIVERS[giverIdx] is read here, its absence becomes a real out-of-bounds read.
    if (giverIdx >= GIVER_COUNT) return 0xFF;
    for (u32 i = 0; i < COUNT; i++) {
        if (QUESTS[i].giverIdx != giverIdx) continue;
        if (p.state[i] != static_cast<u8>(State::COMPLETE)) return static_cast<u8>(i);
    }
    return 0xFF;
}

} // namespace Quest
