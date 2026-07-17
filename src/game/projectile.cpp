#include "game/projectile.h"
#include "game/player.h"
#include "game/combat.h"
#include "game/item.h"      // SkillId::PROJECTILE_PARRY (Mirror Aegis reflect check)
#include "renderer/particles.h"
#include "world/combat_query.h"
#include "world/raycast.h"
#include "world/collision.h"
#include "world/spatial_grid.h"
#include "audio/audio.h"
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
static ProjectileSystem::PlayerHitCallback s_playerHitCallback = nullptr;  // M10.3

void ProjectileSystem::setSplashCallback(SplashCallback cb) {
    s_splashCallback = cb;
}
void ProjectileSystem::setHitCallback(HitCallback cb) {
    s_hitCallback = cb;
}
void ProjectileSystem::setDamageNumberCallback(DamageNumberCallback cb) {
    s_dmgNumCallback = cb;
}
void ProjectileSystem::setPlayerHitCallback(PlayerHitCallback cb) {  // M10.3
    s_playerHitCallback = cb;
}

void ProjectileSystem::init(ProjectilePool& pool) {
    pool.activeCount = 0;
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        pool.projectiles[i].active = false;
        pool.activeList[i] = 0;
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
            pool.activeList[pool.activeCount] = static_cast<u16>(i);
            pool.activeCount++;
            return static_cast<u16>(i);
        }
    }
    return 0xFFFF;
}

static void destroyProjectile(ProjectilePool& pool, u32 idx) {
    pool.projectiles[idx].active = false;
    // Swap-remove from activeList
    for (u32 a = 0; a < pool.activeCount; a++) {
        if (pool.activeList[a] == static_cast<u16>(idx)) {
            pool.activeList[a] = pool.activeList[pool.activeCount - 1];
            break;
        }
    }
    if (pool.activeCount > 0) pool.activeCount--;
}

// Public wrapper so the engine can retire a projectile (Infinity Chakram per-owner cap).
void ProjectileSystem::despawn(ProjectilePool& pool, u16 idx) {
    if (idx >= MAX_PROJECTILES || !pool.projectiles[idx].active) return;
    destroyProjectile(pool, idx);
}

// Result of testing one enemy projectile against one player. REFLECTED = Mirror Aegis parry:
// the projectile SURVIVES, now player-owned and outbound — the caller must not destroy it.
enum class PlayerHitResult { MISS, DEFLECTED, HIT, REFLECTED };

// Apply an enemy projectile to one player: Wanderer Deflect absorb (full immunity
// incl. status), else damage + on-hit status. Factored out of the old P1-only branch
// so every local player (split-screen) gets identical treatment.
// M10.3: fires s_playerHitCallback on a confirmed hit so the server can emit
// SV_DAMAGE_TO_ME to the victim's network slot.
static PlayerHitResult tryHitPlayer(Projectile& p, const AABB& projBox, Player& player) {
    // A dead player (corpse) doesn't collide — the projectile passes through and is still tested
    // against any living players after this one. Single guard here covers the primary AND every
    // extra (and any future caller), mirroring EnemyAI skipping dead targets. Without it an enemy
    // projectile would "hit" a corpse: wasted damage + the SV_DAMAGE_TO_ME callback to a dead slot.
    if (player.health <= 0.0f) return PlayerHitResult::MISS;
    AABB playerBox = {
        player.position + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
        player.position + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
    };
    if (!CombatQuery::aabbOverlap(projBox, playerBox)) return PlayerHitResult::MISS;
    // Wanderer Deflect: absorb into the deflect pool (full immunity incl. status).
    if (player.deflectTimer > 0.0f) {
        player.deflectAbsorbed += p.damage;
        player.deflectHitCount++;
        p.lifetime = 0.0f; // destroy absorbed projectile (reaped next frame)
        return PlayerHitResult::DEFLECTED;
    }
    Combat::BlockOutcome outcome = Combat::applyDamageToPlayer(player, p.damage, &p.position);
    // Mirror Aegis: a PERFECT block reflects the projectile instead of eating it. Checked
    // before the on-hit status below — a parried shot must not also poison/slow the blocker —
    // and before the damage callback (a fully-negated, returned hit is not damage feedback).
    if (outcome == Combat::BlockOutcome::PERFECT &&
        player.offhandSkill == static_cast<u8>(SkillId::PROJECTILE_PARRY)) {
        ProjectileSystem::reflectAsParry(p, player.netSlot);
        return PlayerHitResult::REFLECTED;
    }
    // M10.3: notify the server so it can send SV_DAMAGE_TO_ME to the victim client.
    if (s_playerHitCallback) s_playerHitCallback(p.ownerSlot, p.clientTick, p.damage, &player);
    // Apply on-hit status effect from projectile (or default slow)
    if (p.onHitEffect == 1) {        // poison
        player.poisonTimer = fmaxf(player.poisonTimer, p.onHitDuration);
        player.poisonDps = 4.0f;
    } else if (p.onHitEffect == 2) { // slow
        player.slowTimer = fmaxf(player.slowTimer, p.onHitDuration);
    } else if (p.onHitEffect == 3) { // burn
        player.burnTimer = fmaxf(player.burnTimer, p.onHitDuration);
    } else if (p.onHitEffect == 4) { // freeze
        player.freezeTimer = fmaxf(player.freezeTimer, p.onHitDuration);
    } else {
        player.slowTimer = 2.5f;     // default mild slow
    }
    return PlayerHitResult::HIT;
}

// PvP (Arena mode): a PLAYER projectile tests the registered PvP combatants — everyone but its
// owner. Mirrors tryHitPlayer (its enemy-projectile twin above) with three deliberate
// differences: the owner never hits themselves, the SV_DAMAGE_TO_ME callback is skipped (a PvP
// victim never predicted this damage — their HP adopts from the snapshot), and the enemy-only
// DEFAULT slow is dropped (only authored on-hit statuses — poison/slow/burn/freeze — carry over).
enum struct PvpProjResult : u8 { MISS, CONSUMED, REFLECTED };
static PvpProjResult tryHitPvpTargets(Projectile& p, const AABB& projBox) {
    u32 n = 0;
    const Combat::PvpTarget* ts = Combat::pvpTargets(n);
    for (u32 i = 0; i < n; i++) {
        Player* v = ts[i].view;
        if (!v || ts[i].slot == p.ownerSlot || v->health <= 0.0f) continue;
        AABB box = {
            v->position + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
            v->position + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
        };
        if (!CombatQuery::aabbOverlap(projBox, box)) continue;
        // Deflect / block / statuses / kill credit all live in the engine's atomic apply —
        // this function owns only geometry and the projectile's fate.
        Combat::PvpHit hit{p.damage, p.position, p.ownerSlot, /*projectile=*/true,
                           p.onHitEffect, p.onHitDuration};
        Combat::PvpHitOutcome out = Combat::pvpApply(ts[i].slot, hit);
        v->health = out.newHealth;   // keep the geometry snapshot honest for later samples
        if (out.deflected) {
            p.lifetime = 0.0f;       // absorbed — reaped like the enemy-projectile deflect
            return PvpProjResult::CONSUMED;
        }
        if (out.block == Combat::BlockOutcome::PERFECT &&
            v->offhandSkill == static_cast<u8>(SkillId::PROJECTILE_PARRY)) {
            // Mirror Aegis: the shot flies back under the blocker's ownership — in PvP that
            // means it can kill the original shooter. Registry slot, not v->netSlot: the slot
            // is authoritative for remote views and couch lanes alike.
            ProjectileSystem::reflectAsParry(p, ts[i].slot);
            return PvpProjResult::REFLECTED;
        }
        return PvpProjResult::CONSUMED;
    }
    return PvpProjResult::MISS;
}

void ProjectileSystem::update(ProjectilePool& pool,
                               const LevelGrid& grid,
                               EntityPool& entities,
                               Player& player,
                               f32 dt,
                               const SpatialGrid* spatialGrid,
                               Player** extraPlayers,
                               u32 extraPlayerCount)
{
    // Snapshot activeCount before iterating — destroyProjectile() decrements it
    // mid-loop, which would cause early exit and leave later projectiles stuck.
    u32 startCount = pool.activeCount;
    u32 seen = 0;
    for (u32 i = 0; i < MAX_PROJECTILES && seen < startCount; i++) {
        Projectile& p = pool.projectiles[i];
        if (!p.active) continue;
        seen++;

        // (L8) Credit any entity kill this projectile causes (direct or splash) to the firer,
        // even though the projectile resolves frames after it was fired. Enemy projectiles
        // (fromPlayer=false) damage players, not entities, so 0xFF is the safe default.
        Combat::setAttackingPlayer(p.fromPlayer ? p.ownerSlot : 0xFF);

        // Lifetime. Infinity Chakram (PROJ_INFINITE_BOUNCE) never expires on time — it only
        // despawns on hitting a target — so its lifetime instead counts UP as an age that the
        // engine's per-owner cap uses to retire the oldest one. Everything else decays + dies.
        if (p.projFlags & PROJ_INFINITE_BOUNCE) {
            p.lifetime += dt; // age only
        } else {
            p.lifetime -= dt;
            if (p.lifetime <= 0.0f) {
                destroyProjectile(pool, i);
                continue;
            }
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
            // Chakram ricochet: reflect off the wall instead of despawning, up to bouncesLeft times.
            // The raycast already gives the outward face normal (axis-aligned ±X/±Z/±Y), so a bounce
            // is the standard reflection v' = v - 2(v·n)n. Speed is preserved across bounces; the
            // projectile dies on the next wall once bounces run out (falls through below) or when its
            // lifetime backstop expires.
            // PROJ_INFINITE_BOUNCE (Infinity Chakram) bounces forever; the normal chakram bounces
            // until bouncesLeft runs out.
            bool infinite = (p.projFlags & PROJ_INFINITE_BOUNCE) != 0;
            if ((p.projFlags & PROJ_BOUNCE) && (infinite || p.bouncesLeft > 0)) {
                if (!infinite) p.bouncesLeft--;
                // Sit just off the struck face (along its normal) so next tick's raycast doesn't
                // immediately re-hit the same wall and consume another bounce.
                p.position = wallHit.position + wallHit.normal * (p.radius + 0.02f);
                p.velocity = p.velocity - wallHit.normal * (2.0f * dot(p.velocity, wallHit.normal));
                // Metallic ricochet at the bounce (chakram / any PROJ_BOUNCE projectile). Positional
                // so it attenuates with distance; player is the listener.
                AudioSystem::playAt(SfxId::RICOCHET, wallHit.position, player.position);
                continue; // keep flying; skip this frame's despawn + move (already repositioned)
            }
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

        // Move (pre-move position kept: swept projectiles test entities along this segment)
        const Vec3 preMovePos = p.position;
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
            //
            // SAMPLE POINTS along the whole per-tick travel segment instead of only the
            // post-move endpoint — derived from travel vs radius for EVERY projectile (see
            // sweepSampleCount's header comment for the tunneling math; the old opt-in `swept`
            // flag covered throwing knives only, and crossbows/shurikens/staff bolts kept
            // stepping over grazes and small enemies). The LAST sample is the exact post-move
            // position, so slow projectiles (samples == 1) run the byte-identical old test.
            const u32 sweepSamples = sweepSampleCount(travel, p.radius);
            bool hit = false;
            u16 primaryHitIdx = 0xFFFF;
            if (!(p.projFlags & PROJ_ORB))   // Frozen Orb phases through enemies
            for (u32 sw = 1; sw <= sweepSamples && !hit; sw++) {
                const Vec3 samplePos = (sw == sweepSamples)
                    ? p.position
                    : preMovePos + dir * (travel * static_cast<f32>(sw) / static_cast<f32>(sweepSamples));
                projBox = { samplePos - Vec3{p.radius, p.radius, p.radius},
                            samplePos + Vec3{p.radius, p.radius, p.radius} };
                u16 nearby[72]; // 3x3 cells × 8 per cell max
                u32 nearCount = 0;
                if (spatialGrid) {
                    nearCount = SpatialGridSystem::queryNeighbors(*spatialGrid, samplePos, nearby, 72);
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
                    if (ent.flags & ENT_BURROWED) continue;   // underground — shots pass over it

                    if (CombatQuery::aabbOverlap(projBox, entityAABB(ent))) {
                        // Impact happened at the sample, not the endpoint — pull the projectile
                        // back so the damage origin, splash center and destroy-FX sit on the hit.
                        p.position = samplePos;
                        EntityHandle h = {static_cast<u16>(e), ent.generation};
                        // p.isCrit was set at spawn in Combat::fireProjectile — pass it
                        // through so the CRIT feedback tier fires on direct hits.
                        // Splash hits below are intentionally NON-crit (AoE doesn't crit).
                        Combat::applyDamage(entities, h, p.damage, &p.position, p.isCrit);
                        if (p.freezeDuration > 0.0f) {
                            ent.freezeTimer = p.freezeDuration;
                            if (p.projFlags & PROJ_SPARK) ent.stunTimer = fmaxf(ent.stunTimer, 0.1f);
                        }
                        // Stun (Stun Grenade). fmaxf so a weaker stun can't cut a stronger one short.
                        if (p.stunDuration > 0.0f)
                            ent.stunTimer = fmaxf(ent.stunTimer, p.stunDuration);
                        // Poison DoT (Poison Arrow). poisonSrcSlot credits the kill to the firer —
                        // the DoT resolves frames later, long after this call returns.
                        if (p.poisonDuration > 0.0f && p.poisonDps > 0.0f) {
                            ent.poisonTimer   = fmaxf(ent.poisonTimer, p.poisonDuration);
                            ent.poisonDps     = fmaxf(ent.poisonDps, p.poisonDps);
                            ent.poisonSrcSlot = p.ownerSlot;
                        }
                        if (s_hitCallback) s_hitCallback(p.position, h, p.ownerSlot, p.damage);
                        primaryHitIdx = static_cast<u16>(e);
                        hit = true;
                        break;
                    }
                }
                // PvP (Arena): the same sweep sample also tests rival players, so cover and
                // anti-tunneling behave identically for a monster and a human target. Runs
                // only when the sample found no entity (registry is empty outside the arena).
                if (!hit && Combat::pvpActive()) {
                    PvpProjResult pr = tryHitPvpTargets(p, projBox);
                    if (pr == PvpProjResult::CONSUMED) {
                        p.position = samplePos;   // impact at the sample, like the entity path
                        hit = true;               // falls into the shared destroy below
                    } else if (pr == PvpProjResult::REFLECTED) {
                        break;                    // parried: keeps flying under new ownership
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
                            // A stun grenade must stun what it BLASTS, not just what it happens to
                            // strike — the blast is the whole point of the weapon.
                            if (p.stunDuration > 0.0f)
                                ent2.stunTimer = fmaxf(ent2.stunTimer, p.stunDuration);
                        }
                    }
                    if (s_splashCallback) s_splashCallback(p.position, p.splashRadius);
                }
                destroyProjectile(pool, i);
                continue;
            }
        } else {
            // Enemy projectile — it must also collide with the player's FRIENDLY entities
            // (Engineer turret, summoned drones, party NPCs), not just the player. The enemy
            // AI already aims at these targets; without this branch the projectile passes
            // straight through them and could never damage a turret. Mirror the player-
            // projectile entity loop, filtering for ENT_FRIENDLY instead of hostiles.
            bool hitFriendly = false;
            if (!(p.projFlags & PROJ_ORB)) {
                u16 nearby[72];
                u32 nearCount = 0;
                if (spatialGrid) {
                    nearCount = SpatialGridSystem::queryNeighbors(*spatialGrid, p.position, nearby, 72);
                } else {
                    for (u32 a = 0; a < entities.activeCount && nearCount < 72; a++)
                        nearby[nearCount++] = static_cast<u16>(entities.activeList[a]);
                }
                for (u32 n = 0; n < nearCount; n++) {
                    u32 e = nearby[n];
                    Entity& ent = entities.entities[e];
                    if (!(ent.flags & ENT_FRIENDLY)) continue;
                    if (ent.flags & ENT_DEAD) continue;
                    if (ent.flags & ENT_UNTARGETABLE) continue;
                    if (ent.enemyType == EnemyType::PROP) continue;
                    if (CombatQuery::aabbOverlap(projBox, entityAABB(ent))) {
                        EntityHandle h = {static_cast<u16>(e), ent.generation};
                        Combat::applyDamage(entities, h, p.damage);
                        // Freeze is the only on-hit effect a projectile fully specifies (it
                        // carries no DoT rate), so apply just that; basic ranged enemies set none.
                        if (p.onHitEffect == 4) ent.freezeTimer = fmaxf(ent.freezeTimer, p.onHitDuration);
                        hitFriendly = true;
                        break;
                    }
                }
            }
            if (hitFriendly) { destroyProjectile(pool, i); continue; }

            // Then the primary player, then each extra local player (split-screen). First
            // DEFLECT/HIT consumes the projectile; primary wins ties so singleplayer behavior
            // is byte-identical.
            PlayerHitResult r = tryHitPlayer(p, projBox, player);
            if (r == PlayerHitResult::MISS && extraPlayers) {
                for (u32 ep = 0; ep < extraPlayerCount; ep++) {
                    if (!extraPlayers[ep]) continue;
                    r = tryHitPlayer(p, projBox, *extraPlayers[ep]);
                    if (r != PlayerHitResult::MISS) break;
                }
            }
            if (r == PlayerHitResult::HIT)       { destroyProjectile(pool, i); continue; }
            if (r == PlayerHitResult::DEFLECTED) { continue; } // lifetime already 0
            if (r == PlayerHitResult::REFLECTED) { continue; } // parried: lives on, player-owned
        }
    }
}
