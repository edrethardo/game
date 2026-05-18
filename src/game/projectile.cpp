#include "game/projectile.h"
#include "game/player.h"
#include "game/combat.h"
#include "renderer/particles.h"
#include "world/combat_query.h"
#include "world/raycast.h"
#include "world/collision.h"
#include "world/spatial_grid.h"
#include <cstdlib>

// Set by Engine::init() so projectiles can spawn trail particles
static ParticlePool* s_trailPool = nullptr;
void ProjectileSystem_setTrailPool(ParticlePool* pool) { s_trailPool = pool; }

static f32 randf(f32 lo, f32 hi) {
    return lo + (hi - lo) * (std::rand() / static_cast<f32>(RAND_MAX));
}

static ProjectileSystem::SplashCallback s_splashCallback = nullptr;
static ProjectileSystem::HitCallback s_hitCallback = nullptr;
static ProjectileSystem::DamageNumberCallback s_dmgNumCallback = nullptr;

void ProjectileSystem::setSplashCallback(SplashCallback cb) {
    s_splashCallback = cb;
}
void ProjectileSystem::setHitCallback(HitCallback cb) {
    s_hitCallback = cb;
}
void ProjectileSystem::setDamageNumberCallback(DamageNumberCallback cb) {
    s_dmgNumCallback = cb;
}

void ProjectileSystem::init(ProjectilePool& pool) {
    pool.activeCount = 0;
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        pool.projectiles[i].active = false;
    }
}

u16 ProjectileSystem::spawn(ProjectilePool& pool,
                             Vec3 origin, Vec3 direction, f32 speed,
                             f32 damage, f32 radius, f32 lifetime,
                             bool fromPlayer, u8 extraFlags)
{
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        if (!pool.projectiles[i].active) {
            Projectile& p = pool.projectiles[i];
            p.position   = origin;
            p.velocity   = normalize(direction) * speed;
            p.radius     = radius;
            p.damage     = damage;
            p.lifetime   = lifetime;
            p.active     = true;
            p.projFlags  = extraFlags;
            p.gravity    = 0.0f;
            p.splashRadius = 0.0f;
            p.splashDamage = 0.0f;
            p.subTimer   = 0.0f;
            p.orbAngle   = 0.0f;
            p.meshId     = 0;
            p.fromPlayer = fromPlayer;
            pool.activeCount++;
            return static_cast<u16>(i);
        }
    }
    return 0xFFFF;
}

static void destroyProjectile(ProjectilePool& pool, u32 idx) {
    pool.projectiles[idx].active = false;
    if (pool.activeCount > 0) pool.activeCount--;
}

void ProjectileSystem::update(ProjectilePool& pool,
                               const LevelGrid& grid,
                               EntityPool& entities,
                               Player& player,
                               f32 dt,
                               const SpatialGrid* spatialGrid)
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
                for (u32 a = 0; a < entities.activeCount; a++) {
                    u32 e = entities.activeList[a];
                    Entity& ent = entities.entities[e];
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

        // Spawn trail particles for skill projectiles (every ~3rd frame to avoid flooding)
        if (s_trailPool && p.fromPlayer && ((i + static_cast<u32>(p.lifetime * 60.0f)) % 3 == 0)) {
            if (p.projFlags & PROJ_SPLASH) {
                // Fireball — red/orange trailing embers
                Particle tp = {};
                tp.position = p.position;
                tp.velocity = {randf(-0.3f, 0.3f), randf(0.1f, 0.4f), randf(-0.3f, 0.3f)};
                tp.life = 0.25f; tp.maxLife = 0.25f;
                tp.size = randf(0.03f, 0.06f); tp.baseAlpha = 0.9f;
                tp.r = 255; tp.g = static_cast<u8>(randf(60, 140)); tp.b = 0;
                tp.type = PTYPE_GEOMETRIC; tp.flags = PFLAG_FADE | PFLAG_SHRINK;
                ParticleSystem::spawn(*s_trailPool, tp);
            } else if (p.projFlags & PROJ_SPARK) {
                // Shock bolt — blue-white electric trail
                Particle tp = {};
                tp.position = p.position;
                tp.velocity = {randf(-0.5f, 0.5f), randf(-0.5f, 0.5f), randf(-0.5f, 0.5f)};
                tp.life = 0.15f; tp.maxLife = 0.15f;
                tp.size = randf(0.02f, 0.04f); tp.baseAlpha = 0.9f;
                tp.r = static_cast<u8>(randf(180, 220)); tp.g = static_cast<u8>(randf(200, 240)); tp.b = 255;
                tp.type = PTYPE_GEOMETRIC; tp.flags = PFLAG_FADE;
                ParticleSystem::spawn(*s_trailPool, tp);
            } else if (p.projFlags & PROJ_ORB) {
                // Frozen orb — cyan ice trail
                Particle tp = {};
                tp.position = p.position;
                tp.velocity = {randf(-0.4f, 0.4f), randf(-0.4f, 0.4f), randf(-0.4f, 0.4f)};
                tp.life = 0.2f; tp.maxLife = 0.2f;
                tp.size = randf(0.03f, 0.05f); tp.baseAlpha = 0.7f;
                tp.r = 100; tp.g = 200; tp.b = 255;
                tp.type = PTYPE_GEOMETRIC; tp.flags = PFLAG_FADE | PFLAG_SHRINK;
                ParticleSystem::spawn(*s_trailPool, tp);
            }
        }

        // Entity collision (AABB overlap)
        AABB projBox = {
            p.position - Vec3{p.radius, p.radius, p.radius},
            p.position + Vec3{p.radius, p.radius, p.radius}
        };

        if (p.fromPlayer) {
            // Hit hostile enemies only. Use spatial grid for O(1) neighbor lookup
            // instead of iterating all active entities (O(N) → O(~8) per projectile).
            bool hit = false;
            u16 primaryHitIdx = 0xFFFF;
            if (!(p.projFlags & PROJ_ORB)) { // Frozen Orb phases through enemies
                u16 nearby[72]; // 3x3 cells × 8 per cell max
                u32 nearCount = 0;
                if (spatialGrid) {
                    nearCount = SpatialGridSystem::queryNeighbors(*spatialGrid, p.position, nearby, 72);
                } else {
                    // Fallback: scan all active entities (no grid available)
                    for (u32 a = 0; a < entities.activeCount && nearCount < 72; a++)
                        nearby[nearCount++] = static_cast<u16>(entities.activeList[a]);
                }
                for (u32 n = 0; n < nearCount; n++) {
                    u32 e = nearby[n];
                    Entity& ent = entities.entities[e];
                    if (ent.flags & ENT_DEAD) continue;
                    if (ent.flags & ENT_FRIENDLY) continue;
                    if (ent.enemyType == EnemyType::PROP) continue;

                    if (CombatQuery::aabbOverlap(projBox, entityAABB(ent))) {
                        EntityHandle h = {static_cast<u16>(e), ent.generation};
                        Combat::applyDamage(entities, h, p.damage, &p.position);
                        if (p.freezeDuration > 0.0f) {
                            ent.freezeTimer = p.freezeDuration;
                            if (p.projFlags & PROJ_SPARK) ent.stunTimer = fmaxf(ent.stunTimer, 0.1f);
                        }
                        if (s_hitCallback) s_hitCallback(p.position, h);
                        primaryHitIdx = static_cast<u16>(e);
                        hit = true;
                        break;
                    }
                }
            }
            if (hit) {
                // AoE splash — use spatial grid for neighbor query
                if ((p.projFlags & PROJ_SPLASH) && p.splashRadius > 0.0f) {
                    u16 splashNear[72];
                    u32 splashCount = 0;
                    if (spatialGrid) {
                        splashCount = SpatialGridSystem::queryNeighbors(*spatialGrid, p.position, splashNear, 72);
                    } else {
                        for (u32 a2 = 0; a2 < entities.activeCount && splashCount < 72; a2++)
                            splashNear[splashCount++] = static_cast<u16>(entities.activeList[a2]);
                    }
                    for (u32 n = 0; n < splashCount; n++) {
                        u32 e2 = splashNear[n];
                        if (e2 == primaryHitIdx) continue;
                        Entity& ent2 = entities.entities[e2];
                        if (ent2.flags & ENT_DEAD) continue;
                        if (ent2.flags & ENT_FRIENDLY) continue;
                        if (ent2.enemyType == EnemyType::PROP) continue;
                        f32 distSq = lengthSq(ent2.position - p.position);
                        if (distSq < p.splashRadius * p.splashRadius) {
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
                // Wanderer Deflect: active parry window reflects projectiles back
                if (player.deflectTimer > 0.0f) {
                    p.velocity = p.velocity * -1.0f; // reverse direction
                    p.fromPlayer = true;              // now a player-owned projectile
                    continue;                         // skip damage this frame
                }
                Combat::applyDamageToPlayer(player, p.damage, &p.position);
                // Apply on-hit status effect from projectile (or default slow)
                if (p.onHitEffect == 1) {  // poison
                    player.poisonTimer = fmaxf(player.poisonTimer, p.onHitDuration);
                    player.poisonDps = 4.0f;
                } else if (p.onHitEffect == 2) {  // slow
                    player.slowTimer = fmaxf(player.slowTimer, p.onHitDuration);
                } else if (p.onHitEffect == 3) {  // burn
                    player.burnTimer = fmaxf(player.burnTimer, p.onHitDuration);
                } else if (p.onHitEffect == 4) {  // freeze
                    player.freezeTimer = fmaxf(player.freezeTimer, p.onHitDuration);
                } else {
                    // Default: mild slow for enemy projectiles
                    player.slowTimer = 2.5f;
                }
                destroyProjectile(pool, i);
                continue;
            }
        }
    }
}
