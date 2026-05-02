#include "game/combat.h"
#include "game/player.h"
#include "game/projectile.h"
#include "world/combat_query.h"
#include "core/log.h"
#include <cmath>

void Combat::applyDamage(EntityPool& pool, EntityHandle target, f32 damage) {
    Entity* e = handleGet(pool, target);
    if (!e) return;
    if (e->flags & ENT_DEAD) return;

    e->health -= damage;
    e->flashTimer = 0.12f;

    if (e->health <= 0.0f) {
        e->health     = 0.0f;
        e->flags     |= ENT_DEAD;
        e->aiState    = AIState::DEAD;
        e->deathTimer = 1.0f;
        e->velocity   = {0,0,0};
    }
}

void Combat::applyDamageToPlayer(Player& player, f32 damage) {
    player.health -= damage;
    player.damageFlashTimer = 0.15f;
    if (player.health <= 0.0f) {
        player.health = 0.0f;
        // TODO: death state
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

bool Combat::fireProjectile(const WeaponDef& weapon,
                             Vec3 eyePos, Vec3 forward,
                             ProjectilePool& projectiles)
{
    f32 lifetime = 3.0f; // 3 second lifetime
    ProjectileSystem::spawn(projectiles, eyePos, forward,
                            weapon.projectileSpeed, weapon.damage,
                            weapon.projectileRadius, lifetime, true);
    return true;
}
