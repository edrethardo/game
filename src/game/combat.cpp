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

void Combat::applyDamageToPlayer(Player& player, f32 damage) {
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

bool Combat::fireProjectile(const WeaponDef& weapon,
                             Vec3 eyePos, Vec3 forward,
                             ProjectilePool& projectiles,
                             u8 extraFlags)
{
    f32 lifetime = 3.0f;
    ProjectileSystem::spawn(projectiles, eyePos, forward,
                            weapon.projectileSpeed, weapon.damage,
                            weapon.projectileRadius, lifetime, true, extraFlags);
    return true;
}

bool Combat::fireProjectile(const WeaponDef& weapon,
                             Vec3 eyePos, Vec3 forward,
                             ProjectilePool& projectiles,
                             f32 gravity, f32 splashRadius, f32 splashDamage)
{
    f32 lifetime = 5.0f; // longer lifetime for arcing projectiles

    // Find an inactive slot and configure it
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        if (!projectiles.projectiles[i].active) {
            Projectile& p = projectiles.projectiles[i];
            p.position   = eyePos;
            p.velocity   = normalize(forward) * weapon.projectileSpeed;
            p.radius     = weapon.projectileRadius;
            p.damage     = weapon.damage;
            p.lifetime   = lifetime;
            p.active     = true;
            p.fromPlayer = true;
            p.projFlags  = 0;
            p.subTimer   = 0.0f;
            p.orbAngle   = 0.0f;
            p.gravity      = gravity;
            p.splashRadius = splashRadius;
            p.splashDamage = splashDamage;

            if (gravity > 0.0f) p.projFlags |= PROJ_GRAVITY;
            if (splashRadius > 0.0f) p.projFlags |= PROJ_SPLASH;

            projectiles.activeCount++;
            return true;
        }
    }
    return false;
}
