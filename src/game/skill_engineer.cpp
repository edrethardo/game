// skill_engineer.cpp — fire* helpers for the Combat Engineer class skills:
// ShockBolt, DeployTurret, TeslaCoil, MechOverdrive.
// Called from SkillSystem::tryActivate in skill_system.cpp.

#include "game/skill_internal.h"

// Electric bolt — triple bolt triangle formation for visual impact.
// Three parallel bolts travel in a tight triangle, all dealing full damage.
void fireShockBolt(Vec3 origin, Vec3 forward, const SkillDef* def,
                   ProjectilePool& projectiles)
{
    f32 damage = spellScaled((def->damage > 0.0f ? def->damage : 20.0f));

    // Build perpendicular axes for the triangle offset
    Vec3 right = normalize(Vec3{-forward.z, 0.0f, forward.x});
    Vec3 up = {0.0f, 1.0f, 0.0f};

    // Triangle offsets: center, up-left, down-right (~0.15m apart)
    Vec3 offsets[3] = {
        {0.0f, 0.0f, 0.0f},                         // center bolt
        right * -0.1f + up * 0.1f,                   // up-left
        right * 0.1f + up * -0.1f,                   // down-right
    };

    for (u32 b = 0; b < 3; b++) {
        Vec3 spawnPos = origin + offsets[b];
        u16 idx = ProjectileSystem::spawn(projectiles, spawnPos, forward,
                                           35.0f, damage, 0.10f, 2.5f, true, PROJ_SPARK);
        if (idx != 0xFFFF) {
            projectiles.projectiles[idx].freezeDuration = 1.5f; // slow on hit
            projectiles.projectiles[idx].meshId = s_boltMeshId;
            projectiles.projectiles[idx].lightColor = {0.5f, 0.7f, 1.0f};
            // Stun built into freezeDuration — the projectile collision path
            // applies freezeTimer which halves move speed. For the brief stagger,
            // we use a slightly longer freeze (acts as stun since enemies can't act while frozen).
        }
    }

    // Spark burst at muzzle for visual punch
    if (s_particlePool) ParticleSystem::spawnSparks(*s_particlePool, origin, forward, 6);
    LOG_INFO("Shock Bolt fired (triangle formation)");
}

// Spawn a friendly turret entity 2m in front of the player.
void fireDeployTurret(Vec3 origin, Vec3 forward, EntityPool& /*entities*/)
{
    Vec3 spawnPos = origin + forward * 2.0f;
    if (s_droneSpawnCallback) s_droneSpawnCallback(spawnPos, 2); // type 2 = turret
    LOG_INFO("Turret requested");
}

// AoE lightning burst — 360° starburst of sparks + chain arcs to every hit enemy.
void fireTeslaCoil(Vec3 origin, const SkillDef* def,
                   EntityPool& entities, ProjectilePool& pool)
{
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    f32 radius = def->radius > 0.0f ? def->radius : 4.0f;
    f32 damage = spellScaled((def->damage > 0.0f ? def->damage : 30.0f));

    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, radius,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage);
        // Brief electric stagger on all hit enemies
        Entity* e = handleGet(entities, hits[i]);
        if (e) e->stunTimer = fmaxf(e->stunTimer, 0.1f);
    }

    // Massive starburst: 3 rings of sparks ×4 density (144 total)
    // Ring 1: 64 horizontal sparks (fast, wide)
    for (u32 s = 0; s < 64; s++) {
        f32  angle = s * (6.28318f / 64.0f);
        f32  speed = 12.0f + (s % 6) * 2.0f;
        Vec3 dir   = {cosf(angle), 0.05f, sinf(angle)};
        u16 si = ProjectileSystem::spawn(pool, origin, dir, speed, 0.0f, 0.05f, 0.6f,
                                true, PROJ_SPARK);
        if (si != 0xFFFF) pool.projectiles[si].lightColor = {0.4f, 0.7f, 1.0f};
    }
    // Ring 2: 48 upward-angled sparks (medium speed, rising)
    for (u32 s = 0; s < 48; s++) {
        f32  angle = s * (6.28318f / 48.0f) + 0.13f;
        Vec3 dir   = {cosf(angle) * 0.7f, 0.6f, sinf(angle) * 0.7f};
        u16 si = ProjectileSystem::spawn(pool, origin + Vec3{0, 0.3f, 0}, dir, 10.0f, 0.0f, 0.04f, 0.5f,
                                true, PROJ_SPARK);
        if (si != 0xFFFF) pool.projectiles[si].lightColor = {0.3f, 0.6f, 1.0f};
    }
    // Ring 3: 32 downward-angled sparks (fast, ground-hugging)
    for (u32 s = 0; s < 32; s++) {
        f32  angle = s * (6.28318f / 32.0f) + 0.2f;
        Vec3 dir   = {cosf(angle), -0.3f, sinf(angle)};
        u16 si = ProjectileSystem::spawn(pool, origin, dir, 18.0f, 0.0f, 0.06f, 0.35f,
                                true, PROJ_SPARK);
        if (si != 0xFFFF) pool.projectiles[si].lightColor = {0.5f, 0.8f, 1.0f};
    }

    // Chain FX arcs from origin to every hit enemy — visual lightning web
    if (s_chainCallback && hitCount > 0) {
        Vec3 chainPts[24];
        u8 ptCount = 1;
        chainPts[0] = origin;
        for (u32 i = 0; i < hitCount && ptCount < 24; i++) {
            Entity* e = handleGet(entities, hits[i]);
            if (e) chainPts[ptCount++] = e->position + Vec3{0, e->halfExtents.y, 0};
        }
        if (ptCount > 1) s_chainCallback(chainPts, ptCount);
    }

    // Visual feedback: bright nova + sparks + shake
    if (s_novaCallback) s_novaCallback(origin, radius, {0.6f, 0.85f, 1.0f});
    if (s_particlePool) ParticleSystem::spawnSparks(*s_particlePool, origin, {0, 1, 0}, 12);
    if (s_screenShake)  s_screenShake->trigger(0.12f, 0.6f);
    LOG_INFO("Tesla Coil hit %u enemies", hitCount);
}

// Overdrive: restore HP + clear debuffs + 5s damage/speed buff.
void fireMechOverdrive(Player& player)
{
    player.health = (player.health + 20.0f > player.maxHealth)
                    ? player.maxHealth : player.health + 20.0f;
    player.slowTimer   = 0.0f;
    player.freezeTimer = 0.0f;
    player.overdriveTimer = 5.0f; // 5s buff — consumed by combat/movement systems
    LOG_INFO("Mech Overdrive activated (5s buff)");
}
