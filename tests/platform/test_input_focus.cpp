// test_input_focus.cpp — the window-focus input gate (platform/input_focus.h).
//
// Pins the rules that let the game sit unfocused on a second screen while Autoplay drives it:
// real devices go dead, the mouse is released, the delta is dropped on both edges — and the BOT
// keeps playing, because outside keyboard/mouse activity must not read as a human takeover.
#include "doctest/doctest.h"
#include "platform/input_focus.h"

TEST_CASE("input focus: real devices are readable only while focused") {
    CHECK(InputFocus::devicesReadable(true));
    CHECK_FALSE(InputFocus::devicesReadable(false));
}

TEST_CASE("input focus: relative mouse mode is never held while unfocused") {
    // Gameplay wants the grab, but tabbed out the cursor must belong to the desktop.
    CHECK(InputFocus::effectiveRelative(/*want=*/true, /*focused=*/true));
    CHECK_FALSE(InputFocus::effectiveRelative(/*want=*/true, /*focused=*/false));
    // A menu that does NOT want relative mode still doesn't get it back on refocus.
    CHECK_FALSE(InputFocus::effectiveRelative(/*want=*/false, /*focused=*/true));
    CHECK_FALSE(InputFocus::effectiveRelative(/*want=*/false, /*focused=*/false));
}

TEST_CASE("input focus: the cursor is never hidden while unfocused") {
    // The menu hides the cursor while it is keyboard-driven; that must not follow the pointer out.
    CHECK_FALSE(InputFocus::cursorShouldShow(/*want=*/false, /*focused=*/true));
    CHECK(InputFocus::cursorShouldShow(/*want=*/false, /*focused=*/false));
    CHECK(InputFocus::cursorShouldShow(/*want=*/true, /*focused=*/true));
}

TEST_CASE("input focus: pending mouse delta is dropped on BOTH focus edges") {
    CHECK(InputFocus::shouldDiscardDelta(/*was=*/true,  /*now=*/false));  // leaving: don't bank it
    CHECK(InputFocus::shouldDiscardDelta(/*was=*/false, /*now=*/true));   // returning: no aim snap
    CHECK_FALSE(InputFocus::shouldDiscardDelta(true,  true));             // steady state: keep it
    CHECK_FALSE(InputFocus::shouldDiscardDelta(false, false));
}

TEST_CASE("input focus: keyboard/mouse can't trigger Autoplay takeover while unfocused") {
    // THE bug this whole gate exists for: typing in another window handed control to a "human"
    // who wasn't there, and the game stood still with the bot benched.
    CHECK_FALSE(InputFocus::humanActivity(/*kbm=*/true, /*pad=*/false, /*focused=*/false));
    // Focused, the very same activity must take over instantly (the 2 s resume latch then runs).
    CHECK(InputFocus::humanActivity(/*kbm=*/true, /*pad=*/false, /*focused=*/true));
    // Idle is idle either way.
    CHECK_FALSE(InputFocus::humanActivity(false, false, true));
    CHECK_FALSE(InputFocus::humanActivity(false, false, false));
}

TEST_CASE("input focus: gamepad activity is deliberately NOT focus-gated") {
    // SDL reads pads as background devices regardless of focus, and picking up a controller is
    // unambiguous intent to play — unlike typing into some unrelated window.
    CHECK(InputFocus::humanActivity(/*kbm=*/false, /*pad=*/true, /*focused=*/false));
    CHECK(InputFocus::humanActivity(/*kbm=*/false, /*pad=*/true, /*focused=*/true));
}

TEST_CASE("input focus: fails OPEN until the window has been focused once") {
    // No window manager / kiosk X: SDL may never report input focus. Rather than leave the game
    // permanently input-dead with no way for the player to fix it, the gate stays disengaged.
    CHECK(InputFocus::effectiveFocused(/*sdlFocused=*/false, /*everFocused=*/false));
    CHECK(InputFocus::effectiveFocused(/*sdlFocused=*/true,  /*everFocused=*/false));
    // Once focus has been seen, an unfocused report is believed.
    CHECK_FALSE(InputFocus::effectiveFocused(/*sdlFocused=*/false, /*everFocused=*/true));
    CHECK(InputFocus::effectiveFocused(/*sdlFocused=*/true, /*everFocused=*/true));
}
