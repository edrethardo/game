#pragma once

#include "core/types.h"

// input_focus.h — the window-focus input gate, expressed as PURE rules.
//
// WHY THIS EXISTS. The game is meant to be watchable on a second screen while the player works in
// another app (Autoplay driving itself). That only works if an unfocused window is a good desktop
// citizen, and SDL alone does not make it one:
//
//   * SDL's X11 backend selects XInput2 RAW pointer motion on the ROOT window while relative mouse
//     mode is on, and its XI_RawMotion handler gates ONLY on `mouse->relative_mode` — never on
//     keyboard focus. So an unfocused game keeps receiving every pointer delta made anywhere on the
//     desktop. It fed the aim, and (worse) it tripped Autoplay's human-takeover latch, so the bot
//     handed control to a "human" who was actually typing in another window and the game stood still.
//   * `mouse->relative_mode` staying true also keeps the OS cursor HIDDEN desktop-wide, which is
//     what "it captures my mouse" looks like from the outside.
//
// SDL does release the X11 pointer GRAB on focus loss, so the fix is not about the grab: it is about
// turning relative mode itself off while unfocused and refusing to read the devices.
//
// The rules live here (header-only, SDL-free) so they can be unit-tested and so there is exactly one
// written-down answer to "what does unfocused mean?". input.cpp is their only caller.
namespace InputFocus {

    // Fail-open. A window that has NEVER held focus (headless X with no window manager, some
    // remote-desktop / kiosk setups) would otherwise be input-dead forever with no way for the
    // player to fix it. Until focus has been observed at least once we behave exactly as before.
    constexpr bool effectiveFocused(bool sdlFocused, bool everFocused) {
        return sdlFocused || !everFocused;
    }

    // May a REAL keyboard/mouse read reach the game? Only while focused. Note this is deliberately
    // not asked of the Autoplay overlay: synthetic bot actions are OR'd in ahead of the device read
    // (checkActionRaw), so the bot keeps playing with the window tabbed out — the whole point.
    constexpr bool devicesReadable(bool focused) { return focused; }

    // Relative ("grabbed", cursor-hidden) mouse mode as actually pushed to SDL: the mode GAMEPLAY
    // asked for, but never ON while unfocused. The game's own desire is remembered untouched so
    // focusing again restores whatever the current screen wants (in-game yes, menu/inventory no).
    constexpr bool effectiveRelative(bool wantRelative, bool focused) {
        return wantRelative && focused;
    }

    // OS cursor visibility. The menu hides the cursor while it is keyboard/controller-driven; that
    // must not follow the pointer out of the window, so unfocused always shows it.
    constexpr bool cursorShouldShow(bool wantVisible, bool focused) {
        return wantVisible || !focused;
    }

    // Accumulated relative mouse delta must be dropped on BOTH focus edges — losing focus so the
    // motion made on the way out is not banked, and gaining it so a long cursor journey made in
    // another app cannot snap the aim on the first frame back.
    constexpr bool shouldDiscardDelta(bool wasFocused, bool nowFocused) {
        return wasFocused != nowFocused;
    }

    // Autoplay's takeover trigger: "a human touched a gameplay device this frame". Keyboard/mouse
    // activity only counts while focused. GAMEPAD activity is deliberately NOT focus-gated: SDL
    // reads pads as background devices regardless of focus, and picking up a controller is
    // unambiguous intent to play — unlike typing into some other window, which is not.
    constexpr bool humanActivity(bool kbmActive, bool padActive, bool focused) {
        return (kbmActive && focused) || padActive;
    }
}
