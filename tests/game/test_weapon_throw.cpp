// test_weapon_throw.cpp — the pure melee-throw module (game/weapon_throw.h).
// Covers the CDR-immune cooldown length, the tenacity-scaled slow, and the tap-vs-hold arbiter.
#include "doctest/doctest.h"
#include "game/weapon_throw.h"

TEST_CASE("WeaponThrow cooldown is CDR-immune 90 ticks") {
    // 1.5 s * 60 Hz, with no (1 - cdr) term anywhere in the math.
    CHECK(WeaponThrow::cooldownTicks() == 90u);
}

TEST_CASE("WeaponThrow slow scales by tenacity, resist capped at 60%") {
    CHECK(WeaponThrow::slowDuration(0.0f) == doctest::Approx(0.2f));   // no resist -> full 0.2 s
    CHECK(WeaponThrow::slowDuration(0.5f) == doctest::Approx(0.1f));   // 50% resist -> half
    CHECK(WeaponThrow::slowDuration(0.9f) == doctest::Approx(0.08f));  // clamped to 0.60 -> 0.2*0.4
}

TEST_CASE("WeaponThrow bot policy: throw at ranged enemies beyond swing reach") {
    using WeaponThrow::botShouldThrow;
    const f32 reach = 4.3f;   // a sword
    // The case the feature exists for: a visible archer standing off, throw off cooldown.
    CHECK(botShouldThrow(/*melee=*/true, /*ready=*/true, /*ranged=*/true, /*los=*/true, 10.0f, reach));
    // Melee attacker: keep the weapon in hand, it closes the gap for you.
    CHECK_FALSE(botShouldThrow(true, true, /*ranged=*/false, true, 10.0f, reach));
    // Already in swing range: hitting it is free.
    CHECK_FALSE(botShouldThrow(true, true, true, true, reach, reach));
    // No line of sight / not a melee build / still cooling down.
    CHECK_FALSE(botShouldThrow(true, true, true, /*los=*/false, 10.0f, reach));
    CHECK_FALSE(botShouldThrow(/*melee=*/false, true, true, true, 10.0f, reach));
    CHECK_FALSE(botShouldThrow(true, /*ready=*/false, true, true, 10.0f, reach));
    // Across the map: not worth the weapon.
    CHECK_FALSE(botShouldThrow(true, true, true, true, WeaponThrow::BOT_THROW_MAX_M + 1.0f, reach));
}

TEST_CASE("WeaponThrow bot tap: release, brief press, release — and the press is a real TAP") {
    using namespace WeaponThrow;
    CHECK_FALSE(botTapFireHeld(0.0f));                 // phase 1: release clears decide()'s `held`
    CHECK(botTapFireHeld(BOT_TAP_CLEAR + 0.01f));      // phase 2: pressed
    CHECK_FALSE(botTapFireHeld(BOT_TAP_PRESS + 0.01f));// phase 3: released -> this is what throws
    CHECK(botTapDone(BOT_TAP_END));
    CHECK_FALSE(botTapDone(BOT_TAP_PRESS));
    // The press window MUST stay under TAP_SEC or decide() would read it as a hold and swing.
    CHECK((BOT_TAP_PRESS - BOT_TAP_CLEAR) < TAP_SEC);
    // ...and the tap must end after the press, so the release actually happens.
    CHECK(BOT_TAP_END > BOT_TAP_PRESS);
}

TEST_CASE("WeaponThrow decide: tap throws, hold swings, on-cooldown swings instantly") {
    using WeaponThrow::decide;
    const f32 dt = 1.0f / 60.0f;

    // Throw on cooldown -> swing immediately on press, never a throw.
    CHECK(decide(/*fireDown=*/true, /*wasDown=*/false, /*held=*/0.0f, dt, /*throwReady=*/false).doSwing);
    CHECK_FALSE(decide(true, false, 0.0f, dt, false).doThrow);

    // Throw ready, brief hold, still held -> WAIT (disambiguating): neither swing nor throw.
    auto waiting = decide(/*fireDown=*/true, /*wasDown=*/true, /*held=*/0.05f, dt, /*throwReady=*/true);
    CHECK_FALSE(waiting.doThrow);
    CHECK_FALSE(waiting.doSwing);

    // Throw ready, released after a short press -> THROW, hold accumulator reset.
    auto tapped = decide(/*fireDown=*/false, /*wasDown=*/true, /*held=*/0.1f, dt, /*throwReady=*/true);
    CHECK(tapped.doThrow);
    CHECK(tapped.newHeld == doctest::Approx(0.0f));

    // Throw ready, held past TAP_SEC -> committed to a swing, no throw on the eventual release.
    CHECK(decide(true, true, 0.25f, dt, true).doSwing);
    CHECK_FALSE(decide(true, true, 0.25f, dt, true).doThrow);

    // Release with no prior press (wasDown=false) never throws.
    CHECK_FALSE(decide(false, false, 0.0f, dt, true).doThrow);
}
