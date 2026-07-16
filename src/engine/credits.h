#pragma once
// credits.h — the credits roll, single-sourced: the scroll-end value the CREDITS update case
// checks (engine_update.cpp) is derived from the SAME row table the renderer draws
// (engine_render.cpp), so the two can never drift apart — the quickbarLayout discipline.
//
// All values are in 720p-reference pixels; the renderer scales by uiScale (sh/720) and the
// update case advances m_creditsScroll in reference px, so pacing is resolution-independent.

#include "core/types.h"

namespace Credits {

struct Row {
    const char* text;   // "" = spacer line
    u8          size;   // FontSystem scale: 4 = headline, 3 = name/section, 2 = body
};

// Vertical distance from this row's baseline to the next (reference px).
inline f32 rowSpacing(u8 size) { return 14.0f + static_cast<f32>(size) * 10.0f; }

static constexpr Row kRows[] = {
    {"DUNGEON ENGINE",                       4},
    {"",                                     2},
    {"a game by",                            2},
    {"Ed Rethardo",                          3},
    {"",                                     2},
    {"",                                     2},
    {"design - code - art - audio",          2},
    {"Ed Rethardo",                          3},
    {"",                                     2},
    {"built with",                           2},
    {"SDL2 - ENet - OpenGL 3.3 - doctest",   2},
    {"",                                     2},
    {"AI pair programming",                  2},
    {"Claude",                               3},
    {"",                                     2},
    {"sounds",                               2},
    {"the OpenGameArt CC0 community",        2},
    {"",                                     2},
    {"",                                     2},
    {"and you.",                             3},
    {"",                                     2},
    {"Thanks for playing.",                  4},
};
static constexpr u32 ROW_COUNT = sizeof(kRows) / sizeof(kRows[0]);

// Cumulative offset of row i's baseline below row 0 (reference px).
inline f32 rowOffset(u32 i) {
    f32 off = 0.0f;
    for (u32 r = 0; r < i && r < ROW_COUNT; r++) off += rowSpacing(kRows[r].size);
    return off;
}

// Scroll value at which the LAST row has cleared the top of a 720p-reference screen —
// the update case hands off to VICTORY there (a keypress skips earlier).
inline f32 scrollEnd() { return rowOffset(ROW_COUNT - 1) + 720.0f + 60.0f; }

} // namespace Credits
