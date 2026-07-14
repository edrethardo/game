// Tests for the difficulty / floor enemy-scaling helpers in game_constants.h.
//
// Difficulty adds +50 effective floors per tier (Normal +0, Nightmare +50, Hell +100).
// HEALTH compounds off the effective floor (floorHealthMult) so Nightmare/Hell ramp
// exponentially instead of the old flat +10%/floor, which flattened relatively as it climbed
// and made the high tiers feel too easy. DAMAGE stays linear (floorDamageMult) with a flat
// per-tier bump (difficultyDamageBump) so it can't compound into one-shots (player HP scales
// far slower than the enemy's effective-floor count).
//
// floorHealthMult is clamped to max(linear, compounding) so the change can only ever make
// enemies tougher, never weaker — the Normal tier must come out EXACTLY equal to the old
// linear curve. These tests pin that clamp, monotonic growth, the headline tier multipliers
// we balanced around, and the exact damage-bump table.

#include "doctest/doctest.h"
#include "game/game_constants.h"
#include <cmath>

using GameConst::floorHealthMult;
using GameConst::floorDamageMult;
using GameConst::difficultyDamageBump;
using GameConst::FLOOR_STAT_MULT;
using GameConst::DIFFICULTY_HP_COMPOUND_RATE;

// The legacy linear curve every spawn site used before the compounding change.
static f32 legacyLinear(u32 effFloor) {
    return 1.0f + static_cast<f32>(effFloor - 1) * FLOOR_STAT_MULT;
}

TEST_CASE("floorHealthMult: floor 1 is the 1.0x baseline (and 0 is guarded)") {
    CHECK(floorHealthMult(1) == doctest::Approx(1.0f));
    CHECK(floorDamageMult(1) == doctest::Approx(1.0f));
    // Degenerate 0 input is treated as floor 1 — no underflow on (effFloor - 1).
    CHECK(floorHealthMult(0) == doctest::Approx(1.0f));
    CHECK(floorDamageMult(0) == doctest::Approx(1.0f));
}

TEST_CASE("floorHealthMult: floors below the crossover are bit-for-bit the legacy linear curve") {
    // At the 4% rate the compounding term only overtakes linear at effective floor 44, so
    // everything below it must still reproduce the old curve exactly. This is the guarantee that
    // raising the rate did not quietly re-tune the early game: floors 1-43 are untouched.
    for (u32 eff = 1; eff <= 43; ++eff) {
        CHECK(floorHealthMult(eff) == doctest::Approx(legacyLinear(eff)));
    }
}

TEST_CASE("floorHealthMult: the top of Normal is now deliberately tougher than linear") {
    // The 3% -> 4% raise was chosen to lift ALL THREE tiers, not just Hell, so the last handful of
    // Normal floors DO get harder. Pinned so nobody "restores" the old behaviour by accident while
    // believing they are fixing a regression.
    CHECK(floorHealthMult(43) == doctest::Approx(legacyLinear(43)));   // last linear floor
    CHECK(floorHealthMult(44) > legacyLinear(44));                     // crossover
    CHECK(floorHealthMult(50) == doctest::Approx(6.83f).epsilon(0.01)); // was 5.9x
}

TEST_CASE("floorHealthMult: never below linear anywhere (change only ever adds difficulty)") {
    for (u32 eff = 1; eff <= 160; ++eff) {
        CHECK(floorHealthMult(eff) >= legacyLinear(eff) - 1e-4f);
    }
}

TEST_CASE("floorHealthMult: compounding overtakes and ramps in Nightmare/Hell") {
    // Headline numbers we balanced around (effFloor = floor + difficulty*50):
    //   Nightmare floor 50 = eff 100 (~18.7x), Hell floor 50 = eff 150 (~81.8x).
    CHECK(floorHealthMult(100) ==
          doctest::Approx(std::pow(1.0 + DIFFICULTY_HP_COMPOUND_RATE, 99)).epsilon(0.01));
    CHECK(floorHealthMult(150) ==
          doctest::Approx(std::pow(1.0 + DIFFICULTY_HP_COMPOUND_RATE, 149)).epsilon(0.01));
    // Strictly tougher than the old linear values at these depths.
    CHECK(floorHealthMult(100) > legacyLinear(100));  // ~18.7 > 10.9
    CHECK(floorHealthMult(150) > legacyLinear(150));  // ~81.8 > 15.9
    // Hell floor 50 should be a big jump over today's Hell floor 50.
    CHECK(floorHealthMult(150) > 4.0f * legacyLinear(150));
}

TEST_CASE("floorHealthMult: monotonic non-decreasing in effective floor") {
    f32 prev = floorHealthMult(1);
    for (u32 eff = 2; eff <= 200; ++eff) {
        f32 cur = floorHealthMult(eff);
        CHECK(cur >= prev);
        prev = cur;
    }
}

TEST_CASE("floorDamageMult: stays linear (does NOT compound)") {
    for (u32 eff = 1; eff <= 200; ++eff) {
        CHECK(floorDamageMult(eff) == doctest::Approx(legacyLinear(eff)));
    }
}

TEST_CASE("difficultyDamageBump: Normal x1, Nightmare x1.5, Hell x3") {
    CHECK(difficultyDamageBump(0) == doctest::Approx(1.0f));
    CHECK(difficultyDamageBump(1) == doctest::Approx(1.5f));
    CHECK(difficultyDamageBump(2) == doctest::Approx(3.0f));   // 2.0 -> 3.0: Hell was too easy
    // Unexpected values fall back to Normal (no scaling) rather than misbehaving.
    CHECK(difficultyDamageBump(99) == doctest::Approx(1.0f));
}

TEST_CASE("Combined enemy damage = linear floor curve x per-tier bump") {
    // What the spawn sites actually compute for enemy damage. A Hell floor-50 enemy:
    // linear(eff150) * 3.0 = 15.9 * 3 = 47.7x base damage.
    f32 hellF50Dmg = floorDamageMult(150) * difficultyDamageBump(2);
    CHECK(hellF50Dmg == doctest::Approx(15.9f * 3.0f).epsilon(0.001));
    // HP must still outscale damage by a wide margin. Damage is deliberately LINEAR while HP
    // compounds: if this inverted, a Hell enemy would one-shot a player whose HP grows far slower.
    CHECK(floorHealthMult(150) > hellF50Dmg);
}

TEST_CASE("floorHealthMult: the new Hell/Nightmare curve is pinned") {
    // The whole point of the 3% -> 4% raise. Pin the endpoints so a future tweak to the rate is a
    // conscious act with a visible diff, not a silent balance drift.
    CHECK(floorHealthMult(100) == doctest::Approx(48.56f).epsilon(0.01));   // Nightmare 50: was 18.7x
    CHECK(floorHealthMult(150) == doctest::Approx(345.1f).epsilon(0.01));   // Hell 50:      was 81.8x
}
