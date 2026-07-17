// skill_rogue.cpp — fire* helpers for the Rogue class skills:
// KnifeBurst, PoisonCloud, ShadowStrike, FanOfKnives, ShadowStep, ShadowDance.
// Called from SkillSystem::tryActivate in skill_system.cpp.

#include "game/skill_internal.h"

// True if a straight line from `from` to `to` isn't blocked by a solid wall.
// Shadow Step uses this so the rogue can't blink through walls to an enemy.
static bool shadowStepHasLOS(Vec3 from, Vec3 to, const LevelGrid& grid) {
    Vec3 d = to - from;
    f32 dist = length(d);
    if (dist < 0.001f) return true;
    RayHit hit = Raycast::cast(grid, from, d * (1.0f / dist), dist);
    return !hit.hit || hit.distance >= dist - 0.1f;
}

// Three fast knives in a tight -8 / 0 / +8 degree fan.
void fireKnifeBurst(Vec3 origin, Vec3 forward, const SkillDef* def,
                    ProjectilePool& pool)
{
    f32 speed  = def->projectileSpeed > 0.0f ? def->projectileSpeed : 30.0f;
    f32 damage = spellScaled((def->damage > 0.0f ? def->damage : 12.0f));
    f32 angles[3] = {-8.0f, 0.0f, 8.0f};
    for (u32 i = 0; i < 3; i++) {
        Vec3 dir = normalize(rotateY(forward, angles[i]));
        ProjectileSystem::spawn(pool, origin, dir, speed, damage, 0.08f, 1.5f, true);
    }
    LOG_INFO("Knife Burst fired 3 knives");
}

// Drop a toxic cloud at a target point using the scorch-zone system.
void firePoisonCloud(Vec3 origin, Vec3 forward, const SkillDef* def,
                     const LevelGrid& grid, EntityPool& entities, Player& player)
{
    RayHit hit  = Raycast::cast(grid, origin, forward, 40.0f);
    Vec3 target = hit.hit ? (origin + forward * hit.distance)
                          : (origin + forward * 15.0f);
    f32 radius  = def->radius > 0.0f ? def->radius : 2.5f;
    f32 dps     = spellScaled((def->damage > 0.0f ? def->damage * 0.4f : 10.0f));

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

    // Grant stealth to the player
    player.smokeTimer = 2.0f;

    // Green-tinted nova for visual + persistent scorch zone (poison DPS)
    if (s_novaCallback)   s_novaCallback(target, radius, {0.1f, 0.8f, 0.1f});
    if (s_scorchCallback) s_scorchCallback(target, radius, 4.0f, dps);
    LOG_INFO("Poison Cloud deployed (aggro reset + 2s stealth)");
}

// Teleport behind nearest enemy and deal 3x damage.
// Returns false (no target) so a whiff stays free and triggers no cooldown.
bool fireShadowStrike(Vec3 origin, Vec3 forward, const SkillDef* def,
                      const LevelGrid& grid, EntityPool& entities, Player& player)
{
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    // Wide cone (cos 0° = -1) to find any nearby enemy
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, forward, -1.0f, 15.0f,
        hits, dists, MAX_ENTITIES);

    if (hitCount == 0) return false;

    Entity* target = handleGet(entities, hits[0]);
    if (!target) return false;

    // Teleport to just behind the target relative to its facing — RESOLVED: "1 m behind"
    // is inside the wall whenever the target hugs one, and inside the BODY for anything
    // wider than ~0.6 m. The resolver walks the blink line back toward the rogue, so a
    // blocked backstab spot degrades to a melee-range landing on the rogue's side.
    Vec3 behind = target->position + Vec3{sinf(target->yaw), 0.0f, cosf(target->yaw)} * 1.0f;
    Vec3 startPos = player.position;
    player.position = Teleport::resolveDest(grid, entities, startPos, behind);
    // Face the enemy's back: look the same direction it faces, level pitch.
    player.yaw   = target->yaw;
    player.pitch = 0.0f;

    f32 damage = spellScaled((def->damage > 0.0f ? def->damage * 3.0f : 90.0f));
    Combat::applyDamage(entities, hits[0], damage);
    target->freezeTimer = 1.5f; // slow on hit (freezeTimer halves move speed)

    if (s_dashCallback) s_dashCallback(startPos, behind);
    LOG_INFO("Shadow Strike: teleported and dealt %.0f damage", damage);
    return true;
}

// Fan of Knives: 144 knife projectiles in a starburst ring + stealth.
// Three rings at different heights for a 3D knife cloud.
void fireFanOfKnives(Vec3 origin, Vec3 forward, const SkillDef* def,
                     ProjectilePool& pool, Player& player)
{
    f32 speed  = def->projectileSpeed > 0.0f ? def->projectileSpeed : 28.0f;
    f32 damage = spellScaled((def->damage > 0.0f ? def->damage : 10.0f));

    // Ring 1: 64 horizontal knives
    for (u32 i = 0; i < 64; i++) {
        f32 angle = (6.2832f / 64.0f) * i;
        Vec3 dir = {cosf(angle), 0.0f, sinf(angle)};
        ProjectileSystem::spawn(pool, origin, dir, speed, damage, 0.06f, 1.3f, true);
    }
    // Ring 2: 48 upward-angled knives
    for (u32 i = 0; i < 48; i++) {
        f32 angle = (6.2832f / 48.0f) * i;
        Vec3 dir = normalize(Vec3{cosf(angle) * 0.8f, 0.4f, sinf(angle) * 0.8f});
        ProjectileSystem::spawn(pool, origin + Vec3{0, 0.3f, 0}, dir, speed * 0.9f, damage, 0.06f, 1.3f, true);
    }
    // Ring 3: 32 downward-angled knives
    for (u32 i = 0; i < 32; i++) {
        f32 angle = (6.2832f / 32.0f) * i;
        Vec3 dir = normalize(Vec3{cosf(angle) * 0.85f, -0.25f, sinf(angle) * 0.85f});
        ProjectileSystem::spawn(pool, origin - Vec3{0, 0.2f, 0}, dir, speed * 1.1f, damage, 0.06f, 1.3f, true);
    }

    // Brief stealth flicker after the burst
    player.smokeTimer = 0.3f;

    // Dark purple nova burst
    if (s_novaCallback) s_novaCallback(origin, 3.0f, {0.4f, 0.1f, 0.6f});
    if (s_particlePool) ParticleSystem::spawnSparks(*s_particlePool, origin, {0, 1, 0}, 8);
    if (s_screenShake) s_screenShake->trigger(0.06f, 0.3f);
    LOG_INFO("Fan of Knives: 144 knives + 1.5s stealth");
}

// Shadow Step: teleport behind nearest enemy. Backstab 3× from stealth.
// Gain stealth after the strike. Dark purple trail + smoke VFX.
// Returns false (no target / no LOS) so a whiff stays free and triggers no cooldown.
bool fireShadowStep(Vec3 origin, Vec3 forward, const SkillDef* def,
                    const LevelGrid& grid, EntityPool& entities, Player& player)
{
    f32 range = def->distance > 0.0f ? def->distance : 15.0f;
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(
        entities, origin, forward, -1.0f, range,
        hits, dists, MAX_ENTITIES);

    if (hitCount == 0) return false;

    // Shadow Step needs line of sight — no blinking through walls. Scan the
    // distance-sorted hits and take the nearest enemy that's alive, hostile,
    // and actually visible from the rogue's eye (origin).
    Entity*      target       = nullptr;
    EntityHandle targetHandle = {};
    for (u32 i = 0; i < hitCount; i++) {
        Entity* e = handleGet(entities, hits[i]);
        if (!e || (e->flags & ENT_DEAD) || (e->flags & ENT_FRIENDLY)) continue;
        if (!shadowStepHasLOS(origin, e->position, grid)) continue;
        target = e; targetHandle = hits[i];
        break;
    }
    if (!target) return false; // nothing visible — Shadow Step does nothing

    // Teleport behind the target — resolved by the shared landing-spot resolver (full
    // footprint vs walls, no landing inside bodies, floor-snapped). The OLD validation here
    // checked one cell center and, when blocked, fell back to teleporting INTO the target's
    // position — which is precisely the "stuck inside enemies" report.
    Vec3 behind = target->position + Vec3{sinf(target->yaw), 0.0f, cosf(target->yaw)} * 1.0f;
    Vec3 startPos = player.position;
    player.position = Teleport::resolveDest(grid, entities, startPos, behind);
    // Face the enemy's back: look the same direction it faces, level pitch.
    player.yaw   = target->yaw;
    player.pitch = 0.0f;

    // Backstab: 3× damage if attacking from stealth
    f32 baseDmg = spellScaled((def->damage > 0.0f ? def->damage : 40.0f));
    f32 damage = (player.smokeTimer > 0.0f) ? baseDmg * 3.0f : baseDmg;
    Combat::applyDamage(entities, targetHandle, damage);
    target->freezeTimer = 1.5f; // slow on hit

    // Gain stealth after the strike
    player.smokeTimer = 1.5f;

    // Dark purple dash trail + smoke at both ends
    if (s_dashCallback) s_dashCallback(startPos, behind);
    if (s_particlePool) {
        ParticleSystem::spawnSmoke(*s_particlePool, startPos, 10);
        ParticleSystem::spawnSmoke(*s_particlePool, behind, 10);
        ParticleSystem::spawnDebris(*s_particlePool, target->position, 6);
    }
    if (s_novaCallback) s_novaCallback(behind, 1.5f, {0.4f, 0.1f, 0.6f});
    if (s_screenShake) s_screenShake->trigger(0.08f, 0.3f);
    LOG_INFO("Shadow Step: %s for %.0f damage",
             damage > baseDmg ? "BACKSTAB" : "strike", damage);
    return true;
}

// Shadow Dance: 2s stealth + 2× damage + 20% speed. Kills extend by 0.3s.
void fireShadowDance(Player& player)
{
    player.shadowDanceTimer = 2.0f;
    player.smokeTimer = 2.0f;

    // Dramatic dark purple nova + particle explosion
    if (s_novaCallback) s_novaCallback(player.position, 5.0f, {0.3f, 0.1f, 0.5f});
    if (s_particlePool) {
        // 64 dark purple particles burst outward
        for (u32 p = 0; p < 64; p++) {
            f32 angle = (6.2832f / 64.0f) * p;
            f32 speed = 6.0f + (std::rand() / static_cast<f32>(RAND_MAX)) * 3.0f;
            Particle tp = {};
            tp.position = player.position;
            tp.velocity = {cosf(angle) * speed, 1.0f + (std::rand() / static_cast<f32>(RAND_MAX)) * 2.0f,
                           sinf(angle) * speed};
            tp.life = 0.6f; tp.maxLife = 0.6f;
            tp.size = 0.05f;
            tp.baseAlpha = 0.8f;
            tp.r = 100; tp.g = 30; tp.b = 160;
            tp.type = PTYPE_GEOMETRIC; tp.flags = PFLAG_FADE | PFLAG_SHRINK;
            ParticleSystem::spawn(*s_particlePool, tp);
        }
    }
    if (s_screenShake) s_screenShake->trigger(0.06f, 0.3f);
    LOG_INFO("Shadow Dance: 2s stealth + 2x damage + 20%% speed");
}
