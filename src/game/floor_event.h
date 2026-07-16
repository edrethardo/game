#pragma once

#include "core/types.h"

// floor_event.h — the FLOOR EVENT framework.
//
// Every floor is the same BSP dungeon reseeded, so nothing ever *happens* on one: you clear rooms
// and walk to the exit. A floor event is a single notable thing that may occur on a floor — the
// first is the loot goblin, which flees and must be chased down before it escapes.
//
// It is a weighted table rather than a hardcoded roll so that the next event is DATA
// (assets/config/events.json), not another branch in startGame.
//
// The selection is a pure function of (floor, rng) with no engine state, so it is unit-testable —
// see tests/game/test_floor_event.cpp. Placement and spawning live in the Engine.

static constexpr u32 MAX_FLOOR_EVENT_DEFS = 8;

enum struct FloorEventId : u8 {
    NONE = 0,       // no event this floor — the common case
    LOOT_GOBLIN,
    COUNT
};

struct FloorEventDef {
    FloorEventId id      = FloorEventId::NONE;
    u8           weight  = 0;   // relative weight among eligible events
    u8           minFloor = 1;  // effective floor (currentFloor + difficulty*50) required
};

struct FloorEventTable {
    FloorEventDef defs[MAX_FLOOR_EVENT_DEFS] = {};
    u32           count = 0;
    // Chance that a floor rolls ANY event at all, before the weighted pick. Events are meant to be
    // a treat, not furniture: if every floor has one, none of them is an event.
    f32           floorChance = 0.35f;
};

// --- Loot goblin tunables ---
// The chase is the content. It has to be fast enough that you must commit to catching it, meaty
// enough that a couple of opportunistic hits don't end it on the spot (killing it inside the
// escape window is a sustained-DPS check), and short-lived enough that the choice costs you.
namespace Goblin {
    constexpr f32 SPEED_MULT     = 1.35f;  // of the player's base speed — outruns you if you dawdle
    constexpr f32 HEALTH         = 1200.0f; // scaled by the floor's HP curve at spawn. Was 60 — one
                                            // playtest showed it evaporating to incidental swings
                                            // before the chase ever happened
    constexpr f32 ESCAPE_SECONDS = 22.0f;  // counted from the moment it is ATTACKED, not from spawn:
                                           // it sits on its hoard until provoked, then has this long
                                           // of frantic scatter; if it survives it, it vanishes
                                           // with whatever it has not bled — paying nothing further
    constexpr f32 BLEED_SECONDS  = 2.0f;   // one item dropped per this interval while alive
    constexpr u8  BLEED_MAX      = 4;      // items it can bleed before it is out of pocket
    constexpr f32 TAUNT_WINDOW   = 1.5f;   // final stretch of the escape clock: the goblin STOPS,
                                           // faces the player, and gloats (bubble + gold chat)
                                           // before the portal takes it — both a victory lap and
                                           // one last stand-still burst window to kill it through.
                                           // Must stay < the 2.4 s bubble so the taunt can't refire.
    constexpr u8  DEATH_DROPS    = 3;      // the rest of the sack, if you actually catch it — every
                                           // one a guaranteed LEGENDARY (engine_death.cpp forces the
                                           // rarity, boss/champion style); the bleed stays random
    constexpr f32 DETECT_RANGE   = 18.0f;  // it notices you from a long way off

    // Panic serpentine (the D3-style frantic scatter). Every JINK_MIN..JINK_MAX seconds the flee
    // heading is re-rolled as "directly away from the player" swerved by a random angle up to
    // ±JINK_ARC — big enough to visibly zig-zag, small enough that it never charges back through
    // the player on its own (the steering probe additionally penalizes headings toward them).
    constexpr f32 JINK_MIN = 0.35f;
    constexpr f32 JINK_MAX = 0.80f;
    constexpr f32 JINK_ARC = 1.2f;   // radians (~69 degrees)
}

namespace FloorEvent {

// Map a JSON id string to the enum. Returns NONE for anything unknown, so a typo in events.json
// degrades to "no event" instead of spawning the wrong thing.
FloorEventId idFromName(const char* name);
const char*  nameOf(FloorEventId id);

// Pick this floor's event, or NONE. Pure: `rngState` is an in/out LCG state owned by the caller.
// Respects floorChance, minFloor eligibility, and the relative weights.
FloorEventId pick(const FloorEventTable& table, u32 effectiveFloor, u32& rngState);

} // namespace FloorEvent
