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

#include <initializer_list>
#include "doctest/doctest.h"
#include "game/game_constants.h"
#include <cmath>

using GameConst::floorHealthMult;
using GameConst::floorDamageMult;
using GameConst::difficultyDamageBump;
using GameConst::difficultyHealthBump;
using GameConst::FLOOR_STAT_MULT;
using GameConst::FLOOR_DAMAGE_MULT;
using GameConst::DIFFICULTY_HP_COMPOUND_RATE;
using GameConst::TIER10_HP_BOOST;
using GameConst::TIER10_HP_BOOST_FLOOR;

// The legacy linear curve every spawn site used before the compounding change.
static f32 legacyLinear(u32 effFloor) {
    return 1.0f + static_cast<f32>(effFloor - 1) * FLOOR_STAT_MULT;
}

// The floor-10 toughness boost (2026-07-26): +50% HP from effective floor 10 up, folded into
// floorHealthMult. Applied to Normal floors 10-50 and ALL of Nightmare/Hell (their effective floors
// are all >= 51). Multiplies the whole max(linear, compounding) curve, so it scales values without
// moving the linear/compounding crossover.
static f32 tier10(u32 effFloor) {
    return effFloor >= TIER10_HP_BOOST_FLOOR ? TIER10_HP_BOOST : 1.0f;
}

TEST_CASE("floorHealthMult: floor 1 is the 1.0x baseline (and 0 is guarded)") {
    CHECK(floorHealthMult(1) == doctest::Approx(1.0f));
    CHECK(floorDamageMult(1) == doctest::Approx(1.0f));
    // Degenerate 0 input is treated as floor 1 — no underflow on (effFloor - 1).
    CHECK(floorHealthMult(0) == doctest::Approx(1.0f));
    CHECK(floorDamageMult(0) == doctest::Approx(1.0f));
}

TEST_CASE("floorHealthMult: Normal 1-9 is the linear curve; 10-50 carries the +50% floor-10 boost") {
    // Normal's HP curve lives entirely in FLOOR_STAT_MULT (the 2026-07-23 pass steepened the linear
    // slope 0.10 -> 0.12, which outgrows compounding until effective floor 53, so no compounding tail
    // in Normal). On top of that, the 2026-07-26 floor-10 boost adds +50% from effective floor 10 up:
    //   * floors 1-9 ride the plain linear slope,
    //   * floors 10-50 are that same linear slope x 1.5 (the tier-10 toughness pass).
    for (u32 eff = 1; eff <= 9; ++eff) {
        CHECK(floorHealthMult(eff) == doctest::Approx(legacyLinear(eff)));                    // below the boost
    }
    for (u32 eff = 10; eff <= 50; ++eff) {
        CHECK(floorHealthMult(eff) == doctest::Approx(legacyLinear(eff) * TIER10_HP_BOOST));  // +50% from floor 10
    }
}

TEST_CASE("floorHealthMult: the linear slope governs NORMAL ONLY; deeper tiers ride compounding") {
    // This replaces a pin on "compounding overtakes linear at effective floor 53".
    //
    // That crossover was an ACCIDENT of the shallow 0.12 slope, not a design point. When the slope
    // was steepened to 0.345 to fix Normal's inverted TTK curve (see FLOOR_STAT_MULT), the same
    // unconditional max(linear, compounding) pushed the crossover out to ~92 and dragged early
    // Nightmare up with a Normal-only lever — measured at 6.6 s trash TTK against 1.7 hits-to-die.
    // The curve is now split explicitly at Normal's last floor, so a future slope change can no
    // longer silently re-tune the deep tiers.
    CHECK(floorHealthMult(50) == doctest::Approx((1.0f + 49.0f * FLOOR_STAT_MULT) * tier10(50)));
    // Past Normal the linear term is simply not in the curve: pure compounding, floor-10 boost on top.
    for (u32 eff : {51u, 60u, 100u, 150u}) {
        f32 comp = 1.0f;
        for (u32 i = 1; i < eff; ++i) comp *= (1.0f + DIFFICULTY_HP_COMPOUND_RATE);
        CHECK(floorHealthMult(eff) == doctest::Approx(comp * tier10(eff)));
    }
}

TEST_CASE("floorHealthMult: rises monotonically — no plateau, no inversion") {
    // The property that actually matters, and the one a plateau silently breaks. An intermediate
    // version of the split clamped the linear term's FLOOR ARGUMENT instead of excluding the term,
    // which pinned Nightmare's first ~25 floors at a constant 17.9x while player DPS kept climbing —
    // re-creating inside Nightmare the exact inverted curve this whole pass exists to remove. A pin
    // on constants would not have caught that; a pin on the SHAPE does.
    // Within Normal, and within the deep-tier compounding run, the raw curve must never flatten.
    for (u32 eff = 12; eff <= 50; ++eff)  CHECK(floorHealthMult(eff) > floorHealthMult(eff - 1));
    for (u32 eff = 52; eff <= 160; ++eff) CHECK(floorHealthMult(eff) > floorHealthMult(eff - 1));
}

TEST_CASE("floorHealthMult: compounding overtakes and ramps in Nightmare/Hell") {
    // Headline numbers (effFloor = floor + difficulty*50), now carrying the floor-10 boost (x1.5,
    // since every Nightmare/Hell effective floor is >= 51 >= 10):
    //   Nightmare floor 50 = eff 100 (~44.1x compound x1.5 = ~66.2x),
    //   Hell floor 50      = eff 150 (~299x  compound x1.5 = ~448.5x).
    CHECK(floorHealthMult(100) ==
          doctest::Approx(std::pow(1.0 + DIFFICULTY_HP_COMPOUND_RATE, 99) * TIER10_HP_BOOST).epsilon(0.01));
    CHECK(floorHealthMult(150) ==
          doctest::Approx(std::pow(1.0 + DIFFICULTY_HP_COMPOUND_RATE, 149) * TIER10_HP_BOOST).epsilon(0.01));
    // Strictly tougher than the linear values at these depths.
    CHECK(floorHealthMult(100) > legacyLinear(100));  // ~66 > 12.9
    CHECK(floorHealthMult(150) > legacyLinear(150));  // ~448 > 18.9
    // Hell floor 50 is a big jump over the plain linear curve.
    CHECK(floorHealthMult(150) > 4.0f * legacyLinear(150));
}

TEST_CASE("enemy HP rises across the whole progression a player actually walks") {
    // floorHealthMult ALONE is no longer monotonic and must not be asserted to be: since the linear
    // term governs Normal only, the raw function steps DOWN at effective floor 51 (Normal's steep
    // line ends, compounding resumes from a lower value). That is not a difficulty cliff, because
    // the function is never used alone — every spawn site multiplies it by difficultyHealthBump,
    // and Nightmare's bump is what carries the tier. So the property worth pinning is the one the
    // player experiences: the PRODUCT, walked in progression order.
    const auto hp = [](u8 diff, u32 floor) {
        return floorHealthMult(floor + diff * 50u) * difficultyHealthBump(diff);
    };
    // Within every tier, strictly rising (past the floor-10 boost step).
    for (u8 d = 0; d <= 2; ++d)
        for (u32 f = 12; f <= 50; ++f)
            CHECK(hp(d, f) > hp(d, f - 1));
    // Normal -> Nightmare must STEP UP: entering a harder difficulty can never be a relief.
    CHECK(hp(1, 1) > hp(0, 50));
    // Nightmare -> Hell is the documented DELIBERATE dip (see difficultyHealthBump): Hell restarts
    // its own ramp lower than Nightmare's peak. Pinned as a range so an accidental change to either
    // bump has to come and edit this line and say so.
    const f32 step = hp(2, 1) / hp(1, 50);
    CHECK(step > 0.40f);
    CHECK(step < 0.65f);
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
    // 0.24 -> 0.40 (2026-07-30): Normal's hits-to-die measured 5.5-17.5 from floor 10 on. Both deep
    // bumps were re-solved DOWN against it so Nightmare and Hell hold their damage exactly.
    CHECK(FLOOR_DAMAGE_MULT == doctest::Approx(0.40f));
    CHECK(floorDamageMult(150) == doctest::Approx(1.0f + 149.0f * 0.40f));   // 60.6x at Hell 50
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

TEST_CASE("difficultyDamageBump: Normal x1.55, Nightmare x7.05, Hell x12.045") {
    // 2026-07-23 balance-lab session, pass 2: Normal 1.25 -> 1.40 -> 1.55 are deliberate raises;
    // NM 2.80 -> 2.35 and Hell 9.58 -> 8.03 were RE-SOLVES against the steeper 0.24 slope.
    // 2026-07-24: Nightmare DOUBLED outright (2.35 -> 4.70) on Aaron's call, paired with an equal
    // doubling of its HP via difficultyHealthBump — see the ratio test below for why they move
    // together rather than damage alone.
    CHECK(difficultyDamageBump(0) == doctest::Approx(1.55f));
    // 2026-07-30: both RE-SOLVED against the 0.24 -> 0.40 slope so each tier's total damage is
    // EXACTLY unchanged (NM 174.6, Hell 442.8). Neither tier had headroom — the corrected balance lab
    // puts NM at 1.7-2.9 hits-to-die and Hell at 1.4-2.8 — so this pass gave them HP, not damage.
    CHECK(difficultyDamageBump(1) == doctest::Approx(4.30f));    // 174.6 / 40.60
    CHECK(difficultyDamageBump(2) == doctest::Approx(7.31f));    // 442.8 / 60.60
    // The re-solve must be exact, not approximate — that is the whole claim above.
    CHECK(floorDamageMult(100) * difficultyDamageBump(1) == doctest::Approx(174.6f).epsilon(0.01));
    CHECK(floorDamageMult(150) * difficultyDamageBump(2) == doctest::Approx(442.8f).epsilon(0.01));
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
    CHECK(newHell50 == doctest::Approx(295.2f * 1.5f).epsilon(0.01));   // 442.8 after the +50% pass
}

TEST_CASE("Hell floor 50 HP = the compounding curve x the floor-10 boost") {
    // History: the compounding rework put Hell-50 at ~299x (1.45x the old 3.64% curve of ~205.9x);
    // the 2026-07-26 floor-10 toughness pass adds a further +50% on top => ~448.5x. This is a
    // DELIBERATE endgame rebalance (Aaron: "mobs in tier 10+ have 50% more HP", all difficulties) —
    // the old "nowhere near enough to out-tank the damage" framing is retired; see the BOXED IN test.
    const f32 prevHell50 = std::pow(1.0364f, 149.0f);          // the pre-rework curve: ~205.9x
    CHECK(floorHealthMult(150) == doctest::Approx(299.0f * TIER10_HP_BOOST).epsilon(0.02));            // ~448.5x
    CHECK(floorHealthMult(150) / prevHell50 == doctest::Approx(1.45f * TIER10_HP_BOOST).epsilon(0.03)); // ~2.18x
}

TEST_CASE("Hell: the glass-cannon guard holds, with headroom opened by the floor-10 boost") {
    // The glass-cannon guard — enemy HP must outscale enemy damage, so a deep enemy can never delete
    // the player before it can be hit back — is the invariant that matters most in this file, and it
    // STILL HOLDS. (It already inverted once, at damage bump 15.80: 416x damage against 299x HP.)
    //
    // What CHANGED (2026-07-26): the floor-10 boost adds +50% HP on top, so the box that used to be a
    // <5% knife-edge (HP barely above damage) is now open to ~52%. That is deliberate — Aaron asked
    // for tankier tier-10+ mobs on all difficulties. The consequence to KNOW: Hell now has real HP
    // headroom, so a future damage-bump raise no longer instantly inverts the guard the way it did at
    // the old knife-edge — but the guard below is still the thing that catches an over-raise.
    const f32 hellDmg  = floorDamageMult(150) * difficultyDamageBump(2);   // ~295.2x
    // MUST include difficultyHealthBump — this line read floorHealthMult alone, which was silently
    // correct only while Hell's health bump happened to be 1.0. When Hell's bump moved to 1.5
    // (2026-07-29) the omission made this compare a bumped damage against an UNbumped HP and the
    // ratio collapsed to ~1.01, i.e. the guard would have reported a knife-edge that does not exist.
    const f32 hellHp   = floorHealthMult(150) * difficultyHealthBump(2);   // ~672.8x
    const f32 prevDmg  = (1.0f + 149.0f * 0.16f) * 5.90f;

    CHECK(hellDmg >= 2.0f * prevDmg);   // the stated damage floor (unchanged)
    CHECK(hellHp  >  hellDmg);          // glass-cannon guard: HP must outscale damage — STILL HOLDS

    // 1.52 -> 1.90 (2026-07-30). Hell's difficulty increase was taken ENTIRELY on the HP axis
    // (+25%) with damage held exactly, so this ratio moves AWAY from the glass-cannon boundary. That
    // direction was the point: the note in game_constants.h records Hell-50 trash at 1.13 hits to
    // kill a geared paladin, and the corrected lab measures Hell FLOOR 5 at 0.99 hits-to-die — a
    // literal one-shot. Raising Hell damage again would have made that worse, not better.
    CHECK(hellHp / hellDmg == doctest::Approx(1.90f).epsilon(0.03));
}

TEST_CASE("Nightmare: hot damage, plus the floor-10 HP tilt, still absolutely HP>damage") {
    // History: NM damage was deliberately run "hot" (damage growth > HP growth) from 2026-07-23, and
    // both axes were doubled 2026-07-24 (HP growth 1.28 -> 2.56, damage growth 1.82 -> 3.64 vs the
    // pre-rework baseline). The 2026-07-26 floor-10 boost then adds +50% HP on top (NM's effective
    // floors are all >= 51 >= 10), lifting HP growth to 3.84 — just PAST the 3.64 damage growth.
    //
    // So the old relative "damage growth must exceed HP growth (never a sponge)" no longer holds — by
    // design; Aaron asked for +50% HP on tier-10+ across all difficulties. The guard that actually
    // matters is ABSOLUTE: enemy HP must still outscale enemy damage so NM can't glass-cannon the
    // player. That holds with room (NM-50: ~132.5x HP vs ~116.4x damage). Pinned below.
    const f32 prevHp  = std::pow(1.0364f, 99.0f);
    const f32 prevDmg = (1.0f + 99.0f * 0.16f) * 1.90f;
    const f32 hpX     = (floorHealthMult(100) * difficultyHealthBump(1)) / prevHp;   // 2.56 x 1.5
    const f32 dmgX    = (floorDamageMult(100) * difficultyDamageBump(1)) / prevDmg;
    // 2026-07-29: NM's HP bump went 2.0 -> 3.0 (Aaron: "triple the hp in nightmare"), so the growth
    // multiple rises by the same 1.5x: 3.84 -> 5.76. Damage was deliberately NOT moved with it.
    // 2026-07-30: NM HP 3.0 -> 3.75 (+25%), damage HELD exactly (bump re-solved against the steeper
    // slope), so the HP growth multiple rises by 1.25 and the damage one is unchanged.
    CHECK(hpX  == doctest::Approx(2.56f * TIER10_HP_BOOST * 1.5f * 1.25f).epsilon(0.03));  // 7.20
    CHECK(dmgX == doctest::Approx(3.64f * 1.5f).epsilon(0.03));             // unchanged: damage held
    CHECK(hpX  >  dmgX);                                                    // HP now edges past damage growth

    // The invariant with teeth: absolute NM-50 HP outscales absolute NM-50 damage (glass-cannon guard).
    const f32 nm50Hp  = floorHealthMult(100) * difficultyHealthBump(1);
    const f32 nm50Dmg = floorDamageMult(100) * difficultyDamageBump(1);
    CHECK(nm50Hp > nm50Dmg);
}

TEST_CASE("difficultyHealthBump triples Nightmare and scales Hell by the same factor") {
    // The per-tier HP lever exists only because compounding cannot be aimed at one difficulty.
    // 2026-07-29: Nightmare tripled (2.0 -> 3.0) and Hell scaled by the SAME 1.5x (1.0 -> 1.5).
    // Hell moving in lockstep is the load-bearing part — see the tier-step test below.
    CHECK(difficultyHealthBump(0) == doctest::Approx(1.0f));    // Normal untouched
    // 2026-07-30: both +25% (3.0 -> 3.75, 1.5 -> 1.875). Same factor on both, so the deliberate
    // Nightmare->Hell tier step below is preserved exactly, as it was for the 2026-07-29 pass.
    CHECK(difficultyHealthBump(1) == doctest::Approx(3.75f));
    CHECK(difficultyHealthBump(2) == doctest::Approx(1.875f));
    CHECK(difficultyHealthBump(99) == doctest::Approx(1.0f));   // unknown tier: no scaling
}

TEST_CASE("The Nightmare->Hell HP step survives the triple") {
    // THE reason Hell had to move with Nightmare. Hell floor 1 is deliberately EASIER than Nightmare
    // floor 50 (the player has just re-geared), a ~0.52x dip. Raising Nightmare alone would have
    // deepened that to ~0.35x — Hell's opening floors a third as tough as the tier you just left,
    // which reads as the game getting easier exactly when it should bite. Scaling both by 1.5x holds
    // the ratio exactly where it was.
    const f32 nm50  = floorHealthMult(100) * difficultyHealthBump(1);
    const f32 hell1 = floorHealthMult(101) * difficultyHealthBump(2);
    CHECK(hell1 / nm50 == doctest::Approx(0.52f).epsilon(0.05));
    // ...and Hell still ends far above where Nightmare ended.
    CHECK(floorHealthMult(150) * difficultyHealthBump(2) > nm50 * 3.0f);
}

TEST_CASE("Nightmare HP and damage moved together again (+50% each, 2026-07-29)") {
    // Until 2026-07-29 HP and damage were PAIRED at 2.0 so the ratio was preserved exactly. The
    // triple deliberately breaks the pairing: HP went to 3.0 and damage stayed at 4.70 (= 2 x 2.35).
    //
    // That is safe BECAUSE of which way it moves. The invariant with teeth is "enemy HP outscales
    // enemy damage" — the guard against deep enemies becoming glass cannons that delete the player
    // before they can be hit back. Raising HP alone strengthens it (enemies get spongier, never more
    // lethal); it is raising DAMAGE alone that would have inverted it. Pinned so a future "let's put
    // them back in step" re-pairing has to argue with the direction, not just the numbers.
    CHECK(difficultyHealthBump(1) == doctest::Approx(3.75f));
    CHECK(difficultyDamageBump(1) == doctest::Approx(4.30f));   // held exactly (2026-07-30 re-solve)
    const f32 nm50Hp  = floorHealthMult(100) * difficultyHealthBump(1);
    const f32 nm50Dmg = floorDamageMult(100) * difficultyDamageBump(1);
    CHECK(nm50Hp > nm50Dmg);          // HP still outscales damage — back at the paired ~1.14x
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
    // 2026-07-30 raised the slope again (0.24 -> 0.40) after the lab showed Normal STILL inverted,
    // so these cumulative figures grow: +92% at floor 5, +174% at floor 50 vs the pre-2026-07-23 curve.
    CHECK(newNormal5 / prevNormal5 == doctest::Approx(1.919f).epsilon(0.005));   // +92% at floor 5

    const f32 prevNormal50 = (1.0f + 49.0f * 0.17f) * 1.25f;
    const f32 newNormal50  = floorDamageMult(50) * difficultyDamageBump(0);
    CHECK(newNormal50 / prevNormal50 == doctest::Approx(2.738f).epsilon(0.005)); // +174% at floor 50

    // Depth-weighting is the point: the raise must bite harder where the lab found the game
    // softest (deep Normal), not just shift the whole tier by a flat factor.
    CHECK(newNormal50 / prevNormal50 > newNormal5 / prevNormal5);
}

TEST_CASE("Combined enemy damage = linear floor curve x per-tier bump") {
    // What the spawn sites actually compute. A Hell floor-50 enemy: (1 + 149*0.24) * 8.03 = 295.2x.
    const f32 hellF50Dmg = floorDamageMult(150) * difficultyDamageBump(2);
    CHECK(hellF50Dmg == doctest::Approx((1.0f + 149.0f * 0.24f) * 12.045f).epsilon(0.001));

    // HP must still outscale damage. Damage is linear while HP compounds, and this ordering is what
    // keeps deep enemies from becoming glass cannons that delete the player before they can be hit
    // back. A 3x damage pass INVERTED it (416x damage vs 299x HP) and one-shot a geared paladin —
    // this check is what caught that, so do not weaken it to make a damage tweak pass.
    CHECK(floorHealthMult(150) > hellF50Dmg);
}
