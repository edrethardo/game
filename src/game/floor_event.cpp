// floor_event.cpp — weighted selection of a floor's event. Pure; no engine state.
// The loader (JSON -> FloorEventTable) lives in floor_event_loader.cpp so this stays testable
// without dragging nlohmann/json into the unit-test binary.

#include "game/floor_event.h"
#include <cstring>

namespace {
inline u32 nextRand(u32& s) { s = s * 1664525u + 1013904223u; return s; }
inline u32 randRange(u32& s, u32 n) { return n ? (nextRand(s) >> 8) % n : 0; }
}

FloorEventId FloorEvent::idFromName(const char* name) {
    if (!name) return FloorEventId::NONE;
    if (std::strcmp(name, "loot_goblin") == 0) return FloorEventId::LOOT_GOBLIN;
    return FloorEventId::NONE;   // unknown id -> no event, never the wrong event
}

const char* FloorEvent::nameOf(FloorEventId id) {
    switch (id) {
        case FloorEventId::LOOT_GOBLIN: return "loot_goblin";
        case FloorEventId::NONE:        return "none";
        default:                        return "none";
    }
}

FloorEventId FloorEvent::pick(const FloorEventTable& table, u32 effectiveFloor, u32& rngState) {
    if (table.count == 0) return FloorEventId::NONE;

    // Does this floor get an event at all? Rolled first so the weights only ever decide WHICH event,
    // never WHETHER — otherwise adding a second event to the table would silently make events more
    // frequent, which is the kind of coupling that makes a data table untunable.
    const u32 roll = randRange(rngState, 1000);
    if (static_cast<f32>(roll) >= table.floorChance * 1000.0f) return FloorEventId::NONE;

    // Weighted pick among the events legal at this depth.
    u32 total = 0;
    for (u32 i = 0; i < table.count; i++) {
        const FloorEventDef& d = table.defs[i];
        if (d.id == FloorEventId::NONE) continue;
        if (effectiveFloor < d.minFloor) continue;
        total += d.weight;
    }
    if (total == 0) return FloorEventId::NONE;   // nothing eligible yet at this depth

    u32 pick = randRange(rngState, total);
    for (u32 i = 0; i < table.count; i++) {
        const FloorEventDef& d = table.defs[i];
        if (d.id == FloorEventId::NONE) continue;
        if (effectiveFloor < d.minFloor) continue;
        if (pick < d.weight) return d.id;
        pick -= d.weight;
    }
    return FloorEventId::NONE;   // unreachable while total > 0; a safe default beats UB
}
