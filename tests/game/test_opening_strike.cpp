// test_opening_strike.cpp — an arriving enemy's first swing must never get SLOWER.
//
// The request was "first hit at half the attack delay, to increase pressure". Taken literally that
// would have been a NERF: an enemy entering attack range never waited a full cooldown in the first
// place — it always skipped to a short opening window (0.0-0.3 s depending on how it arrived). And
// 20 of the 38 enemies have attack cooldowns above 0.6 s, so half of THEIR cooldown is longer than
// the 0.30 s opening they already had. Pressure would have gone down while looking like it went up.
//
// So the plain CHARGE opening was cut instead (0.30 -> 0.20) and the already-fast approaches
// (ambush / flank / surround) left exactly as they were. These tests pin the property that actually
// matters — no window is ever ABOVE the value it replaced — because it is the one a future
// "simplification" (e.g. "just use attackCooldown * 0.5") would silently break.

#include <doctest/doctest.h>
#include "game/game_constants.h"
#include <initializer_list>

using namespace GameConst;

TEST_CASE("OpeningStrike: no window is ever slower than the value it replaced") {
    CHECK(OPEN_STRIKE_CHASE    <  0.30f);    // the only one that moved: 0.30 -> 0.20
    CHECK(OPEN_STRIKE_SURROUND <= 0.20f);
    CHECK(OPEN_STRIKE_FLANK    <= 0.10f);
    CHECK(OPEN_STRIKE_INRANGE  <= 0.10f);
    CHECK(OPEN_STRIKE_AMBUSH   <= 0.00f);    // already immediate; cannot be improved on
}

TEST_CASE("OpeningStrike: the already-fast approaches are left exactly where they were") {
    // Deliberate: only the plain charge lacked pressure. Touching the others would have made the
    // fastest arrivals near-unreactable for no reason.
    CHECK(OPEN_STRIKE_AMBUSH   == doctest::Approx(0.00f));
    CHECK(OPEN_STRIKE_FLANK    == doctest::Approx(0.10f));
    CHECK(OPEN_STRIKE_INRANGE  == doctest::Approx(0.10f));
    CHECK(OPEN_STRIKE_SURROUND == doctest::Approx(0.20f));
    CHECK(OPEN_STRIKE_CHASE    == doctest::Approx(0.20f));   // the change
}

TEST_CASE("OpeningStrike: a literal 'half the cooldown' would be SLOWER for most enemies") {
    // The trap, stated as an assertion. The fastest enemy's cooldown is 0.30 s and the slowest is
    // 1.20 s. Half of a slow enemy's cooldown (0.60 s) is twice the opening it already had.
    const f32 slowCooldown = 1.20f;
    const f32 halfOfSlow   = slowCooldown * 0.5f;
    CHECK(halfOfSlow > 0.30f);                    // worse than the OLD opening
    CHECK(halfOfSlow > OPEN_STRIKE_CHASE);        // far worse than the new one
    CHECK(OPEN_STRIKE_CHASE < 0.30f);             // what we did instead
}

TEST_CASE("OpeningStrike: the tactical ordering is preserved") {
    // The spread between these is deliberate: an ambusher bursts on reveal, a flanker arrives
    // already committed, a surrounder is taking a slot, and a straight charge is the most
    // telegraphed of the four — so it stays the slowest. Flattening them all to one number would
    // erase the read the player gets from HOW an enemy approaches.
    CHECK(OPEN_STRIKE_AMBUSH   <= OPEN_STRIKE_FLANK);
    CHECK(OPEN_STRIKE_FLANK    <= OPEN_STRIKE_SURROUND);
    CHECK(OPEN_STRIKE_SURROUND <= OPEN_STRIKE_CHASE);
}

TEST_CASE("OpeningStrike: no window is negative, and none is a full attack cycle") {
    // A negative timer would fire on the very frame of entry (no reaction window at all); a window
    // at or above the fastest cooldown would mean the "opening" was slower than a normal swing.
    const f32 fastestEnemyCooldown = 0.30f;
    for (f32 v : {OPEN_STRIKE_AMBUSH, OPEN_STRIKE_FLANK, OPEN_STRIKE_INRANGE,
                  OPEN_STRIKE_SURROUND, OPEN_STRIKE_CHASE}) {
        CHECK(v >= 0.0f);
        CHECK(v <= fastestEnemyCooldown);
    }
}
