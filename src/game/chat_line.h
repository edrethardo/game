#pragma once

#include "core/types.h"
#include <cstdio>

// chat_line.h — the one rule for composing a HUD chat line.
//
// It is a pure header rather than three lines inline in Engine::addChatMessage because that is
// where it carried two defects for the whole life of the project, and neither was reachable by a
// test while the rule lived inside the engine:
//   * the buffer was 48 bytes, which cut nine of the ten quest blurbs mid-sentence — several
//     losing the clause that told the player what to DO;
//   * the format was an unconditional "%s: %s", so every SPEAKERLESS line (quest offers, act
//     completions, gate refusals, pickup names — the majority of callers) rendered with a stray
//     leading ": ".
// With the rule out here, tests/game/test_chat_line.cpp can pin both against the real quest table.
namespace Chat {

// Chat line capacity, including the NUL. Single-sourced here so Engine::ChatLine's buffer and the
// test that pins "no quest blurb is truncated" cannot drift apart.
//
// 128 is safe to DRAW, which is the only reason it is not larger still: the HUD chat is a
// left-anchored, unclipped FontSystem::drawText at (GLYPH_W + GLYPH_SPACING) = 6 px per glyph
// scaled by screenHeight/720, so a full 127-char line spans ~61% of a 16:9 screen (and ~81% of
// a 4:3 one) from a left margin of 15 px. No clamp at draw time is needed at this width.
inline constexpr u32 LINE_LEN = 128;

// Compose "speaker: message", or the bare message when `speaker` is null/empty. `dst` is always
// NUL-terminated (given cap > 0).
//
// Returns snprintf's own count — the length the line WOULD have needed, excluding the NUL — so a
// caller or a test can detect truncation as `format(...) >= cap` rather than by re-deriving the
// format string somewhere else and going stale.
inline u32 format(char* dst, u32 cap, const char* speaker, const char* msg) {
    if (!dst || cap == 0) return 0;
    if (!msg) msg = "";
    const int n = (speaker && speaker[0])
                      ? std::snprintf(dst, cap, "%s: %s", speaker, msg)
                      : std::snprintf(dst, cap, "%s", msg);
    return n < 0 ? 0u : static_cast<u32>(n);   // negative is an encoding error; report "empty"
}

} // namespace Chat
