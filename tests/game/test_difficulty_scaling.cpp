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

TEST_CASE("floorHealthMult: Normal floors 1-45 are byte-for-byte the legacy linear curve") {
    // This test USED to assert all of Normal (1..50) was untouched. That promise is gone, on purpose.
    //
    // Tripling Hell's HP needed the compounding rate at 4.41%, and compounding cannot be aimed at one
    // tier: raising it moved the crossover (where compounding overtakes the legacy linear curve) from
    // effective floor 52 down to 46. So the last few floors of Normal DO get tougher now. That is the
    // "normal only gets a wee bit harder" that was asked for.
    //
    // The honest guarantee is what is pinned here: everything up to floor 45 is still EXACTLY the old
    // curve, so the early and middle game is bit-for-bit what it always was. Do not widen this back to
    // 50 — it would not pass, and the point of the test is to state where the line actually is.
    for (u32 eff = 1; eff <= 45; ++eff) {
        CHECK(floorHealthMult(eff) == doctest::Approx(legacyLinear(eff)));
    }
}

TEST_CASE("floorHealthMult: the tail of Normal gets tougher, but only a wee bit") {
    // The other side of the same coin. Floors 36-50 now ride the compounding curve. It must be a
    // nudge, not a re-tune: if Normal 50 ever more than doubled, the base game would have been
    // re-balanced as a side effect of a Hell change, which is exactly what nobody asked for.
    CHECK(floorHealthMult(46) > legacyLinear(46));           // crossover has happened by here
    CHECK(floorHealthMult(45) == doctest::Approx(legacyLinear(45)));  // ...but not before it

    const f32 normal50 = floorHealthMult(50) / legacyLinear(50);
    CHECK(normal50 > 1.0f);
    CHECK(normal50 < 1.20f);                                 // ~+10%: a whisker
}

TEST_CASE("floorHealthMult: never below linear anywhere (change only ever adds difficulty)") {
    for (u32 eff = 1; eff <= 160; ++eff) {
        CHECK(floorHealthMult(eff) >= legacyLinear(eff) - 1e-4f);
    }
}

TEST_CASE("floorHealthMult: compounding overtakes and ramps in Nightmare/Hell") {
    // Headline numbers (effFloor = floor + difficulty*50):
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
    // 0.10 -> 0.13 -> 0.16 -> 0.18 -> 0.17. This slope feeds EVERY difficulty, which is why it only ever moves
    // in small steps — it is the one lever that cannot be aimed at Hell alone.
    CHECK(FLOOR_DAMAGE_MULT > FLOOR_STAT_MULT);
    CHECK(FLOOR_DAMAGE_MULT == doctest::Approx(0.17f));
    CHECK(floorDamageMult(150) == doctest::Approx(1.0f + 149.0f * 0.17f));   // 26.3x at Hell 50
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

TEST_CASE("difficultyDamageBump: Normal x1.25, Nightmare x2.30, Hell x11.20") {
    CHECK(difficultyDamageBump(0) == doctest::Approx(1.25f));
    CHECK(difficultyDamageBump(1) == doctest::Approx(2.30f));
    CHECK(difficultyDamageBump(2) == doctest::Approx(11.20f));
    // Unexpected values fall back to Normal rather than misbehaving.
    CHECK(difficultyDamageBump(99) == doctest::Approx(1.25f));
    // Ordering is the invariant that actually matters: deeper tier => strictly more damage.
    CHECK(difficultyDamageBump(0) < difficultyDamageBump(1));
    CHECK(difficultyDamageBump(1) < difficultyDamageBump(2));
}

TEST_CASE("Hell floor 50 damage is AT LEAST double what it was — the stated hard floor") {
    // The one damage requirement that was stated as a floor rather than a target, so it is pinned as a
    // floor. A 3x pass was tried and pulled back (it one-shot a fully geared paladin); 2x is what
    // survived. If a later re-tune drops under this, it broke a promise.
    const f32 prevHell50 = (1.0f + 149.0f * 0.16f) * 5.90f;    // the previous curve: 146.6x
    const f32 newHell50  = floorDamageMult(150) * difficultyDamageBump(2);
    CHECK(newHell50 >= 2.0f * prevHell50);
    CHECK(newHell50 == doctest::Approx(294.9f).epsilon(0.01));
}

TEST_CASE("Hell floor 50 HP went up, but nowhere near enough to out-tank the damage cap") {
    // The HP rate was pulled back 4.41% -> 3.9% because tripling HP turned a Hell-50 trash mob into a
    // 23-swing slog. It still rises (1.45x the 3.64% curve), just not absurdly.
    const f32 prevHell50 = std::pow(1.0364f, 149.0f);          // the previous curve: ~205.9x
    CHECK(floorHealthMult(150) / prevHell50 == doctest::Approx(1.45f).epsilon(0.03));
    CHECK(floorHealthMult(150) == doctest::Approx(299.0f).epsilon(0.02));
}

TEST_CASE("The Hell bump is BOXED IN by the 2x floor and the HP>damage invariant") {
    // The two requirements bracket the Hell bump from both sides, and at a 3.9% HP rate the gap
    // between them is only [11.14, 11.36] — 11.20 sits inside it with almost no room.
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

TEST_CASE("Nightmare is neither a bullet sponge nor a glass cannon") {
    // The trap, as an assertion — and it has teeth in BOTH directions, which is why it stays.
    //
    // Compounding cannot be aimed at one tier, so DIFFICULTY_HP_COMPOUND_RATE sets Nightmare's HP
    // whether anyone meant it to or not (now x1.28). Nightmare's DAMAGE comes from a separate per-tier
    // bump. Let those drift apart and Nightmare breaks one of two ways:
    //   * bump too LOW  -> tanky enemies that barely hit harder: a sponge. (At 4.41%/1.90 it was
    //                      heading for x2.07 HP against x1.18 damage.)
    //   * bump too HIGH -> a glass cannon. (Pulling the rate back to 3.9% while leaving the bump at
    //                      3.53 would have given x1.97 damage against x1.28 HP.)
    //
    // So the bump is SOLVED to make Nightmare's damage growth track its own HP growth. Any future edit
    // to the HP rate must re-solve it, and this is what catches a forgetful one.
    const f32 prevHp  = std::pow(1.0364f, 99.0f);
    const f32 prevDmg = (1.0f + 99.0f * 0.16f) * 1.90f;
    const f32 hpX     = floorHealthMult(100) / prevHp;
    const f32 dmgX    = (floorDamageMult(100) * difficultyDamageBump(1)) / prevDmg;
    CHECK(hpX  == doctest::Approx(1.28f).epsilon(0.03));
    CHECK(dmgX == doctest::Approx(hpX).epsilon(0.05));   // damage tracks tankiness, neither way
}

TEST_CASE("The shared slope keeps Normal's collateral small") {
    // The slope feeds every tier, so raising it taxes Normal too — and since it buys nothing in Hell
    // (see the NORMAL-dial test above), that tax is the ONLY thing it does. Keep it a nudge.
    const f32 prevNormal5  = (1.0f + 4.0f * 0.16f) * 1.25f;
    const f32 newNormal5   = floorDamageMult(5) * difficultyDamageBump(0);
    CHECK(newNormal5 / prevNormal5 < 1.05f);      // ~+2%

    const f32 prevNormal50 = (1.0f + 49.0f * 0.16f) * 1.25f;
    const f32 newNormal50  = floorDamageMult(50) * difficultyDamageBump(0);
    CHECK(newNormal50 / prevNormal50 < 1.10f);    // ~+6%: a whisker, not a re-tune
}

TEST_CASE("Combined enemy damage = linear floor curve x per-tier bump") {
    // What the spawn sites actually compute. A Hell floor-50 enemy: (1 + 149*0.17) * 11.20 = 294.9x.
    const f32 hellF50Dmg = floorDamageMult(150) * difficultyDamageBump(2);
    CHECK(hellF50Dmg == doctest::Approx((1.0f + 149.0f * 0.17f) * 11.20f).epsilon(0.001));

    // HP must still outscale damage. Damage is linear while HP compounds, and this ordering is what
    // keeps deep enemies from becoming glass cannons that delete the player before they can be hit
    // back. A 3x damage pass INVERTED it (416x damage vs 299x HP) and one-shot a geared paladin —
    // this check is what caught that, so do not weaken it to make a damage tweak pass.
    CHECK(floorHealthMult(150) > hellF50Dmg);
}
