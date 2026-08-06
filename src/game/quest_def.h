// quest_def.h — the Act 1 quest chain: what each quest asks, where it happens, and how it completes.
//
// A parody of Diablo 2's Act 1 quest line, beat for beat, in this game's voice — the running gag
// being that the dungeon is software and the "corruption" is a system falling over. D2 gives you six
// Act 1 quests; this ships the five that map onto zones the act actually has.
//
// Deliberately SMALL. This is not a quest engine: there is no dialogue tree, no journal UI, no
// prerequisite graph, no rewards table. Each quest is (a) a place, (b) one of three completion
// triggers the engine can already answer, and (c) a per-character completion bit. That covers the
// whole act, and everything it does NOT do is a thing that would have to be designed rather than
// guessed at. When Act 2 needs escorts or fetch chains, this is the place to grow — not before.
//
// Header-only and engine-free (the free_play.h / zone_def.h pattern) so the rules unit-test without a
// GL or engine context.
#pragma once

#include "core/types.h"
#include "game/zone_def.h"

namespace Quest {

// How a quest is satisfied. These three are exactly what the engine can already observe without new
// bookkeeping — which is why there are three and not ten.
enum struct Trigger : u8 {
    CLEAR_ZONE,   // kill every hostile in the quest's zone (D2's "Den of Evil" beat)
    SLAY,         // kill the named champion/boss in the quest's zone
    REACH,        // simply arrive — the quest is finding the place at all
    COUNT
};

struct QuestDef {
    u8          zoneFloor;   // where it happens (a Zone::ZONES floor)
    const char* name;        // player-facing title
    const char* blurb;       // one line, shown when it is offered and when it completes
    Trigger     trigger;
    const char* target;      // SLAY: the enemy name. Otherwise unused ("").
};

// ACT 1, in walking order. The parody names carry the joke; the BEATS are D2's.
//
//   D2                          here
//   Den of Evil (clear it)   -> Free the Allocation
//   Sisters' Burial Grounds  -> The Rebaser        (Blood Raven raises the dead; a rebase rewrites it)
//   Tools of the Trade       -> Restore the Toolchain
//   The Search for Cain      -> The Search for Deckard Cache   (the pun the whole act was built for)
//   Sisters to the Slaughter -> Terminal Access     (the act boss, at the tube mouth)
inline constexpr QuestDef QUESTS[] = {
    { 53, "Free the Allocation",
          "Something is still holding the Den. Clear it.",
          Trigger::CLEAR_ZONE, "" },

    { 55, "The Rebaser",
          "The graveyard keeps bringing its history back. Stop whatever is rewriting it.",
          Trigger::SLAY, "The Garbage Collector" },

    // D2's Cairn Stones beat. Deckard Cain is not FOUND by walking to Tristram — the stones in the
    // Stony Field are what open the way, which is why this quest sits here and the portal stands in
    // this field. The stones read as monuments to abandoned features, so "getting them to agree" is
    // the same joke as the zone's name.
    { 56, "Align the Standing Stones",
          "Monuments to abandoned features, and none of them agree. Clear the field and they will.",
          Trigger::CLEAR_ZONE, "" },

    // ACT 1's CLIMAX. D2 puts Griswold in the ruins of the town he used to serve; the beat lands
    // harder here because TristRAM's whole conceit is a village restored from backup once too
    // often — its smith is what the last restore actually produced. A SLAY quest, not the REACH it
    // used to be: arriving somewhere is a weak note to end an act on.
    { 57, "The Search for Deckard Cache",
          "The village was restored from backup once too often. Something in the forge came back wrong.",
          Trigger::SLAY, "Griswald, the Unfinished Build" },

    // The way onward. Deliberately a REACH now that Griswald carries the act's fight: the station is
    // the epilogue and the door to Act 2, not a second climax competing with the first.
    { 59, "Terminal Access",
          "The road ends at a boarded station. Find the way down.",
          Trigger::REACH, "" },

    // --- ACT 2: "Hellgate: Localhost" ---
    // Hellgate London's shape: the survivors are underground, the tunnels belong to the demons, and
    // the rift is the thing you eventually have to close. The beats are its, the names are ours.
    { 61, "Signal Restored",
          "Somebody down here still has the lights on. Find them.",
          Trigger::REACH, "" },

    { 62, "Break the Loop",
          "The Circle Line is running, and it is not carrying passengers. Clear it.",
          Trigger::CLEAR_ZONE, "" },

    { 64, "Insufficient Funds",
          "Something has been drawing on Bank for a long time. Settle it.",
          Trigger::SLAY, "The Perpetual Commuter" },

    // The rift-opening beat, and Act 2's answer to the Cairn Stones. Piccadilly is where the gate
    // is forced; until the circus is cleared there is nothing to force it with. "Buffer Overflow"
    // is already the zone's joke, so the quest that breaks it open is the obvious escalation.
    { 65, "Privilege Escalation",
          "The gate will not open while the circus is this crowded. Make room, then force it.",
          Trigger::CLEAR_ZONE, "" },

    { 66, "Kill -9",
          "The gate is running on this machine. Terminate it.",
          Trigger::SLAY, "Signal Failure" },
};

inline constexpr u32 COUNT = sizeof(QUESTS) / sizeof(QUESTS[0]);

// The quest hosted by `zoneFloor`, or nullptr. One quest per zone at most — the act is a walk, not a
// hub, so a zone with two objectives would just read as noise.
inline const QuestDef* forZone(u8 zoneFloor) {
    for (u32 i = 0; i < COUNT; i++)
        if (QUESTS[i].zoneFloor == zoneFloor) return &QUESTS[i];
    return nullptr;
}

// Bit index for the per-character completion mask. Table POSITION, like the waypoint mask, which
// makes QUESTS effectively append-only: reordering it silently reassigns every saved hero's progress.
inline u8 bitFor(u8 zoneFloor) {
    for (u32 i = 0; i < COUNT; i++)
        if (QUESTS[i].zoneFloor == zoneFloor) return static_cast<u8>(i);
    return 0xFF;
}

inline bool isComplete(u64 mask, u8 zoneFloor) {
    const u8 b = bitFor(zoneFloor);
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

} // namespace Quest
