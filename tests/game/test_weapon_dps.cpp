// tests/game/test_weapon_dps.cpp — pins the single-source sustained-DPS cycle formula
// (extracted from build_score.h so the scorer and the balance lab cannot drift apart).
#include "doctest/doctest.h"
#include "game/weapon_dps.h"

TEST_CASE("no-clip weapon: dps = perHit / cooldown") {
    CHECK(WeaponDps::sustained(30.0f, 0.5f, 0.0f, 0.0f) == doctest::Approx(60.0f));
}

TEST_CASE("clip weapon pays the reload cycle") {
    // 8 shots x 12 dmg, 0.35 s between shots, 1.2 s reload: 96 / (2.8 + 1.2) = 24
    CHECK(WeaponDps::sustained(12.0f, 0.35f, 8.0f, 1.2f) == doctest::Approx(24.0f));
}

TEST_CASE("instant reload collapses to the no-clip formula") {
    CHECK(WeaponDps::sustained(12.0f, 0.35f, 8.0f, 0.0f)
          == doctest::Approx(WeaponDps::sustained(12.0f, 0.35f, 0.0f, 0.0f)));
}

TEST_CASE("expected crit multiplier") {
    CHECK(WeaponDps::expectedCritMult(0.05f, 2.0f) == doctest::Approx(1.05f));   // baseline
    CHECK(WeaponDps::expectedCritMult(0.20f, 2.5f) == doctest::Approx(1.30f));   // dagger
}
