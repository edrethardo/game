#include "game/skill.h"
#include "game/combat.h"
#include "game/player.h"
#include "audio/audio.h"
#include "world/combat_query.h"
#include "world/raycast.h"
#include "renderer/debug_draw.h"
#include "renderer/particles.h"
#include "renderer/camera.h"  // for ScreenShake
#include "core/log.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

// Not static — extern'd by engine.cpp for rendering the targeting circles
PendingMeteor s_meteors[MAX_PENDING_METEORS];
static ParticlePool* s_particlePool = nullptr;

// Skill power scaling (0.0 = base, 1.0 = max). Set by engine before tryActivate.
// Class skills get 0.0; legendary item skills get a value scaled by item level.
static f32 s_skillPower = 0.0f;
void SkillSystem::setSkillPower(f32 power) { s_skillPower = power; }

// Class skill damage multiplier — scales damage/heal numbers by effective floor.
// Set to floor-based mult for class skills, 1.0 for item skills.
static f32 s_classDmgMult = 1.0f;
void SkillSystem::setClassDamageMult(f32 mult) { s_classDmgMult = mult; }

// Equipped weapon base damage — set by engine before marksman skill activation.
// Marksman skills scale off weapon damage instead of fixed skill damage.
static f32 s_weaponDamage = 10.0f;
void SkillSystem::setWeaponDamage(f32 dmg) { s_weaponDamage = dmg; }
static ScreenShake*  s_screenShake  = nullptr;
static SkillSystem::NovaCallback s_novaCallback = nullptr;
static SkillSystem::DashCallback s_dashCallback = nullptr;
static SkillSystem::ScorchCallback s_scorchCallback = nullptr;
static SkillSystem::DroneSpawnCallback s_droneSpawnCallback = nullptr;
static SkillSystem::ChainCallback s_chainCallback = nullptr;
static SkillSystem::BeamCallback s_beamCallback = nullptr;
static SkillSystem::ReloadCallback s_reloadCallback = nullptr;
static u8 s_boltMeshId = 0;    // set by engine during init for shock bolt projectiles
static u8 s_shockBoltMatId = 0;

// Overcharged Magazine state (Marksman buff)
static f32 s_overchargeTimer = 0.0f;
static u8  s_overchargeShots = 0;

void SkillSystem::setNovaCallback(NovaCallback cb) { s_novaCallback = cb; }
void SkillSystem::setDashCallback(DashCallback cb) { s_dashCallback = cb; }
void SkillSystem::setScorchCallback(ScorchCallback cb) { s_scorchCallback = cb; }
void SkillSystem::setDroneSpawnCallback(DroneSpawnCallback cb) { s_droneSpawnCallback = cb; }
void SkillSystem::setChainCallback(ChainCallback cb) { s_chainCallback = cb; }
void SkillSystem::setBeamCallback(BeamCallback cb) { s_beamCallback = cb; }
void SkillSystem::setReloadCallback(ReloadCallback cb) { s_reloadCallback = cb; }

bool SkillSystem::isOvercharged() { return s_overchargeTimer > 0.0f && s_overchargeShots > 0; }
void SkillSystem::consumeOverchargeShot() {
    if (s_overchargeShots > 0) s_overchargeShots--;
    if (s_overchargeShots == 0) s_overchargeTimer = 0.0f;
}
void SkillSystem::tickOvercharge(f32 dt) {
    if (s_overchargeTimer > 0.0f) {
        s_overchargeTimer -= dt;
        if (s_overchargeTimer <= 0.0f) { s_overchargeTimer = 0.0f; s_overchargeShots = 0; }
    }
}
void SkillSystem::setBoltMeshId(u8 meshId, u8 matId) { s_boltMeshId = meshId; s_shockBoltMatId = matId; }
void SkillSystem::setFXTargets(ParticlePool* particles, ScreenShake* shake) {
    s_particlePool = particles;
    s_screenShake  = shake;
}

// ---------------------------------------------------------------------------
// Static helpers (individual skill fires)
// ---------------------------------------------------------------------------

static void fireFrozenOrb(Vec3 origin, Vec3 direction, const SkillDef* def,
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

static void fireChainLightning(Vec3 origin, Vec3 direction, const SkillDef* def,
                                const LevelGrid& /*grid*/, EntityPool& entities)
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

static void fireMeteorStrike(Vec3 origin, Vec3 direction, const SkillDef* def,
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
// 5% lifesteal based on total damage dealt.
static void fireCleave(Vec3 origin, Vec3 forward, const SkillDef* def,
                       EntityPool& entities, Player& player)
{
    WeaponDef temp;
    temp.name        = "Cleave";
    temp.type        = WeaponType::MELEE;
    temp.damage      = (def->damage > 0.0f ? def->damage : 35.0f) * s_classDmgMult;
    temp.range       = def->radius  > 0.0f ? def->radius : 3.0f;
    temp.coneAngleDeg = 120.0f;
    temp.cooldown    = 0.0f;
    temp.recoilKick  = 0.0f;
    AttackResult result = Combat::fireMelee(temp, origin, forward, entities);

    // Heal player for 5% of damage dealt
    f32 hitCount = static_cast<f32>(result.entitiesHit);
    f32 healAmt = temp.damage * 0.05f * hitCount;
    player.health = fminf(player.health + healAmt, player.maxHealth);
    LOG_INFO("Cleave fired, hit %u, healed %.1f", result.entitiesHit, healAmt);
}

// Ground stomp AoE — damages, stuns, and slows all enemies in radius.
// Stun duration scales with upgrade: 0.2s base, 0.5s upgraded.
static void fireThunderclap(Vec3 origin, const SkillDef* def, EntityPool& entities)
{
    // Ground-level position — origin is eye height, drop to feet
    Vec3 groundPos = {origin.x, origin.y - 1.7f, origin.z};

    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    f32 range  = def->radius > 0.0f ? def->radius : 5.0f;
    f32 damage = (def->damage > 0.0f ? def->damage : 25.0f) * s_classDmgMult;
    // Query from ground position with omnidirectional search
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, groundPos, {0.0f, -1.0f, 0.0f}, -1.0f, range,
        hits, dists, MAX_ENTITIES);

    f32 stunTime = def->duration > 0.0f ? def->duration : 0.2f;
    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage);
        Entity* e = handleGet(entities, hits[i]);
        if (e) {
            e->stunTimer = stunTime;       // full immobilize
            e->freezeTimer = 1.5f;         // slow after stun wears off
            // The Butcher is especially vulnerable to thunderclap
            if (e->nameTag && std::strcmp(e->nameTag, "The Butcher") == 0) {
                e->stunTimer = 3.0f;
                e->speechText = "Oh no, I'm dizzy!";
                e->speechTimer = 3.0f;
            }
        }
    }

    // Brown-grey ground stomp visual at feet level (earthy shockwave)
    if (s_novaCallback) s_novaCallback(groundPos, range, {0.5f, 0.4f, 0.25f});
    // Persistent scorch zone — lingering fire damage in the stomp area
    if (s_scorchCallback) {
        s_scorchCallback(groundPos, range * 0.6f, 3.0f, 5.0f); // 60% radius, 3s duration, 5 DPS
    }
    LOG_INFO("Thunderclap hit %u enemies", hitCount);
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
        if (e) e->stunTimer = 2.0f;  // 2-second stun
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
    f32 damage   = (def->damage > 0.0f ? def->damage : 30.0f) * s_classDmgMult;
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, range,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage);
    }
    if (s_novaCallback) s_novaCallback(origin, range, {0.8f, 0.3f, 0.1f});
    LOG_INFO("Whirlwind hit %u enemies", hitCount);
}

// Slam the ground — reuses meteor pool for an instant at-feet blast + knockback.
static void fireEarthquake(Vec3 origin, const SkillDef* def, EntityPool& entities)
{
    f32 radius = def->radius > 0.0f ? def->radius : 4.0f;

    for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
        if (!s_meteors[i].active) {
            s_meteors[i].position = origin;
            s_meteors[i].damage   = (def->damage > 0.0f ? def->damage : 40.0f) * s_classDmgMult;
            s_meteors[i].radius   = radius;
            // Very short delay so it fires on the next updateMeteors tick
            s_meteors[i].timer    = 0.05f;
            s_meteors[i].active   = true;
            break;
        }
    }

    // Immediate knockback — push entities outward from the epicentre
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = entities.entities[i];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        Vec3 diff = e.position - origin;
        f32 dist2 = diff.x*diff.x + diff.z*diff.z;
        if (dist2 < radius * radius && dist2 > 0.01f) {
            Vec3 pushDir = normalize(diff);
            e.position = e.position + pushDir * 2.0f;
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

// Five projectiles spread in a -20 / -10 / 0 / +10 / +20 degree horizontal fan.
static void fireMultiShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                          ProjectilePool& pool)
{
    f32 speed  = def->projectileSpeed > 0.0f ? def->projectileSpeed : 25.0f;
    f32 damage = (def->damage > 0.0f ? def->damage : 20.0f) * s_classDmgMult;
    f32 angles[5] = {-20.0f, -10.0f, 0.0f, 10.0f, 20.0f};
    for (u32 i = 0; i < 5; i++) {
        Vec3 dir = normalize(rotateY(forward, angles[i]));
        ProjectileSystem::spawn(pool, origin, dir, speed, damage, 0.1f, 2.0f, true);
    }
    LOG_INFO("Multi Shot fired 5 arrows");
}

// Instant AoE at a raycasted target point — no delay, like a meteor that fires now.
static void fireRainOfArrows(Vec3 origin, Vec3 forward, const SkillDef* def,
                              const LevelGrid& grid, EntityPool& entities)
{
    RayHit hit    = Raycast::cast(grid, origin, forward, 50.0f);
    Vec3 target   = hit.hit ? (origin + forward * hit.distance)
                            : (origin + forward * 20.0f);
    f32 radius    = def->radius > 0.0f ? def->radius : 3.5f;
    f32 damage    = (def->damage > 0.0f ? def->damage : 25.0f) * s_classDmgMult;

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
    f32 damage = (def->damage > 0.0f ? def->damage : 18.0f) * s_classDmgMult;
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
    temp.damage  = (def->damage > 0.0f ? def->damage * 2.5f : 60.0f) * s_classDmgMult;
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
    f32 damage      = (def->damage > 0.0f ? def->damage : 35.0f) * s_classDmgMult;
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
        p.lightColor   = {1.0f, 0.4f, 0.1f}; // orange fire glow
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
    f32 damage = (def->damage > 0.0f ? def->damage : 12.0f) * s_classDmgMult;
    f32 angles[3] = {-8.0f, 0.0f, 8.0f};
    for (u32 i = 0; i < 3; i++) {
        Vec3 dir = normalize(rotateY(forward, angles[i]));
        ProjectileSystem::spawn(pool, origin, dir, speed, damage, 0.08f, 1.5f, true);
    }
    LOG_INFO("Knife Burst fired 3 knives");
}

// Drop a toxic cloud at a target point using the scorch-zone system.
static void firePoisonCloud(Vec3 origin, Vec3 forward, const SkillDef* def,
                             const LevelGrid& grid, EntityPool& entities)
{
    RayHit hit  = Raycast::cast(grid, origin, forward, 40.0f);
    Vec3 target = hit.hit ? (origin + forward * hit.distance)
                          : (origin + forward * 15.0f);
    f32 radius  = def->radius > 0.0f ? def->radius : 2.5f;
    f32 dps     = (def->damage > 0.0f ? def->damage * 0.4f : 10.0f) * s_classDmgMult;

    // Immediate aggro reset — enemies in the cloud lose track of the player
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = entities.entities[i];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        Vec3 diff = e.position - target;
        f32 dist2 = diff.x*diff.x + diff.z*diff.z;
        if (dist2 < radius * radius) {
            e.aiState = AIState::IDLE;
            e.stunTimer = 1.5f; // brief stun to prevent immediate re-aggro
        }
    }

    // Green-tinted nova for visual + persistent scorch zone (poison DPS)
    if (s_novaCallback)   s_novaCallback(target, radius, {0.1f, 0.8f, 0.1f});
    if (s_scorchCallback) s_scorchCallback(target, radius, 4.0f, dps);
    LOG_INFO("Poison Cloud deployed (aggro reset)");
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

    f32 damage = (def->damage > 0.0f ? def->damage * 3.0f : 90.0f) * s_classDmgMult;
    Combat::applyDamage(entities, hits[0], damage);
    target->freezeTimer = 1.5f; // slow on hit (freezeTimer halves move speed)

    if (s_dashCallback) s_dashCallback(startPos, behind);
    LOG_INFO("Shadow Strike: teleported and dealt %.0f damage", damage);
}

// ---------------------------------------------------------------------------
// Paladin skills
// ---------------------------------------------------------------------------

// Holy Bombardment state — persistent zone that spawns pillars over time
static f32  s_bombardmentTimer  = 0.0f;
static Vec3 s_bombardmentCenter = {0,0,0};
static f32  s_bombardmentAccum  = 0.0f;
static f32  s_bombardmentDamage = 0.0f;
static f32  s_bombardmentRadius = 0.0f;

// Holy Nova delayed second hit state
static f32  s_holyNovaTimer     = 0.0f;
static Vec3 s_holyNovaCenter    = {0,0,0};
static f32  s_holyNovaDamage2   = 0.0f;
static f32  s_holyNovaRadius    = 0.0f;
static f32  s_holyNovaHealPct   = 0.0f;

// Dash-smite: 3m forward dash, stops on first enemy, spawns gold judgment pillar.
// Heals 8 flat + 20% of damage dealt.
static void fireHolySmite(Vec3 origin, Vec3 forward, const SkillDef* def,
                           EntityPool& entities, Player& player,
                           const LevelGrid& grid)
{
    // Flatten to XZ plane for dash direction
    Vec3 dashDir = {forward.x, 0.0f, forward.z};
    if (lengthSq(dashDir) < 0.001f) dashDir = {0.0f, 0.0f, -1.0f};
    else dashDir = normalize(dashDir);

    f32 dashDist = def->distance > 0.0f ? def->distance : 3.0f;

    // Raycast for wall obstruction
    Vec3 rayOrigin = player.position + Vec3{0.0f, 0.5f, 0.0f};
    RayHit wallHit = Raycast::cast(grid, rayOrigin, dashDir, dashDist);
    f32 actualDist = wallHit.hit ? (wallHit.distance - 0.5f) : dashDist;
    if (actualDist < 0.5f) actualDist = 0.5f;

    Vec3 startPos = player.position;

    // Find first enemy in a narrow cone along the dash path
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, startPos + Vec3{0.0f, 0.5f, 0.0f}, dashDir,
        cosf(radians(15.0f)), actualDist, hits, dists, MAX_ENTITIES);

    f32 dashDmg = def->damage * s_classDmgMult;
    f32 pillarDmg = 15.0f * s_classDmgMult;
    Vec3 pillarPos = startPos + dashDir * actualDist; // default: end of dash
    f32 totalDmg = 0.0f;

    // Stop on first hostile enemy
    for (u32 i = 0; i < hitCount; i++) {
        Entity* ent = handleGet(entities, hits[i]);
        if (!ent || (ent->flags & ENT_FRIENDLY)) continue;
        // Dash to this enemy's position
        actualDist = dists[i];
        pillarPos = ent->position;
        // Deal dash damage + stun
        Combat::applyDamage(entities, hits[i], dashDmg);
        ent->stunTimer = fmaxf(ent->stunTimer, 0.3f);
        totalDmg += dashDmg;
        break;
    }

    // Teleport player to dash endpoint
    Vec3 endPos = startPos + dashDir * actualDist;
    player.position = endPos;

    // Spawn gold judgment pillar at target
    for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
        if (!s_meteors[i].active) {
            s_meteors[i].position    = pillarPos;
            s_meteors[i].damage      = pillarDmg;
            s_meteors[i].radius      = def->radius > 0.0f ? def->radius : 1.5f;
            s_meteors[i].timer       = 0.1f;
            s_meteors[i].active      = true;
            s_meteors[i].healsPlayer = true;
            s_meteors[i].color       = {1.0f, 0.9f, 0.3f};
            totalDmg += pillarDmg; // anticipated pillar damage for heal calc
            break;
        }
    }

    // Flat heal + 20% of damage dealt
    f32 heal = 8.0f * s_classDmgMult + totalDmg * 0.2f;
    player.health = fminf(player.health + heal, player.maxHealth);

    // Gold dash trail visual
    if (s_dashCallback) s_dashCallback(startPos, endPos);
    if (s_screenShake) s_screenShake->trigger(0.04f, 0.2f);
    LOG_INFO("Holy Smite: dashed %.1fm, dealt %.0f dmg, healed %.0f", actualDist, totalDmg, heal);
}

// Holy Bombardment: places a 6m judgment zone. For 4s, gold pillars rain on enemies.
// Each pillar hit heals 3% max HP. Ground scorch zone for residual DPS.
static void fireHolyBombardment(Vec3 origin, const SkillDef* def, Player& player)
{
    s_bombardmentRadius = def->radius > 0.0f ? def->radius : 6.0f;
    s_bombardmentDamage = (def->damage > 0.0f ? def->damage : 18.0f) * s_classDmgMult;
    s_bombardmentCenter = origin;
    s_bombardmentTimer  = def->duration > 0.0f ? def->duration : 4.0f;
    s_bombardmentAccum  = 0.0f;

    // Scorch zone for residual ground DPS
    f32 scorchDps = 4.0f * s_classDmgMult;
    if (s_scorchCallback) s_scorchCallback(origin, s_bombardmentRadius, 4.0f, scorchDps);

    // Gold nova ring on activation
    if (s_novaCallback) s_novaCallback(origin, s_bombardmentRadius, {1.0f, 0.9f, 0.3f});
    if (s_screenShake) s_screenShake->trigger(0.05f, 0.3f);
    LOG_INFO("Holy Bombardment: judgment zone active for 4s");
}

// Holy Nova: instant 360° dual-hit AoE. Ring wave damages + heals instantly,
// particle wave deals second hit after 0.3s. Both heal allies (NPCs + players).
static void fireHolyNova(Vec3 origin, const SkillDef* def,
                          EntityPool& entities, Player& player)
{
    f32 radius = def->radius > 0.0f ? def->radius : 5.0f;
    f32 ringDmg = def->damage * s_classDmgMult;
    f32 healPct = def->allyHealPct > 0.0f ? def->allyHealPct : 0.08f;

    // Ring wave (instant) — damage enemies, heal allies
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, radius,
        hits, dists, MAX_ENTITIES);

    u32 enemiesHit = 0;
    u32 alliesHealed = 0;
    for (u32 i = 0; i < hitCount; i++) {
        Entity* ent = handleGet(entities, hits[i]);
        if (!ent) continue;
        if (ent->flags & ENT_FRIENDLY) {
            // Heal friendly NPCs (turrets, summons)
            ent->health = fminf(ent->health + ent->maxHealth * healPct, ent->maxHealth);
            alliesHealed++;
        } else {
            Combat::applyDamage(entities, hits[i], ringDmg);
            enemiesHit++;
        }
    }

    // Expanding gold ring nova (this IS the ring damage visual)
    if (s_novaCallback) s_novaCallback(origin, radius, {1.0f, 0.85f, 0.3f});

    // 144 golden particles in a sphere burst (3 rings)
    if (s_particlePool) {
        // Ring 1: 64 horizontal
        for (u32 p = 0; p < 64; p++) {
            f32 angle = (6.2832f / 64.0f) * p;
            f32 speed = 8.0f + (std::rand() / static_cast<f32>(RAND_MAX)) * 4.0f;
            Particle tp = {};
            tp.position = origin;
            tp.velocity = {cosf(angle) * speed, 0.2f, sinf(angle) * speed};
            tp.life = 0.5f; tp.maxLife = 0.5f;
            tp.size = 0.04f + (std::rand() / static_cast<f32>(RAND_MAX)) * 0.04f;
            tp.baseAlpha = 0.9f;
            tp.r = 255; tp.g = 200; tp.b = 50;
            tp.type = PTYPE_GEOMETRIC; tp.flags = PFLAG_FADE | PFLAG_SHRINK;
            ParticleSystem::spawn(*s_particlePool, tp);
        }
        // Ring 2: 48 upward-angled
        for (u32 p = 0; p < 48; p++) {
            f32 angle = (6.2832f / 48.0f) * p;
            f32 speed = 9.0f + (std::rand() / static_cast<f32>(RAND_MAX)) * 3.0f;
            Particle tp = {};
            tp.position = origin;
            tp.velocity = {cosf(angle) * speed * 0.7f, speed * 0.5f, sinf(angle) * speed * 0.7f};
            tp.life = 0.5f; tp.maxLife = 0.5f;
            tp.size = 0.05f + (std::rand() / static_cast<f32>(RAND_MAX)) * 0.03f;
            tp.baseAlpha = 0.85f;
            tp.r = 255; tp.g = 210; tp.b = 40;
            tp.type = PTYPE_GEOMETRIC; tp.flags = PFLAG_FADE | PFLAG_SHRINK;
            ParticleSystem::spawn(*s_particlePool, tp);
        }
        // Ring 3: 32 downward-angled
        for (u32 p = 0; p < 32; p++) {
            f32 angle = (6.2832f / 32.0f) * p;
            f32 speed = 10.0f + (std::rand() / static_cast<f32>(RAND_MAX)) * 2.0f;
            Particle tp = {};
            tp.position = origin;
            tp.velocity = {cosf(angle) * speed * 0.8f, -speed * 0.3f, sinf(angle) * speed * 0.8f};
            tp.life = 0.5f; tp.maxLife = 0.5f;
            tp.size = 0.04f + (std::rand() / static_cast<f32>(RAND_MAX)) * 0.04f;
            tp.baseAlpha = 0.8f;
            tp.r = 255; tp.g = 180; tp.b = 30;
            tp.type = PTYPE_GEOMETRIC; tp.flags = PFLAG_FADE | PFLAG_SHRINK;
            ParticleSystem::spawn(*s_particlePool, tp);
        }
        // Upward sparks
        ParticleSystem::spawnSparks(*s_particlePool, origin, {0.0f, 1.0f, 0.0f}, 8);
    }

    // Set up delayed second hit (particle wave arrives 0.3s later)
    s_holyNovaTimer   = 0.3f;
    s_holyNovaCenter  = origin;
    s_holyNovaDamage2 = (def->secondaryDamage > 0.0f ? def->secondaryDamage : 20.0f) * s_classDmgMult;
    s_holyNovaRadius  = radius;
    s_holyNovaHealPct = healPct;

    if (s_screenShake) s_screenShake->trigger(0.08f, 0.4f);
    LOG_INFO("Holy Nova: ring hit %u enemies, healed %u allies", enemiesHit, alliesHealed);
}

// Divine Judgment: cleanse + invuln + AoE stun + 3 massive judgment pillars on nearest enemies.
// Heals 10% max HP on activation + 15% per kill from pillars.
static void fireDivineJudgment(Player& player, EntityPool& entities, const SkillDef* def)
{
    // Phase 1: Cleanse all debuffs
    player.slowTimer   = 0.0f;
    player.poisonTimer = 0.0f;
    player.poisonDps   = 0.0f;
    player.burnTimer   = 0.0f;
    player.burnDps     = 0.0f;
    player.freezeTimer = 0.0f;

    // 10% max HP heal + invulnerability
    player.health = fminf(player.health + player.maxHealth * 0.10f, player.maxHealth);
    player.invulnTimer = 1.5f;

    // AoE stun — 1.5s on nearby hostiles
    f32 stunRadius = 5.0f;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = entities.entities[i];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        Vec3 diff = e.position - player.position;
        f32 dist2 = diff.x*diff.x + diff.z*diff.z;
        if (dist2 < stunRadius * stunRadius) {
            e.stunTimer = 1.5f;
        }
    }

    // Massive gold nova visual
    if (s_novaCallback) s_novaCallback(player.position, stunRadius, {1.0f, 0.95f, 0.4f});
    if (s_particlePool) ParticleSystem::spawnExplosion(*s_particlePool, player.position, 3.0f);

    // Phase 2: 3 judgment pillars on nearest enemies (10m range)
    EntityHandle nearest[MAX_ENTITIES];
    f32          nearDists[MAX_ENTITIES];
    u32 found = CombatQuery::queryConeSorted(
        entities, player.position, {0.0f, 0.0f, -1.0f}, -1.0f, 10.0f,
        nearest, nearDists, MAX_ENTITIES);

    // Filter to hostiles only, pick up to 3
    f32 pillarDmg = (def->damage > 0.0f ? def->damage : 60.0f) * s_classDmgMult;
    f32 pillarRadius = def->radius > 0.0f ? def->radius : 2.5f;
    u8 pillarsSpawned = 0;
    Vec3 pillarPositions[4]; // origin + up to 3 targets for chain visual
    pillarPositions[0] = player.position;

    for (u32 i = 0; i < found && pillarsSpawned < 3; i++) {
        Entity* ent = handleGet(entities, nearest[i]);
        if (!ent || (ent->flags & ENT_FRIENDLY)) continue;

        for (u32 m = 0; m < MAX_PENDING_METEORS; m++) {
            if (!s_meteors[m].active) {
                s_meteors[m].position    = ent->position;
                s_meteors[m].damage      = pillarDmg;
                s_meteors[m].radius      = pillarRadius;
                s_meteors[m].timer       = 0.15f + pillarsSpawned * 0.15f;
                s_meteors[m].active      = true;
                s_meteors[m].healsPlayer = true; // kill-heal checked in updateMeteors
                s_meteors[m].color       = {1.0f, 0.95f, 0.4f};
                pillarPositions[pillarsSpawned + 1] = ent->position;
                pillarsSpawned++;
                break;
            }
        }
    }

    // Chain visual connecting pillar positions (divine triangle seal)
    if (s_chainCallback && pillarsSpawned > 0) {
        s_chainCallback(pillarPositions, pillarsSpawned + 1);
    }

    if (s_screenShake) s_screenShake->trigger(0.15f, 0.7f);
    LOG_INFO("Divine Judgment: invuln 1.5s, stunned nearby, %u pillars spawned", pillarsSpawned);
}

// ---------------------------------------------------------------------------
// Combat Engineer skills
// ---------------------------------------------------------------------------

// Electric bolt — triple bolt triangle formation for visual impact.
// Three parallel bolts travel in a tight triangle, all dealing full damage.
static void fireShockBolt(Vec3 origin, Vec3 forward, const SkillDef* def,
                           ProjectilePool& projectiles)
{
    f32 damage = (def->damage > 0.0f ? def->damage : 20.0f) * s_classDmgMult;

    // Build perpendicular axes for the triangle offset
    Vec3 right = normalize(Vec3{-forward.z, 0.0f, forward.x});
    Vec3 up = {0.0f, 1.0f, 0.0f};

    // Triangle offsets: center, up-left, down-right (~0.15m apart)
    Vec3 offsets[3] = {
        {0.0f, 0.0f, 0.0f},                         // center bolt
        right * -0.1f + up * 0.1f,                   // up-left
        right * 0.1f + up * -0.1f,                   // down-right
    };

    for (u32 b = 0; b < 3; b++) {
        Vec3 spawnPos = origin + offsets[b];
        u16 idx = ProjectileSystem::spawn(projectiles, spawnPos, forward,
                                           35.0f, damage, 0.10f, 2.5f, true, PROJ_SPARK);
        if (idx != 0xFFFF) {
            projectiles.projectiles[idx].freezeDuration = 1.5f; // slow on hit
            projectiles.projectiles[idx].meshId = s_boltMeshId;
            projectiles.projectiles[idx].lightColor = {0.5f, 0.7f, 1.0f};
            // Stun built into freezeDuration — the projectile collision path
            // applies freezeTimer which halves move speed. For the brief stagger,
            // we use a slightly longer freeze (acts as stun since enemies can't act while frozen).
        }
    }

    // Spark burst at muzzle for visual punch
    if (s_particlePool) ParticleSystem::spawnSparks(*s_particlePool, origin, forward, 6);
    LOG_INFO("Shock Bolt fired (triangle formation)");
}

// Spawn a friendly turret entity 2m in front of the player.
static void fireDeployTurret(Vec3 origin, Vec3 forward, EntityPool& /*entities*/)
{
    Vec3 spawnPos = origin + forward * 2.0f;
    if (s_droneSpawnCallback) s_droneSpawnCallback(spawnPos, 2); // type 2 = turret
    LOG_INFO("Turret requested");
}

// AoE lightning burst — 360° starburst of sparks + chain arcs to every hit enemy.
static void fireTeslaCoil(Vec3 origin, const SkillDef* def,
                           EntityPool& entities, ProjectilePool& pool)
{
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    f32 radius = def->radius > 0.0f ? def->radius : 4.0f;
    f32 damage = (def->damage > 0.0f ? def->damage : 30.0f) * s_classDmgMult;

    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f, radius,
        hits, dists, MAX_ENTITIES);

    for (u32 i = 0; i < hitCount; i++) {
        Combat::applyDamage(entities, hits[i], damage);
        // Brief electric stagger on all hit enemies
        Entity* e = handleGet(entities, hits[i]);
        if (e) e->stunTimer = fmaxf(e->stunTimer, 0.1f);
    }

    // Massive starburst: 3 rings of sparks ×4 density (144 total)
    // Ring 1: 64 horizontal sparks (fast, wide)
    for (u32 s = 0; s < 64; s++) {
        f32  angle = s * (6.28318f / 64.0f);
        f32  speed = 12.0f + (s % 6) * 2.0f;
        Vec3 dir   = {cosf(angle), 0.05f, sinf(angle)};
        u16 si = ProjectileSystem::spawn(pool, origin, dir, speed, 0.0f, 0.05f, 0.6f,
                                true, PROJ_SPARK);
        if (si != 0xFFFF) pool.projectiles[si].lightColor = {0.4f, 0.7f, 1.0f};
    }
    // Ring 2: 48 upward-angled sparks (medium speed, rising)
    for (u32 s = 0; s < 48; s++) {
        f32  angle = s * (6.28318f / 48.0f) + 0.13f;
        Vec3 dir   = {cosf(angle) * 0.7f, 0.6f, sinf(angle) * 0.7f};
        u16 si = ProjectileSystem::spawn(pool, origin + Vec3{0, 0.3f, 0}, dir, 10.0f, 0.0f, 0.04f, 0.5f,
                                true, PROJ_SPARK);
        if (si != 0xFFFF) pool.projectiles[si].lightColor = {0.3f, 0.6f, 1.0f};
    }
    // Ring 3: 32 downward-angled sparks (fast, ground-hugging)
    for (u32 s = 0; s < 32; s++) {
        f32  angle = s * (6.28318f / 32.0f) + 0.2f;
        Vec3 dir   = {cosf(angle), -0.3f, sinf(angle)};
        u16 si = ProjectileSystem::spawn(pool, origin, dir, 18.0f, 0.0f, 0.06f, 0.35f,
                                true, PROJ_SPARK);
        if (si != 0xFFFF) pool.projectiles[si].lightColor = {0.5f, 0.8f, 1.0f};
    }

    // Chain FX arcs from origin to every hit enemy — visual lightning web
    if (s_chainCallback && hitCount > 0) {
        Vec3 chainPts[24];
        u8 ptCount = 1;
        chainPts[0] = origin;
        for (u32 i = 0; i < hitCount && ptCount < 24; i++) {
            Entity* e = handleGet(entities, hits[i]);
            if (e) chainPts[ptCount++] = e->position + Vec3{0, e->halfExtents.y, 0};
        }
        if (ptCount > 1) s_chainCallback(chainPts, ptCount);
    }

    // Visual feedback: bright nova + sparks + shake
    if (s_novaCallback) s_novaCallback(origin, radius, {0.6f, 0.85f, 1.0f});
    if (s_particlePool) ParticleSystem::spawnSparks(*s_particlePool, origin, {0, 1, 0}, 12);
    if (s_screenShake)  s_screenShake->trigger(0.12f, 0.6f);
    LOG_INFO("Tesla Coil hit %u enemies", hitCount);
}

// Overdrive: restore HP + clear debuffs + 5s damage/speed buff.
// Overdrive: restore HP + clear debuffs + 5s damage/speed buff.
static void fireMechOverdrive(Player& player)
{
    player.health = (player.health + 20.0f > player.maxHealth)
                    ? player.maxHealth : player.health + 20.0f;
    player.slowTimer   = 0.0f;
    player.freezeTimer = 0.0f;
    player.overdriveTimer = 5.0f; // 5s buff — consumed by combat/movement systems
    LOG_INFO("Mech Overdrive activated (5s buff)");
}

// ---------------------------------------------------------------------------
// Marksman skills
// ---------------------------------------------------------------------------

// Penetrating Rail — pierces through ALL enemies in a line. Weapon damage × 3.
// Leaves visible beam trail. Applies 2s slow to all hit.
static void fireAimedShot(Vec3 origin, Vec3 forward, const SkillDef* def,
                           const LevelGrid& grid, EntityPool& entities)
{
    // Scales off weapon damage (Marksman gimmick: skills amplify weapon)
    f32 damage = s_weaponDamage * 1.2f * s_classDmgMult;
    f32 range  = 80.0f;

    // Penetrating rail: test ray against every entity AABB along the line.
    // This gives precise aim (like hitscan) but pierces through all of them.
    u32 hitCount = 0;
    for (u32 a = 0; a < entities.activeCount; a++) {
        u32 idx = entities.activeList[a];
        Entity& e = entities.entities[idx];
        if (e.flags & ENT_DEAD) continue;
        if (e.flags & ENT_FRIENDLY) continue;

        // Ray-AABB intersection test
        AABB box = entityAABB(e);
        Vec3 invDir = {1.0f / (forward.x != 0.0f ? forward.x : 0.0001f),
                       1.0f / (forward.y != 0.0f ? forward.y : 0.0001f),
                       1.0f / (forward.z != 0.0f ? forward.z : 0.0001f)};
        f32 t1 = (box.min.x - origin.x) * invDir.x;
        f32 t2 = (box.max.x - origin.x) * invDir.x;
        f32 t3 = (box.min.y - origin.y) * invDir.y;
        f32 t4 = (box.max.y - origin.y) * invDir.y;
        f32 t5 = (box.min.z - origin.z) * invDir.z;
        f32 t6 = (box.max.z - origin.z) * invDir.z;
        f32 tmin = fmaxf(fmaxf(fminf(t1, t2), fminf(t3, t4)), fminf(t5, t6));
        f32 tmax = fminf(fminf(fmaxf(t1, t2), fmaxf(t3, t4)), fmaxf(t5, t6));
        if (tmax < 0.0f || tmin > tmax || tmin > range) continue;

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
static void fireExplosiveRound(Vec3 origin, Vec3 forward, const SkillDef* def,
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
static void fireOverchargedMagazine(Vec3 origin, Vec3 forward)
{
    s_overchargeTimer = 4.0f;
    s_overchargeShots = 5;

    // Activation VFX
    if (s_particlePool) {
        ParticleSystem::spawnSparks(*s_particlePool, origin, forward, 6);
        ParticleSystem::spawnMagicBurst(*s_particlePool, origin, 255, 200, 50, 8);
    }
    if (s_screenShake) s_screenShake->trigger(0.04f, 0.2f);
    LOG_INFO("Overcharged Magazine: 5 shots buffed for 4s");
}

// Execution Strike — execute <25% HP, chain-execute nearby wounded, massive VFX.
static void fireHeadshot(Vec3 origin, Vec3 forward, const SkillDef* def,
                          const LevelGrid& grid, EntityPool& entities,
                          SkillState& skillState)
{
    // Find target in narrow cone
    EntityHandle eHits[MAX_ENTITIES];
    f32          eDists[MAX_ENTITIES];
    u32 cnt = CombatQuery::queryConeSorted(entities, origin, forward,
                                           cosf(radians(2.0f)), 80.0f,
                                           eHits, eDists, MAX_ENTITIES);

    // Beam trail to wall/target regardless of hit
    RayHit wallHit = Raycast::cast(grid, origin, forward, 80.0f);
    Vec3 beamEnd = wallHit.hit ? (origin + forward * wallHit.distance)
                               : (origin + forward * 80.0f);

    if (cnt > 0) {
        Entity* e = handleGet(entities, eHits[0]);
        if (e && !(e->flags & ENT_DEAD) && !(e->flags & ENT_FRIENDLY)) {
            bool isExecute = (e->health < e->maxHealth * 0.25f);
            // Non-execute: scales off weapon damage (Marksman gimmick)
            f32 damage = isExecute
                         ? e->health + 1.0f
                         : s_weaponDamage * 2.5f * s_classDmgMult;
            Combat::applyDamage(entities, eHits[0], damage);
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
                    if (chainHits[j].index == eHits[0].index) continue; // skip primary
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
    f32 damage  = (def->damage > 0.0f ? def->damage : 15.0f) * s_classDmgMult;
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

    // Holy Nova delayed second hit (particle wave) — ticked here since no entity access needed
    if (s_holyNovaTimer > 0.0f) {
        s_holyNovaTimer -= dt;
        if (s_holyNovaTimer <= 0.0f) {
            s_holyNovaTimer = 0.0f;
            // Spawn a PendingMeteor for the delayed AoE (resolved in updateMeteors)
            for (u32 m = 0; m < MAX_PENDING_METEORS; m++) {
                if (!s_meteors[m].active) {
                    s_meteors[m].position    = s_holyNovaCenter;
                    s_meteors[m].damage      = s_holyNovaDamage2;
                    s_meteors[m].radius      = s_holyNovaRadius;
                    s_meteors[m].timer       = 0.001f; // triggers next frame
                    s_meteors[m].active      = true;
                    s_meteors[m].healsPlayer = true;
                    s_meteors[m].color       = {1.0f, 0.85f, 0.3f};
                    break;
                }
            }
        }
    }
}

bool SkillSystem::tryActivate(SkillState& ss, const SkillDef* skillDefs, u32 skillDefCount,
                               Vec3 eyePos, Vec3 forward, f32 /*yaw*/,
                               ProjectilePool& projectiles, EntityPool& entities,
                               const LevelGrid& grid, Player& player,
                               f32 cooldownReduction)
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

    ss.cooldownTimer = def->cooldown * (1.0f - cooldownReduction);
    if (ss.cooldownTimer < 0.05f) ss.cooldownTimer = 0.05f; // hard minimum

    // Play skill activation sound — mapped by theme so each skill has audio feedback
    switch (ss.activeSkill) {
    case SkillId::FIREBALL:
    case SkillId::CONSECRATION:
    case SkillId::HOLY_BOMBARDMENT:
        AudioSystem::play(SfxId::SKILL_FIRE); break;
    case SkillId::FROZEN_ORB:
        AudioSystem::play(SfxId::SKILL_ICE); break;
    case SkillId::CHAIN_LIGHTNING:
    case SkillId::SHOCK_BOLT:
    case SkillId::TESLA_COIL:
        AudioSystem::play(SfxId::SKILL_LIGHTNING); break;
    case SkillId::BLOOD_NOVA:
    case SkillId::POISON_CLOUD:
    case SkillId::POISON_ARROW:
        AudioSystem::play(SfxId::SKILL_BLOOD); break;
    case SkillId::PHASE_DASH:
    case SkillId::SHADOW_STRIKE:
    case SkillId::SHADOW_SHOT:
        AudioSystem::play(SfxId::SKILL_DASH); break;
    case SkillId::HOLY_SMITE:
        AudioSystem::play(SfxId::SKILL_STUN); break;   // thunderous divine impact
    case SkillId::DIVINE_SHIELD:
    case SkillId::DIVINE_JUDGMENT:
        AudioSystem::play(SfxId::SKILL_HEAL); break;
    case SkillId::HOLY_NOVA:
        AudioSystem::play(SfxId::SKILL_EXPLOSION); break;
    case SkillId::WAR_CRY:
    case SkillId::MECH_OVERDRIVE:
        AudioSystem::play(SfxId::SKILL_BUFF); break;
    case SkillId::DEPLOY_TURRET:
    case SkillId::COMBAT_DRONE:
    case SkillId::SWARM_DRONES:
        AudioSystem::play(SfxId::SKILL_SUMMON); break;
    case SkillId::METEOR_STRIKE:
    case SkillId::EXPLOSIVE_ROUND:
    case SkillId::EARTHQUAKE:
    case SkillId::STUN_GRENADE:
    case SkillId::RAIN_OF_ARROWS:
        AudioSystem::play(SfxId::SKILL_EXPLOSION); break;
    case SkillId::THUNDERCLAP:
    case SkillId::CLEAVE:
    case SkillId::WHIRLWIND:
        AudioSystem::play(SfxId::SKILL_STUN); break;
    case SkillId::MULTI_SHOT:
    case SkillId::AIMED_SHOT:
    case SkillId::RAPID_FIRE:
    case SkillId::HEADSHOT:
    case SkillId::KNIFE_BURST:
        AudioSystem::play(SfxId::WEAPON_BOW, 0.5f); break;
    case SkillId::OVERCHARGED_MAGAZINE:
        AudioSystem::play(SfxId::SKILL_BUFF); break;
    default: break;
    }

    // Particle burst on activation — spawned at muzzle/cast point for immediate feedback
    if (s_particlePool) {
        Vec3 castPos = eyePos + forward * 0.5f;
        switch (ss.activeSkill) {
        case SkillId::FIREBALL:     // trails handled by projectile system
        case SkillId::SHOCK_BOLT:   // trails handled by projectile system
        case SkillId::FROZEN_ORB:   // trails handled by projectile system
            break;
        case SkillId::CONSECRATION:
        case SkillId::HOLY_BOMBARDMENT:
            ParticleSystem::spawnMagicBurst(*s_particlePool, castPos, 255, 210, 50, 12);
            break;
        case SkillId::HOLY_NOVA:
        case SkillId::DIVINE_JUDGMENT:
            break; // particles spawned inside the fire functions
        case SkillId::CHAIN_LIGHTNING:
        case SkillId::TESLA_COIL:
            ParticleSystem::spawnSparks(*s_particlePool, castPos, forward, 6);
            break;
        case SkillId::BLOOD_NOVA:
            break;
        case SkillId::PHASE_DASH:
        case SkillId::SHADOW_STRIKE:
            ParticleSystem::spawnSmoke(*s_particlePool, eyePos, 8);
            break;
        case SkillId::EARTHQUAKE:
            ParticleSystem::spawnExplosion(*s_particlePool, eyePos + forward * 2.0f, 2.0f);
            if (s_screenShake) s_screenShake->trigger(0.1f, 0.5f);
            break;
        case SkillId::EXPLOSIVE_ROUND:
        case SkillId::AIMED_SHOT:
        case SkillId::HEADSHOT:
        case SkillId::OVERCHARGED_MAGAZINE:
            break; // VFX handled inside fire functions
        case SkillId::METEOR_STRIKE:
            break; // no cast-time VFX — explosion + smoke spawn on impact in updateMeteors
        case SkillId::THUNDERCLAP:
            ParticleSystem::spawnSparks(*s_particlePool, eyePos, {0.0f, 1.0f, 0.0f}, 10);
            if (s_screenShake) s_screenShake->trigger(0.06f, 0.3f);
            break;
        default: break;
        }
    }

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
        fireCleave(eyePos, forward, def, entities, player);
        break;
    case SkillId::THUNDERCLAP:
        fireThunderclap(eyePos, def, entities);
        break;
    case SkillId::WAR_CRY:
        fireWarCry(eyePos, def, entities);
        break;
    case SkillId::WHIRLWIND:
        fireWhirlwind(eyePos, def, entities);
        break;
    case SkillId::EARTHQUAKE:
        fireEarthquake(eyePos, def, entities);
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
        firePoisonCloud(eyePos, forward, def, grid, entities);
        break;
    case SkillId::SHADOW_STRIKE:
        fireShadowStrike(eyePos, forward, def, entities, player);
        break;

    // ---- Paladin ----
    case SkillId::HOLY_SMITE:
        fireHolySmite(eyePos, forward, def, entities, player, grid);
        break;
    case SkillId::CONSECRATION:
    case SkillId::HOLY_BOMBARDMENT:
        fireHolyBombardment(eyePos, def, player);
        break;
    case SkillId::HOLY_NOVA:
        fireHolyNova(eyePos, def, entities, player);
        break;
    case SkillId::DIVINE_SHIELD:
    case SkillId::DIVINE_JUDGMENT:
        fireDivineJudgment(player, entities, def);
        break;

    // ---- Combat Engineer ----
    case SkillId::SHOCK_BOLT:
        fireShockBolt(eyePos, forward, def, projectiles);
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
    case SkillId::OVERCHARGED_MAGAZINE:
        fireOverchargedMagazine(eyePos, forward);
        break;
    case SkillId::HEADSHOT:
        fireHeadshot(eyePos, forward, def, grid, entities, ss);
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

            // Spawn shards in random spherical directions (2x count for dense burst)
            u8 burstCount = def->shardCount * 2;
            for (u8 s = 0; s < burstCount; s++) {
                // Random phi (azimuth) and theta (elevation) for full sphere coverage
                f32 phi   = (rand() / static_cast<f32>(RAND_MAX)) * 6.28318f;
                f32 theta = (rand() / static_cast<f32>(RAND_MAX)) * 3.14159f - 1.5708f; // -90 to +90 deg
                Vec3 shardDir = {cosf(theta) * cosf(phi), sinf(theta), cosf(theta) * sinf(phi)};

                // Shard damage scales: classDmgMult (stored on orb) + skillPower (item level)
                f32 orbClassMult = p.damage > 0.0f ? p.damage : 1.0f; // stored at spawn
                f32 scaledShardDmg = def->shardDamage * orbClassMult * (1.0f + s_skillPower);
                u16 shardIdx = ProjectileSystem::spawn(pool, p.position, shardDir, def->shardSpeed,
                                        scaledShardDmg, def->shardRadius, 0.6f, true);

                // Mark the freshly spawned shard (bit 1 = isOrbShard)
                if (shardIdx != 0xFFFF) {
                    pool.projectiles[shardIdx].projFlags = PROJ_ORB_SHARD;
                    pool.projectiles[shardIdx].lightColor = {0.2f, 0.5f, 0.8f}; // light blue
                }
            }

            // Rotate the spoke angle for the spirograph effect
            p.orbAngle += radians(def->angleStepDeg);
        }
    }
}

void SkillSystem::updateMeteors(EntityPool& entities, Player& player, f32 dt) {
    // Holy Bombardment tick — needs entity access for smart targeting
    if (s_bombardmentTimer > 0.0f) {
        s_bombardmentTimer -= dt;
        s_bombardmentAccum += dt;
        while (s_bombardmentAccum >= 0.4f) {
            s_bombardmentAccum -= 0.4f;

            // Target nearest hostile enemy in zone
            Vec3 pillarPos = s_bombardmentCenter;
            f32 bestDist2 = s_bombardmentRadius * s_bombardmentRadius;
            bool foundTarget = false;
            for (u32 i = 0; i < MAX_ENTITIES; i++) {
                Entity& e = entities.entities[i];
                if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
                if (e.flags & ENT_FRIENDLY) continue;
                Vec3 diff = e.position - s_bombardmentCenter;
                f32 d2 = diff.x*diff.x + diff.z*diff.z;
                if (d2 < bestDist2) {
                    // Pick a random enemy (cycle via rand to avoid always targeting the same one)
                    if (!foundTarget || (std::rand() % 3 == 0)) {
                        pillarPos = e.position;
                        foundTarget = true;
                    }
                }
            }
            if (!foundTarget) {
                // No enemies — random position in zone for area denial visual
                f32 angle = (std::rand() / static_cast<f32>(RAND_MAX)) * 6.2832f;
                f32 dist  = (std::rand() / static_cast<f32>(RAND_MAX)) * s_bombardmentRadius * 0.8f;
                pillarPos.x += cosf(angle) * dist;
                pillarPos.z += sinf(angle) * dist;
            }

            for (u32 m = 0; m < MAX_PENDING_METEORS; m++) {
                if (!s_meteors[m].active) {
                    s_meteors[m].position    = pillarPos;
                    s_meteors[m].damage      = s_bombardmentDamage;
                    s_meteors[m].radius      = 1.5f;
                    s_meteors[m].timer       = 0.3f;
                    s_meteors[m].active      = true;
                    s_meteors[m].healsPlayer = true;
                    s_meteors[m].color       = {1.0f, 0.9f, 0.3f};
                    break;
                }
            }
            if (s_screenShake) s_screenShake->trigger(0.03f, 0.15f);
        }
        if (s_bombardmentTimer <= 0.0f) s_bombardmentTimer = 0.0f;
    }

    // Process pending meteors (fire + holy pillar impacts)
    for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
        PendingMeteor& m = s_meteors[i];
        if (!m.active) continue;

        m.timer -= dt;
        if (m.timer <= 0.0f) {
            // Explode — damage enemies, heal allies within blast radius
            EntityHandle hits[MAX_ENTITIES];
            f32          dists[MAX_ENTITIES];
            u32 hitCount = CombatQuery::queryConeSorted(
                entities, m.position, {0.0f, -1.0f, 0.0f}, -1.0f, m.radius,
                hits, dists, MAX_ENTITIES);

            u32 enemiesHit = 0;
            for (u32 j = 0; j < hitCount; j++) {
                Entity* ent = handleGet(entities, hits[j]);
                if (!ent) continue;
                if (m.healsPlayer && (ent->flags & ENT_FRIENDLY)) {
                    // Holy pillar heals allies
                    ent->health = fminf(ent->health + ent->maxHealth * 0.08f, ent->maxHealth);
                } else {
                    // Check if kill for Divine Judgment bonus
                    f32 hpBefore = ent->health;
                    Combat::applyDamage(entities, hits[j], m.damage);
                    if (m.healsPlayer && hpBefore > 0.0f && ent->health <= 0.0f) {
                        // Kill heal: 15% max HP
                        player.health = fminf(player.health + player.maxHealth * 0.15f, player.maxHealth);
                    }
                    enemiesHit++;
                }
            }

            // Holy pillar heals player 3% max HP on hit
            if (m.healsPlayer && enemiesHit > 0) {
                player.health = fminf(player.health + player.maxHealth * 0.03f, player.maxHealth);
            }

            m.active = false;

            if (m.healsPlayer) {
                // Gold holy pillar impact VFX
                if (s_novaCallback) s_novaCallback(m.position, m.radius, m.color);
                if (s_particlePool) {
                    ParticleSystem::spawnMagicBurst(*s_particlePool, m.position, 255, 210, 50, 8);
                }
            } else {
                // Fire meteor impact VFX (original behavior)
                if (s_novaCallback) s_novaCallback(m.position, m.radius, {1.0f, 0.5f, 0.1f});
                if (s_scorchCallback) s_scorchCallback(m.position, m.radius, 2.0f, m.damage * 0.3f);
                if (s_particlePool) {
                    ParticleSystem::spawnExplosion(*s_particlePool, m.position, m.radius);
                    ParticleSystem::spawnSmoke(*s_particlePool, m.position, 12);
                    ParticleSystem::spawnSmoke(*s_particlePool, m.position + Vec3{0, 0.5f, 0}, 8);
                }
                if (s_screenShake) s_screenShake->trigger(0.1f, 0.5f);
            }
            LOG_INFO("Meteor/pillar struck: hit %u enemies", enemiesHit);
        }
    }
}
