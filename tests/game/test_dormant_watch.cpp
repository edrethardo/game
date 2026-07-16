// Weeping-angel wake rule — the pure view-cone half of "is anyone watching the statue".
//
// Dormant gargoyles are invulnerable stone and mimic chests hold their pose while a player
// has them on screen; both stir only when someone is inside the trigger bubble AND nobody
// is watching (enemy_ai_states.cpp DORMANT). The full rule needs a grid for the LOS half,
// but the cone half is pure math whose forward-vector convention MUST match the server aim
// vector in engine.cpp (yaw 0 faces -Z, positive pitch looks up). This file pins that
// convention: a camera/aim refactor that flips it would silently invert "being seen" into
// "free to move" — statues lunging while stared at — and nothing else would catch it.

#include <doctest/doctest.h>
#include "game/enemy_ai.h"

namespace {
constexpr f32 kCone   = 0.5f;         // WATCH_CONE_COS — generous ~60° half-angle
constexpr f32 kHalfPi = 1.5707963f;
}

TEST_CASE("inViewCone: yaw 0 faces -Z — ahead is watched, behind is not") {
    Vec3 eye{0, 1.7f, 0};
    CHECK(EnemyAI::inViewCone(eye, 0.0f, 0.0f, {0, 1.7f, -5.0f}, kCone));
    CHECK_FALSE(EnemyAI::inViewCone(eye, 0.0f, 0.0f, {0, 1.7f, 5.0f}, kCone));
}

TEST_CASE("inViewCone: ~60 degree half-angle boundary") {
    Vec3 eye{0, 1.7f, 0};
    // 45° off-axis: dot ≈ 0.707 ≥ 0.5 — near the screen edge still counts as watched.
    CHECK(EnemyAI::inViewCone(eye, 0.0f, 0.0f, {5.0f, 1.7f, -5.0f}, kCone));
    // 75° off-axis: dot ≈ 0.26 < 0.5 — clearly off screen, free to stir.
    CHECK_FALSE(EnemyAI::inViewCone(eye, 0.0f, 0.0f, {7.46f, 1.7f, -2.0f}, kCone));
}

TEST_CASE("inViewCone: yaw rotates the cone (yaw +90 faces -X)") {
    Vec3 eye{0, 1.7f, 0};
    CHECK(EnemyAI::inViewCone(eye, kHalfPi, 0.0f, {-5.0f, 1.7f, 0}, kCone));
    CHECK_FALSE(EnemyAI::inViewCone(eye, kHalfPi, 0.0f, {5.0f, 1.7f, 0}, kCone));
}

TEST_CASE("inViewCone: pitch matters — staring at the ceiling is looking away") {
    Vec3 eye{0, 1.7f, 0};
    const f32 up80 = 1.3962634f;
    // Statue at eye level dead ahead, player craned 80° upward: not watching it.
    CHECK_FALSE(EnemyAI::inViewCone(eye, 0.0f, up80, {0, 1.7f, -5.0f}, kCone));
    // Looking straight up IS watching a point overhead.
    CHECK(EnemyAI::inViewCone(eye, 0.0f, kHalfPi, {0, 6.7f, 0}, kCone));
}

TEST_CASE("inViewCone: point-blank is always seen, even behind the eyeline") {
    // Inside 0.5 m you are standing on it — a statue must never wake against your back
    // just because the camera faces elsewhere. Mirrors Interact::inReach's grab exemption.
    Vec3 eye{0, 1.7f, 0};
    CHECK(EnemyAI::inViewCone(eye, 0.0f, 0.0f, {0, 1.7f, 0.3f}, kCone));
}
