#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/item.h"   // SHRINE_*_ID sentinels + ItemInstance (item.h does NOT include this file)

// shrine.h — walk-up shrines that grant a timed buff.
//
// Player::shrineBuff has existed for a long time, been READ every frame, and been written by
// nothing: there was no shrine entity, prop, or spawn code anywhere. Only the SPEED case even had a
// consumer; POWER and VITALITY had no reader at all, and there was no duration field, so a buff
// once granted would never have expired. This finishes the feature rather than leaving a stray
// if-statement that reads like a bug forever.
//
// Shrines are WorldItem SENTINELS (SHRINE_*_ID in item.h), which buys spawning, snapshot
// replication and the server-authoritative pickup path for free.

namespace ShrineBuff {
    constexpr u8 NONE     = 0;
    constexpr u8 POWER    = 1;   // +damage
    constexpr u8 SPEED    = 2;   // +move speed
    constexpr u8 VITALITY = 3;   // +max HP, and heals by the same amount (see below)
    constexpr u8 COUNT    = 4;
}

namespace Shrine {

constexpr f32 DURATION_SEC   = 45.0f;

constexpr f32 POWER_BONUS    = 0.30f;   // +30% damage
constexpr f32 SPEED_BONUS    = 0.25f;   // +25% move speed
constexpr f32 VITALITY_BONUS = 0.40f;   // +40% max HP

// Chance a given room contains a shrine, and the cap per floor. At 25% across the ~12 candidate
// rooms of a floor, ~97% of floors hold at least one — shrines are meant to be found, and the
// decision is which one to detour for, not whether one exists.
constexpr f32 ROOM_CHANCE    = 0.25f;
constexpr u8  MAX_PER_FLOOR  = 2;

inline f32 bonusFor(u8 buff) {
    switch (buff) {
        case ShrineBuff::POWER:    return POWER_BONUS;
        case ShrineBuff::SPEED:    return SPEED_BONUS;
        case ShrineBuff::VITALITY: return VITALITY_BONUS;
        default:                   return 0.0f;
    }
}

inline const char* nameOf(u8 buff) {
    switch (buff) {
        case ShrineBuff::POWER:    return "Shrine of Power";
        case ShrineBuff::SPEED:    return "Shrine of Speed";
        case ShrineBuff::VITALITY: return "Shrine of Vitality";
        default:                   return "Shrine";
    }
}

// Which buff a shrine world-item grants. The sentinel-defId → buff mapping had been written out
// longhand at every site that needed it (spawn, render, the pickup handler); the minimap would have
// been a fourth copy, which is where that kind of duplication starts silently disagreeing.
inline u8 buffOf(const ItemInstance& item) {
    switch (item.defId) {
        case SHRINE_POWER_ID:    return ShrineBuff::POWER;
        case SHRINE_SPEED_ID:    return ShrineBuff::SPEED;
        case SHRINE_VITALITY_ID: return ShrineBuff::VITALITY;
        default:                 return ShrineBuff::NONE;
    }
}

// The inverse of buffOf, used at spawn to stamp the sentinel onto the WorldItem. Kept next to its
// partner so the two directions of one mapping cannot drift.
inline u16 defIdFor(u8 buff) {
    switch (buff) {
        case ShrineBuff::POWER:    return SHRINE_POWER_ID;
        case ShrineBuff::SPEED:    return SHRINE_SPEED_ID;
        default:                   return SHRINE_VITALITY_ID;
    }
}

// A shrine's signature colour. The in-world crystal and the minimap icon both read it from here, so
// the colour you learn in the room is the colour you hunt for on the map — that is the entire value
// of colour-coding, and it evaporates the moment the two are allowed to drift apart.
inline Vec3 colorOf(u8 buff) {
    switch (buff) {
        case ShrineBuff::POWER:    return {1.00f, 0.35f, 0.30f};   // red
        case ShrineBuff::SPEED:    return {0.40f, 0.85f, 1.00f};   // cyan
        case ShrineBuff::VITALITY: return {0.45f, 1.00f, 0.55f};   // green
        default:                   return {1.00f, 1.00f, 1.00f};
    }
}

} // namespace Shrine
