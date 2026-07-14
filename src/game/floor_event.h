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
// The chase is the content. It has to be fast enough that you must commit to catching it, fragile
// enough that committing works, and short-lived enough that the choice actually costs you something.
namespace Goblin {
    constexpr f32 SPEED_MULT     = 1.35f;  // of the player's base speed — outruns you if you dawdle
    constexpr f32 HEALTH         = 60.0f;  // scaled by the floor's HP curve at spawn
    constexpr f32 ESCAPE_SECONDS = 22.0f;  // counted from the moment it is ATTACKED, not from spawn:
                                           // it idles on its hoard until provoked, then has this long
                                           // to reach the exit, paying nothing further if it makes it
    constexpr f32 BLEED_SECONDS  = 2.0f;   // one item dropped per this interval while alive
    constexpr u8  BLEED_MAX      = 4;      // items it can bleed before it is out of pocket
    constexpr u8  DEATH_DROPS    = 3;      // the rest of the sack, if you actually catch it
    constexpr f32 DETECT_RANGE   = 18.0f;  // it notices you from a long way off
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
