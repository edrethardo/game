#include "game/projectile.h"
#include "game/player.h"
#include "game/combat.h"
#include "world/combat_query.h"
#include "world/raycast.h"
#include "world/collision.h"

static ProjectileSystem::SplashCallback s_splashCallback = nullptr;

void ProjectileSystem::setSplashCallback(SplashCallback cb) {
    s_splashCallback = cb;
}

void ProjectileSystem::init(ProjectilePool& pool) {
    pool.activeCount = 0;
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        pool.projectiles[i].active = false;
    }
}

void ProjectileSystem::spawn(ProjectilePool& pool,
                              Vec3 origin, Vec3 direction, f32 speed,
                              f32 damage, f32 radius, f32 lifetime,
                              bool fromPlayer, u8 extraFlags)
{
    // Find first inactive slot
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        if (!pool.projectiles[i].active) {
            Projectile& p = pool.projectiles[i];
            p.position   = origin;
            p.velocity   = normalize(direction) * speed;
            p.radius     = radius;
            p.damage     = damage;
            p.lifetime   = lifetime;
            p.active     = true;
            p.projFlags  = extraFlags;  // caller can set PROJ_SPARK etc.
            p.gravity    = 0.0f;
            p.splashRadius = 0.0f;
            p.splashDamage = 0.0f;
            p.subTimer   = 0.0f;
            p.orbAngle   = 0.0f;
            p.fromPlayer = fromPlayer;
            pool.activeCount++;
            return;
        }
    }
}

static void destroyProjectile(ProjectilePool& pool, u32 idx) {
    pool.projectiles[idx].active = false;
    if (pool.activeCount > 0) pool.activeCount--;
}

void ProjectileSystem::update(ProjectilePool& pool,
                               const LevelGrid& grid,
                               EntityPool& entities,
                               Player& player,
                               f32 dt)
{
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        Projectile& p = pool.projectiles[i];
        if (!p.active) continue;

        // Lifetime
        p.lifetime -= dt;
        if (p.lifetime <= 0.0f) {
            destroyProjectile(pool, i);
            continue;
        }

        // Compute travel this frame
        f32 speed = length(p.velocity);
        f32 travel = speed * dt;
        if (travel < 0.0001f) {
            destroyProjectile(pool, i);
            continue;
        }
        Vec3 dir = p.velocity * (1.0f / speed);

        // Wall collision via short raycast
        RayHit wallHit = Raycast::cast(grid, p.position, dir, travel + p.radius);
        if (wallHit.hit && wallHit.distance <= travel + p.radius) {
            // AoE splash on wall impact
            if ((p.projFlags & PROJ_SPLASH) && p.splashRadius > 0.0f) {
                for (u32 e = 0; e < MAX_ENTITIES; e++) {
                    Entity& ent = entities.entities[e];
                    if (!(ent.flags & ENT_ACTIVE)) continue;
                    if (ent.flags & ENT_DEAD) continue;
                    Vec3 delta = ent.position - p.position;
                    f32 dist = length(delta);
                    if (dist < p.splashRadius) {
                        EntityHandle h = {static_cast<u16>(e), ent.generation};
                        Combat::applyDamage(entities, h, p.splashDamage);
                    }
                }
                if (s_splashCallback) s_splashCallback(p.position, p.splashRadius);
            }
            destroyProjectile(pool, i);
            continue;
        }

        // Apply gravity for arcing projectiles (e.g., molotov)
        if (p.projFlags & PROJ_GRAVITY) {
            p.velocity.y -= p.gravity * dt;
        }

        // Move
        p.position += p.velocity * dt;

        // Entity collision (AABB overlap)
        AABB projBox = {
            p.position - Vec3{p.radius, p.radius, p.radius},
            p.position + Vec3{p.radius, p.radius, p.radius}
        };

        if (p.fromPlayer) {
            // Hit enemies
            bool hit = false;
            for (u32 e = 0; e < MAX_ENTITIES; e++) {
                Entity& ent = entities.entities[e];
                if (!(ent.flags & ENT_ACTIVE)) continue;
                if (ent.flags & ENT_DEAD) continue;

                if (CombatQuery::aabbOverlap(projBox, entityAABB(ent))) {
                    EntityHandle h = {static_cast<u16>(e), ent.generation};
                    Combat::applyDamage(entities, h, p.damage);
                    hit = true;
                    break; // one hit per projectile
                }
            }
            if (hit) {
                // AoE splash on entity impact
                if ((p.projFlags & PROJ_SPLASH) && p.splashRadius > 0.0f) {
                    for (u32 e2 = 0; e2 < MAX_ENTITIES; e2++) {
                        Entity& ent2 = entities.entities[e2];
                        if (!(ent2.flags & ENT_ACTIVE)) continue;
                        if (ent2.flags & ENT_DEAD) continue;
                        Vec3 delta = ent2.position - p.position;
                        f32 dist = length(delta);
                        if (dist < p.splashRadius) {
                            EntityHandle h2 = {static_cast<u16>(e2), ent2.generation};
                            Combat::applyDamage(entities, h2, p.splashDamage);
                        }
                    }
                    if (s_splashCallback) s_splashCallback(p.position, p.splashRadius);
                }
                destroyProjectile(pool, i);
                continue;
            }
        } else {
            // Hit player
            AABB playerBox = {
                player.position + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
                player.position + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
            };
            if (CombatQuery::aabbOverlap(projBox, playerBox)) {
                Combat::applyDamageToPlayer(player, p.damage);
                destroyProjectile(pool, i);
                continue;
            }
        }
    }
}
