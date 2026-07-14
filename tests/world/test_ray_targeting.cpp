// test_ray_targeting.cpp — precision-shot targeting geometry.
//
// The Marksman's Headshot used a 2° cone (queryConeSorted), which measures the angle from the eye to
// the entity's CENTRE. A cone's *linear* aim tolerance is dist * tan(angle), so it COLLAPSES as the
// target approaches: ~70 cm of slack at 20 m, but only ~5 cm at 1.5 m. The result was a skill that
// worked at range and was nearly impossible to land on a point-blank enemy filling the screen —
// and, for a skill called Headshot, aiming at the head missed at every range under 20 m.
//
// A ray-vs-AABB test has the SAME tolerance at every distance: the size of the target. These tests
// pin that, because the bug is invisible to the compiler and to every existing test.

#include <doctest/doctest.h>
#include "world/combat_query.h"
#include "game/entity.h"
#include <cmath>
#include <initializer_list>

static constexpr f32 EYE = 1.6f;   // player eye height

// Add a hostile directly to the pool. Built by hand rather than via EntitySystem::spawn so the test
// links only the targeting geometry (combat_query) — spawn() drags in the whole Combat/death chain.
static u16 addEnemy(EntityPool& pool, Vec3 centre) {
    const u16 i = static_cast<u16>(pool.activeCount);
    Entity& e = pool.entities[i];
    e = Entity{};
    e.position    = centre;
    e.halfExtents = {0.4f, 0.9f, 0.4f};
    e.flags       = ENT_ACTIVE;
    e.health      = 100.0f;
    e.maxHealth   = 100.0f;
    e.generation  = 1;
    pool.activeList[pool.activeCount++] = i;
    return i;
}

// One hostile, feet on the floor, `dist` metres straight ahead (+Z).
static void placeEnemy(EntityPool& pool, f32 dist) {
    addEnemy(pool, {0.0f, 0.9f, dist});
}

// Look from the eye at a point `offsetY` above the enemy's centre.
static Vec3 aimAt(f32 dist, f32 offsetY) {
    Vec3 d = { 0.0f, (0.9f + offsetY) - EYE, dist };
    return normalize(d);
}

TEST_CASE("RayTargeting: a body shot connects at EVERY range, including point-blank") {
    // The regression. The old 2° cone missed everything below ~20 m unless you hit the exact centre.
    for (f32 dist : {1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 10.0f, 20.0f, 40.0f}) {
        EntityPool pool;
        placeEnemy(pool, dist);
        EntityHandle hit; f32 t = 0.0f;
        // Aim at the HEAD (+0.6 m above centre) — the natural aim point for this skill.
        REQUIRE(CombatQuery::rayNearestEntity(pool, {0, EYE, 0}, aimAt(dist, 0.6f), 80.0f, hit, t));
    }
}

TEST_CASE("RayTargeting: aim tolerance does NOT shrink with distance") {
    // The property the cone lacked. Anywhere on the body hits, near or far.
    for (f32 dist : {1.5f, 20.0f}) {
        for (f32 offY : {-0.8f, -0.4f, 0.0f, 0.4f, 0.8f}) {
            EntityPool pool;
            placeEnemy(pool, dist);
            EntityHandle hit; f32 t = 0.0f;
            CHECK(CombatQuery::rayNearestEntity(pool, {0, EYE, 0}, aimAt(dist, offY), 80.0f, hit, t));
        }
    }
}

TEST_CASE("RayTargeting: aiming clearly off the body still misses") {
    // Precision must stay precise — the fix must not become an aimbot.
    EntityPool pool;
    placeEnemy(pool, 5.0f);
    EntityHandle hit; f32 t = 0.0f;
    CHECK_FALSE(CombatQuery::rayNearestEntity(pool, {0, EYE, 0}, aimAt(5.0f, 3.0f), 80.0f, hit, t));
}

TEST_CASE("RayTargeting: nothing behind the player is ever a target") {
    // rayVsAABB used to report a HIT for boxes entirely behind the origin: tmin and tmax were both
    // negative, `tmin > tmax` was false, and it fell into the "ray starts inside the box" branch and
    // returned t = 0. CombatQuery::raycast only escaped it because a `t > 0.01f` guard threw those
    // away — which also threw away genuine point-blank overlaps.
    EntityPool pool;
    placeEnemy(pool, 5.0f);                       // enemy is at +Z
    EntityHandle hit; f32 t = 0.0f;
    CHECK_FALSE(CombatQuery::rayNearestEntity(pool, {0, EYE, 0}, {0, 0, -1}, 80.0f, hit, t));
}

TEST_CASE("RayTargeting: an enemy overlapping the player is hittable") {
    // Point-blank overlap: the origin is INSIDE the collider, so the surface is at t = 0.
    EntityPool pool;
    placeEnemy(pool, 0.2f);
    EntityHandle hit; f32 t = 0.0f;
    REQUIRE(CombatQuery::rayNearestEntity(pool, {0, 0.9f, 0}, {0, 0, 1}, 80.0f, hit, t));
    CHECK(t == doctest::Approx(0.0f));
}

TEST_CASE("RayTargeting: the NEAREST enemy is chosen, not just any on the line") {
    EntityPool pool;
    addEnemy(pool, {0, 0.9f, 20.0f});                 // far
    const u16 nearIdx = addEnemy(pool, {0, 0.9f, 4.0f});  // near

    EntityHandle hit; f32 t = 0.0f;
    REQUIRE(CombatQuery::rayNearestEntity(pool, {0, 0.9f, 0}, {0, 0, 1}, 80.0f, hit, t));
    CHECK(hit.index == nearIdx);
}

TEST_CASE("RayTargeting: dead, friendly and prop entities are not targets") {
    EntityPool pool;
    const u16 idx = addEnemy(pool, {0, 0.9f, 5.0f});
    Entity* e = &pool.entities[idx];

    EntityHandle hit; f32 t = 0.0f;
    const Vec3 origin{0, 0.9f, 0}, dir{0, 0, 1};

    e->flags |= ENT_FRIENDLY;
    CHECK_FALSE(CombatQuery::rayNearestEntity(pool, origin, dir, 80.0f, hit, t));
    e->flags &= static_cast<u8>(~ENT_FRIENDLY);

    e->flags |= ENT_DEAD;
    CHECK_FALSE(CombatQuery::rayNearestEntity(pool, origin, dir, 80.0f, hit, t));
    e->flags &= static_cast<u8>(~ENT_DEAD);

    e->enemyType = EnemyType::PROP;
    CHECK_FALSE(CombatQuery::rayNearestEntity(pool, origin, dir, 80.0f, hit, t));
}

TEST_CASE("RayTargeting: this is exactly what a narrow cone gets WRONG") {
    // Documents the defect in the terms that matter: a 2° cone's linear tolerance is dist*tan(2°).
    // At 20 m that is ~70 cm (most of a body). At 1.5 m it is ~5 cm — while the enemy fills the
    // screen. The ray test has no such collapse.
    auto coneToleranceCm = [](f32 dist) { return dist * std::tan(2.0f * 3.14159265f / 180.0f) * 100.0f; };
    CHECK(coneToleranceCm(20.0f) > 60.0f);   // forgiving far away
    CHECK(coneToleranceCm(1.5f)  < 10.0f);   // unusable up close  <-- the bug
}
