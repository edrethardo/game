#include "game/skill.h"
#include "game/combat.h"
#include "game/player.h"
#include "world/combat_query.h"
#include "world/raycast.h"
#include "renderer/debug_draw.h"
#include "core/log.h"
#include <cmath>

static PendingMeteor s_meteors[MAX_PENDING_METEORS];

// ---------------------------------------------------------------------------
// Static helpers (individual skill fires)
// ---------------------------------------------------------------------------

static void fireFrozenOrb(Vec3 origin, Vec3 direction, const SkillDef* def,
                           ProjectilePool& pool)
{
    ProjectileSystem::spawn(pool, origin, direction, def->projectileSpeed,
                            def->damage, def->radius, def->duration, true);

    // Mark the most recently spawned inactive-flagged projectile as an orb
    for (u32 i = MAX_PROJECTILES; i > 0; i--) {
        Projectile& p = pool.projectiles[i - 1];
        if (p.active && p.projFlags == 0 &&
            fabsf(p.damage - def->damage) < 0.1f &&
            fabsf(p.radius - def->radius) < 0.1f) {
            p.projFlags = 1;  // bit 0 = isOrb
            p.subTimer  = 0.0f;
            p.orbAngle  = 0.0f;
            break;
        }
    }
}

static void fireChainLightning(Vec3 origin, Vec3 direction, const SkillDef* def,
                                const LevelGrid& /*grid*/, EntityPool& entities)
{
    Vec3 currentPos    = origin;
    Vec3 currentDir    = direction;
    f32  currentDamage = def->damage;

    // Track the last entity hit so we don't bounce back immediately
    Entity* lastHit = nullptr;

    for (u8 bounce = 0; bounce <= def->bounces; bounce++) {
        // Wide cone on first hit for forgiving aiming; sphere search on bounces
        f32 cosCone = (bounce == 0) ? cosf(radians(5.0f)) : -1.0f;
        f32 range   = (bounce == 0) ? 50.0f : def->bounceRange;

        EntityHandle hits[MAX_ENTITIES];
        f32          dists[MAX_ENTITIES];

        u32 hitCount = CombatQuery::queryConeSorted(
            entities, currentPos, currentDir, cosCone, range,
            hits, dists, MAX_ENTITIES);

        if (hitCount == 0) break;

        // Pick the nearest hit that isn't the one we just struck
        Entity* hit = nullptr;
        EntityHandle hitHandle = {};
        for (u32 k = 0; k < hitCount; k++) {
            Entity* e = handleGet(entities, hits[k]);
            if (!e || e == lastHit) continue;
            hit       = e;
            hitHandle = hits[k];
            break;
        }
        if (!hit) break;

        // Visual debug line for the lightning bolt
        DebugDraw::line(currentPos, hit->position, {0.8f, 0.8f, 1.0f});

        Combat::applyDamage(entities, hitHandle, currentDamage);

        // Setup next bounce
        Vec3 prevPos = currentPos;
        currentPos    = hit->position;
        currentDamage *= def->damageFalloff;
        lastHit        = hit;

        if (bounce < def->bounces) {
            // Find the nearest OTHER active entity for the next chain link
            f32  bestDist = def->bounceRange + 1.0f;
            bool found    = false;
            for (u32 a = 0; a < entities.activeCount; a++) {
                u32    idx = entities.activeList[a];
                Entity& e  = entities.entities[idx];
                if (e.flags & ENT_DEAD)  continue;
                if (&e == lastHit)       continue;

                Vec3 toE = e.position - currentPos;
                f32  d   = length(toE);
                if (d < def->bounceRange && d < bestDist) {
                    bestDist   = d;
                    currentDir = toE * (1.0f / d);
                    found      = true;
                }
            }
            if (!found) break;
        }
        (void)prevPos;
    }
}

static void fireBloodNova(Vec3 origin, const SkillDef* def, EntityPool& entities)
{
    // 360-degree AoE: pass cosAngle of -1 (covers all directions)
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, def->radius,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], def->damage);
    }

    LOG_INFO("Blood Nova hit %u enemies", hitCount);
}

static void fireMeteorStrike(Vec3 origin, Vec3 direction, const SkillDef* def,
                              const LevelGrid& grid)
{
    // Raycast to find a ground/wall target position
    RayHit hit = Raycast::cast(grid, origin, direction, 50.0f);
    Vec3 targetPos = hit.hit
        ? (origin + direction * hit.distance)
        : (origin + direction * 20.0f);

    // Slot into the pending-meteor pool
    for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
        if (!s_meteors[i].active) {
            s_meteors[i].position = targetPos;
            s_meteors[i].damage   = def->damage;
            s_meteors[i].radius   = def->radius;
            s_meteors[i].timer    = def->delay;
            s_meteors[i].active   = true;
            break;
        }
    }
}

static void firePhaseDash(Vec3 /*eyePos*/, Vec3 forward, const SkillDef* def,
                           const LevelGrid& grid, EntityPool& entities,
                           Player& player)
{
    // Flatten to XZ plane
    Vec3 dashDir = {forward.x, 0.0f, forward.z};
    if (lengthSq(dashDir) < 0.001f) {
        dashDir = {0.0f, 0.0f, -1.0f};
    } else {
        dashDir = normalize(dashDir);
    }

    // Raycast for wall obstruction
    Vec3  rayOrigin = player.position + Vec3{0.0f, 0.5f, 0.0f};
    RayHit wallHit  = Raycast::cast(grid, rayOrigin, dashDir, def->distance);
    f32    dashDist = wallHit.hit ? (wallHit.distance - 0.5f) : def->distance;
    if (dashDist < 0.5f) dashDist = 0.5f;

    Vec3 startPos = player.position;
    Vec3 endPos   = player.position + dashDir * dashDist;

    // Damage entities in a narrow cone along the dash corridor
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, startPos + Vec3{0.0f, 0.5f, 0.0f}, dashDir,
        cosf(radians(30.0f)), dashDist,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], def->damage);
    }

    // Teleport player
    player.position = endPos;

    LOG_INFO("Phase Dash: moved %.1fm, hit %u enemies", dashDist, hitCount);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void SkillSystem::init() {
    for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
        s_meteors[i] = {};
    }
}

const SkillDef* SkillSystem::findSkillDef(const SkillDef* defs, u32 count, SkillId id) {
    for (u32 i = 0; i < count; i++) {
        if (defs[i].id == id) return &defs[i];
    }
    return nullptr;
}

void SkillSystem::update(SkillState& ss, f32 dt) {
    // Energy regen: 10/sec
    ss.energy += 10.0f * dt;
    if (ss.energy > ss.maxEnergy) ss.energy = ss.maxEnergy;

    // Cooldown tick
    if (ss.cooldownTimer > 0.0f) {
        ss.cooldownTimer -= dt;
        if (ss.cooldownTimer < 0.0f) ss.cooldownTimer = 0.0f;
    }
}

bool SkillSystem::tryActivate(SkillState& ss, const SkillDef* skillDefs, u32 skillDefCount,
                               Vec3 eyePos, Vec3 forward, f32 /*yaw*/,
                               ProjectilePool& projectiles, EntityPool& entities,
                               const LevelGrid& grid, Player& player)
{
    if (ss.activeSkill == SkillId::NONE)  return false;
    if (ss.cooldownTimer > 0.0f)          return false;

    const SkillDef* def = findSkillDef(skillDefs, skillDefCount, ss.activeSkill);
    if (!def) return false;

    // Energy / resource check
    if (ss.activeSkill == SkillId::BLOOD_NOVA) {
        f32 cost = player.health * def->healthCostPct;
        if (player.health <= cost + 1.0f) return false; // refuse to suicide
        player.health         -= cost;
        player.damageFlashTimer = 0.1f;
    } else {
        if (ss.energy < def->energyCost) return false;
        ss.energy -= def->energyCost;
    }

    ss.cooldownTimer = def->cooldown;

    switch (ss.activeSkill) {
    case SkillId::FROZEN_ORB:
        fireFrozenOrb(eyePos, forward, def, projectiles);
        break;
    case SkillId::CHAIN_LIGHTNING:
        fireChainLightning(eyePos, forward, def, grid, entities);
        break;
    case SkillId::BLOOD_NOVA:
        fireBloodNova(eyePos, def, entities);
        break;
    case SkillId::METEOR_STRIKE:
        fireMeteorStrike(eyePos, forward, def, grid);
        break;
    case SkillId::PHASE_DASH:
        firePhaseDash(eyePos, forward, def, grid, entities, player);
        break;
    default:
        return false;
    }

    return true;
}

void SkillSystem::updateOrbProjectiles(ProjectilePool& pool,
                                        const SkillDef* skillDefs, u32 skillDefCount,
                                        f32 dt)
{
    const SkillDef* def = findSkillDef(skillDefs, skillDefCount, SkillId::FROZEN_ORB);
    if (!def) return;

    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        Projectile& p = pool.projectiles[i];
        if (!p.active || !(p.projFlags & 1)) continue; // not an orb

        p.subTimer += dt;

        while (p.subTimer >= def->shardInterval) {
            p.subTimer -= def->shardInterval;

            // Spawn shards in a rotating spoke pattern (D2 Frozen Orb style)
            for (u8 s = 0; s < def->shardCount; s++) {
                f32  angle    = p.orbAngle + s * (6.28318f / def->shardCount);
                Vec3 shardDir = {cosf(angle), 0.0f, sinf(angle)};

                ProjectileSystem::spawn(pool, p.position, shardDir, def->shardSpeed,
                                        def->shardDamage, def->shardRadius, 0.6f, true);

                // Mark the freshly spawned shard (bit 1 = isOrbShard)
                for (u32 j = MAX_PROJECTILES; j > 0; j--) {
                    Projectile& sp = pool.projectiles[j - 1];
                    if (sp.active && sp.projFlags == 0 &&
                        fabsf(sp.damage - def->shardDamage) < 0.1f &&
                        fabsf(sp.radius - def->shardRadius) < 0.01f) {
                        sp.projFlags = 2; // bit 1 = isOrbShard
                        break;
                    }
                }
            }

            // Rotate the spoke angle for the spirograph effect
            p.orbAngle += radians(def->angleStepDeg);
        }
    }
}

void SkillSystem::updateMeteors(EntityPool& entities, f32 dt) {
    for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
        PendingMeteor& m = s_meteors[i];
        if (!m.active) continue;

        m.timer -= dt;
        if (m.timer <= 0.0f) {
            // Explode — damage all entities within the blast radius
            EntityHandle hits[MAX_ENTITIES];
            f32          dists[MAX_ENTITIES];
            u32 hitCount = CombatQuery::queryConeSorted(
                entities, m.position, {0.0f, -1.0f, 0.0f}, -1.0f, m.radius,
                hits, dists, MAX_ENTITIES);

            for (u32 j = 0; j < hitCount; j++) {
                Combat::applyDamage(entities, hits[j], m.damage);
            }

            m.active = false;
            LOG_INFO("Meteor struck: hit %u enemies", hitCount);
        }
    }
}
