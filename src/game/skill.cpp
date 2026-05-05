#include "game/skill.h"
#include "game/combat.h"
#include "game/player.h"
#include "world/combat_query.h"
#include "world/raycast.h"
#include "renderer/debug_draw.h"
#include "core/log.h"
#include <cmath>

// Not static — extern'd by engine.cpp for rendering the targeting circles
PendingMeteor s_meteors[MAX_PENDING_METEORS];
static SkillSystem::NovaCallback s_novaCallback = nullptr;
static SkillSystem::DashCallback s_dashCallback = nullptr;
static SkillSystem::ScorchCallback s_scorchCallback = nullptr;
static SkillSystem::DroneSpawnCallback s_droneSpawnCallback = nullptr;
static SkillSystem::ChainCallback s_chainCallback = nullptr;

void SkillSystem::setNovaCallback(NovaCallback cb) { s_novaCallback = cb; }
void SkillSystem::setDashCallback(DashCallback cb) { s_dashCallback = cb; }
void SkillSystem::setScorchCallback(ScorchCallback cb) { s_scorchCallback = cb; }
void SkillSystem::setDroneSpawnCallback(DroneSpawnCallback cb) { s_droneSpawnCallback = cb; }
void SkillSystem::setChainCallback(ChainCallback cb) { s_chainCallback = cb; }

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

    // Store chain positions for the visual arc effect
    static constexpr u32 MAX_CHAIN_PTS = 24;
    Vec3 chainPoints[MAX_CHAIN_PTS];
    u8   chainCount = 0;
    chainPoints[chainCount++] = origin;

    // Track last TWO hits to avoid ping-ponging between the same pair
    Entity* lastHit = nullptr;
    Entity* prevHit = nullptr;

    for (u8 bounce = 0; bounce <= def->bounces && chainCount < MAX_CHAIN_PTS; bounce++) {
        // Wide cone on first hit for forgiving aiming; sphere search on bounces
        f32 cosCone = (bounce == 0) ? cosf(radians(5.0f)) : -1.0f;
        f32 range   = (bounce == 0) ? 50.0f : def->bounceRange;

        EntityHandle hits[MAX_ENTITIES];
        f32          dists[MAX_ENTITIES];

        u32 hitCount = CombatQuery::queryConeSorted(
            entities, currentPos, currentDir, cosCone, range,
            hits, dists, MAX_ENTITIES);

        if (hitCount == 0) break;

        // Pick nearest entity — skip only the PREVIOUS target to allow re-hitting
        Entity* hit = nullptr;
        EntityHandle hitHandle = {};
        for (u32 k = 0; k < hitCount; k++) {
            Entity* e = handleGet(entities, hits[k]);
            if (!e) continue;
            if (e == lastHit) continue; // don't bounce to same target twice in a row
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

        if (bounce < def->bounces) {
            // Find nearest entity for next bounce (can re-hit prevHit but not lastHit)
            f32  bestDist = def->bounceRange + 1.0f;
            bool found    = false;
            for (u32 a = 0; a < entities.activeCount; a++) {
                u32    idx = entities.activeList[a];
                Entity& e  = entities.entities[idx];
                if (e.flags & ENT_DEAD)     continue;
                if (e.flags & ENT_FRIENDLY) continue;
                if (&e == lastHit)          continue; // no immediate bounce-back

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
        (void)prevHit;
    }

    // Emit visual chain arc via callback
    if (s_chainCallback && chainCount > 1) {
        s_chainCallback(chainPoints, chainCount);
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

    // Trigger expanding red ring visual
    if (s_novaCallback) s_novaCallback(origin, def->radius, {1.0f, 0.15f, 0.1f});
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

    // Trigger blue trail visual
    if (s_dashCallback) s_dashCallback(startPos, endPos);
    LOG_INFO("Phase Dash: moved %.1fm, hit %u enemies", dashDist, hitCount);
}

// ---------------------------------------------------------------------------
// Warrior skills
// ---------------------------------------------------------------------------

// Wide 120-degree melee swing — hits everything in a short, broad arc.
static void fireCleave(Vec3 origin, Vec3 forward, const SkillDef* def,
                       EntityPool& entities)
{
    WeaponDef temp;
    temp.name        = "Cleave";
    temp.type        = WeaponType::MELEE;
    temp.damage      = def->damage > 0.0f ? def->damage : 35.0f;
    temp.range       = def->radius  > 0.0f ? def->radius : 3.0f;
    temp.coneAngleDeg = 120.0f;
    temp.cooldown    = 0.0f;
    temp.recoilKick  = 0.0f;
    Combat::fireMelee(temp, origin, forward, entities);
    LOG_INFO("Cleave fired");
}

// Stun all enemies within 6m with a war-cry shout.
static void fireWarCry(Vec3 origin, const SkillDef* def, EntityPool& entities)
{
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    f32 range    = def->radius > 0.0f ? def->radius : 6.0f;
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, range,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        Entity* e = handleGet(entities, hits[i]);
        if (e) e->freezeTimer = 2.0f;  // 2-second stun
    }
    if (s_novaCallback) s_novaCallback(origin, range, {1.0f, 0.85f, 0.2f});
    LOG_INFO("War Cry stunned %u enemies", hitCount);
}

// Instant 360-degree AoE melee spin at short range (no health cost — uses energy).
static void fireWhirlwind(Vec3 origin, const SkillDef* def, EntityPool& entities)
{
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    f32 range    = def->radius > 0.0f ? def->radius : 2.5f;
    f32 damage   = def->damage > 0.0f ? def->damage : 30.0f;
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, range,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage);
    }
    if (s_novaCallback) s_novaCallback(origin, range, {0.8f, 0.3f, 0.1f});
    LOG_INFO("Whirlwind hit %u enemies", hitCount);
}

// Slam the ground — reuses meteor pool for an instant at-feet blast.
static void fireEarthquake(Vec3 origin, const SkillDef* def)
{
    for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
        if (!s_meteors[i].active) {
            s_meteors[i].position = origin;
            s_meteors[i].damage   = def->damage > 0.0f ? def->damage : 40.0f;
            s_meteors[i].radius   = def->radius  > 0.0f ? def->radius : 4.0f;
            // Very short delay so it fires on the next updateMeteors tick
            s_meteors[i].timer    = 0.05f;
            s_meteors[i].active   = true;
            break;
        }
    }
    LOG_INFO("Earthquake triggered");
}

// ---------------------------------------------------------------------------
// Ranger skills
// ---------------------------------------------------------------------------

// Helper: rotate a direction vector by angleDeg degrees around Y axis.
static Vec3 rotateY(Vec3 v, f32 angleDeg)
{
    f32 a = radians(angleDeg);
    f32 c = cosf(a), s = sinf(a);
    return { v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
}

// Three projectiles spread in a -15 / 0 / +15 degree horizontal fan.
static void fireMultiShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                          ProjectilePool& pool)
{
    f32 speed  = def->projectileSpeed > 0.0f ? def->projectileSpeed : 25.0f;
    f32 damage = def->damage          > 0.0f ? def->damage          : 20.0f;
    f32 angles[3] = {-15.0f, 0.0f, 15.0f};
    for (u32 i = 0; i < 3; i++) {
        Vec3 dir = normalize(rotateY(forward, angles[i]));
        ProjectileSystem::spawn(pool, origin, dir, speed, damage, 0.1f, 2.0f, true);
    }
    LOG_INFO("Multi Shot fired 3 arrows");
}

// Instant AoE at a raycasted target point — no delay, like a meteor that fires now.
static void fireRainOfArrows(Vec3 origin, Vec3 forward, const SkillDef* def,
                              const LevelGrid& grid, EntityPool& entities)
{
    RayHit hit    = Raycast::cast(grid, origin, forward, 50.0f);
    Vec3 target   = hit.hit ? (origin + forward * hit.distance)
                            : (origin + forward * 20.0f);
    f32 radius    = def->radius > 0.0f ? def->radius : 3.5f;
    f32 damage    = def->damage > 0.0f ? def->damage : 25.0f;

    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, target, {0.0f, -1.0f, 0.0f}, -1.0f, radius,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage);
    }
    if (s_novaCallback) s_novaCallback(target, radius, {0.6f, 0.4f, 0.1f});
    LOG_INFO("Rain of Arrows hit %u enemies", hitCount);
}

// Single projectile — engine's normal hit path applies damage; visual tint via flags.
// Poison DoT would be wired through Projectile::onHitEffect in a future pass.
static void firePoisonArrow(Vec3 origin, Vec3 forward, const SkillDef* def,
                             ProjectilePool& pool)
{
    f32 speed  = def->projectileSpeed > 0.0f ? def->projectileSpeed : 22.0f;
    f32 damage = def->damage          > 0.0f ? def->damage          : 18.0f;
    // Spawn with PROJ_SPARK flag repurposed as green-tint hint for renderer
    ProjectileSystem::spawn(pool, origin, forward, speed, damage, 0.12f, 3.0f,
                            true, PROJ_SPARK);
    LOG_INFO("Poison Arrow fired");
}

// High-speed, high-damage hitscan shot (piercing is a future projectile.cpp feature).
static void fireShadowShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                            const LevelGrid& grid, EntityPool& entities)
{
    WeaponDef temp;
    temp.name    = "Shadow Shot";
    temp.type    = WeaponType::HITSCAN;
    temp.damage  = def->damage > 0.0f ? def->damage * 2.5f : 60.0f;
    temp.range   = 80.0f;
    temp.coneAngleDeg  = 1.0f;
    temp.cooldown      = 0.0f;
    temp.recoilKick    = 0.02f;
    Combat::fireHitscan(temp, origin, forward, grid, entities);
    LOG_INFO("Shadow Shot fired");
}

// ---------------------------------------------------------------------------
// Sorcerer skills
// ---------------------------------------------------------------------------

// Lobbed explosive projectile with splash damage on impact.
static void fireFireball(Vec3 origin, Vec3 forward, const SkillDef* def,
                         ProjectilePool& pool)
{
    f32 speed       = def->projectileSpeed > 0.0f ? def->projectileSpeed : 18.0f;
    f32 damage      = def->damage          > 0.0f ? def->damage          : 35.0f;
    f32 splashR     = def->radius          > 0.0f ? def->radius          : 2.0f;
    f32 splashDmg   = damage * 0.6f;  // 60% damage in splash zone
    // PROJ_SPLASH + slight gravity arc for feel
    u16 slot = ProjectileSystem::spawn(pool, origin, forward, speed, damage,
                                       0.2f, 3.0f, true, PROJ_SPLASH);
    if (slot != 0xFFFF) {
        Projectile& p  = pool.projectiles[slot];
        p.splashRadius = splashR;
        p.splashDamage = splashDmg;
        p.gravity      = 4.0f;
    }
    LOG_INFO("Fireball launched");
}

// ---------------------------------------------------------------------------
// Rogue skills
// ---------------------------------------------------------------------------

// Three fast knives in a tight -8 / 0 / +8 degree fan.
static void fireKnifeBurst(Vec3 origin, Vec3 forward, const SkillDef* def,
                            ProjectilePool& pool)
{
    f32 speed  = def->projectileSpeed > 0.0f ? def->projectileSpeed : 30.0f;
    f32 damage = def->damage          > 0.0f ? def->damage          : 12.0f;
    f32 angles[3] = {-8.0f, 0.0f, 8.0f};
    for (u32 i = 0; i < 3; i++) {
        Vec3 dir = normalize(rotateY(forward, angles[i]));
        ProjectileSystem::spawn(pool, origin, dir, speed, damage, 0.08f, 1.5f, true);
    }
    LOG_INFO("Knife Burst fired 3 knives");
}

// Drop a toxic cloud at a target point using the scorch-zone system.
static void firePoisonCloud(Vec3 origin, Vec3 forward, const SkillDef* def,
                             const LevelGrid& grid)
{
    RayHit hit  = Raycast::cast(grid, origin, forward, 40.0f);
    Vec3 target = hit.hit ? (origin + forward * hit.distance)
                          : (origin + forward * 15.0f);
    f32 radius  = def->radius > 0.0f ? def->radius : 2.5f;
    f32 dps     = def->damage > 0.0f ? def->damage * 0.4f : 10.0f;

    // Green-tinted nova for visual + persistent scorch zone (poison DPS)
    if (s_novaCallback)   s_novaCallback(target, radius, {0.1f, 0.8f, 0.1f});
    if (s_scorchCallback) s_scorchCallback(target, radius, 4.0f, dps);
    LOG_INFO("Poison Cloud deployed");
}

// Teleport behind nearest enemy and deal 3x damage.
static void fireShadowStrike(Vec3 origin, Vec3 forward, const SkillDef* def,
                              EntityPool& entities, Player& player)
{
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    // Wide cone (cos 0° = -1) to find any nearby enemy
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, forward, -1.0f, 15.0f,
        hits, dists, MAX_ENTITIES);

    if (hitCount == 0) return;

    Entity* target = handleGet(entities, hits[0]);
    if (!target) return;

    // Teleport to just behind the target relative to its facing
    Vec3 behind = target->position + Vec3{sinf(target->yaw), 0.0f, cosf(target->yaw)} * 1.0f;
    Vec3 startPos = player.position;
    player.position = behind;

    f32 damage = def->damage > 0.0f ? def->damage * 3.0f : 90.0f;
    Combat::applyDamage(entities, hits[0], damage);

    if (s_dashCallback) s_dashCallback(startPos, behind);
    LOG_INFO("Shadow Strike: teleported and dealt %.0f damage", damage);
}

// ---------------------------------------------------------------------------
// Paladin skills
// ---------------------------------------------------------------------------

// Narrow melee cone; heal player for a fraction of damage dealt.
static void fireHolySmite(Vec3 origin, Vec3 forward, const SkillDef* def,
                           EntityPool& entities, Player& player)
{
    WeaponDef temp;
    temp.name         = "Holy Smite";
    temp.type         = WeaponType::MELEE;
    temp.damage       = def->damage > 0.0f ? def->damage : 40.0f;
    temp.range        = 2.5f;
    temp.coneAngleDeg = 60.0f;
    temp.cooldown     = 0.0f;
    temp.recoilKick   = 0.0f;

    AttackResult result = Combat::fireMelee(temp, origin, forward, entities);

    // Heal for 10% of total damage inflicted
    f32 totalDmg = temp.damage * static_cast<f32>(result.entitiesHit);
    f32 heal     = totalDmg * 0.1f;
    player.health = (player.health + heal > player.maxHealth)
                    ? player.maxHealth : player.health + heal;
    LOG_INFO("Holy Smite hit %u enemies, healed %.1f HP", result.entitiesHit, heal);
}

// Healing zone at player feet — heals the player instantly and burns a green nova.
static void fireConsecration(Vec3 origin, const SkillDef* def, Player& player)
{
    f32 healAmt = def->damage > 0.0f ? def->damage : 30.0f; // "damage" repurposed as heal amt
    f32 radius  = def->radius > 0.0f ? def->radius : 4.0f;

    player.health = (player.health + healAmt > player.maxHealth)
                    ? player.maxHealth : player.health + healAmt;

    // Green ring signals the holy ground to the player
    if (s_novaCallback) s_novaCallback(origin, radius, {0.2f, 1.0f, 0.3f});
    LOG_INFO("Consecration healed %.1f HP", healAmt);
}

// Brief invulnerability: heal to full + clear all debuffs.
static void fireDivineShield(Player& player)
{
    player.health        = player.maxHealth;
    player.slowTimer     = 0.0f;
    player.poisonTimer   = 0.0f;
    player.poisonDps     = 0.0f;
    player.burnTimer     = 0.0f;
    player.burnDps       = 0.0f;
    player.freezeTimer   = 0.0f;
    LOG_INFO("Divine Shield: healed to full, all debuffs cleared");
}

// ---------------------------------------------------------------------------
// Combat Engineer skills
// ---------------------------------------------------------------------------

// Hitscan bolt that freezes the first enemy hit for 0.5s.
static void fireShockBolt(Vec3 origin, Vec3 forward, const SkillDef* def,
                           const LevelGrid& grid, EntityPool& entities)
{
    WeaponDef temp;
    temp.name         = "Shock Bolt";
    temp.type         = WeaponType::HITSCAN;
    temp.damage       = def->damage > 0.0f ? def->damage : 20.0f;
    temp.range        = 40.0f;
    temp.coneAngleDeg = 1.0f;
    temp.cooldown     = 0.0f;
    temp.recoilKick   = 0.01f;

    AttackResult result = Combat::fireHitscan(temp, origin, forward, grid, entities);

    if (result.hitEntity) {
        // Apply freeze to the first entity the hitscan actually hit
        RayHit rh = Raycast::cast(grid, origin, forward, temp.range);
        (void)rh; // We only need the closest entity, which fireHitscan already handled.
        // Walk the pool to find the just-hit entity (nearest active in cone).
        EntityHandle eHits[MAX_ENTITIES];
        f32          eDists[MAX_ENTITIES];
        u32 cnt = CombatQuery::queryConeSorted(entities, origin, forward,
                                               cosf(radians(2.0f)), temp.range,
                                               eHits, eDists, MAX_ENTITIES);
        if (cnt > 0) {
            Entity* e = handleGet(entities, eHits[0]);
            if (e) e->freezeTimer = 0.5f;
        }
    }
    LOG_INFO("Shock Bolt fired");
}

// Spawn a friendly turret entity 2m in front of the player.
static void fireDeployTurret(Vec3 origin, Vec3 forward, EntityPool& /*entities*/)
{
    Vec3 spawnPos = origin + forward * 2.0f;
    if (s_droneSpawnCallback) s_droneSpawnCallback(spawnPos, 2); // type 2 = turret
    LOG_INFO("Turret requested");
}

// AoE lightning in 4m radius + outward spark projectiles for visual.
static void fireTeslaCoil(Vec3 origin, const SkillDef* def,
                           EntityPool& entities, ProjectilePool& pool)
{
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    f32 radius = def->radius > 0.0f ? def->radius : 4.0f;
    f32 damage = def->damage > 0.0f ? def->damage : 30.0f;

    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, radius,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage);
    }

    // Eight spark projectiles evenly spaced for the electrical burst visual
    for (u32 s = 0; s < 8; s++) {
        f32  angle = s * (6.28318f / 8.0f);
        Vec3 dir   = {cosf(angle), 0.0f, sinf(angle)};
        ProjectileSystem::spawn(pool, origin, dir, 12.0f, 0.0f, 0.05f, 0.4f,
                                true, PROJ_SPARK);
    }

    if (s_novaCallback) s_novaCallback(origin, radius, {0.5f, 0.7f, 1.0f});
    LOG_INFO("Tesla Coil hit %u enemies", hitCount);
}

// Overdrive: restore HP + clear debuffs (simplified buff without a timer system).
static void fireMechOverdrive(Player& player)
{
    player.health = (player.health + 20.0f > player.maxHealth)
                    ? player.maxHealth : player.health + 20.0f;
    player.slowTimer   = 0.0f;
    player.freezeTimer = 0.0f;
    LOG_INFO("Mech Overdrive activated");
}

// ---------------------------------------------------------------------------
// Marksman skills
// ---------------------------------------------------------------------------

// 3x damage hitscan aimed shot.
static void fireAimedShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                           const LevelGrid& grid, EntityPool& entities)
{
    WeaponDef temp;
    temp.name         = "Aimed Shot";
    temp.type         = WeaponType::HITSCAN;
    temp.damage       = (def->damage > 0.0f ? def->damage : 25.0f) * 3.0f;
    temp.range        = 80.0f;
    temp.coneAngleDeg = 0.5f;
    temp.cooldown     = 0.0f;
    temp.recoilKick   = 0.03f;
    Combat::fireHitscan(temp, origin, forward, grid, entities);
    LOG_INFO("Aimed Shot fired");
}

// Hitscan that triggers a splash AoE at the hit position.
static void fireExplosiveRound(Vec3 origin, Vec3 forward, const SkillDef* def,
                                const LevelGrid& grid, EntityPool& entities)
{
    f32 damage    = def->damage > 0.0f ? def->damage : 20.0f;
    f32 splashR   = def->radius > 0.0f ? def->radius : 2.5f;

    // Find hit position via grid raycast
    RayHit rh      = Raycast::cast(grid, origin, forward, 60.0f);
    Vec3 detonPos  = rh.hit ? (origin + forward * rh.distance)
                            : (origin + forward * 60.0f);

    // Direct hitscan damage
    WeaponDef temp;
    temp.name         = "Explosive Round";
    temp.type         = WeaponType::HITSCAN;
    temp.damage       = damage;
    temp.range        = 60.0f;
    temp.coneAngleDeg = 1.0f;
    temp.cooldown     = 0.0f;
    temp.recoilKick   = 0.02f;
    Combat::fireHitscan(temp, origin, forward, grid, entities);

    // Splash AoE at detonation point
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, detonPos, {0.0f, -1.0f, 0.0f}, -1.0f, splashR,
        hits, dists, MAX_ENTITIES);
    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage * 0.5f);
    }
    if (s_novaCallback) s_novaCallback(detonPos, splashR, {1.0f, 0.6f, 0.1f});
    LOG_INFO("Explosive Round detonated, splash hit %u enemies", hitCount);
}

// Three rapid hitscan shots with slight random spread.
static void fireRapidFire(Vec3 origin, Vec3 forward, const SkillDef* def,
                           const LevelGrid& grid, EntityPool& entities)
{
    f32 damage = def->damage > 0.0f ? def->damage : 15.0f;

    // Bake tiny deterministic spreads into the three shots (no RNG needed)
    static const f32 kSpreads[3][2] = { {-2.0f, 0.5f}, {0.0f, 0.0f}, {2.0f, -0.5f} };

    for (u32 i = 0; i < 3; i++) {
        Vec3 dir = normalize(rotateY(forward, kSpreads[i][0])
                             + Vec3{0.0f, kSpreads[i][1] * 0.02f, 0.0f});
        WeaponDef temp;
        temp.name         = "Rapid Fire";
        temp.type         = WeaponType::HITSCAN;
        temp.damage       = damage;
        temp.range        = 50.0f;
        temp.coneAngleDeg = 2.0f;
        temp.cooldown     = 0.0f;
        temp.recoilKick   = 0.01f;
        Combat::fireHitscan(temp, dir, dir, grid, entities);
    }
    LOG_INFO("Rapid Fire: 3 shots fired");
}

// Hitscan that instantly kills enemies below 20% health, otherwise deals normal damage.
static void fireHeadshot(Vec3 origin, Vec3 forward, const SkillDef* def,
                          const LevelGrid& grid, EntityPool& entities)
{
    // First, find the entity that would be hit
    EntityHandle eHits[MAX_ENTITIES];
    f32          eDists[MAX_ENTITIES];
    u32 cnt = CombatQuery::queryConeSorted(entities, origin, forward,
                                           cosf(radians(2.0f)), 80.0f,
                                           eHits, eDists, MAX_ENTITIES);

    if (cnt > 0) {
        Entity* e = handleGet(entities, eHits[0]);
        if (e && !(e->flags & ENT_DEAD)) {
            f32 damage = (e->health < e->maxHealth * 0.2f)
                         ? e->health + 1.0f  // instant kill
                         : (def->damage > 0.0f ? def->damage : 25.0f);
            Combat::applyDamage(entities, eHits[0], damage);
            LOG_INFO("Headshot: %.0f damage (instant kill: %s)",
                     damage, damage > e->health ? "yes" : "no");
            return;
        }
    }

    // Fallback: regular hitscan if no entity in cone
    WeaponDef temp;
    temp.name         = "Headshot";
    temp.type         = WeaponType::HITSCAN;
    temp.damage       = def->damage > 0.0f ? def->damage : 25.0f;
    temp.range        = 80.0f;
    temp.coneAngleDeg = 2.0f;
    temp.cooldown     = 0.0f;
    temp.recoilKick   = 0.02f;
    Combat::fireHitscan(temp, origin, forward, grid, entities);
}

// ---------------------------------------------------------------------------
// Tinkerer skills
// ---------------------------------------------------------------------------

// Drone spawning delegated to engine via callback — engine has mesh registry access.
// type 0 = combat drone (spider), type 1 = swarm drone (bat), type 2 = turret
static void fireCombatDrone(Vec3 origin, Vec3 forward, EntityPool& /*entities*/)
{
    Vec3 right    = {forward.z, 0.0f, -forward.x};
    Vec3 spawnPos = origin + right * 2.0f;
    if (s_droneSpawnCallback) s_droneSpawnCallback(spawnPos, 0);
    LOG_INFO("Combat Drone requested");
}

static void fireSwarmDrones(Vec3 origin, Vec3 forward, EntityPool& /*entities*/)
{
    f32 angles[2] = {-30.0f, 30.0f};
    for (u32 i = 0; i < 2; i++) {
        Vec3 dir      = normalize(rotateY(forward, angles[i]));
        Vec3 spawnPos = origin + dir * 1.5f + Vec3{0.0f, 0.5f, 0.0f};
        if (s_droneSpawnCallback) s_droneSpawnCallback(spawnPos, 1);
    }
    LOG_INFO("Swarm Drones requested (2 units)");
}

// Gravity-arc projectile that splashes on impact; freeze-on-hit via PROJ_SPLASH path.
static void fireStunGrenade(Vec3 origin, Vec3 forward, const SkillDef* def,
                             ProjectilePool& pool)
{
    f32 damage  = def->damage > 0.0f ? def->damage : 15.0f;
    f32 splashR = def->radius > 0.0f ? def->radius : 2.0f;

    u16 slot = ProjectileSystem::spawn(pool, origin, forward,
                                       14.0f, damage, 0.18f, 2.5f,
                                       true, PROJ_GRAVITY | PROJ_SPLASH);
    if (slot != 0xFFFF) {
        Projectile& p  = pool.projectiles[slot];
        p.gravity      = 9.8f;
        p.splashRadius = splashR;
        p.splashDamage = damage;
    }
    LOG_INFO("Stun Grenade thrown");
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
    // ---- Legacy legendary skills ----
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

    // ---- Warrior ----
    case SkillId::CLEAVE:
        fireCleave(eyePos, forward, def, entities);
        break;
    case SkillId::WAR_CRY:
        fireWarCry(eyePos, def, entities);
        break;
    case SkillId::WHIRLWIND:
        fireWhirlwind(eyePos, def, entities);
        break;
    case SkillId::EARTHQUAKE:
        fireEarthquake(eyePos, def);
        break;

    // ---- Ranger ----
    case SkillId::MULTI_SHOT:
        fireMultiShot(eyePos, forward, def, projectiles);
        break;
    case SkillId::RAIN_OF_ARROWS:
        fireRainOfArrows(eyePos, forward, def, grid, entities);
        break;
    case SkillId::POISON_ARROW:
        firePoisonArrow(eyePos, forward, def, projectiles);
        break;
    case SkillId::SHADOW_SHOT:
        fireShadowShot(eyePos, forward, def, grid, entities);
        break;

    // ---- Sorcerer ----
    case SkillId::FIREBALL:
        fireFireball(eyePos, forward, def, projectiles);
        break;

    // ---- Rogue ----
    case SkillId::KNIFE_BURST:
        fireKnifeBurst(eyePos, forward, def, projectiles);
        break;
    case SkillId::POISON_CLOUD:
        firePoisonCloud(eyePos, forward, def, grid);
        break;
    case SkillId::SHADOW_STRIKE:
        fireShadowStrike(eyePos, forward, def, entities, player);
        break;

    // ---- Paladin ----
    case SkillId::HOLY_SMITE:
        fireHolySmite(eyePos, forward, def, entities, player);
        break;
    case SkillId::CONSECRATION:
        fireConsecration(eyePos, def, player);
        break;
    case SkillId::DIVINE_SHIELD:
        fireDivineShield(player);
        break;

    // ---- Combat Engineer ----
    case SkillId::SHOCK_BOLT:
        fireShockBolt(eyePos, forward, def, grid, entities);
        break;
    case SkillId::DEPLOY_TURRET:
        fireDeployTurret(eyePos, forward, entities);
        break;
    case SkillId::TESLA_COIL:
        fireTeslaCoil(eyePos, def, entities, projectiles);
        break;
    case SkillId::MECH_OVERDRIVE:
        fireMechOverdrive(player);
        break;

    // ---- Marksman ----
    case SkillId::AIMED_SHOT:
        fireAimedShot(eyePos, forward, def, grid, entities);
        break;
    case SkillId::EXPLOSIVE_ROUND:
        fireExplosiveRound(eyePos, forward, def, grid, entities);
        break;
    case SkillId::RAPID_FIRE:
        fireRapidFire(eyePos, forward, def, grid, entities);
        break;
    case SkillId::HEADSHOT:
        fireHeadshot(eyePos, forward, def, grid, entities);
        break;

    // ---- Tinkerer ----
    case SkillId::COMBAT_DRONE:
        fireCombatDrone(eyePos, forward, entities);
        break;
    case SkillId::SWARM_DRONES:
        fireSwarmDrones(eyePos, forward, entities);
        break;
    case SkillId::STUN_GRENADE:
        fireStunGrenade(eyePos, forward, def, projectiles);
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
            // Fire explosion visual + ground scorch zone (2s burn at 30% of impact damage)
            if (s_novaCallback) s_novaCallback(m.position, m.radius, {1.0f, 0.5f, 0.1f});
            if (s_scorchCallback) s_scorchCallback(m.position, m.radius, 2.0f, m.damage * 0.3f);
            LOG_INFO("Meteor struck: hit %u enemies", hitCount);
        }
    }
}
