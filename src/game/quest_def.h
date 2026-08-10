// quest_def.h — the act quest chain: what each quest asks, who asks it, and how each step completes.
//
// A parody of Diablo 2's Act 1 quest line, beat for beat, in this game's voice — the running gag
// being that the dungeon is software and the "corruption" is a system falling over. Act 2 follows
// Hellgate London's shape underneath London.
//
// A quest is (a) a place, (b) a NAMED GIVER who offers it, (c) an ordered list of OBJECTIVES, each
// satisfied by something the engine can already observe, and (d) a narration paragraph for the
// Journal. Objective 0 is always TALK: every D2 quest opens by speaking to somebody, and it gives
// the Journal a first row that is true the moment the quest is known.
//
// Still deliberately SMALL: no dialogue TREE (a giver has one line), no prerequisite graph (the
// chain is the road's own order), no rewards table. Those would have to be designed rather than
// guessed at.
//
// Header-only and engine-free (the free_play.h / zone_def.h pattern) so the rules unit-test without a
// GL or engine context.
#pragma once

#include "core/types.h"
#include "game/zone_def.h"

namespace Quest {

// Capacity, sized for growth rather than for today. `Quest::Progress` (quest_state.h) IS serialized
// at these sizes in the SAVE_VERSION 7 per-player tail, so enlarging either one now costs a version
// bump and another pair of legacy readers — which is exactly why the widening was done up front,
// while v7 was still unreleased and it was free. Ten quests are authored today.
inline constexpr u32 MAX_QUESTS = 32;   // per-character state arrays are sized to this
inline constexpr u32 MAX_OBJ    = 4;    // objectives per quest

// completionMask() and actComplete() carry quest completion in a u64, so a quest past bit 63
// cannot be represented at all — actComplete would never return true for its act and the onward
// gate would refuse forever. Raising this past 64 means widening those two first.
static_assert(MAX_QUESTS <= 64, "completionMask()/actComplete() are u64 — quest 64+ is unrepresentable");

// How one OBJECTIVE is satisfied. These are exactly what the engine can already observe without
// new bookkeeping, plus ACTIVATE for the Cairn Stones.
enum struct Trigger : u8 {
    CLEAR_ZONE,   // every hostile in the quest's zone is dead  (live count, never stored)
    SLAY,         // the named enemy died                        (boolean)
    REACH,        // the quest's zone was entered                (boolean)
    TALK,         // the giver was spoken to                     (boolean, NEVER a prerequisite)
    ACTIVATE,     // N world fixtures interacted with            (bitmask, stored)
    COUNT
};

// THE CAIRN STONES, in one place. Four things must agree on this number — the authored objective
// below, the anchor array buildZoneLevel fills, the spawn loop that stands them up, and the bitmask
// each stone's ordinal rides in — and the day they disagree the quest either cannot complete or the
// field grows a stone nothing can count. Named here so the authored row USES it rather than
// repeating a literal 5.
inline constexpr u8 CAIRN_COUNT = 5;
inline constexpr u8 CAIRN_ZONE  = 56;   // the Field of Unmerged Branches

struct ObjectiveDef {
    Trigger     trigger;
    const char* text;      // journal row label: "Hostiles remaining", "Stones aligned"
    const char* target;    // enemy name for SLAY, fixture tag for ACTIVATE, "" otherwise
    u8          required;  // 1 for a boolean step; 5 for the Cairn Stones
};

// A quest giver: a named NPC standing in an act hub. GiverDef exists rather than deriving the
// giver straight from the act so an act can field two or three the way D2's Rogue Encampment
// fields Akara, Kashya and Charsi — one authored byte, no duplicated fact (the ACT is still
// derived, from the giver's own hubFloor).
struct GiverDef {
    u8          hubFloor;  // 98 = the town (Act 1), 61 = Null Terminus (Act 2)
    const char* name;      // interact prompt, nameplate, journal attribution
    const char* greeting;  // the one short line spoken to chat on talk
};

inline constexpr GiverDef GIVERS[] = {
    { 98, "Akara, the Allocator",   "You came back. Good. Something here still will not free." },
    { 98, "Charsi, the Forgemaid",  "Steel I can fix. What is out there, I cannot." },
    { 61, "The Signalman",          "Mind the gap. Mind everything, really." },
    { 61, "Kashya of the Platform", "We hold this platform. Nothing else down here is held." },
};
inline constexpr u32 GIVER_COUNT = sizeof(GIVERS) / sizeof(GIVERS[0]);

struct QuestDef {
    u8           zoneFloor;
    const char*  name;
    const char*  blurb;         // the one-line offer; still goes to chat
    const char*  narration;     // the journal body. No length limit — the Journal wraps it.
    u8           giverIdx;      // index into GIVERS[]
    u8           objectiveCount;
    ObjectiveDef objectives[MAX_OBJ];
};

// ACT 1, in walking order. The parody names carry the joke; the BEATS are D2's.
//
//   D2                          here
//   Den of Evil (clear it)   -> Free the Allocation
//   Sisters' Burial Grounds  -> The Rebaser        (Blood Raven raises the dead; a rebase rewrites it)
//   The Cairn Stones         -> Align the Standing Stones
//   The Search for Cain      -> The Search for Deckard Cache   (the pun the whole act was built for)
//   Sisters to the Slaughter -> Terminal Access     (the tube mouth, the act's epilogue)
//
// ORDER IS APPEND-ONLY. A row's POSITION is its slot in Progress::state and its bit in the derived
// completion mask, so resorting this table silently reassigns every saved hero's progress.
inline constexpr QuestDef QUESTS[] = {
    { 53, "Free the Allocation",
          "Something is still holding the Den. Clear it.",
          "The Den was freed once and never released. Whatever holds it now has held it since "
          "before anyone here kept records. Go down, and let it go.",
          /*giver*/ 0, /*objCount*/ 2,
          { { Trigger::TALK,       "Speak to Akara",        "", 1 },
            { Trigger::CLEAR_ZONE, "Hostiles remaining",    "", 1 } } },

    { 55, "The Rebaser",
          "The graveyard keeps bringing its history back. Stop whatever is rewriting it.",
          "The graves do not stay written. Every night the history is replayed onto them and "
          "whatever was buried comes back with it. Find what is doing the rewriting, and stop it.",
          /*giver*/ 0, /*objCount*/ 2,
          { { Trigger::TALK, "Speak to Akara",          "",                      1 },
            { Trigger::SLAY, "Slay The Garbage Collector", "The Garbage Collector", 1 } } },

    // D2's Cairn Stones beat, and the act's only ACTIVATE quest.
    //
    // The ORDER matters and is not fussiness: ZoneRoute::linkOpen gates the onward road on
    // zoneSettled(56), so a quest 56 whose trigger has no implementation SEALS the way to TristRAM
    // and strands anyone playing this commit. That is why this row stayed CLEAR_ZONE until the five
    // stone fixtures existed, and why the flip lands in the same commit that spawns them.
    { CAIRN_ZONE, "Align the Standing Stones",
          "Monuments to abandoned features, and none of them agree. Align them.",
          "Five stones, each raised for something that was going to be finished. They disagree "
          "about what the field was for, and while they disagree the way to TristRAM stays shut. "
          "Touch each in turn and let them settle it.",
          /*giver*/ 1, /*objCount*/ 2,
          { { Trigger::TALK,     "Speak to Charsi", "",      1 },
            { Trigger::ACTIVATE, "Stones aligned",  "cairn", CAIRN_COUNT } } },

    { 57, "The Search for Deckard Cache",
          "The village was restored from backup once too often. Something in the forge came back wrong.",
          "TristRAM has been restored from backup more times than anyone kept count of. Each "
          "restore came back a little further from the village that was saved. The smith came "
          "back worst of all.",
          /*giver*/ 1, /*objCount*/ 2,
          { { Trigger::TALK, "Speak to Charsi",                    "",                              1 },
            { Trigger::SLAY, "Slay Griswald, the Unfinished Build", "Griswald, the Unfinished Build", 1 } } },

    { 59, "Terminal Access",
          "The road ends at a boarded station. Find the way down.",
          "The road out of the fields ends at a station nobody has boarded a train from in a very "
          "long time. It is boarded, not locked. There is a difference, and it matters.",
          /*giver*/ 1, /*objCount*/ 2,
          { { Trigger::TALK,  "Speak to Charsi",              "", 1 },
            { Trigger::REACH, "Reach Whitechapel Terminal",   "", 1 } } },

    // --- ACT 2: "Hellgate: Localhost" ---
    { 61, "Signal Restored",
          "Somebody down here still has the lights on. Find them.",
          "London fell and the survivors went underground. One platform still has power, which "
          "means somebody down there is still running it. Find them before whatever else is in "
          "the tunnels does.",
          /*giver*/ 2, /*objCount*/ 2,
          { { Trigger::TALK,  "Speak to the Signalman", "", 1 },
            { Trigger::REACH, "Reach Null Terminus",    "", 1 } } },

    { 62, "Break the Loop",
          "The Circle Line is running, and it is not carrying passengers. Clear it.",
          "The Circle Line never stopped running. It has no passengers, no drivers and no "
          "timetable, and it has been going round since the gate opened. Break it.",
          /*giver*/ 2, /*objCount*/ 2,
          { { Trigger::TALK,       "Speak to the Signalman", "", 1 },
            { Trigger::CLEAR_ZONE, "Hostiles remaining",     "", 1 } } },

    { 64, "Insufficient Funds",
          "Something has been drawing on Bank for a long time. Settle it.",
          "Something has been drawing on Bank Station since before the gate, and the balance has "
          "never once been questioned. Go and question it.",
          /*giver*/ 3, /*objCount*/ 2,
          { { Trigger::TALK, "Speak to Kashya",              "",                       1 },
            { Trigger::SLAY, "Slay The Perpetual Commuter",  "The Perpetual Commuter", 1 } } },

    { 65, "Privilege Escalation",
          "The gate will not open while the circus is this crowded. Make room, then force it.",
          "The rift at Piccadilly is held shut by everything crowded around it. Clear the circus "
          "and it can be forced. It should not be possible to force it. It is.",
          /*giver*/ 3, /*objCount*/ 2,
          { { Trigger::TALK,       "Speak to Kashya",    "", 1 },
            { Trigger::CLEAR_ZONE, "Hostiles remaining", "", 1 } } },

    { 66, "Kill -9",
          "The gate is running on this machine. Terminate it.",
          "The gate is not a door. It is a process, and it is running on this machine. It will "
          "not close politely. Terminate it.",
          /*giver*/ 3, /*objCount*/ 2,
          { { Trigger::TALK, "Speak to Kashya",       "",               1 },
            { Trigger::SLAY, "Slay Signal Failure",   "Signal Failure", 1 } } },
};

inline constexpr u32 COUNT = sizeof(QUESTS) / sizeof(QUESTS[0]);

// The quest hosted by `zoneFloor`, or nullptr. One quest per zone at most — the act is a walk, not a
// hub, so a zone with two objectives would just read as noise.
inline const QuestDef* forZone(u8 zoneFloor) {
    for (u32 i = 0; i < COUNT; i++)
        if (QUESTS[i].zoneFloor == zoneFloor) return &QUESTS[i];
    return nullptr;
}

// The objective that carries the QUEST — the first non-TALK step. Objective 0 is always "speak to
// the giver", which is never what a consumer means by asking what a quest wants: the engine's
// SLAY/CLEAR_ZONE/REACH hooks and the bot's task router are all asking about the DEED. Derived
// rather than stored as a second field, so it cannot disagree with the objective list it describes.
// nullptr only for a quest authored with nothing but a TALK step, which the data lint forbids.
inline const ObjectiveDef* deedObjective(const QuestDef& q) {
    for (u32 o = 0; o < q.objectiveCount; o++)
        if (q.objectives[o].trigger != Trigger::TALK) return &q.objectives[o];
    return nullptr;
}

// The quest's row in QUESTS[], which is ALSO its slot in Progress::state and its bit in the derived
// mask. That makes QUESTS[] effectively append-only: reordering it silently reassigns every saved
// hero's progress. 0xFF = this zone hosts no quest.
inline u8 indexForZone(u8 zoneFloor) {
    for (u32 i = 0; i < COUNT; i++)
        if (QUESTS[i].zoneFloor == zoneFloor) return static_cast<u8>(i);
    return 0xFF;
}

inline bool isComplete(u64 mask, u8 zoneFloor) {
    const u8 b = indexForZone(zoneFloor);
    return b != 0xFF && (mask & (1ull << b)) != 0;
}

// Which act a quest belongs to, derived from its zone rather than stored: Act 1 is the surface
// chain (zones 52-59), Act 2 is the Underground (60+). One less field to keep in sync.
inline u8 actOf(u8 zoneFloor) { return zoneFloor >= 60 ? 2 : 1; }

// True when every quest of `act` is done. Act-scoped rather than global so finishing Act 1 still
// announces itself once Act 2's quests exist — a global check would silently stop firing the day a
// second act was added, which is exactly the kind of quiet regression a chain like this invites.
inline bool actComplete(u64 mask, u8 act) {
    for (u32 i = 0; i < COUNT; i++) {
        if (actOf(QUESTS[i].zoneFloor) != act) continue;
        if ((mask & (1ull << i)) == 0) return false;
    }
    return true;
}

// The table must fit the per-character state arrays it is indexed against (quest_state.h sizes
// them to MAX_QUESTS). A 33rd quest would NOT overflow anything — every loop over those arrays is
// bounded by MAX_QUESTS — it would be silently IGNORED: its completion could never appear in the
// derived mask, so actComplete() for its act could never return true and the onward gate would
// refuse forever. Caught here at compile time instead of as a stranded run.
static_assert(COUNT <= MAX_QUESTS, "QUESTS[] outgrew the per-character state arrays — raise MAX_QUESTS (save bump)");

} // namespace Quest
