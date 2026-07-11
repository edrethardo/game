#pragma once

#include "core/types.h"

// On-screen keyboard (OSK) for the desktop Host-IP entry screen (menu subState 9).
//
// Why this exists: Steam "Full Controller Support" requires every function to be reachable
// with a controller alone. The Join → Host-IP screen was keyboard-scancode-only (PC assumed a
// physical keyboard; Input::openVirtualKeyboard is a no-op off-Switch), so a gamepad user could
// navigate to it but had no way to type an address. This grid lets a controller move a cursor
// (D-pad) and press A to enter characters.
//
// This header is the SINGLE SOURCE OF TRUTH for the key layout, shared by the input handler
// (engine_menu.cpp) and the renderer (engine_render_menus.cpp) so the highlight the player sees
// and the character that gets typed can never disagree.
namespace MenuOsk {
    // Character set matches the physical-keyboard scancode map for subState 9: decimal digits,
    // IPv6 hex digits a-f, ':' and '[' ']' for IPv6 literals, and '.' for IPv4 — so controller
    // and keyboard entry accept exactly the same input (no regression). The two trailing entries
    // are control keys (sentinels, not literal characters): backspace and done/connect.
    // Lobby-code entry does NOT reuse this set — it has its own keyboard (CodeOsk, below) so the
    // address screen stays exactly as tight as it was.
    static constexpr char BACKSPACE = '\b';
    static constexpr char DONE      = '\n';
    static constexpr char KEYS[] = {
        '0','1','2','3','4','5','6','7',
        '8','9','a','b','c','d','e','f',
        '.',':','[',']', BACKSPACE, DONE,
    };
    static constexpr u32 COUNT = sizeof(KEYS) / sizeof(KEYS[0]);   // 22
    static constexpr u32 COLS  = 8;                                // grid width
    static constexpr u32 ROWS  = (COUNT + COLS - 1) / COLS;        // 3 (last row partially filled)

    inline bool isBackspace(u32 i) { return i < COUNT && KEYS[i] == BACKSPACE; }
    inline bool isDone(u32 i)      { return i < COUNT && KEYS[i] == DONE; }

    // Move the flat cursor index by (dx, dy) grid steps, clamping to the grid. Stepping onto an
    // empty trailing cell (the last row isn't full) snaps back to the last valid key so the
    // cursor can never point past the array.
    inline u32 moveCursor(u32 cur, s32 dx, s32 dy) {
        if (cur >= COUNT) cur = COUNT - 1;
        s32 r = static_cast<s32>(cur / COLS) + dy;
        s32 c = static_cast<s32>(cur % COLS) + dx;
        if (r < 0) r = 0;
        if (r > static_cast<s32>(ROWS) - 1) r = static_cast<s32>(ROWS) - 1;
        if (c < 0) c = 0;
        if (c > static_cast<s32>(COLS) - 1) c = static_cast<s32>(COLS) - 1;
        u32 idx = static_cast<u32>(r) * COLS + static_cast<u32>(c);
        if (idx >= COUNT) idx = COUNT - 1;   // clamp into the partial last row
        return idx;
    }
}

// On-screen keyboard for the LOBBY-CODE entry screen (menu subState 21).
//
// Deliberately a SEPARATE keyboard from MenuOsk rather than a widened shared one: the address screen
// only ever wants digits + hex + IP punctuation, and stuffing 26 letters into it to serve a different
// screen would make typing an IP worse for everyone. Two small focused layouts beat one bloated set.
//
// Keys are exactly the code alphabet — Crockford base-32, which omits I, L, O and U (ambiguous
// glyphs / accidental profanity). There is nothing else to type: no separators, since a code is only
// 4 glyphs. Same single-source-of-truth contract as MenuOsk — the input handler (engine_menu.cpp) and
// the renderer (engine_render_menus.cpp) both read this, so the highlighted key and the typed
// character can never disagree.
namespace CodeOsk {
    static constexpr char BACKSPACE = '\b';
    static constexpr char DONE      = '\n';
    static constexpr char KEYS[] = {
        '0','1','2','3','4','5','6','7',
        '8','9','A','B','C','D','E','F',
        'G','H','J','K','M','N','P','Q',
        'R','S','T','V','W','X','Y','Z',
        BACKSPACE, DONE,
    };
    static constexpr u32 COUNT = sizeof(KEYS) / sizeof(KEYS[0]);   // 34
    static constexpr u32 COLS  = 8;                                // grid width
    static constexpr u32 ROWS  = (COUNT + COLS - 1) / COLS;        // 5 (last row partially filled)

    inline bool isBackspace(u32 i) { return i < COUNT && KEYS[i] == BACKSPACE; }
    inline bool isDone(u32 i)      { return i < COUNT && KEYS[i] == DONE; }

    // Same clamping grid walk as MenuOsk::moveCursor — see the note there.
    inline u32 moveCursor(u32 cur, s32 dx, s32 dy) {
        if (cur >= COUNT) cur = COUNT - 1;
        s32 r = static_cast<s32>(cur / COLS) + dy;
        s32 c = static_cast<s32>(cur % COLS) + dx;
        if (r < 0) r = 0;
        if (r > static_cast<s32>(ROWS) - 1) r = static_cast<s32>(ROWS) - 1;
        if (c < 0) c = 0;
        if (c > static_cast<s32>(COLS) - 1) c = static_cast<s32>(COLS) - 1;
        u32 idx = static_cast<u32>(r) * COLS + static_cast<u32>(c);
        if (idx >= COUNT) idx = COUNT - 1;   // clamp into the partial last row
        return idx;
    }
}
