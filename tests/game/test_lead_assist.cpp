#include <doctest/doctest.h>
#include "game/lead_assist.h"

// Throwing-knife lead assist (game/lead_assist.h) — pure intercept math. The scenarios mirror
// the diagnosis that motivated the feature: a knife at 25-35 m/s vs enemies the Phase-2 AI
// keeps strafing at 1.8-9 m/s, where an on-crosshair throw measured ~0-7% hits.

TEST_CASE("LeadAssist: stationary target intercepts at dist/speed, straight ahead") {
    f32 t = 0.0f;
    REQUIRE(LeadAssist::interceptTime({8, 0, 0}, {0, 0, 0}, 30.0f, t));
    CHECK(t == doctest::Approx(8.0f / 30.0f));
}

TEST_CASE("LeadAssist: the solved intercept actually connects with a strafing target") {
    // Revenant-ish: 8 m ahead on +X, strafing 3 m/s on +Z; knife at 30 m/s.
    const Vec3 rel = {8, 0, 0};
    const Vec3 vel = {0, 0, 3};
    const f32  speed = 30.0f;
    f32 t = 0.0f;
    REQUIRE(LeadAssist::interceptTime(rel, vel, speed, t));
    const Vec3 aimPoint  = rel + vel * t;            // where the assist aims
    const Vec3 knifeAt_t = normalize(aimPoint) * (speed * t);   // where the knife is at t
    const Vec3 targetAt_t = rel + vel * t;                       // where the target is at t
    CHECK(length(knifeAt_t - targetAt_t) < 0.01f);   // they meet (well under any hitbox)
}

TEST_CASE("LeadAssist: a target outrunning the projectile is unreachable") {
    f32 t = 0.0f;
    // Fleeing directly away at 40 m/s vs a 30 m/s knife — no positive root.
    CHECK_FALSE(LeadAssist::interceptTime({8, 0, 0}, {40, 0, 0}, 30.0f, t));
}

TEST_CASE("LeadAssist: intercepts beyond MAX_LEAD_SEC are rejected") {
    f32 t = 0.0f;
    // 60 m away at 30 m/s = 2 s flight > 1.5 s cap — don't lead shots the knife can't make.
    CHECK_FALSE(LeadAssist::interceptTime({60, 0, 0}, {0, 0, 0}, 30.0f, t));
}

TEST_CASE("LeadAssist: clampToward returns the ideal aim inside the cap, caps outside") {
    const Vec3 aim = {1, 0, 0};

    // 5° off — inside the 12° cap: take the ideal exactly.
    Vec3 ideal = normalize(Vec3{cosf(0.0873f), 0, sinf(0.0873f)});
    Vec3 out = LeadAssist::clampToward(aim, ideal, LeadAssist::MAX_CORRECT_RAD);
    CHECK(dot(out, ideal) == doctest::Approx(1.0f));

    // 45° off — way outside: result must sit EXACTLY at the cap angle from the aim,
    // rotated toward the ideal (positive Z here), and stay unit-length.
    ideal = normalize(Vec3{1, 0, 1});
    out = LeadAssist::clampToward(aim, ideal, LeadAssist::MAX_CORRECT_RAD);
    CHECK(dot(out, aim) == doctest::Approx(cosf(LeadAssist::MAX_CORRECT_RAD)));
    CHECK(out.z > 0.0f);
    CHECK(length(out) == doctest::Approx(1.0f));
}

TEST_CASE("LeadAssist: parallel vectors don't explode the rotation axis") {
    const Vec3 aim = {1, 0, 0};
    // Identical aim/ideal: cross is zero-length; must return aim, not NaN.
    Vec3 out = LeadAssist::clampToward(aim, aim, LeadAssist::MAX_CORRECT_RAD);
    CHECK(out.x == doctest::Approx(1.0f));
    CHECK(length(out) == doctest::Approx(1.0f));
}

TEST_CASE("LeadAssist: the motivating scenario — full correction stays inside the cap") {
    // The diagnosis case: 8 m, 3 m/s strafe, 30 m/s knife. Required lead ≈ atan(3/30) ≈ 5.7°,
    // comfortably inside the 12° cap — so the assist fully connects this shot.
    const Vec3 rel = {8, 0, 0};
    const Vec3 vel = {0, 0, 3};
    f32 t = 0.0f;
    REQUIRE(LeadAssist::interceptTime(rel, vel, 30.0f, t));
    const Vec3 aim   = {1, 0, 0};                       // crosshair dead on current position
    const Vec3 ideal = normalize(rel + vel * t);
    const Vec3 out   = LeadAssist::clampToward(aim, ideal, LeadAssist::MAX_CORRECT_RAD);
    CHECK(dot(out, ideal) == doctest::Approx(1.0f));    // no clamping needed — exact intercept
}
