// tests/game/test_armor_mitigation.cpp — pins the armor curve the balance lab's EHP
// conversion rides on. Compiles WITHOUT combat.cpp: that is the point of the test —
// the function must be header-inline so tests-only code can use the real curve.
#include "doctest/doctest.h"
#include "game/combat.h"

TEST_CASE("armorMitigation: diminishing returns, 100 armor = 50%, hard cap 80%") {
    CHECK(Combat::armorMitigation(-5.0f)    == doctest::Approx(0.0f));
    CHECK(Combat::armorMitigation(0.0f)     == doctest::Approx(0.0f));
    CHECK(Combat::armorMitigation(100.0f)   == doctest::Approx(0.5f));
    CHECK(Combat::armorMitigation(300.0f)   == doctest::Approx(0.75f));
    CHECK(Combat::armorMitigation(400.0f)   == doctest::Approx(0.80f));   // knee: raw curve meets the cap
    CHECK(Combat::armorMitigation(10000.0f) == doctest::Approx(0.80f));   // cap
}
