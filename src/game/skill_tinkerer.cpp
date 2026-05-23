// skill_tinkerer.cpp — fire* helpers for the Tinkerer (Swarm Overlord) class skills:
// CombatDrone, SwarmDrones, StunGrenade, SwarmDeploy, Overclock, DetonateSwarm, SwarmQueen.
// Called from SkillSystem::tryActivate in skill_system.cpp.

#include "game/skill_internal.h"

// Drone spawning delegated to engine via callback — engine has mesh registry access.
// type 0 = combat drone (spider), type 1 = swarm drone (bat), type 2 = turret
void fireCombatDrone(Vec3 origin, Vec3 forward, EntityPool& /*entities*/)
{
    Vec3 right    = {forward.z, 0.0f, -forward.x};
    Vec3 spawnPos = origin + right * 2.0f;
    if (s_droneSpawnCallback) s_droneSpawnCallback(spawnPos, 0);
    LOG_INFO("Combat Drone requested");
}

void fireSwarmDrones(Vec3 origin, Vec3 forward, EntityPool& /*entities*/)
{
    f32 angles[2] = {-30.0f, 30.0f};
    for (u32 i = 0; i < 2; i++) {
        Vec3 dir      = normalize(rotateY(forward, angles[i]));
        Vec3 spawnPos = origin + dir * 1.5f + Vec3{0.0f, 0.5f, 0.0f};
        if (s_droneSpawnCallback) s_droneSpawnCallback(spawnPos, 1);
    }
    LOG_INFO("Swarm Drones requested (2 units)");
}

// Gravity-arc projectile that splashes on impact; freeze-on-hit via PROJ_SPLASH path.
void fireStunGrenade(Vec3 origin, Vec3 forward, const SkillDef* def,
                     ProjectilePool& pool)
{
    f32 damage  = (def->damage > 0.0f ? def->damage : 15.0f) * s_classDmgMult;
    f32 splashR = def->radius > 0.0f ? def->radius : 2.0f;

    u16 slot = ProjectileSystem::spawn(pool, origin, forward,
                                       14.0f, damage, 0.18f, 2.5f,
                                       true, PROJ_GRAVITY | PROJ_SPLASH);
    if (slot != 0xFFFF) {
        Projectile& p  = pool.projectiles[slot];
        p.gravity      = 9.8f;
        p.splashRadius = splashR;
        p.splashDamage = damage;
    }
    LOG_INFO("Stun Grenade thrown");
}

// Swarm Deploy: spawn 3 spiders + 3 bats in a semicircle. Cap at 24 friendly drones.
void fireSwarmDeploy(Vec3 origin, Vec3 forward, EntityPool& entities)
{
    // Count existing friendly drones and cull oldest if over cap
    constexpr u32 DRONE_CAP = 24;
    u32 droneCount = 0;
    for (u32 a = 0; a < entities.activeCount; a++) {
        Entity& e = entities.entities[entities.activeList[a]];
        if ((e.flags & ENT_FRIENDLY) && !(e.flags & ENT_DEAD) &&
            e.enemyType != EnemyType::PROP && e.npcClass == NpcClass::NONE) {
            droneCount++;
        }
    }
    // Kill oldest drones if spawning 6 more would exceed cap
    while (droneCount + 6 > DRONE_CAP) {
        // Find the friendly drone with lowest animTimer (oldest)
        f32 oldest = 999999.0f;
        u32 oldestIdx = 0xFFFF;
        for (u32 a = 0; a < entities.activeCount; a++) {
            u32 idx = entities.activeList[a];
            Entity& e = entities.entities[idx];
            if ((e.flags & ENT_FRIENDLY) && !(e.flags & ENT_DEAD) &&
                e.enemyType != EnemyType::PROP && e.npcClass == NpcClass::NONE) {
                // Lower animTimer = spawned earlier (timer counts up)
                if (e.animTimer < oldest) { oldest = e.animTimer; oldestIdx = idx; }
            }
        }
        if (oldestIdx == 0xFFFF) break;
        entities.entities[oldestIdx].flags |= ENT_DEAD;
        entities.entities[oldestIdx].aiState = AIState::DEAD;
        entities.entities[oldestIdx].deathTimer = 0.01f;
        droneCount--;
    }

    // Spawn 3 spiders (ground melee) in front semicircle
    Vec3 right = {forward.z, 0.0f, -forward.x};
    for (u32 i = 0; i < 3; i++) {
        f32 angle = -30.0f + i * 30.0f; // -30, 0, +30 degrees
        Vec3 dir = normalize(rotateY(forward, angle));
        Vec3 pos = origin + dir * 2.0f;
        if (s_droneSpawnCallback) s_droneSpawnCallback(pos, 0);
    }
    // Spawn 3 bats (flying hitscan) slightly above and behind
    for (u32 i = 0; i < 3; i++) {
        f32 angle = -40.0f + i * 40.0f; // -40, 0, +40 degrees
        Vec3 dir = normalize(rotateY(forward, angle));
        Vec3 pos = origin + dir * 1.5f + Vec3{0.0f, 1.0f, 0.0f};
        if (s_droneSpawnCallback) s_droneSpawnCallback(pos, 1);
    }

    if (s_particlePool) ParticleSystem::spawnSparks(*s_particlePool, origin, {0, 1, 0}, 6);
    LOG_INFO("Swarm Deploy: 6 drones (3 spider + 3 bat), total active ~%u", droneCount + 6);
    (void)right; // right is used indirectly via the loop — suppress unused warning
}

// Overclock: buff all friendly drones +100% dmg +50% speed for 5s.
void fireOverclock(Vec3 origin, const SkillDef* def, EntityPool& entities)
{
    f32 duration = def->duration > 0.0f ? def->duration : 5.0f;
    u32 buffed = 0;
    for (u32 a = 0; a < entities.activeCount; a++) {
        Entity& e = entities.entities[entities.activeList[a]];
        if (!(e.flags & ENT_FRIENDLY) || (e.flags & ENT_DEAD)) continue;
        if (e.npcClass != NpcClass::NONE) continue; // skip class NPCs (cleric, archer, etc.)
        e.overclockTimer = duration;
        buffed++;
    }
    if (s_novaCallback) s_novaCallback(origin, 8.0f, {1.0f, 0.8f, 0.2f});
    if (s_screenShake) s_screenShake->trigger(0.04f, 0.2f);
    LOG_INFO("Overclock: buffed %u drones for %.1fs", buffed, duration);
}

// Detonate Swarm: every friendly drone explodes (3m AoE each), then dies.
void fireDetonateSwarm(const SkillDef* def, EntityPool& entities)
{
    f32 damage = (def->damage > 0.0f ? def->damage : 15.0f) * s_classDmgMult;
    f32 radius = def->radius > 0.0f ? def->radius : 3.0f;
    u32 detonated = 0;

    for (u32 a = 0; a < entities.activeCount; a++) {
        u32 idx = entities.activeList[a];
        Entity& drone = entities.entities[idx];
        if (!(drone.flags & ENT_FRIENDLY) || (drone.flags & ENT_DEAD)) continue;
        if (drone.npcClass != NpcClass::NONE) continue;

        // AoE damage around this drone
        EntityHandle hits[MAX_ENTITIES];
        f32 dists[MAX_ENTITIES];
        u32 hitCount = CombatQuery::queryConeSorted(
            entities, drone.position, {0, -1, 0}, -1.0f, radius,
            hits, dists, MAX_ENTITIES);
        f32 totalDmg = 0.0f;
        for (u32 j = 0; j < hitCount; j++) {
            Entity* target = handleGet(entities, hits[j]);
            if (!target || (target->flags & ENT_FRIENDLY)) continue;
            Combat::applyDamage(entities, hits[j], damage);
            totalDmg += damage;
        }
        // Show explosion damage number at drone position even if no enemies hit
        Combat::spawnDamageNumber(drone.position + Vec3{0, 0.5f, 0}, totalDmg > 0 ? totalDmg : damage);

        // VFX per drone
        if (s_novaCallback) s_novaCallback(drone.position, radius, {1.0f, 0.5f, 0.1f});
        if (s_particlePool) ParticleSystem::spawnDebris(*s_particlePool, drone.position, 4);

        // Kill the drone
        drone.flags |= ENT_DEAD;
        drone.aiState = AIState::DEAD;
        drone.deathTimer = 0.01f;
        detonated++;
    }

    if (s_screenShake && detonated > 0) {
        f32 intensity = 0.05f * (detonated < 8 ? detonated : 8);
        s_screenShake->trigger(intensity, 0.5f);
    }
    LOG_INFO("Detonate Swarm: %u drones exploded for %.0f damage each", detonated, damage);
}

// Swarm Queen: summon a large tanky drone that auto-spawns minis every 2s.
void fireSwarmQueen(Vec3 origin, Vec3 forward)
{
    Vec3 spawnPos = origin + forward * 2.0f;
    // Type 0 (spider) but the engine callback will check queenLifeTimer to make it a queen
    if (s_droneSpawnCallback) s_droneSpawnCallback(spawnPos, 3); // type 3 = queen
    if (s_particlePool) {
        ParticleSystem::spawnMagicBurst(*s_particlePool, spawnPos, 255, 200, 50, 12);
        ParticleSystem::spawnSparks(*s_particlePool, spawnPos, {0, 1, 0}, 8);
    }
    if (s_screenShake) s_screenShake->trigger(0.06f, 0.3f);
    LOG_INFO("Swarm Queen deployed");
}
