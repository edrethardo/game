// skill_ranger.cpp — fire* helpers for the Ranger class skills:
// MultiShot, RainOfArrows, PoisonArrow, ShadowShot, Volley, PiercingShot, Barrage, MarkPrey.
// Also defines rotateY, used by multiple families via skill_internal.h.

#include "game/skill_internal.h"

// Helper: rotate a direction vector by angleDeg degrees around Y axis.
Vec3 rotateY(Vec3 v, f32 angleDeg)
{
    f32 a = radians(angleDeg);
    f32 c = cosf(a), s = sinf(a);
    return { v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
}

// Five projectiles spread in a -20 / -10 / 0 / +10 / +20 degree horizontal fan.
void fireMultiShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                   ProjectilePool& pool)
{
    f32 speed  = def->projectileSpeed > 0.0f ? def->projectileSpeed : 25.0f;
    f32 damage = (def->damage > 0.0f ? def->damage : 20.0f) * s_classDmgMult;
    f32 angles[5] = {-20.0f, -10.0f, 0.0f, 10.0f, 20.0f};
    for (u32 i = 0; i < 5; i++) {
        Vec3 dir = normalize(rotateY(forward, angles[i]));
        ProjectileSystem::spawn(pool, origin, dir, speed, damage, 0.1f, 2.0f, true);
    }
    LOG_INFO("Multi Shot fired 5 arrows");
}

// Instant AoE at a raycasted target point — no delay, like a meteor that fires now.
void fireRainOfArrows(Vec3 origin, Vec3 forward, const SkillDef* def,
                      const LevelGrid& grid, EntityPool& entities)
{
    RayHit hit    = Raycast::cast(grid, origin, forward, 50.0f);
    Vec3 target   = hit.hit ? (origin + forward * hit.distance)
                            : (origin + forward * 20.0f);
    f32 radius    = def->radius > 0.0f ? def->radius : 3.5f;
    f32 damage    = (def->damage > 0.0f ? def->damage : 25.0f) * s_classDmgMult;

    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, target, {0.0f, -1.0f, 0.0f}, -1.0f, radius,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage);
    }
    if (s_novaCallback) s_novaCallback(target, radius, {0.6f, 0.4f, 0.1f});
    LOG_INFO("Rain of Arrows hit %u enemies", hitCount);
}

// Single projectile — engine's normal hit path applies damage; visual tint via flags.
// Poison DoT would be wired through Projectile::onHitEffect in a future pass.
void firePoisonArrow(Vec3 origin, Vec3 forward, const SkillDef* def,
                     ProjectilePool& pool)
{
    f32 speed  = def->projectileSpeed > 0.0f ? def->projectileSpeed : 22.0f;
    f32 damage = (def->damage > 0.0f ? def->damage : 18.0f) * s_classDmgMult;
    // Spawn with PROJ_SPARK flag repurposed as green-tint hint for renderer
    u16 slot = ProjectileSystem::spawn(pool, origin, forward, speed, damage, 0.12f, 3.0f,
                                       true, PROJ_SPARK);
    // The poison. This skill previously applied NONE — it spawned a bare projectile whose only
    // "poison" was PROJ_SPARK, a renderer TINT flag. The DoT was left as a TODO and never landed,
    // so Poison Arrow was a plain arrow with a green trail.
    if (slot != 0xFFFF) {
        Projectile& p = pool.projectiles[slot];
        p.poisonDuration = def->duration > 0.0f ? def->duration : 4.0f;
        p.poisonDps      = (def->poisonDps > 0.0f ? def->poisonDps : 6.0f) * s_classDmgMult;
    }
    LOG_INFO("Poison Arrow fired");
}

// High-speed, high-damage hitscan shot (piercing is a future projectile.cpp feature).
void fireShadowShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                    const LevelGrid& grid, EntityPool& entities)
{
    WeaponDef temp;
    temp.name    = "Shadow Shot";
    temp.type    = WeaponType::HITSCAN;
    temp.damage  = (def->damage > 0.0f ? def->damage * 2.5f : 60.0f) * s_classDmgMult;
    temp.range   = 80.0f;
    temp.coneAngleDeg  = 1.0f;
    temp.cooldown      = 0.0f;
    temp.recoilKick    = 0.02f;
    Combat::fireHitscan(temp, origin, forward, grid, entities);
    LOG_INFO("Shadow Shot fired");
}

// Volley: 4 damage waves + 80 cosmetic arrows raining in 4 visual volleys.
// Damage via PendingMeteors (zone-wide AoE), visuals via PROJ_ORB arrows (no collision).
void fireVolley(Vec3 origin, Vec3 forward, const SkillDef* def,
                const LevelGrid& grid, ProjectilePool& pool)
{
    RayHit hit = Raycast::cast(grid, origin, forward, 20.0f);
    Vec3 target = hit.hit ? (origin + forward * hit.distance) : (origin + forward * 15.0f);
    f32 radius = (def->radius > 0.0f ? def->radius : 4.0f) * 1.2f;
    f32 arrowDmg = s_weaponDamage * 0.6f * s_classDmgMult;

    // 80 arrows in 4 visual waves (20 per wave at staggered heights).
    // Each arrow deals real damage via projectile-entity collision.
    static const f32 waveHeight[4] = {5.0f, 8.0f, 11.0f, 14.0f};
    for (u32 w = 0; w < 4; w++) {
        for (u32 a = 0; a < 20; a++) {
            f32 angle = (std::rand() / static_cast<f32>(RAND_MAX)) * 6.2832f;
            f32 dist  = (std::rand() / static_cast<f32>(RAND_MAX)) * radius;
            // Per-arrow height jitter within the wave for natural scatter
            f32 hJitter = ((std::rand() / static_cast<f32>(RAND_MAX)) - 0.5f) * 1.0f;
            Vec3 spawnPos = target + Vec3{cosf(angle) * dist, waveHeight[w] + hJitter, sinf(angle) * dist};

            f32 sx = ((std::rand() / static_cast<f32>(RAND_MAX)) - 0.5f) * 1.5f;
            f32 sz = ((std::rand() / static_cast<f32>(RAND_MAX)) - 0.5f) * 1.5f;
            Vec3 dir = normalize(Vec3{sx, -8.0f, sz});

            u16 idx = ProjectileSystem::spawn(pool, spawnPos, dir, 12.0f, arrowDmg,
                                               0.4f, 3.0f, true, PROJ_GRAVITY);
            if (idx != 0xFFFF) {
                pool.projectiles[idx].gravity = 6.0f;
                pool.projectiles[idx].meshId = (a % 2 == 0) ? s_arrowMeshId : s_boltMeshId;
            }
        }
    }

    // Target zone: persistent ground ring + crosshair pattern (no DPS, just visual)
    if (s_scorchCallback) s_scorchCallback(target, radius, 1.5f, 0.0f);
    if (s_novaCallback) s_novaCallback(target, radius, {0.6f, 0.4f, 0.1f});
    if (s_screenShake) s_screenShake->trigger(0.04f, 0.3f);
    LOG_INFO("Volley: 80 arrows in 4 waves, dmg %.1f each", arrowDmg);
}

// Piercing Shot: ray-AABB penetrating arrow + bleed DoT. Weapon-scaling damage.
void firePiercingShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                      const LevelGrid& grid, EntityPool& entities)
{
    f32 damage = s_weaponDamage * 1.5f * s_classDmgMult;
    f32 range  = 80.0f;

    // Ray-AABB intersection against all entities (same pattern as Aimed Shot)
    u32 hitCount = 0;
    for (u32 a = 0; a < entities.activeCount; a++) {
        u32 idx = entities.activeList[a];
        Entity& e = entities.entities[idx];
        if (e.flags & ENT_DEAD) continue;
        if (e.flags & ENT_FRIENDLY) continue;

        AABB box = entityAABB(e);
        Vec3 invDir = {1.0f / (forward.x != 0.0f ? forward.x : 0.0001f),
                       1.0f / (forward.y != 0.0f ? forward.y : 0.0001f),
                       1.0f / (forward.z != 0.0f ? forward.z : 0.0001f)};
        f32 t1 = (box.min.x - origin.x) * invDir.x;
        f32 t2 = (box.max.x - origin.x) * invDir.x;
        f32 t3 = (box.min.y - origin.y) * invDir.y;
        f32 t4 = (box.max.y - origin.y) * invDir.y;
        f32 t5 = (box.min.z - origin.z) * invDir.z;
        f32 t6 = (box.max.z - origin.z) * invDir.z;
        f32 tmin = fmaxf(fmaxf(fminf(t1, t2), fminf(t3, t4)), fminf(t5, t6));
        f32 tmax = fminf(fminf(fmaxf(t1, t2), fmaxf(t3, t4)), fmaxf(t5, t6));
        if (tmax < 0.0f || tmin > tmax || tmin > range) continue;

        EntityHandle h = {static_cast<u16>(idx), e.generation};
        Combat::applyDamage(entities, h, damage);
        // Apply bleed DoT: 20% of hit damage per second for 3s, crediting the caster (the engine
        // sets s_attackingPlayer to the caster's slot around skill activation) so the bleed kill
        // grants mana-on-kill / loot to them.
        e.poisonTimer = 3.0f;
        e.poisonDps   = damage * 0.2f;
        e.poisonSrcSlot = Combat::getAttackingPlayer();
        if (s_particlePool) ParticleSystem::spawnDebris(*s_particlePool, e.position, 3);
        hitCount++;
    }

    // Beam trail
    RayHit wallHit = Raycast::cast(grid, origin, forward, range);
    Vec3 beamEnd = wallHit.hit ? (origin + forward * wallHit.distance)
                               : (origin + forward * range);
    if (s_beamCallback) s_beamCallback(origin, beamEnd, {0.4f, 0.6f, 0.2f});
    if (s_particlePool) ParticleSystem::spawnSparks(*s_particlePool, origin, forward, 4);
    if (s_screenShake) s_screenShake->trigger(0.05f, 0.25f);
    LOG_INFO("Piercing Shot: hit %u enemies, %.0f damage + bleed", hitCount, damage);
}

// Barrage: 10 arrows in a tight forward cone. Weapon-scaling damage.
void fireBarrage(Vec3 origin, Vec3 forward, const SkillDef* def,
                 ProjectilePool& pool)
{
    f32 speed  = def->projectileSpeed > 0.0f ? def->projectileSpeed : 30.0f;
    f32 damage = s_weaponDamage * 0.4f * s_classDmgMult;

    for (u32 i = 0; i < 10; i++) {
        // Random spread ±5° horizontal, ±2° vertical
        f32 yawSpread   = ((std::rand() / static_cast<f32>(RAND_MAX)) - 0.5f) * 10.0f;
        f32 pitchSpread = ((std::rand() / static_cast<f32>(RAND_MAX)) - 0.5f) * 4.0f;
        Vec3 dir = normalize(rotateY(forward, yawSpread) +
                             Vec3{0.0f, pitchSpread * 0.017f, 0.0f}); // degrees to ~radians
        ProjectileSystem::spawn(pool, origin, dir, speed, damage, 0.08f, 1.5f, true);
    }

    if (s_particlePool) ParticleSystem::spawnSparks(*s_particlePool, origin, forward, 6);
    if (s_screenShake) s_screenShake->trigger(0.06f, 0.3f);
    LOG_INFO("Barrage: 10 arrows, %.0f damage each", damage);
}

// Mark Prey: mark nearest enemy for 2× damage. Chain clear on kill.
// Returns false (no valid target) so a whiff stays free and triggers no cooldown.
bool fireMarkPrey(Vec3 origin, Vec3 forward, const SkillDef* def,
                  EntityPool& entities)
{
    f32 range = 15.0f;
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 cnt = CombatQuery::queryConeSorted(
        entities, origin, forward, cosf(radians(30.0f)), range,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < cnt; i++) {
        Entity* e = handleGet(entities, hits[i]);
        if (!e || (e->flags & ENT_DEAD) || (e->flags & ENT_FRIENDLY)) continue;

        e->markPreyTimer  = def->duration > 0.0f ? def->duration : 5.0f;
        e->markPreyDmgMult = 2.0f;

        // Red-orange mark visual
        if (s_novaCallback) s_novaCallback(e->position, 1.5f, {1.0f, 0.3f, 0.1f});
        if (s_particlePool) ParticleSystem::spawnMagicBurst(*s_particlePool, e->position, 255, 80, 30, 8);
        if (s_screenShake) s_screenShake->trigger(0.04f, 0.2f);
        LOG_INFO("Mark Prey: marked entity %u for 2x damage", hits[i].index);
        return true; // mark only the first valid target
    }
    LOG_INFO("Mark Prey: no valid target found");
    return false; // nothing marked — free, no cooldown
}
