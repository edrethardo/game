// skill_warrior.cpp — fire* helpers for the Warrior class skills:
// Cleave, Thunderclap, War Cry, Whirlwind, Earthquake.
// Each function is called from SkillSystem::tryActivate in skill_system.cpp.

#include "game/skill_internal.h"

// Wide 120-degree melee swing — hits everything in a short, broad arc.
// 5% lifesteal based on total damage dealt.
void fireCleave(Vec3 origin, Vec3 forward, const SkillDef* def,
                EntityPool& entities, Player& player)
{
    WeaponDef temp;
    temp.name        = "Cleave";
    temp.type        = WeaponType::MELEE;
    temp.damage      = (def->damage > 0.0f ? def->damage : 35.0f) * s_classDmgMult;
    temp.range       = def->radius  > 0.0f ? def->radius : 3.0f;
    temp.coneAngleDeg = 120.0f;
    temp.cooldown    = 0.0f;
    temp.recoilKick  = 0.0f;
    AttackResult result = Combat::fireMelee(temp, origin, forward, entities);

    // Heal player for 5% of damage dealt
    f32 hitCount = static_cast<f32>(result.entitiesHit);
    f32 healAmt = temp.damage * 0.05f * hitCount;
    player.health = fminf(player.health + healAmt, player.maxHealth);

    // VFX: wide red arc + debris on hit + screen shake
    if (s_novaCallback) s_novaCallback(origin, temp.range, {0.9f, 0.25f, 0.1f});
    if (s_particlePool && result.entitiesHit > 0) {
        ParticleSystem::spawnDebris(*s_particlePool, origin + forward * 1.5f, 8);
        ParticleSystem::spawnSparks(*s_particlePool, origin + forward * 1.5f, forward, 6);
    }
    if (s_screenShake) s_screenShake->trigger(0.04f, 0.2f);
    LOG_INFO("Cleave fired, hit %u, healed %.1f", result.entitiesHit, healAmt);
}

// Ground stomp AoE — damages, stuns, and slows all enemies in radius.
// Stun duration scales with upgrade: 0.2s base, 0.5s upgraded.
void fireThunderclap(Vec3 origin, const SkillDef* def, EntityPool& entities)
{
    // Ground-level position — origin is eye height, drop to feet
    Vec3 groundPos = {origin.x, origin.y - 1.7f, origin.z};

    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    f32 range  = def->radius > 0.0f ? def->radius : 5.0f;
    f32 damage = (def->damage > 0.0f ? def->damage : 25.0f) * s_classDmgMult;
    // Query from ground position with omnidirectional search
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, groundPos, {0.0f, -1.0f, 0.0f}, -1.0f, range,
        hits, dists, MAX_ENTITIES);

    f32 stunTime = def->duration > 0.0f ? def->duration : 0.2f;
    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage);
        Entity* e = handleGet(entities, hits[i]);
        if (e) {
            e->stunTimer = stunTime;       // full immobilize
            e->freezeTimer = 1.5f;         // slow after stun wears off
            // The Butcher is especially vulnerable to thunderclap
            if (e->nameTag && std::strcmp(e->nameTag, "The Butcher") == 0) {
                e->stunTimer = 3.0f;
                e->speechText = "Oh no, I'm dizzy!";
                e->speechTimer = 3.0f;
            }
        }
    }

    // Brown-grey ground stomp visual at feet level (earthy shockwave)
    if (s_novaCallback) s_novaCallback(groundPos, range, {0.5f, 0.4f, 0.25f});
    // Persistent scorch zone — lingering fire damage in the stomp area
    if (s_scorchCallback) {
        s_scorchCallback(groundPos, range * 0.6f, 3.0f, 5.0f); // 60% radius, 3s duration, 5 DPS
    }
    LOG_INFO("Thunderclap hit %u enemies", hitCount);
}

// Stun all enemies within 6m with a war-cry shout.
void fireWarCry(Vec3 origin, const SkillDef* def, EntityPool& entities,
                Player& player)
{
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    f32 range    = def->radius > 0.0f ? def->radius : 6.0f;
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, range,
        hits, dists, MAX_ENTITIES);

    u32 stunned = 0;
    for (u32 i = 0; i < hitCount; i++) {
        Entity* e = handleGet(entities, hits[i]);
        if (!e) continue;
        if (e->flags & ENT_FRIENDLY) {
            // Buff allies: +30% speed for 3s via overclock timer (reuse existing field)
            e->overclockTimer = 3.0f;
        } else {
            e->stunTimer = 2.0f;
            stunned++;
        }
    }

    // Buff player: +30% speed for 3s via shrine speed buff (temporary)
    // Use a simple timer on the player — reuse overdriveTimer for the speed boost
    player.overdriveTimer = fmaxf(player.overdriveTimer, 3.0f);

    // VFX: massive golden shockwave + upward sparks + screen shake
    if (s_novaCallback) s_novaCallback(origin, range, {1.0f, 0.85f, 0.2f});
    if (s_particlePool) {
        ParticleSystem::spawnSparks(*s_particlePool, origin, {0, 1, 0}, 12);
        ParticleSystem::spawnMagicBurst(*s_particlePool, origin, 255, 220, 50, 10);
    }
    if (s_screenShake) s_screenShake->trigger(0.08f, 0.4f);
    LOG_INFO("War Cry: stunned %u enemies, buffed allies with +30%% speed for 3s", stunned);
}

// Instant 360-degree AoE melee spin at short range (no health cost — uses energy).
void fireWhirlwind(Vec3 origin, const SkillDef* def, EntityPool& entities)
{
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    f32 range    = def->radius > 0.0f ? def->radius : 2.5f;
    f32 damage   = (def->damage > 0.0f ? def->damage : 30.0f) * s_classDmgMult;
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, range,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage);
    }

    // VFX: orange shockwave + spinning debris ring + screen shake
    if (s_novaCallback) s_novaCallback(origin, range, {0.8f, 0.3f, 0.1f});
    if (s_particlePool) {
        // Spinning debris ring around player
        for (u32 p = 0; p < 12; p++) {
            f32 angle = (6.2832f / 12.0f) * p;
            Vec3 sparkPos = origin + Vec3{cosf(angle) * range * 0.8f, 0.5f, sinf(angle) * range * 0.8f};
            ParticleSystem::spawnSparks(*s_particlePool, sparkPos, {0, 1, 0}, 2);
        }
    }
    if (s_screenShake) s_screenShake->trigger(0.06f, 0.3f);
    LOG_INFO("Whirlwind hit %u enemies", hitCount);
}

// Slam the ground — reuses meteor pool for an instant at-feet blast + knockback.
void fireEarthquake(Vec3 origin, const SkillDef* def, EntityPool& entities)
{
    f32 radius = def->radius > 0.0f ? def->radius : 4.0f;

    for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
        if (!s_meteors[i].active) {
            s_meteors[i].position = origin;
            s_meteors[i].damage   = (def->damage > 0.0f ? def->damage : 40.0f) * s_classDmgMult;
            s_meteors[i].radius   = radius;
            // Very short delay so it fires on the next updateMeteors tick
            s_meteors[i].timer    = 0.05f;
            s_meteors[i].active   = true;
            break;
        }
    }

    // Immediate knockback — push entities outward from the epicentre
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = entities.entities[i];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        Vec3 diff = e.position - origin;
        f32 dist2 = diff.x*diff.x + diff.z*diff.z;
        if (dist2 < radius * radius && dist2 > 0.01f) {
            Vec3 pushDir = normalize(diff);
            e.position = e.position + pushDir * 2.0f;
        }
    }

    LOG_INFO("Earthquake triggered");
}
