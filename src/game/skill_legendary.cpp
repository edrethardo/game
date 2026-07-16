// skill_legendary.cpp — fire* helpers for the five legendary item skills:
// Frozen Orb, Chain Lightning, Blood Nova, Meteor Strike, Phase Dash.
// These skills are equipped via legendary items (legendarySkill field in items.json)
// and triggered by SkillSystem::tryActivate in skill_system.cpp.

#include "game/skill_internal.h"

void fireFrozenOrb(Vec3 origin, Vec3 direction, const SkillDef* def,
                   ProjectilePool& pool)
{
    // Orb itself phases through enemies; shards do the damage.
    // We store s_classDmgMult in the orb's damage field (otherwise 0) so
    // updateOrbProjectiles can read it back for shard damage scaling.
    f32 orbRadius = def->radius * 0.7f;
    u16 orbIdx = ProjectileSystem::spawn(pool, origin, direction, def->projectileSpeed,
                            s_classDmgMult, orbRadius, def->duration, true);
    if (orbIdx != 0xFFFF) {
        pool.projectiles[orbIdx].projFlags = PROJ_ORB;
        pool.projectiles[orbIdx].subTimer  = 0.0f;
        pool.projectiles[orbIdx].orbAngle  = 0.0f;
        pool.projectiles[orbIdx].lightColor = {0.3f, 0.7f, 1.0f}; // cyan glow
    }
}

// True if a straight line from `from` to `to` is not blocked by a solid wall.
// Mirrors hasLOSToPoint in the AI code; used so lightning can't arc through walls.
static bool chainHasLOS(Vec3 from, Vec3 to, const LevelGrid& grid) {
    Vec3 d = to - from;
    f32 dist = length(d);
    if (dist < 0.001f) return true;
    RayHit hit = Raycast::cast(grid, from, d * (1.0f / dist), dist);
    return !hit.hit || hit.distance >= dist - 0.1f;
}

void fireChainLightning(Vec3 origin, Vec3 direction, const SkillDef* def,
                        const LevelGrid& grid, EntityPool& entities)
{
    Vec3 currentPos    = origin;
    Vec3 currentDir    = direction;
    f32  currentDamage = def->damage * s_classDmgMult;

    // Store chain positions for the visual arc effect
    static constexpr u32 MAX_CHAIN_PTS = 24;
    Vec3 chainPoints[MAX_CHAIN_PTS];
    u8   chainCount = 0;
    chainPoints[chainCount++] = origin;

    // Track last TWO hits to avoid ping-ponging between the same pair
    Entity* lastHit = nullptr;
    Entity* prevHit = nullptr;

    // Scale bounces by item level: 3 (base) to 20 (Hell max)
    u8 effectiveBounces = static_cast<u8>(3.0f + s_skillPower * 17.0f);
    if (effectiveBounces < def->bounces) effectiveBounces = def->bounces; // never below skill def

    for (u8 bounce = 0; bounce <= effectiveBounces && chainCount < MAX_CHAIN_PTS; bounce++) {
        // Wide cone on first hit for forgiving aiming; sphere search on bounces
        f32 cosCone = (bounce == 0) ? cosf(radians(15.0f)) : -1.0f;
        f32 range   = (bounce == 0) ? 15.0f : def->bounceRange;

        EntityHandle hits[MAX_ENTITIES];
        f32          dists[MAX_ENTITIES];

        u32 hitCount = CombatQuery::queryConeSorted(
            entities, currentPos, currentDir, cosCone, range,
            hits, dists, MAX_ENTITIES);

        if (hitCount == 0) break;

        // Pick nearest valid hostile entity — skip dead, friendly, props, and lastHit
        Entity* hit = nullptr;
        EntityHandle hitHandle = {};
        for (u32 k = 0; k < hitCount; k++) {
            Entity* e = handleGet(entities, hits[k]);
            if (!e) continue;
            if (e->flags & ENT_DEAD) continue;
            if (e->flags & ENT_FRIENDLY) continue;
            if (e->enemyType == EnemyType::PROP) continue;
            if (e == lastHit) continue; // don't bounce to same target twice in a row
            if (!chainHasLOS(currentPos, e->position, grid)) continue; // no zapping/chaining through walls
            hit       = e;
            hitHandle = hits[k];
            break;
        }
        if (!hit) break;

        Combat::applyDamage(entities, hitHandle, currentDamage);
        chainPoints[chainCount++] = hit->position + Vec3{0, hit->halfExtents.y, 0};

        // Setup next bounce
        prevHit       = lastHit;
        currentPos    = hit->position + Vec3{0, hit->halfExtents.y, 0};
        currentDamage *= def->damageFalloff;
        lastHit        = hit;

        if (bounce < effectiveBounces) {
            // Find nearest entity for next bounce (can re-hit prevHit but not lastHit)
            f32  bestDist = def->bounceRange + 1.0f;
            bool found    = false;
            for (u32 a = 0; a < entities.activeCount; a++) {
                u32    idx = entities.activeList[a];
                Entity& e  = entities.entities[idx];
                if (e.flags & ENT_DEAD)     continue;
                if (e.flags & ENT_FRIENDLY) continue;
                if (e.enemyType == EnemyType::PROP) continue;
                if (&e == lastHit)          continue; // no immediate bounce-back

                Vec3 toE = e.position - currentPos;
                f32  d   = length(toE);
                if (d < def->bounceRange && d < bestDist &&
                    chainHasLOS(currentPos, e.position, grid)) { // only aim at reachable enemies
                    bestDist   = d;
                    currentDir = toE * (1.0f / d);
                    found      = true;
                }
            }
            if (!found) break;
        }
        (void)prevHit;
    }

    // Emit visual chain arc via callback
    if (s_chainCallback && chainCount > 1) {
        s_chainCallback(chainPoints, chainCount);
    }
}

void fireBloodNova(Vec3 origin, const SkillDef* def, EntityPool& entities)
{
    // Radius scales with item level: 1.0x base to 1.5x at max power
    f32 scaledRadius = def->radius * (1.0f + s_skillPower * 0.5f);

    // 360-degree AoE: pass cosAngle of -1 (covers all directions)
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, scaledRadius,
        hits, dists, MAX_ENTITIES);

    f32 novaDmg = def->damage * s_classDmgMult;
    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], novaDmg);
    }

    // Trigger expanding red ring visual
    if (s_novaCallback) s_novaCallback(origin, scaledRadius, {1.0f, 0.15f, 0.1f});
    LOG_INFO("Blood Nova hit %u enemies", hitCount);
}

void fireMeteorStrike(Vec3 origin, Vec3 direction, const SkillDef* def,
                      const LevelGrid& grid)
{
    // Raycast to find a ground/wall target position
    RayHit hit = Raycast::cast(grid, origin, direction, 50.0f);
    Vec3 targetPos = hit.hit
        ? (origin + direction * hit.distance)
        : (origin + direction * 20.0f);

    // Number of meteors scales with item level: 1 (base) to 5 (Hell max)
    u8 meteorCount = static_cast<u8>(1.0f + s_skillPower * 4.0f);

    for (u8 m = 0; m < meteorCount; m++) {
        // Spread extra meteors around the target in a ring
        Vec3 meteorPos = targetPos;
        if (m > 0) {
            f32 angle = (6.2832f / (meteorCount - 1)) * (m - 1);
            f32 spread = def->radius * 1.5f;
            meteorPos.x += cosf(angle) * spread;
            meteorPos.z += sinf(angle) * spread;
        }

        // Slot into the pending-meteor pool
        for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
            if (!s_meteors[i].active) {
                s_meteors[i].position = meteorPos;
                s_meteors[i].damage   = def->damage * s_classDmgMult;
                s_meteors[i].radius   = def->radius;
                s_meteors[i].timer    = def->delay + m * 0.15f; // stagger impacts
                s_meteors[i].active   = true;
                break;
            }
        }
    }
}

void firePhaseDash(Vec3 /*eyePos*/, Vec3 forward, const SkillDef* def,
                   const LevelGrid& grid, EntityPool& entities, Player& player)
{
    // Flatten to XZ plane
    Vec3 dashDir = {forward.x, 0.0f, forward.z};
    if (lengthSq(dashDir) < 0.001f) {
        dashDir = {0.0f, 0.0f, -1.0f};
    } else {
        dashDir = normalize(dashDir);
    }

    // Distance scales with item level: 1.0x base to 1.5x at max power
    f32 scaledDist = def->distance * (1.0f + s_skillPower * 0.5f);

    // Raycast for wall obstruction
    Vec3  rayOrigin = player.position + Vec3{0.0f, 0.5f, 0.0f};
    RayHit wallHit  = Raycast::cast(grid, rayOrigin, dashDir, scaledDist);
    f32    dashDist = wallHit.hit ? (wallHit.distance - 0.5f) : scaledDist;
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

    f32 dashDmg = def->damage * s_classDmgMult;
    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], dashDmg);
    }

    // Teleport — resolved so a dash THROUGH the pack can't end inside the last body it
    // passed, and a thin-ray wall graze can't leave the footprint clipped into the wall.
    endPos = Teleport::resolveDest(grid, entities, startPos, endPos);
    player.position = endPos;

    // Trigger blue trail visual
    if (s_dashCallback) s_dashCallback(startPos, endPos);
    LOG_INFO("Phase Dash: moved %.1fm, hit %u enemies", dashDist, hitCount);
}
