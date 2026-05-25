// enemy_ai_roles.cpp — Archetype role modifiers for EnemyAI::update.
// Handles SUMMONER (necromancer resurrection + curse), HEALER (shaman heal/
// resurrect), CHARGER (sprint + suicide-bomber + retreat cycle), RANGED_CASTER
// (strafe/retreat range management), BOMBER (post-attack retreat), and
// SHIELD_BEARER (face-player + surround state). Also applies the far-enemy
// stagger early-exit (>15m enemies skip full FSM on off-frames).
//
// Returns AIStep::BreakLoop  if the suicide-bomber self-destruct `break` fires
//         AIStep::NextEntity if the far-enemy stagger `continue` fires
//         AIStep::Continue   otherwise
//
// See CLAUDE.md "Data Lifecycles" for entity handle usage.

#include "game/enemy_ai_internal.h"
#include "game/game_constants.h"
#include <cmath>
#include <cstdlib>  // std::rand for boss speech-line variety

AIStep applyRoleModifiers(Entity& e, u32 i,
                           EntityPool& pool,
                           Player& player, Player* targetPlayer,
                           const LevelGrid& grid, f32 dt,
                           f32 dist, Vec3 playerEye)
{
    // --- Archetype special abilities ---

    // Necromancer: resurrect nearby dead enemies every 3s, curse player every 3s
    if (e.enemyRole & EnemyRole::SUMMONER) {
        bool isBoss = (e.enemyType == EnemyType::BOSS); // Sethrak the lich rambles + raises 3
        e.tacticalTimer -= dt;
        if (e.tacticalTimer <= 0.0f) {
            e.tacticalTimer = 3.0f;
            // Bosses raise up to 3 corpses at once; regular necromancers raise 1.
            u32 reviveCount = isBoss ? 3 : 1;
            u32 revived = 0;
            for (u32 r = 0; r < reviveCount; r++) {
                // Find nearest still-dead entity within 10m. Reviving clears ENT_DEAD,
                // so each pass naturally picks the next-nearest corpse.
                f32 bestDist2 = 10.0f * 10.0f;
                u32 bestIdx = 0xFFFF;
                for (u32 di = 0; di < MAX_ENTITIES; di++) {
                    Entity& dead = pool.entities[di];
                    if (!(dead.flags & ENT_DEAD)) continue;
                    if (dead.flags & ENT_FRIENDLY) continue;
                    if (dead.deathTimer <= 0.0f) continue; // slot about to be freed
                    if (dead.isBoss) continue; // never raise a boss — it has guaranteed loot + an exit lock
                    Vec3 diff = dead.position - e.position;
                    f32 d2 = diff.x * diff.x + diff.z * diff.z;
                    if (d2 < bestDist2) {
                        bestDist2 = d2;
                        bestIdx = di;
                    }
                }
                if (bestIdx == 0xFFFF) break; // no more corpses in range
                Entity& revivedEnt = pool.entities[bestIdx];
                bool wasFlying = (revivedEnt.flags & ENT_FLYING) != 0;
                revivedEnt.flags = ENT_ACTIVE | (wasFlying ? ENT_FLYING : 0);
                revivedEnt.health = revivedEnt.maxHealth * 0.3f;
                revivedEnt.aiState = AIState::IDLE;
                revivedEnt.velocity = {0, 0, 0};
                revivedEnt.deathTimer = 0.0f;
                revivedEnt.flashTimer = 0.3f; // flash to show resurrection
                // Ensure revived enemy isn't inside a wall
                Collision::ensureNotInWall(revivedEnt.position, revivedEnt.halfExtents, grid);
                if (!wasFlying) snapEntityToFloor(revivedEnt, grid);
                e.resurrectCount++;
                revived++;
            }
            if (revived > 0) {
                if (isBoss) {
                    static const char* kRiseLines[] = {
                        "Rise! Death is but a brief inconvenience.",
                        "Why rest, my servants, when there is killing to do?",
                        "The grave gives up what I demand of it!",
                    };
                    e.speechText  = kRiseLines[std::rand() % 3];
                    e.speechTimer = 2.5f;
                } else {
                    e.speechText  = "RISE!";
                    e.speechTimer = 2.0f;
                }
            }
        }
        // Necromancer curse: +5% damage taken per stack (max 4), 3s cooldown
        e.kiteTimer -= dt;
        if (e.kiteTimer <= 0.0f && dist < e.detectionRange) {
            e.kiteTimer = 3.0f;
            if (targetPlayer->curseStacks < 4) targetPlayer->curseStacks++;
            targetPlayer->curseTimer = 5.0f;
            if (isBoss) {
                static const char* kCurseLines[] = {
                    "Wither. Rot. Become as I am.",
                    "I curse the warm blood in your veins!",
                    "Your flesh betrays you already, mortal.",
                };
                e.speechText  = kCurseLines[std::rand() % 3];
                e.speechTimer = 2.5f;
            } else {
                e.speechText  = "CURSE!";
                e.speechTimer = 1.5f;
            }
        }
    }

    // Shaman: heal lowest-HP ally within 8m every 1s
    if ((e.enemyRole & EnemyRole::HEALER) && e.aiState != AIState::IDLE) {
        e.tacticalTimer -= dt;
        if (e.tacticalTimer <= 0.0f) {
            e.tacticalTimer = 1.0f;
            f32 lowestHpPct = 1.0f;
            u32 healIdx = 0xFFFF;
            for (u32 ha = 0; ha < pool.activeCount; ha++) {
                u32 hi = pool.activeList[ha];
                Entity& ally = pool.entities[hi];
                if (hi == i) continue; // don't heal self
                if (!(ally.flags & ENT_ACTIVE) || (ally.flags & ENT_DEAD)) continue;
                if (ally.flags & ENT_FRIENDLY) continue;
                Vec3 diff = ally.position - e.position;
                f32 d2 = diff.x * diff.x + diff.z * diff.z;
                if (d2 > 8.0f * 8.0f) continue;
                f32 hpPct = ally.health / ally.maxHealth;
                if (hpPct < lowestHpPct && ally.health < ally.maxHealth) {
                    lowestHpPct = hpPct;
                    healIdx = hi;
                }
            }
            if (healIdx != 0xFFFF) {
                Entity& target = pool.entities[healIdx];
                target.health = fminf(target.health + target.maxHealth * 0.3f, target.maxHealth);
                target.flashTimer = 0.2f; // visual feedback
                e.speechText = "HEAL!";
                e.speechTimer = 1.5f;
            } else {
                // No one to heal — try to resurrect a dead enemy
                e.tacticalTimer = 3.0f;
                f32 bestDist2 = 10.0f * 10.0f;
                u32 bestIdx = 0xFFFF;
                for (u32 di = 0; di < MAX_ENTITIES; di++) {
                    Entity& dead = pool.entities[di];
                    if (!(dead.flags & ENT_DEAD)) continue;
                    if (dead.flags & ENT_FRIENDLY) continue;
                    if (dead.deathTimer <= 0.0f) continue;
                    if (dead.isBoss) continue; // never raise a boss (see SUMMONER note)
                    Vec3 diff = dead.position - e.position;
                    f32 d2 = diff.x * diff.x + diff.z * diff.z;
                    if (d2 < bestDist2) {
                        bestDist2 = d2;
                        bestIdx = di;
                    }
                }
                if (bestIdx != 0xFFFF) {
                    Entity& revived = pool.entities[bestIdx];
                    revived.flags = ENT_ACTIVE;
                    revived.health = revived.maxHealth * 0.3f;
                    revived.aiState = AIState::IDLE;
                    revived.velocity = {0, 0, 0};
                    revived.deathTimer = 0.0f;
                    revived.flashTimer = 0.3f;
                    e.resurrectCount++;
                    e.speechText = "RISE!";
                    e.speechTimer = 2.0f;
                }
            }
        }
    }

    // Charger: sprint at 1.5x speed, retreat after landing attacks
    if (e.enemyRole & EnemyRole::CHARGER) {
        // Sprint at 1.5x speed during CHASE, capped at 10 m/s to avoid overshooting
        if (e.aiState == AIState::CHASE && e.bossDefIdx == 0xFF) {
            f32 sprintSpeed = e.baseMoveSpeed * 1.5f;
            if (sprintSpeed > 10.0f) sprintSpeed = 10.0f;
            e.moveSpeed = sprintSpeed;
        }

        // Suicide bomber: charger+bomber combo self-destructs on reaching target.
        // Kill via damage BEFORE setting ENT_DEAD so the death callback fires
        // and triggers the bomber AoE explosion.
        if ((e.enemyRole & EnemyRole::BOMBER) && e.aiState == AIState::ATTACK) {
            e.attackAnimT = 0.3f;
            EntityHandle selfH = {static_cast<u16>(i), e.generation};
            Combat::applyDamage(pool, selfH, e.health + 1.0f); // lethal — triggers death callback + explosion
            // Original code: `break;` — exits the outer entity for-loop entirely.
            // This entity is now dead; stop processing all further entities this tick.
            return AIStep::BreakLoop;
        }

        // Charger retreat cycle: brief disengage after 3 hits, not every hit.
        // Bosses use their personality system (BERSERKER never retreats).
        if (e.bossDefIdx == 0xFF) {
            // Count hits via hasRetreated + tacticalTimer as hit counter
            if (e.attackAnimT > 0.2f && e.aiState == AIState::ATTACK && !e.hasRetreated) {
                e.hasRetreated = true;
                e.tacticalTimer += 1.0f; // count hits
            }
            if (e.attackAnimT <= 0.0f && e.hasRetreated) {
                e.hasRetreated = false;
            }
            // Retreat briefly after 3+ hits, then reset
            if (e.tacticalTimer >= 3.0f && e.aiState == AIState::ATTACK) {
                e.aiState = AIState::RETREAT;
                e.tacticalTimer = 0.5f; // short retreat
            }
            // Re-engage after retreat
            if (e.aiState == AIState::RETREAT && e.tacticalTimer <= 0.0f) {
                e.aiState = AIState::CHASE;
                e.tacticalTimer = 0.0f;
            }
        }
    }

    // Ranged Caster: prefer strafe at range, retreat if player closes
    if (e.enemyRole & EnemyRole::RANGED_CASTER) {
        if (dist < e.attackRange * 0.5f && e.aiState != AIState::RETREAT) {
            // Player too close — retreat to preferred range
            e.aiState = AIState::RETREAT;
            e.tacticalTimer = 1.0f;
        } else if (dist < e.attackRange && e.aiState == AIState::CHASE) {
            // At firing range — switch to strafe
            e.aiState = AIState::STRAFE;
        }
    }

    // Bomber: dive-bomb (FLYBY) then retreat. Death explosion handled in death callback.
    if (e.enemyRole & EnemyRole::BOMBER) {
        if (e.aiState == AIState::ATTACK) {
            // After attack, immediately retreat
            e.aiState = AIState::RETREAT;
            e.tacticalTimer = 2.0f;
        }
    }

    // Shield Bearer: always face player (for frontal damage reduction in combat.cpp)
    if (e.enemyRole & EnemyRole::SHIELD_BEARER) {
        // Always face toward target for maximum frontal coverage
        Vec3 toTarget = playerEye - e.position;
        e.yaw = atan2f(toTarget.x, toTarget.z);
        // Prefer surround state to spread out with other melee
        if (e.aiState == AIState::CHASE && dist < e.attackRange * 2.0f) {
            e.aiState = AIState::SURROUND;
        }
    }

    // Stagger: distant enemies (>15m) skip the full state machine on off-frames.
    // They still apply velocity from previous decisions — just skip the expensive
    // LOS checks, pathfinding, and state transitions. Saves ~67% of AI cost for
    // enemies the player isn't actively fighting.
    bool farEnemy = (dist > 15.0f);
    if (farEnemy && ((i + s_frameTick) % 3 != 0)) {
        // Apply existing velocity + collision only
        entityMoveAndSlide(e, grid, dt, targetPlayer->position, PLAYER_HALF_WIDTH);
        if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);
        // Original code: `continue;` — skip to next entity in the outer loop.
        return AIStep::NextEntity;
    }

    return AIStep::Continue;
}
