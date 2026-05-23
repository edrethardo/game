// Boss AI: personality-driven behavior system for boss enemies.
// Bosses use the same role-based FSM as regular enemies but with personality
// overrides (BERSERKER, KITER, TELEPORTER, DUELIST) that change state selection.
// Called from EnemyAI::update when enemyType == BOSS. See boss_def.h for structs
// and the enemy/boss rework design spec for full architecture.

#include "game/boss_ai.h"
#include "game/entity.h"
#include "game/boss_def.h"
#include "game/combat.h"
#include "world/collision.h"
#include "game/projectile.h"
#include "game/player.h"
#include "world/level_grid.h"
#include "renderer/particles.h"
#include <cmath>
#include <cstdlib>

// Particle callback for teleport VFX — set by Engine
static void(*s_magicBurstCb)(Vec3 pos, f32 r, f32 g, f32 b, u32 count) = nullptr;
void BossAI::setMagicBurstCallback(void(*cb)(Vec3, f32, f32, f32, u32)) { s_magicBurstCb = cb; }

// Damage number callback — set by Engine (shared with Combat)
static void(*s_bossSkillDmgCb)(Vec3 pos, f32 dmg) = nullptr;
void BossAI::setDamageNumberCallback(void(*cb)(Vec3, f32)) { s_bossSkillDmgCb = cb; }

// ---------------------------------------------------------------------------
// Teleport to a random walkable cell near target position
// ---------------------------------------------------------------------------
static bool tryTeleport(Entity& e, const LevelGrid& grid, Vec3 targetPos) {
    // Try up to 8 random positions within 8m
    for (u32 attempt = 0; attempt < 8; attempt++) {
        f32 angle = (std::rand() % 360) * (3.14159f / 180.0f);
        f32 dist = 3.0f + (std::rand() % 50) * 0.1f; // 3-8m
        Vec3 candidate = {
            targetPos.x + sinf(angle) * dist,
            e.position.y,
            targetPos.z + cosf(angle) * dist
        };

        // Check full AABB doesn't overlap any solid cells
        if (Collision::entityOverlapsGrid(candidate, e.halfExtents, grid)) continue;

        // Teleport — VFX at origin and destination
        if (s_magicBurstCb) {
            s_magicBurstCb(e.position, 0.6f, 0.3f, 1.0f, 8); // purple burst at origin
            s_magicBurstCb(candidate, 0.6f, 0.3f, 1.0f, 8);   // purple burst at destination
        }
        e.position = candidate;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Boss AI update — called per-tick for each BOSS entity
// ---------------------------------------------------------------------------
void BossAI::update(Entity& e, const BossDef& def,
                    EntityPool& pool, ProjectilePool& projectiles,
                    Player& player, const LevelGrid& grid, f32 dt) {

    Vec3 playerPos = player.position;
    Vec3 delta = playerPos - e.position;
    f32 dist = sqrtf(delta.x * delta.x + delta.z * delta.z);

    // --- Enrage curve: boss gets faster/more aggressive as HP drops ---
    f32 hpPct = (e.maxHealth > 0.0f) ? (e.health / e.maxHealth) : 0.0f;
    f32 enrageMult = 1.0f + (1.0f - hpPct) * def.enrageFactor;
    // Berserker personality gets extra enrage scaling
    if (def.personality == BossPersonality::BERSERKER) {
        enrageMult *= 1.0f + (1.0f - hpPct) * 0.5f;
    }

    // Apply enrage to effective stats
    f32 effectiveSpeed = def.speed * enrageMult;
    f32 effectiveCooldown = def.atkCooldown / enrageMult;
    e.moveSpeed = effectiveSpeed;
    e.attackCooldown = effectiveCooldown;

    // --- Personality-driven state selection ---
    switch (def.personality) {
        case BossPersonality::BERSERKER: {
            // Relentless pursuit — force CHASE from any non-combat state.
            // ATTACK is allowed (melee swings), DEAD is untouchable.
            if (e.aiState != AIState::ATTACK && e.aiState != AIState::DEAD) {
                e.aiState = AIState::CHASE;
            }
        } break;

        case BossPersonality::KITER: {
            // Maintain preferred range — retreat if player is too close
            f32 preferredRange = def.atkRange * 1.5f;
            if (preferredRange < 6.0f) preferredRange = 6.0f;

            if (dist < preferredRange * 0.7f) {
                e.aiState = AIState::RETREAT;
            } else if (dist > preferredRange * 1.3f) {
                e.aiState = AIState::CHASE;
            } else {
                // At preferred range — strafe if ranged, otherwise idle/surround
                if (def.roles & EnemyRole::RANGED_CASTER) {
                    e.aiState = AIState::STRAFE;
                }
            }
        } break;

        case BossPersonality::TELEPORTER: {
            // Blink every 4-6s to a random position near the player
            e.kiteTimer -= dt;
            if (e.kiteTimer <= 0.0f) {
                e.kiteTimer = 4.0f + (std::rand() % 20) * 0.1f; // 4-6s
                tryTeleport(e, grid, playerPos);
            }
            // Between teleports, strafe if ranged, chase if melee
            if (def.roles & EnemyRole::RANGED_CASTER) {
                if (e.aiState == AIState::IDLE) e.aiState = AIState::STRAFE;
            } else {
                if (e.aiState == AIState::IDLE) e.aiState = AIState::CHASE;
            }
        } break;

        case BossPersonality::DUELIST: {
            // Circle-strafe by default, periodic counter-charge bursts
            if (e.aiState == AIState::IDLE) {
                e.aiState = AIState::STRAFE;
            }
            // Periodic counter-charge: strafe for 2-3s, then charge for 0.8s
            if (e.sprintTimer > 0.0f) {
                e.aiState = AIState::CHASE;
            } else {
                e.aiState = AIState::STRAFE;
                // Reset sprint timer for next charge burst
                e.tacticalTimer -= dt;
                if (e.tacticalTimer <= 0.0f && dist < def.atkRange * 3.0f) {
                    e.sprintTimer = 0.8f;
                    e.tacticalTimer = 2.0f + (std::rand() % 10) * 0.1f;
                }
            }
        } break;

        default: break;
    }

    // --- Boss ability firing is owned by the per-boss dispatch in enemy_ai.cpp ---
    // That switch (keyed on the boss floor) provides each boss's *signature* attack
    // — poison nova, frost fan, summons, etc. — and shares the same `e.flybyTimer`
    // cooldown. Firing a generic projectile here too made projectile-enabled bosses
    // decrement flybyTimer twice per tick and fire two attacks at once. We keep
    // BossAI for personality/movement/enrage only and let the switch be the single
    // ability authority. (Migrating those patterns into BossDef/bosses.json to
    // retire the floor-keyed switch entirely is a separate refactor.)
    (void)projectiles; (void)pool;

    // --- Summoner role: spawn minions ---
    if (def.roles & EnemyRole::SUMMONER) {
        // Summoner behavior is handled by the existing archetype code in enemy_ai.cpp
        // Boss summoners also spawn fresh minions (not just resurrect)
        // This is done via the same tacticalTimer path
    }

    // --- Boss skill casting ---
    // Skills are cast on a separate timer (sprintTimer repurposed for skill cooldown)
    // Actual skill execution happens in enemy_ai.cpp where the skill system is accessible
}

// ---------------------------------------------------------------------------
// Check if any minion spawned by a boss is still alive (for minion shield)
// ---------------------------------------------------------------------------
bool BossAI::hasMinionAlive(const EntityPool& pool, u16 bossIdx) {
    for (u32 a = 0; a < pool.activeCount; a++) {
        u32 i = pool.activeList[a];
        const Entity& e = pool.entities[i];
        if (e.flags & ENT_DEAD) continue;
        if (e.spawnerIdx == bossIdx) return true;
    }
    return false;
}
