// skill_marksman.cpp — fire* helpers for the Marksman class skills:
// AimedShot, ExplosiveRound, OverchargedMagazine, Headshot.
//
// Marksman skills scale off s_weaponDamage (set by engine before activation).
// s_overchargeTimer / s_overchargeShots are defined in skill_system.cpp
// (owned by SkillSystem::isOvercharged / consumeOverchargeShot / tickOvercharge)
// and written here by fireOverchargedMagazine.

#include "game/skill_internal.h"

// Penetrating Rail — pierces through ALL enemies in a line. Weapon damage × 1.2.
// Leaves visible beam trail. Applies 2s slow to all hit.
void fireAimedShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                   const LevelGrid& grid, EntityPool& entities)
{
    // Scales off weapon damage (Marksman gimmick: skills amplify weapon)
    f32 damage = s_weaponDamage * 1.2f * s_classDmgMult;
    f32 range  = 80.0f;

    // Penetrating rail: test the ray against every entity AABB along the line. Precise like a
    // hitscan, but pierces all of them.
    // Uses the shared CombatQuery::rayVsAABB rather than a hand-rolled slab test — the duplicate
    // copy that used to live here silently disagreed with the shared one (it reported a hit for
    // boxes entirely BEHIND the player, which the shared version now rejects).
    u32 hitCount = 0;
    for (u32 a = 0; a < entities.activeCount; a++) {
        u32 idx = entities.activeList[a];
        Entity& e = entities.entities[idx];
        if (e.flags & ENT_DEAD) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;   // don't waste the rail on decorations

        f32 t; Vec3 n;
        if (!CombatQuery::rayVsAABB(origin, forward, entityAABB(e), t, n)) continue;
        if (t > range) continue;

        // Ray intersects this entity's AABB — apply damage
        EntityHandle h = {static_cast<u16>(idx), e.generation};
        Combat::applyDamage(entities, h, damage);
        e.freezeTimer = 2.0f;
        if (s_particlePool) ParticleSystem::spawnDebris(*s_particlePool, e.position, 4);
        hitCount++;
    }

    // Beam trail from origin to wall/max range
    RayHit wallHit = Raycast::cast(grid, origin, forward, range);
    Vec3 beamEnd = wallHit.hit ? (origin + forward * wallHit.distance)
                               : (origin + forward * range);
    if (s_beamCallback) s_beamCallback(origin, beamEnd, {1.0f, 0.8f, 0.4f});

    // Muzzle flash sparks + recoil shake
    if (s_particlePool) ParticleSystem::spawnSparks(*s_particlePool, origin, forward, 4);
    if (s_screenShake) s_screenShake->trigger(0.05f, 0.25f);

    // Small nova at wall impact
    if (wallHit.hit && s_novaCallback) {
        s_novaCallback(beamEnd, 0.5f, {1.0f, 0.7f, 0.3f});
    }
    LOG_INFO("Aimed Shot: pierced %u enemies", hitCount);
}

// Devastation Shot — hitscan detonation with massive AoE, scorch crater, knockback.
void fireExplosiveRound(Vec3 origin, Vec3 forward, const SkillDef* def,
                        const LevelGrid& grid, EntityPool& entities)
{
    // Scales off weapon damage (Marksman gimmick: skills amplify weapon)
    f32 damage  = s_weaponDamage * 1.2f * s_classDmgMult;
    f32 splashR = def->radius > 0.0f ? def->radius : 3.5f;

    // Find detonation point via raycast
    RayHit rh = Raycast::cast(grid, origin, forward, 60.0f);
    Vec3 detonPos = rh.hit ? (origin + forward * rh.distance)
                           : (origin + forward * 60.0f);

    // Direct hitscan damage to first target
    WeaponDef temp;
    temp.name         = "Explosive Round";
    temp.type         = WeaponType::HITSCAN;
    temp.damage       = damage;
    temp.range        = 60.0f;
    temp.coneAngleDeg = 1.0f;
    temp.cooldown     = 0.0f;
    temp.recoilKick   = 0.02f;
    Combat::fireHitscan(temp, origin, forward, grid, entities);

    // Splash AoE at detonation — 75% damage + 3m knockback
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, detonPos, {0.0f, -1.0f, 0.0f}, -1.0f, splashR,
        hits, dists, MAX_ENTITIES);
    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage * 0.75f);
        Entity* e = handleGet(entities, hits[i]);
        if (e) {
            Vec3 pushDir = e->position - detonPos;
            f32 pushLen2 = pushDir.x*pushDir.x + pushDir.z*pushDir.z;
            if (pushLen2 > 0.01f) {
                pushDir = normalize(pushDir);
                e->position = e->position + pushDir * 3.0f;
            }
        }
    }

    // Beam trail to detonation
    if (s_beamCallback) s_beamCallback(origin, detonPos, {1.0f, 0.6f, 0.2f});

    // Massive explosion visuals
    if (s_novaCallback) s_novaCallback(detonPos, splashR, {1.0f, 0.5f, 0.1f});
    if (s_scorchCallback) s_scorchCallback(detonPos, splashR, 2.0f, 5.0f * s_classDmgMult);
    if (s_particlePool) {
        ParticleSystem::spawnExplosion(*s_particlePool, detonPos, splashR);
        ParticleSystem::spawnDebris(*s_particlePool, detonPos, 12);
        ParticleSystem::spawnSmoke(*s_particlePool, detonPos, 8);
    }
    if (s_screenShake) s_screenShake->trigger(0.12f, 0.5f);
    LOG_INFO("Explosive Round: splash hit %u enemies", hitCount);
}

// Overcharged Magazine — activates a 4s buff. Next 5 weapon shots deal 3× damage + pierce.
void fireOverchargedMagazine(Vec3 origin, Vec3 forward)
{
    s_overchargeTimer[s_castingPlayer] = 4.0f;
    s_overchargeShots[s_castingPlayer] = 5;

    // Activation VFX
    if (s_particlePool) {
        ParticleSystem::spawnSparks(*s_particlePool, origin, forward, 6);
        ParticleSystem::spawnMagicBurst(*s_particlePool, origin, 255, 200, 50, 8);
    }
    if (s_screenShake) s_screenShake->trigger(0.04f, 0.2f);
    LOG_INFO("Overcharged Magazine: 5 shots buffed for 4s");
}

// Execution Strike — execute <25% HP, chain-execute nearby wounded, massive VFX.
void fireHeadshot(Vec3 origin, Vec3 forward, const SkillDef* def,
                  const LevelGrid& grid, EntityPool& entities,
                  SkillState& skillState)
{
    // Nearest enemy the shot actually passes THROUGH.
    //
    // This was a 2° cone (queryConeSorted), and that is why Headshot could not hit anything close.
    // A cone measures the angle from the eye to the entity's CENTRE, so its linear aim tolerance is
    // dist * tan(2°): about 70 cm at 20 m, but only 10 cm at 3 m and 5 cm at 1.5 m. A point-blank
    // enemy filling the screen therefore had a hit window of a few centimetres around one exact
    // point — and aiming at its HEAD, for a skill called Headshot, missed at every range under 20 m.
    // A ray-vs-AABB test has the same tolerance at every distance: the size of the target.
    // (The melee cone hit the same wall and was patched with `horizontalCone`; see combat_query.cpp.)
    EntityHandle target;
    f32          targetT = 0.0f;
    const bool   found = CombatQuery::rayNearestEntity(entities, origin, forward, 80.0f,
                                                       target, targetT);

    // Beam trail to wall/target regardless of hit
    RayHit wallHit = Raycast::cast(grid, origin, forward, 80.0f);
    Vec3 beamEnd = wallHit.hit ? (origin + forward * wallHit.distance)
                               : (origin + forward * 80.0f);

    if (found) {
        Entity* e = handleGet(entities, target);
        if (e && !(e->flags & ENT_DEAD) && !(e->flags & ENT_FRIENDLY)) {
            bool isExecute = (e->health < e->maxHealth * 0.25f);
            // Non-execute: scales off weapon damage (Marksman gimmick)
            f32 damage = isExecute
                         ? e->health + 1.0f
                         : s_weaponDamage * 2.5f * s_classDmgMult;
            Combat::applyDamage(entities, target, damage);
            beamEnd = e->position; // beam goes to target

            if (isExecute) {
                // Execute kill! Reset cooldown + instant reload + chain execute + massive VFX
                skillState.cooldownTimer = 0.0f;
                if (s_reloadCallback) s_reloadCallback();

                // Debris explosion on primary target
                if (s_particlePool) {
                    ParticleSystem::spawnExplosion(*s_particlePool, e->position, 1.5f);
                    ParticleSystem::spawnDebris(*s_particlePool, e->position, 10);
                }
                // Shockwave ring
                if (s_novaCallback) s_novaCallback(e->position, 4.0f, {1.0f, 0.2f, 0.05f});
                if (s_screenShake) s_screenShake->trigger(0.1f, 0.4f);

                // Chain execute: kill all enemies below 25% HP within 4m of target
                EntityHandle chainHits[MAX_ENTITIES];
                f32          chainDists[MAX_ENTITIES];
                u32 chainCnt = CombatQuery::queryConeSorted(
                    entities, e->position, {0.0f, -1.0f, 0.0f}, -1.0f, 4.0f,
                    chainHits, chainDists, MAX_ENTITIES);
                u32 chainKills = 0;
                for (u32 j = 0; j < chainCnt; j++) {
                    if (chainHits[j].index == target.index) continue; // skip primary
                    Entity* ce = handleGet(entities, chainHits[j]);
                    if (!ce || (ce->flags & ENT_DEAD) || (ce->flags & ENT_FRIENDLY)) continue;
                    if (ce->health < ce->maxHealth * 0.25f) {
                        Combat::applyDamage(entities, chainHits[j], ce->health + 1.0f);
                        if (s_particlePool) ParticleSystem::spawnDebris(*s_particlePool, ce->position, 6);
                        chainKills++;
                    }
                }
                LOG_INFO("Headshot EXECUTE + %u chain kills", chainKills);
            } else {
                // Non-execute: small debris at impact
                if (s_particlePool) ParticleSystem::spawnDebris(*s_particlePool, e->position, 3);
                LOG_INFO("Headshot: %.0f damage (no execute)", damage);
            }
        }
    }

    // Always draw beam trail (red-orange for headshot)
    if (s_beamCallback) s_beamCallback(origin, beamEnd, {1.0f, 0.3f, 0.1f});
}
