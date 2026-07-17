// test_crowd_control.cpp — pure CC math: tenacity duration scaling, the 60% resist cap,
// and the PvP stun diminishing-returns ladder. No engine deps (arena.h/stash.h pattern).
#include <doctest/doctest.h>
#include "game/crowd_control.h"

TEST_CASE("CC: tenacity scales duration, cap clamps at 0.60") {
    CHECK(CrowdControl::scaleDuration(2.0f, 0.0f)  == doctest::Approx(2.0f));
    CHECK(CrowdControl::scaleDuration(2.0f, 0.30f) == doctest::Approx(1.4f));
    CHECK(CrowdControl::scaleDuration(2.0f, 0.60f) == doctest::Approx(0.8f));
    // Over-cap resist is clamped by capResist, never by scaleDuration itself:
    CHECK(CrowdControl::capResist(0.95f) == doctest::Approx(0.60f));
    CHECK(CrowdControl::capResist(0.25f) == doctest::Approx(0.25f));
}

TEST_CASE("CC: stun DR ladder 1.0 -> 0.5 -> 0.25 -> 0.0, then window reset") {
    CrowdControl::StunDr dr;                       // fresh: count 0, timer 0
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(1.0f));  // 1st
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(0.5f));  // 2nd
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(0.25f)); // 3rd
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(0.0f));  // 4th: immune
    CHECK(dr.count == 4);
    // Window lapses -> count resets, next stun is full again.
    CrowdControl::tickStunDr(dr, 9.0f);            // 9s > 8s window
    CHECK(dr.count == 0);
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(1.0f));
}
