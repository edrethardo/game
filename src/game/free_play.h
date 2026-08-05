// free_play.h — pure predicates for the post-clear Free-Play level select.
//
// A character has "cleared the game" once it beats floor 50 of Hell or deeper. Because
// triggerFloorDescent() saves the incremented floor BEFORE the floor-50 victory check
// (engine_update.cpp:1741 vs :1769), a cleared save sits at difficulty >= 2, floor 51+ (e.g. save_01
// = floor 57). Header-only and engine-free so it unit-tests without a GL/engine context.
#pragma once

#include "core/types.h"

namespace FreePlay {

inline constexpr u8 DIFFICULTY_COUNT = 4;   // Normal(0), Nightmare(1), Hell(2), Inferno(3)
inline constexpr u8 MIN_FLOOR = 1;
inline constexpr u8 MAX_FLOOR = 50;         // final designed floor of each difficulty tier

// The tier that ENDS the ladder — beating its floor 50 rolls the credits instead of promoting.
inline constexpr u8 FINAL_DIFFICULTY = DIFFICULTY_COUNT - 1;

// True once the save has beaten floor 50 of Hell or deeper.
//
// The threshold stays at HELL (2) rather than following FINAL_DIFFICULTY up to Inferno, and that is
// deliberate: every hero who cleared Hell before Inferno existed is stored as difficulty 2 / floor
// 51+, and this predicate is what gives them the town, the Free-Play select and a Continue that
// lands home. Raising it to 3 would silently un-clear all of them. Inferno is an EXTRA rung above
// the old ending, not a retroactive raise of what "cleared" means.
//
// `floor` is a u32 so callers can pass m_level.savedFloor without a narrowing cast (the on-disk
// floor byte is a u8, but keeping the widest in-memory type avoids a truncation footgun).
inline bool saveCleared(u32 floor, u8 difficulty) {
    return difficulty >= 2u && floor > 50u;
}

// The tier's display name. SINGLE-SOURCED on purpose: the names used to live in two separate
// `const char* diffNames[3]` literals (the Steam browser and the Free-Play select) plus an if/else
// chain for the floor banner and a ternary in the promotion log — four places to miss when a tier is
// added, two of them indexing a size-3 array that a 4th tier would read straight off the end.
// Out-of-range returns "Normal" rather than nullptr so every caller stays printf-safe.
inline const char* difficultyName(u8 d) {
    switch (d) {
        case 1:  return "Nightmare";
        case 2:  return "Hell";
        case 3:  return "Inferno";
        default: return "Normal";
    }
}

// The OVERWORLD (sentinel floors 52-96) is post-INFERNO content: it opens only once a character has
// beaten the final tier, not merely Hell.
//
// Deliberately a SEPARATE predicate from saveCleared rather than a raised threshold inside it.
// saveCleared's "Hell or deeper" test is what grants every pre-Inferno hero their town, Free-Play
// select and cleared Continue; raising it would silently take all of that away from them. The two
// answer different questions — "has this hero finished the game?" and "has this hero earned the
// overworld?" — and conflating them is how a gate change becomes a regression for old saves.
inline bool overworldUnlocked(u32 floor, u8 difficulty) {
    return difficulty >= FINAL_DIFFICULTY && floor > 50u;
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
