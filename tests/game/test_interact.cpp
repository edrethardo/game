// test_interact.cpp — the interact button's priority table and tap/hold machine.
//
// Worth pinning because every failure here is silent and player-facing: a wrong priority row does
// not crash, it quietly steals the item you reached for (or descends a floor you did not mean to
// leave), and a wrong edge in the hold machine either fires twice on one press or never fires at
// all. None of it shows up in a smoke test.

#include <doctest/doctest.h>
#include "game/interact.h"

using Interact::Intent;
using Interact::Target;
using Interact::HoldState;

static constexpr f32 HOLD = 0.35f;
static constexpr f32 DT   = 1.0f / 60.0f;

// --- The priority table ---

TEST_CASE("Interact: a tap takes the ITEM even when a shrine and the exit are in reach") {
    // The whole point of the feature: loot is what you grab constantly, so nothing you are merely
    // standing on may eat the grab.
    CHECK(Interact::choose(Intent::TAP, true, true, true) == Target::ITEM);
    CHECK(Interact::choose(Intent::TAP, true, true, false) == Target::ITEM);
    CHECK(Interact::choose(Intent::TAP, true, false, true) == Target::ITEM);
}

TEST_CASE("Interact: a HOLD reaches past the item to the shrine") {
    CHECK(Interact::choose(Intent::HOLD, true, true, true) == Target::SHRINE);
    CHECK(Interact::choose(Intent::HOLD, true, true, false) == Target::SHRINE);
}

TEST_CASE("Interact: a HOLD takes the exit only when no shrine is in reach") {
    CHECK(Interact::choose(Intent::HOLD, true, false, true) == Target::EXIT);
    CHECK(Interact::choose(Intent::HOLD, false, false, true) == Target::EXIT);
}

TEST_CASE("Interact: with no loot competing, a tap is enough for a shrine or the exit") {
    // Otherwise the player would have to hold for EVERY shrine, which is a tax on the common case.
    CHECK(Interact::choose(Intent::TAP, false, true, true) == Target::SHRINE);
    CHECK(Interact::choose(Intent::TAP, false, false, true) == Target::EXIT);
}

TEST_CASE("Interact: holding with only an item in reach does nothing") {
    // The tap already took it on the press edge; a hold must not then fire a second action.
    CHECK(Interact::choose(Intent::HOLD, true, false, false) == Target::NONE);
}

TEST_CASE("Interact: NONE intent never acts, whatever is in reach") {
    for (int i = 0; i < 8; i++) {
        const bool item = (i & 1) != 0, shrine = (i & 2) != 0, exit = (i & 4) != 0;
        CHECK(Interact::choose(Intent::NONE, item, shrine, exit) == Target::NONE);
    }
}

TEST_CASE("Interact: an empty reach yields nothing") {
    CHECK(Interact::choose(Intent::TAP,  false, false, false) == Target::NONE);
    CHECK(Interact::choose(Intent::HOLD, false, false, false) == Target::NONE);
}

// --- The tap/hold machine ---

TEST_CASE("Interact: with nothing to disambiguate, a tap fires on the PRESS edge") {
    // Plain looting must stay instant — it must not wait for release just because the feature exists.
    HoldState st;
    CHECK(Interact::poll(st, /*down=*/true, /*hasHoldTarget=*/false, DT, HOLD) == Intent::TAP);
    // ...and exactly once, however long the button stays down.
    for (int i = 0; i < 60; i++)
        CHECK(Interact::poll(st, true, false, DT, HOLD) == Intent::NONE);
}

TEST_CASE("Interact: with a hold target, the tap is deferred to RELEASE") {
    // If it fired on press, press-and-hold would already have taken the item and "hold for the
    // shrine" could never work. This is the load-bearing edge of the whole design.
    HoldState st;
    CHECK(Interact::poll(st, true, /*hasHoldTarget=*/true, DT, HOLD) == Intent::NONE);
    CHECK(Interact::poll(st, true, true, DT, HOLD) == Intent::NONE);
    CHECK(Interact::poll(st, /*down=*/false, true, DT, HOLD) == Intent::TAP);   // released quickly
}

TEST_CASE("Interact: holding past the threshold fires HOLD exactly once") {
    HoldState st;
    int holds = 0, taps = 0;
    for (int i = 0; i < 120; i++) {   // 2 seconds down
        const Intent in = Interact::poll(st, true, true, DT, HOLD);
        if (in == Intent::HOLD) holds++;
        if (in == Intent::TAP)  taps++;
    }
    CHECK(holds == 1);   // never repeats while the button stays down
    CHECK(taps == 0);

    // Releasing after a hold must NOT also emit a tap — that would fire both actions on one press.
    CHECK(Interact::poll(st, false, true, DT, HOLD) == Intent::NONE);
}

TEST_CASE("Interact: HOLD fires at the threshold, not before") {
    HoldState st;
    f32 t = 0.0f;
    Intent fired = Intent::NONE;
    for (int i = 0; i < 120 && fired == Intent::NONE; i++) {
        fired = Interact::poll(st, true, true, DT, HOLD);
        t += DT;
    }
    REQUIRE(fired == Intent::HOLD);
    CHECK(t >= doctest::Approx(HOLD).epsilon(0.02));
}

TEST_CASE("Interact: releasing right at the threshold does not double-fire") {
    // The boundary between "quick tap" and "hold" must belong to exactly one of them.
    HoldState st;
    Intent last = Intent::NONE;
    while (last == Intent::NONE) last = Interact::poll(st, true, true, DT, HOLD);
    CHECK(last == Intent::HOLD);
    CHECK(Interact::poll(st, false, true, DT, HOLD) == Intent::NONE);
}

TEST_CASE("Interact: state resets between presses") {
    HoldState st;
    // Press 1: full hold.
    Intent i1 = Intent::NONE;
    while (i1 == Intent::NONE) i1 = Interact::poll(st, true, true, DT, HOLD);
    CHECK(i1 == Intent::HOLD);
    Interact::poll(st, false, true, DT, HOLD);   // release
    CHECK(st.held == doctest::Approx(0.0f));
    CHECK_FALSE(st.consumed);

    // Press 2: a quick tap must still be a tap (a stale `consumed` would swallow it).
    CHECK(Interact::poll(st, true, true, DT, HOLD) == Intent::NONE);
    CHECK(Interact::poll(st, false, true, DT, HOLD) == Intent::TAP);
}

TEST_CASE("Interact: a button never pressed produces nothing") {
    HoldState st;
    for (int i = 0; i < 30; i++)
        CHECK(Interact::poll(st, false, true, DT, HOLD) == Intent::NONE);
    CHECK(st.held == doctest::Approx(0.0f));
}

TEST_CASE("Interact: a hold target appearing mid-press does not retro-fire a tap") {
    // Press with only an item in reach → tap fires immediately (consumed). If a shrine then comes
    // into reach while the button is still down, the press is already spent: it must stay quiet
    // rather than also activating the shrine.
    HoldState st;
    CHECK(Interact::poll(st, true, false, DT, HOLD) == Intent::TAP);
    for (int i = 0; i < 60; i++)
        CHECK(Interact::poll(st, true, /*hasHoldTarget=*/true, DT, HOLD) == Intent::NONE);
}

// ---------------------------------------------------------------------------
// inReach — the aim cone must not apply to the item you are standing on.
//
// The bug: the "standing on it" exemption was hDist <= 0.1 m, on a player 0.7 m wide. Walk over an
// item and stop, and it typically sits 0.2-0.5 m away — outside the exemption, and as often behind
// your eyeline as in front of it. The dot then went negative, the cone refused the grab, and the
// pickup silently did nothing. Whether it worked depended on exactly where you stopped, which is
// what "picking up items is sometimes flaky" looks like from the player's side.
// ---------------------------------------------------------------------------

static constexpr f32 RANGE = 3.5f;
static constexpr f32 GRAB  = 1.2f;
static constexpr f32 MIN_DOT = 0.3f;

TEST_CASE("InReach: THE BUG — an item underfoot but BEHIND you is reachable") {
    // 0.3 m away, dot = -1 (directly behind). Old rule: 0.3 > 0.1, so the cone applied, -1 < 0.3,
    // refused. This is the exact case that ate the pickup.
    CHECK(Interact::inReach(0.3f, -1.0f, RANGE, GRAB, MIN_DOT));
}

TEST_CASE("InReach: anywhere inside the grab radius is reachable, whatever the facing") {
    for (f32 d = 0.0f; d <= GRAB; d += 0.1f) {
        INFO("dist ", d);
        CHECK(Interact::inReach(d, -1.0f, RANGE, GRAB, MIN_DOT));   // facing dead away
        CHECK(Interact::inReach(d,  0.0f, RANGE, GRAB, MIN_DOT));   // side-on
        CHECK(Interact::inReach(d,  1.0f, RANGE, GRAB, MIN_DOT));   // looking right at it
    }
}

TEST_CASE("InReach: beyond the grab radius you must still AIM") {
    // The cone is what lets you pick ONE item out of a scattered pile at range. Keep it.
    CHECK_FALSE(Interact::inReach(2.0f, -1.0f, RANGE, GRAB, MIN_DOT));   // behind you, out of reach
    CHECK_FALSE(Interact::inReach(2.0f,  0.0f, RANGE, GRAB, MIN_DOT));   // side-on
    CHECK_FALSE(Interact::inReach(2.0f,  0.29f, RANGE, GRAB, MIN_DOT));  // just outside the cone
    CHECK(Interact::inReach(2.0f,  0.30f, RANGE, GRAB, MIN_DOT));        // just inside it
    CHECK(Interact::inReach(2.0f,  1.0f, RANGE, GRAB, MIN_DOT));         // straight ahead
}

TEST_CASE("InReach: range still bounds everything") {
    // Even looking straight at it, past INTERACT_RANGE is out of reach — the grab radius widens the
    // near field, it must not extend the far one.
    CHECK_FALSE(Interact::inReach(3.6f, 1.0f, RANGE, GRAB, MIN_DOT));
    CHECK(Interact::inReach(3.4f, 1.0f, RANGE, GRAB, MIN_DOT));
}

TEST_CASE("InReach: the grab radius sits between the body and the aimed range") {
    // A grab radius inside the player's own half-width would reintroduce the bug; one at/over the
    // full range would delete the aim cone entirely and let you vacuum up loot behind you.
    CHECK(GRAB > 0.35f);     // wider than the player's half-width (PLAYER_HALF_WIDTH)
    CHECK(GRAB < RANGE);     // but the cone still governs the far field
}

TEST_CASE("inReach refuses a target on another storey") {
    // Reach was horizontal-only, so on a stacked floor you could target — and grab — loot lying one,
    // two or three stories below your feet, through solid slab. Stories are 3 m apart.
    const f32 range = 3.5f, grab = 1.0f, minDot = 0.5f;

    // Same storey, dead ahead and close: reachable, as always.
    CHECK(Interact::inReach(2.0f, 1.0f, range, grab, minDot, 0.0f));
    // Same storey but on a step/ledge: still reachable — the bound must not be so tight it refuses
    // ordinary terrain.
    CHECK(Interact::inReach(2.0f, 1.0f, range, grab, minDot, 0.5f));
    CHECK(Interact::inReach(2.0f, 1.0f, range, grab, minDot, 1.9f));
    // A storey down (3 m) — refused however perfectly you are aiming or however close in XZ.
    CHECK_FALSE(Interact::inReach(2.0f, 1.0f, range, grab, minDot, 3.0f));
    CHECK_FALSE(Interact::inReach(0.2f, 1.0f, range, grab, minDot, 3.0f));   // even underfoot
    // Three stories down (the Descent's full stack).
    CHECK_FALSE(Interact::inReach(1.0f, 1.0f, range, grab, minDot, 9.0f));
    // Default argument keeps every existing flat-floor caller behaving exactly as before.
    CHECK(Interact::inReach(2.0f, 1.0f, range, grab, minDot));
}
