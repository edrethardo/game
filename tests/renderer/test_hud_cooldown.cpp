// test_hud_cooldown.cpp — unit tests for the pure HUD cooldown/urgency/pop helpers
// (renderer/hud_cooldown_util.h). No GL context needed; the header is inline-only.
#include "doctest/doctest.h"
#include "renderer/hud_cooldown_util.h"

using namespace HudCooldown;

TEST_CASE("cooldownSeconds ceils to whole seconds, min 1 while visible") {
    CHECK(cooldownSeconds(5.0f) == 5);
    CHECK(cooldownSeconds(4.1f) == 5);
    CHECK(cooldownSeconds(0.9f) == 1);
    CHECK(cooldownSeconds(0.05f) == 1);
}

TEST_CASE("showCooldownNumber hides the final sliver") {
    CHECK(showCooldownNumber(1.0f));
    CHECK(showCooldownNumber(0.21f));
    CHECK_FALSE(showCooldownNumber(0.2f));
    CHECK_FALSE(showCooldownNumber(0.0f));
}

TEST_CASE("potionUrgent fires only when ready AND low HP") {
    const f32 LOW = 0.25f;
    CHECK(potionUrgent(0.20f, 0.0f, LOW));        // low + ready
    CHECK(potionUrgent(0.25f, 0.0f, LOW));        // exactly at threshold
    CHECK_FALSE(potionUrgent(0.20f, 1.0f, LOW));  // low but still cooling
    CHECK_FALSE(potionUrgent(0.50f, 0.0f, LOW));  // ready but healthy
}

TEST_CASE("readyPopT maps and clamps a POP_DURATION..0 flash timer to 1..0") {
    CHECK(readyPopT(POP_DURATION) == doctest::Approx(1.0f));
    CHECK(readyPopT(POP_DURATION * 0.5f) == doctest::Approx(0.5f));
    CHECK(readyPopT(0.0f) == doctest::Approx(0.0f));
    CHECK(readyPopT(POP_DURATION * 2.0f) == doctest::Approx(1.0f)); // clamp high
}

TEST_CASE("readyPop ring expands as it fades") {
    f32 rStart = readyPopRadius(1.0f, 20.0f, 1.0f); // just fired
    f32 rEnd   = readyPopRadius(0.0f, 20.0f, 1.0f); // finished
    CHECK(rStart == doctest::Approx(20.0f));
    CHECK(rEnd   == doctest::Approx(20.0f + POP_GROW));
    CHECK(rEnd > rStart);
    CHECK(readyPopAlpha(1.0f) == doctest::Approx(1.0f));
    CHECK(readyPopAlpha(0.0f) == doctest::Approx(0.0f));
}
