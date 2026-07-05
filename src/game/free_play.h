// free_play.h — pure predicates for the post-clear Free-Play level select.
//
// A character has "cleared the game" once it beats Hell floor 50. Because triggerFloorDescent()
// saves the incremented floor BEFORE the floor-50 victory check (engine_update.cpp:1741 vs :1769),
// a cleared Hell save sits at difficulty 2 (Hell), floor 51+ (e.g. save_01 = floor 57). Header-only
// and engine-free so it unit-tests without a GL/engine context.
#pragma once

#include "core/types.h"

namespace FreePlay {

inline constexpr u8 DIFFICULTY_COUNT = 3;   // Normal(0), Nightmare(1), Hell(2)
inline constexpr u8 MIN_FLOOR = 1;
inline constexpr u8 MAX_FLOOR = 50;         // final designed floor of each difficulty tier

// True once the save has beaten Hell floor 50 (Hell == difficulty 2, floor climbed past 50).
// `floor` is a u32 so callers can pass m_level.savedFloor without a narrowing cast (the on-disk
// floor byte is a u8, but keeping the widest in-memory type avoids a truncation footgun).
inline bool saveCleared(u32 floor, u8 difficulty) {
    return difficulty >= 2u && floor > 50u;
}

// Clamp a (possibly stepped) floor into [MIN_FLOOR, MAX_FLOOR].
inline u8 clampFloor(s32 floor) {
    if (floor < static_cast<s32>(MIN_FLOOR)) return MIN_FLOOR;
    if (floor > static_cast<s32>(MAX_FLOOR)) return MAX_FLOOR;
    return static_cast<u8>(floor);
}

// Clamp a difficulty index into [0, DIFFICULTY_COUNT-1].
inline u8 clampDifficulty(s32 d) {
    if (d < 0) return 0;
    if (d > static_cast<s32>(DIFFICULTY_COUNT) - 1) return DIFFICULTY_COUNT - 1;
    return static_cast<u8>(d);
}

} // namespace FreePlay
