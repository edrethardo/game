#pragma once

#include "core/types.h"

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

// Chance a given room contains a shrine, and the cap per floor. A shrine you find every floor is
// furniture; one you find sometimes is a decision about whether to detour for it.
constexpr f32 ROOM_CHANCE    = 0.12f;
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

} // namespace Shrine
