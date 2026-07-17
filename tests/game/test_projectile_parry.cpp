// test_projectile_parry.cpp — Mirror Aegis reflect: a perfectly-blocked projectile survives,
// flips sides, reverses course, doubles its damage, and sheds the enemy's on-hit rider.
// fromPlayer=true is the load-bearing bit: player-owned projectiles are never AABB-tested
// against players, so a reflected shot can never re-hit the blocker or a teammate.

#include <doctest/doctest.h>
#include "game/projectile.h"

TEST_CASE("reflectAsParry: flips owner/side, reverses velocity, doubles damage") {
    Projectile p{};
    p.velocity      = {10.0f, 2.0f, -4.0f};
    p.damage        = 25.0f;
    p.fromPlayer    = false;
    p.ownerSlot     = 0xFF;
    p.lifetime      = 0.05f;               // nearly spent on arrival
    p.onHitEffect   = 1;                   // enemy poison rider
    p.onHitDuration = 3.0f;

    ProjectileSystem::reflectAsParry(p, /*newOwnerSlot=*/2);

    CHECK(p.velocity.x == doctest::Approx(-10.0f));
    CHECK(p.velocity.y == doctest::Approx(-2.0f));
    CHECK(p.velocity.z == doctest::Approx(4.0f));
    CHECK(p.fromPlayer);                    // never re-tested vs players
    CHECK(p.ownerSlot == 2);                // kills credit the blocker
    CHECK(p.damage == doctest::Approx(50.0f));
    CHECK(p.lifetime == doctest::Approx(3.0f));  // fresh flight window for the return trip
    CHECK(p.onHitEffect == 0);              // the rider dies with the parry
    CHECK(p.onHitDuration == 0.0f);
}

// sweepSampleCount — the tunneling fix: a projectile that outruns its own hitbox in one tick
// must be sampled along its travel segment, spaced no wider than its radius, or fast bolts
// (crossbows, shurikens, Velocity-affixed staff shots) step clean over grazes and small enemies.
TEST_CASE("sweepSampleCount: slow projectiles keep the single endpoint test") {
    CHECK(ProjectileSystem::sweepSampleCount(0.10f, 0.15f) == 1);   // travel <= radius
    CHECK(ProjectileSystem::sweepSampleCount(0.15f, 0.15f) == 1);   // exactly radius: no sweep
    CHECK(ProjectileSystem::sweepSampleCount(0.30f, 0.0f)  == 1);   // degenerate radius guard
}

TEST_CASE("sweepSampleCount: fast projectiles sample with spacing <= radius") {
    // Heavy Crossbow: 32.2 m/s -> 0.54 m/tick, r = 0.12.
    u32 n = ProjectileSystem::sweepSampleCount(0.54f, 0.12f);
    CHECK(n >= 5);
    CHECK(0.54f / static_cast<f32>(n) <= 0.12f);
    // Velocity-affixed Void Shuriken (the worst real case): 49 m/s -> 0.82 m/tick, r = 0.08.
    n = ProjectileSystem::sweepSampleCount(0.82f, 0.08f);
    CHECK(n >= 11);
    CHECK(n <= 16);                                                  // cap holds
    CHECK(0.82f / static_cast<f32>(n) <= 0.08f);
}
