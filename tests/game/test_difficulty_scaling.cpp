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
using GameConst::FLOOR_DAMAGE_MULT;
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

TEST_CASE("floorHealthMult: Normal tier is byte-for-byte the legacy linear curve") {
    // Normal = effective floors 1..50. Even at the raised 3.64% rate the compounding curve stays
    // below linear here (it only overtakes at effective floor 52), so the max() clamp must reproduce
    // the old curve exactly — the base game is untouched. This is the guarantee that makes Hell
    // safe to crank: the lever aims itself.
    for (u32 eff = 1; eff <= 50; ++eff) {
        CHECK(floorHealthMult(eff) == doctest::Approx(legacyLinear(eff)));
    }
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

TEST_CASE("floorDamageMult: stays LINEAR (it must never compound)") {
    // Damage is deliberately linear while HP compounds. If damage ever compounded, a deep-floor
    // enemy would one-shot a player whose HP grows far slower — that asymmetry is the whole design.
    for (u32 eff = 1; eff <= 200; ++eff) {
        const f32 expect = 1.0f + static_cast<f32>(eff - 1) * FLOOR_DAMAGE_MULT;
        CHECK(floorDamageMult(eff) == doctest::Approx(expect));
    }
    CHECK(floorDamageMult(1) == doctest::Approx(1.0f));   // floor 1 is still the baseline
}

TEST_CASE("floorDamageMult: the damage slope is STEEPER than the health slope") {
    // 0.10 -> 0.13 -> 0.16, to pay for the gear-health fix: item health had never reached the player,
    // so a geared character is ~3x tankier than every enemy number was tuned against. This slope
    // feeds EVERY difficulty, which is why it only ever moves in small steps — it is the one lever
    // that cannot be aimed at Hell alone (difficultyDamageBump is, and carries the rest).
    CHECK(FLOOR_DAMAGE_MULT > FLOOR_STAT_MULT);
    CHECK(FLOOR_DAMAGE_MULT == doctest::Approx(0.16f));
    CHECK(floorDamageMult(150) == doctest::Approx(1.0f + 149.0f * 0.16f));   // 24.8x at Hell 50
}

TEST_CASE("difficultyDamageBump: Normal x1.25, Nightmare x1.90, Hell x5.90") {
    // The per-tier bump is the only HELL-ISOLATED lever, so it carries the heavy end of the increase
    // while the shared slope moves only a little. Normal is no longer the identity: a Normal player
    // gets item health too, so a Normal enemy has to hit harder.
    CHECK(difficultyDamageBump(0) == doctest::Approx(1.25f));
    CHECK(difficultyDamageBump(1) == doctest::Approx(1.90f));
    CHECK(difficultyDamageBump(2) == doctest::Approx(5.90f));
    // Unexpected values fall back to Normal rather than misbehaving.
    CHECK(difficultyDamageBump(99) == doctest::Approx(1.25f));
    // Ordering is the invariant that actually matters: deeper tier => strictly more damage.
    CHECK(difficultyDamageBump(0) < difficultyDamageBump(1));
    CHECK(difficultyDamageBump(1) < difficultyDamageBump(2));
}

TEST_CASE("Hell floor 50 has 2.5x the HP it used to") {
    // The other half of the brief. The rate is SOLVED, not eyeballed: (1+r)^149 must land on 2.5x
    // the old 3%-curve value. Pinned so a later nudge to the rate shows up as a broken promise
    // rather than as a quietly different endgame.
    const f32 oldHell50 = std::pow(1.03f, 149.0f);            // the previous curve: ~81.8x
    CHECK(floorHealthMult(150) / oldHell50 == doctest::Approx(2.5f).epsilon(0.02));
    CHECK(floorHealthMult(150) == doctest::Approx(205.9f).epsilon(0.02));
}

TEST_CASE("Hell floor 50 deals exactly 3x the damage it used to") {
    // The brief, as an assertion. 5.90 is not a taste value — it is SOLVED so that
    // (1 + 0.16*149) * bump lands on 3.00x the previous Hell-50 damage. If someone later nudges the
    // slope or the bump "a little", this is what tells them they moved the endgame off its target.
    const f32 oldHell50 = (1.0f + 149.0f * 0.13f) * 2.40f;   // the previous curve: 48.9x
    const f32 newHell50 = floorDamageMult(150) * difficultyDamageBump(2);
    CHECK(newHell50 / oldHell50 == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(newHell50 == doctest::Approx(146.7f).epsilon(0.01));
}

TEST_CASE("The shared slope keeps Normal's collateral small") {
    // The slope feeds every tier, so raising it taxes Normal too. That is tolerable only while it
    // stays small: Normal floor 5 must not become a meat grinder because Hell needed to be harder.
    const f32 oldNormal5  = (1.0f + 4.0f * 0.13f) * 1.15f;
    const f32 newNormal5  = floorDamageMult(5) * difficultyDamageBump(0);
    CHECK(newNormal5 / oldNormal5 < 1.25f);      // ~+17%: a nudge, not a re-tune

    const f32 oldNormal50 = (1.0f + 49.0f * 0.13f) * 1.15f;
    const f32 newNormal50 = floorDamageMult(50) * difficultyDamageBump(0);
    CHECK(newNormal50 / oldNormal50 < 1.40f);    // ~+30%
}

TEST_CASE("Combined enemy damage = linear floor curve x per-tier bump") {
    // What the spawn sites actually compute. A Hell floor-50 enemy: (1 + 149*0.16) * 5.90 = 146.7x.
    const f32 hellF50Dmg = floorDamageMult(150) * difficultyDamageBump(2);
    CHECK(hellF50Dmg == doctest::Approx((1.0f + 149.0f * 0.16f) * 5.90f).epsilon(0.001));

    // HP must still outscale damage. Damage is linear while HP compounds, and this ordering is what
    // keeps deep enemies from becoming glass cannons that delete the player before they can be hit
    // back. Tripling the damage briefly INVERTED it (147x damage vs 82x HP) until the HP raise
    // landed — this check is what caught that, so do not weaken it to make a damage tweak pass.
    CHECK(floorHealthMult(150) > hellF50Dmg);
}
