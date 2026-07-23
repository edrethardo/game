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
// enemies tougher, never weaker — the Normal tier must come out EXACTLY equal to the linear
// curve. These tests pin that clamp, monotonic growth, the headline tier multipliers
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

TEST_CASE("floorHealthMult: ALL of Normal (1-50) is byte-for-byte the linear curve") {
    // The crossover (where compounding overtakes the linear curve) has moved twice:
    //   * raising the compounding rate to 4.41% pulled it from effective floor 52 down to 46,
    //     so for a while Normal 46-50 picked up a small compounding tail;
    //   * the 2026-07-23 balance-lab pass steepened the linear HP slope 0.10 -> 0.12, which
    //     outgrows compounding until effective floor 53 — so the tail is gone again and ALL of
    //     Normal (raw floors 1-50 = effective 1-50) rides the linear slope.
    //
    // Note what this pins: Normal's HP curve now lives entirely in FLOOR_STAT_MULT. Normal DID
    // get tougher in that pass — via the slope itself (that was the point), not via compounding.
    for (u32 eff = 1; eff <= 50; ++eff) {
        CHECK(floorHealthMult(eff) == doctest::Approx(legacyLinear(eff)));
    }
}

TEST_CASE("floorHealthMult: compounding overtakes the linear slope at effective floor 53") {
    // The crossover, pinned exactly so a future slope/rate tweak states its consequences here.
    // Effective floor 53 = Nightmare floor 3: compounding governs from early Nightmare onward.
    CHECK(floorHealthMult(52) == doctest::Approx(legacyLinear(52)));  // linear still wins...
    CHECK(floorHealthMult(53) > legacyLinear(53));                    // ...compounding from here
}

TEST_CASE("floorHealthMult: never below linear anywhere (change only ever adds difficulty)") {
    for (u32 eff = 1; eff <= 160; ++eff) {
        CHECK(floorHealthMult(eff) >= legacyLinear(eff) - 1e-4f);
    }
}

TEST_CASE("floorHealthMult: compounding overtakes and ramps in Nightmare/Hell") {
    // Headline numbers (effFloor = floor + difficulty*50):
    //   Nightmare floor 50 = eff 100 (~44.1x), Hell floor 50 = eff 150 (~299x).
    CHECK(floorHealthMult(100) ==
          doctest::Approx(std::pow(1.0 + DIFFICULTY_HP_COMPOUND_RATE, 99)).epsilon(0.01));
    CHECK(floorHealthMult(150) ==
          doctest::Approx(std::pow(1.0 + DIFFICULTY_HP_COMPOUND_RATE, 149)).epsilon(0.01));
    // Strictly tougher than the linear values at these depths.
    CHECK(floorHealthMult(100) > legacyLinear(100));  // ~44.1 > 12.9
    CHECK(floorHealthMult(150) > legacyLinear(150));  // ~299 > 18.9
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
    // 0.10 -> 0.13 -> 0.16 -> 0.18 -> 0.17 -> 0.20 -> 0.24. This slope feeds EVERY difficulty, which
    // is why it only ever moves in small steps — it is the one lever that cannot be aimed at Hell
    // alone. (0.20 and 0.24 are the two 2026-07-23 balance-lab passes: the depth-weighted Normal
    // lever, with the NM + Hell bumps re-solved down each time so both tiers stood still.)
    CHECK(FLOOR_DAMAGE_MULT > FLOOR_STAT_MULT);
    CHECK(FLOOR_DAMAGE_MULT == doctest::Approx(0.24f));
    CHECK(floorDamageMult(150) == doctest::Approx(1.0f + 149.0f * 0.24f));   // 36.8x at Hell 50
}

TEST_CASE("The damage slope is a NORMAL dial, not a Hell one") {
    // The counter-intuitive fact that governs every future tweak to this file, pinned so nobody
    // rediscovers it the hard way.
    //
    // Hell sits at effective floors 101-150, where (1 + slope*149) is dominated by the slope term and
    // the "+1" is noise. So if you steepen the slope and re-solve the Hell bump to hit the same
    // target, the two cancel: Hell lands in the same place. Steepening the slope to "make the endgame
    // hurt" does nothing except tax Normal.
    //
    // Demonstrated directly: rebuild Hell-50 damage with a 10% steeper slope and its correspondingly
    // re-solved bump, and the answer barely moves.
    const f32 hell50   = floorDamageMult(150) * difficultyDamageBump(2);
    const f32 slopeB   = FLOOR_DAMAGE_MULT * 1.10f;
    const f32 bumpB    = hell50 / (1.0f + 149.0f * slopeB);          // re-solved for the SAME target
    const f32 hell1_A  = (1.0f + 100.0f * FLOOR_DAMAGE_MULT) * difficultyDamageBump(2);
    const f32 hell1_B  = (1.0f + 100.0f * slopeB) * bumpB;
    CHECK(hell1_B / hell1_A == doctest::Approx(1.0f).epsilon(0.01));  // Hell floor 1: unchanged

    // ...while Normal moves by the full 10%.
    const f32 norm50_A = (1.0f + 49.0f * FLOOR_DAMAGE_MULT) * difficultyDamageBump(0);
    const f32 norm50_B = (1.0f + 49.0f * slopeB) * difficultyDamageBump(0);
    CHECK(norm50_B > norm50_A * 1.05f);
}

TEST_CASE("difficultyDamageBump: Normal x1.55, Nightmare x2.35, Hell x8.03") {
    // 2026-07-23 balance-lab session, pass 2: Normal 1.25 -> 1.40 -> 1.55 are deliberate raises;
    // NM 2.80 -> 2.35 and Hell 9.58 -> 8.03 are RE-SOLVES against the steeper 0.24 slope (NM-50
    // holds pass-1's 58.2x, Hell-50 holds ~295x — both tiers stand still).
    CHECK(difficultyDamageBump(0) == doctest::Approx(1.55f));
    CHECK(difficultyDamageBump(1) == doctest::Approx(2.35f));
    CHECK(difficultyDamageBump(2) == doctest::Approx(8.03f));
    // Unexpected values fall back to Normal rather than misbehaving.
    CHECK(difficultyDamageBump(99) == doctest::Approx(1.55f));
    // Ordering is the invariant that actually matters: deeper tier => strictly more damage.
    CHECK(difficultyDamageBump(0) < difficultyDamageBump(1));
    CHECK(difficultyDamageBump(1) < difficultyDamageBump(2));
}

TEST_CASE("Hell floor 50 damage is AT LEAST double what it was — the stated hard floor") {
    // The one damage requirement that was stated as a floor rather than a target, so it is pinned as a
    // floor. A 3x pass was tried and pulled back (it one-shot a fully geared paladin); 2x is what
    // survived. If a later re-tune drops under this, it broke a promise.
    //
    // The 2026-07-23 slope raises re-solved the Hell bump each time so this total STOOD STILL:
    // 36.76 x 8.03 = 295.2x vs the original 26.33 x 11.20 = 294.9x (+0.1%). Note the margin over
    // the 2x floor is razor thin (295.2 vs 293.1) — a Hell bump below 7.98 breaks the promise.
    const f32 prevHell50 = (1.0f + 149.0f * 0.16f) * 5.90f;    // the pre-rework curve: 146.6x
    const f32 newHell50  = floorDamageMult(150) * difficultyDamageBump(2);
    CHECK(newHell50 >= 2.0f * prevHell50);
    CHECK(newHell50 == doctest::Approx(295.2f).epsilon(0.01));
}

TEST_CASE("Hell floor 50 HP went up, but nowhere near enough to out-tank the damage cap") {
    // The HP rate was pulled back 4.41% -> 3.9% because tripling HP turned a Hell-50 trash mob into a
    // 23-swing slog. It still rises (1.45x the 3.64% curve), just not absurdly.
    const f32 prevHell50 = std::pow(1.0364f, 149.0f);          // the previous curve: ~205.9x
    CHECK(floorHealthMult(150) / prevHell50 == doctest::Approx(1.45f).epsilon(0.03));
    CHECK(floorHealthMult(150) == doctest::Approx(299.0f).epsilon(0.02));
}

TEST_CASE("The Hell bump is BOXED IN by the 2x floor and the HP>damage invariant") {
    // The two requirements bracket the Hell bump from both sides, and at the 3.9% HP rate and 0.24
    // damage slope the gap between them is only [7.98, 8.13] — 8.03 sits inside it with almost no
    // room. (The box travels with the slope: [11.14, 11.36] at 0.17, [9.52, 9.71] at 0.20; the
    // 2026-07-23 re-solves moved the number, not the squeeze.)
    //
    // This is the test that matters most in this file. It says: you cannot raise Hell's damage any
    // further WITHOUT first buying HP headroom by raising DIFFICULTY_HP_COMPOUND_RATE. Someone who
    // just nudges the bump up to "make Hell hurt" will invert the glass-cannon invariant, and this is
    // what tells them. (It already happened once, at bump 15.80: 416x damage against 299x HP.)
    const f32 hellDmg  = floorDamageMult(150) * difficultyDamageBump(2);
    const f32 hellHp   = floorHealthMult(150);
    const f32 prevDmg  = (1.0f + 149.0f * 0.16f) * 5.90f;

    CHECK(hellDmg >= 2.0f * prevDmg);   // lower bound: the stated damage floor
    CHECK(hellHp  >  hellDmg);          // upper bound: HP must still outscale damage

    // And the window really is that tight — the headroom is under 5%.
    CHECK(hellHp / hellDmg < 1.05f);
}

TEST_CASE("Nightmare runs deliberately hotter than its HP-parity solve") {
    // Compounding cannot be aimed at one tier, so DIFFICULTY_HP_COMPOUND_RATE sets Nightmare's HP
    // whether anyone meant it to or not (x1.28 vs the pre-rework curve). Nightmare's DAMAGE comes
    // from a separate per-tier bump, and 2.30 used to be SOLVED so its damage growth tracked that
    // x1.28 HP growth exactly — this test used to assert dmgX == hpX.
    //
    // That parity was DELIBERATELY broken on 2026-07-23: the balance lab's first report measured NM
    // at 12-19 hits-to-die mid-late tier (closer to Normal's safety than Hell's threat), and Aaron's
    // call was "a bit harder". Pass 1 (bump 2.80 @ slope 0.20) set NM-50 damage growth at x1.82 vs
    // the pre-rework baseline against x1.28 HP growth; pass 2's 2.35 @ slope 0.24 is that SAME heat
    // re-solved (58.24/24.76 ~= 2.35 — NM-50 stays 58.2x, totals drift under 1% across the tier).
    // Hot ON PURPOSE, and pinned exactly so the heat stays a decision rather than drift. The sponge
    // direction still has teeth: damage growth must never fall BELOW HP growth again (that is the
    // bullet-sponge failure the old solve fixed).
    //
    // If the HP rate ever moves, re-derive the parity bump first and re-apply the deliberate heat on
    // top — do not treat 2.35 as a parity number (see difficultyDamageBump's comment).
    const f32 prevHp  = std::pow(1.0364f, 99.0f);
    const f32 prevDmg = (1.0f + 99.0f * 0.16f) * 1.90f;
    const f32 hpX     = floorHealthMult(100) / prevHp;
    const f32 dmgX    = (floorDamageMult(100) * difficultyDamageBump(1)) / prevDmg;
    CHECK(hpX  == doctest::Approx(1.28f).epsilon(0.03));
    CHECK(dmgX >  hpX);                                  // never a sponge again
    CHECK(dmgX == doctest::Approx(1.82f).epsilon(0.03)); // the deliberate heat, pinned
}

TEST_CASE("Normal's damage raise is deliberate, depth-weighted, and pinned") {
    // This test used to assert Normal's collateral from the slope stayed a whisker (<+6%). That
    // promise was RETIRED on purpose on 2026-07-23: the balance lab measured deep Normal as the
    // safest place in the game (~30 hits-to-die at floor 40, TTK falling with depth), so Normal was
    // made harder ON PURPOSE, in two passes the same session — bump 1.25 -> 1.40 -> 1.55 (flat)
    // plus slope 0.17 -> 0.20 -> 0.24 (depth-weighted). The pins below state the CUMULATIVE size of
    // that decision vs the pre-session curve (0.17 slope, 1.25 bump) so any future drift has to be
    // re-stated here as a number.
    const f32 prevNormal5  = (1.0f + 4.0f * 0.17f) * 1.25f;
    const f32 newNormal5   = floorDamageMult(5) * difficultyDamageBump(0);
    CHECK(newNormal5 / prevNormal5 == doctest::Approx(1.447f).epsilon(0.005));   // +45% at floor 5

    const f32 prevNormal50 = (1.0f + 49.0f * 0.17f) * 1.25f;
    const f32 newNormal50  = floorDamageMult(50) * difficultyDamageBump(0);
    CHECK(newNormal50 / prevNormal50 == doctest::Approx(1.696f).epsilon(0.005)); // +70% at floor 50

    // Depth-weighting is the point: the raise must bite harder where the lab found the game
    // softest (deep Normal), not just shift the whole tier by a flat factor.
    CHECK(newNormal50 / prevNormal50 > newNormal5 / prevNormal5);
}

TEST_CASE("Combined enemy damage = linear floor curve x per-tier bump") {
    // What the spawn sites actually compute. A Hell floor-50 enemy: (1 + 149*0.24) * 8.03 = 295.2x.
    const f32 hellF50Dmg = floorDamageMult(150) * difficultyDamageBump(2);
    CHECK(hellF50Dmg == doctest::Approx((1.0f + 149.0f * 0.24f) * 8.03f).epsilon(0.001));

    // HP must still outscale damage. Damage is linear while HP compounds, and this ordering is what
    // keeps deep enemies from becoming glass cannons that delete the player before they can be hit
    // back. A 3x damage pass INVERTED it (416x damage vs 299x HP) and one-shot a geared paladin —
    // this check is what caught that, so do not weaken it to make a damage tweak pass.
    CHECK(floorHealthMult(150) > hellF50Dmg);
}
