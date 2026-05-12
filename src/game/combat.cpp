#include "game/combat.h"
#include "game/player.h"
#include "game/projectile.h"
#include "world/combat_query.h"
#include "core/log.h"
#include <cmath>

static Combat::DeathCallback s_deathCallback = nullptr;
static Combat::PerfectBlockCallback s_perfectBlockCallback = nullptr;

void Combat::setDeathCallback(DeathCallback cb) {
    s_deathCallback = cb;
}

void Combat::setPerfectBlockCallback(PerfectBlockCallback cb) {
    s_perfectBlockCallback = cb;
}

void Combat::applyDamage(EntityPool& pool, EntityHandle target, f32 damage) {
    Entity* e = handleGet(pool, target);
    if (!e) return;
    if (e->flags & ENT_DEAD) return;

    // Paladin passive: 25% damage reduction
    if (e->npcClass == NpcClass::PALADIN) damage *= 0.75f;

    e->health -= damage;
    e->flashTimer = 0.12f;

    if (e->health <= 0.0f) {
        e->health     = 0.0f;
        e->flags     |= ENT_DEAD;
        e->aiState    = AIState::DEAD;
        e->deathTimer = 1.0f;
        e->velocity   = {0,0,0};
        if (s_deathCallback) {
            s_deathCallback(pool, target.index, e->position);
        }
    }
}

void Combat::applyDamageToPlayer(Player& player, f32 damage, const Vec3* attackerPos) {
    // Invulnerability blocks all damage (respawn/floor entry grace period)
    if (player.invulnTimer > 0.0f) return;

    // Class passive damage reduction (e.g. Warrior 30%)
    damage *= (1.0f - player.damageReduction);

    if (player.blocking) {
        if (player.blockTimer < 0.2f) {
            // Perfect block — negate all damage, trigger shield bash via callback
            damage = 0.0f;
            if (s_perfectBlockCallback) s_perfectBlockCallback(player);
        } else {
            // Normal block — halve damage
            damage *= 0.5f;
        }
    }

    player.health -= damage;
    player.damageFlashTimer = 0.15f;
    player.hitShakeTimer = 0.15f;
    // Track damage taken this frame for ring passives (thorns, etc.)
    player.lastDamageTaken = damage;

    // Record hit direction for CS-style directional indicator
    if (attackerPos && damage > 0.0f) {
        Vec3 toAttacker = *attackerPos - player.position;
        f32 worldAngle = atan2f(toAttacker.x, toAttacker.z);
        f32 relAngle = worldAngle - player.yaw;
        for (u32 i = 0; i < Player::MAX_HIT_INDICATORS; i++) {
            if (player.hitIndicators[i].timer <= 0.0f) {
                player.hitIndicators[i] = {relAngle, 0.8f};
                break;
            }
        }
    }

    if (player.health <= 0.0f) {
        player.health = 0.0f;
    }
}

AttackResult Combat::fireMelee(const WeaponDef& weapon,
                                Vec3 eyePos, Vec3 forward,
                                EntityPool& pool)
{
    AttackResult result;
    result.didFire = true;

    f32 halfAngle = radians(weapon.coneAngleDeg * 0.5f);
    f32 cosCone   = cosf(halfAngle);

    EntityHandle hits[MAX_ENTITIES];
    f32 distances[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        pool, eyePos, forward, cosCone, weapon.range,
        hits, distances, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        applyDamage(pool, hits[i], weapon.damage);
    }

    result.entitiesHit = hitCount;
    if (hitCount > 0) {
        result.hitEntity = true;
        Entity* e = handleGet(pool, hits[0]);
        if (e) {
            result.hitPosition = e->position;
            result.hitDistance  = distances[0];
        }
    }

    return result;
}

AttackResult Combat::fireHitscan(const WeaponDef& weapon,
                                  Vec3 eyePos, Vec3 forward,
                                  const LevelGrid& grid,
                                  EntityPool& pool)
{
    AttackResult result;
    result.didFire = true;

    CombatHit hit = CombatQuery::raycast(grid, pool, eyePos, forward, weapon.range);

    if (hit.hit) {
        result.hitPosition = hit.position;
        result.hitNormal   = hit.normal;
        result.hitDistance  = hit.distance;

        if (hit.type == CombatHit::ENTITY) {
            result.hitEntity   = true;
            result.entitiesHit = 1;
            applyDamage(pool, hit.entityHandle, weapon.damage);
        } else {
            result.hitWorld = true;
        }
    }

    return result;
}

u16 Combat::fireProjectile(const WeaponDef& weapon,
                            Vec3 eyePos, Vec3 forward,
                            ProjectilePool& projectiles,
                            u8 extraFlags)
{
    // Spawn 1m ahead of the camera so the projectile doesn't clip the viewmodel
    Vec3 spawnPos = eyePos + forward * 1.0f;
    return ProjectileSystem::spawn(projectiles, spawnPos, forward,
                                    weapon.projectileSpeed, weapon.damage,
                                    weapon.projectileRadius, 3.0f, true, extraFlags);
}

u16 Combat::fireProjectile(const WeaponDef& weapon,
                            Vec3 eyePos, Vec3 forward,
                            ProjectilePool& projectiles,
                            f32 gravity, f32 splashRadius, f32 splashDamage)
{
    Vec3 spawnPos = eyePos + forward * 1.0f;
    u16 idx = ProjectileSystem::spawn(projectiles, spawnPos, forward,
                                       weapon.projectileSpeed, weapon.damage,
                                       weapon.projectileRadius, 5.0f, true);
    if (idx != 0xFFFF) {
        Projectile& p = projectiles.projectiles[idx];
        p.gravity      = gravity;
        p.splashRadius = splashRadius;
        p.splashDamage = splashDamage;
        if (gravity > 0.0f) p.projFlags |= PROJ_GRAVITY;
        if (splashRadius > 0.0f) p.projFlags |= PROJ_SPLASH;
    }
    return idx;
}
