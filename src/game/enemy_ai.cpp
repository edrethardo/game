// Enemy AI: per-tick FSM (IDLE -> CHASE -> ATTACK; FLYBY for flying enemies)
// driven from EnemyAI::update. Handles LOS checks (staggered by aiCheckIdx for
// budget), grid-axis-separated movement (flying ignores floor/ceiling unless
// blocked), and attack execution against the player. Death is routed through
// Combat::applyDamage; this file does not free entities directly. See
// CLAUDE.md "Data Lifecycles" for the entity handle/death flow.

#include "game/enemy_ai.h"
#include "game/player.h"
#include "game/combat.h"
#include "game/game_constants.h"
#include "world/raycast.h"
#include "world/level_grid.h"
#include <cmath>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Grid collision for entities (simplified axis-separated slide)
// ---------------------------------------------------------------------------
static bool entityOverlapsGrid(Vec3 centre, Vec3 halfExtents,
                                const LevelGrid& grid)
{
    f32 minX = centre.x - halfExtents.x;
    f32 maxX = centre.x + halfExtents.x;
    f32 minZ = centre.z - halfExtents.z;
    f32 maxZ = centre.z + halfExtents.z;

    s32 cx0 = static_cast<s32>(std::floor(minX / grid.cellSize));
    s32 cx1 = static_cast<s32>(std::floor((maxX - 0.0001f) / grid.cellSize));
    s32 cz0 = static_cast<s32>(std::floor(minZ / grid.cellSize));
    s32 cz1 = static_cast<s32>(std::floor((maxZ - 0.0001f) / grid.cellSize));

    for (s32 z = cz0; z <= cz1; z++) {
        for (s32 x = cx0; x <= cx1; x++) {
            if (!LevelGridSystem::isInBounds(grid, (u32)x, (u32)z)) return true;
            if (LevelGridSystem::isSolid(grid, (u32)x, (u32)z)) return true;
        }
    }
    return false;
}

// Snap a ground entity's Y to the floor height of its current grid cell.
// Called after XZ movement to keep entities from floating or sinking.
static void snapEntityToFloor(Entity& e, const LevelGrid& grid) {
    u32 gx, gz;
    if (LevelGridSystem::worldToGrid(grid, e.position, gx, gz) &&
        !LevelGridSystem::isSolid(grid, gx, gz)) {
        f32 floorH = LevelGridSystem::getFloorHeight(grid, gx, gz);
        e.position.y = floorH + e.halfExtents.y;
    }
}

static void entityMoveAndSlide(Entity& e, const LevelGrid& grid, f32 dt) {
    Vec3 delta = e.velocity * dt;

    // X axis
    Vec3 tryPos = e.position + Vec3{delta.x, 0, 0};
    if (entityOverlapsGrid(tryPos, e.halfExtents, grid)) {
        e.velocity.x = 0.0f;
    } else {
        e.position.x = tryPos.x;
    }

    // Z axis
    tryPos = e.position + Vec3{0, 0, delta.z};
    if (entityOverlapsGrid(tryPos, e.halfExtents, grid)) {
        e.velocity.z = 0.0f;
    } else {
        e.position.z = tryPos.z;
    }

    // Y axis (flying only — ground enemies snap to floor)
    if (e.flags & ENT_FLYING) {
        tryPos = e.position + Vec3{0, delta.y, 0};
        if (entityOverlapsGrid(tryPos, e.halfExtents, grid)) {
            e.velocity.y = 0.0f;
        } else {
            e.position.y = tryPos.y;
        }

        // Clamp Y to floor/ceiling of current cell
        u32 gx, gz;
        if (LevelGridSystem::worldToGrid(grid, e.position, gx, gz) &&
            !LevelGridSystem::isSolid(grid, gx, gz)) {
            f32 floorH = LevelGridSystem::getFloorHeight(grid, gx, gz);
            f32 ceilH  = LevelGridSystem::getCeilingHeight(grid, gx, gz);
            f32 minY = floorH + e.halfExtents.y;
            f32 maxY = ceilH  - e.halfExtents.y;
            if (e.position.y < minY) e.position.y = minY;
            if (e.position.y > maxY) e.position.y = maxY;
        }
    }
}

// ---------------------------------------------------------------------------
// Line-of-sight check (world only — ignores other entities)
// ---------------------------------------------------------------------------
static bool hasLOS(const Entity& e, const Player& player,
                   const LevelGrid& grid)
{
    Vec3 eyePos = player.position + Vec3{0, player.eyeHeight, 0};
    Vec3 toPlayer = eyePos - e.position;
    f32 dist = length(toPlayer);
    if (dist < 0.001f) return true;

    Vec3 dir = toPlayer * (1.0f / dist);
    RayHit hit = Raycast::cast(grid, e.position, dir, dist);
    return !hit.hit || hit.distance >= dist - 0.1f;
}

// ---------------------------------------------------------------------------
// Main AI update
// ---------------------------------------------------------------------------
void EnemyAI::update(EntityPool& pool, const LevelGrid& grid,
                      Player& player, ProjectilePool& projectiles, f32 dt)
{
    (void)projectiles;
    Vec3 playerEye = player.position + Vec3{0, player.eyeHeight, 0};

    for (u32 a = 0; a < pool.activeCount; a++) {
        u32 i = pool.activeList[a];
        Entity& e = pool.entities[i];
        if (e.flags & ENT_DEAD) continue;

        // Tick animation timer
        e.animTimer += dt;
        if (e.attackAnimT > 0.0f) e.attackAnimT -= dt;

        // Determine if this entity is a friendly NPC ally
        bool isFriendly = (e.flags & ENT_FRIENDLY) != 0;

        // ---------------------------------------------------------------------------
        // Friendly NPC AI — follows player, attacks nearest hostile enemy
        // ---------------------------------------------------------------------------
        if (isFriendly) {
            // Find closest hostile enemy within detection range
            f32 bestEnemyDist = e.detectionRange;
            u16 bestEnemyIdx = 0xFFFF;
            Vec3 enemyPos = {0,0,0};

            for (u32 ni = 0; ni < pool.activeCount; ni++) {
                u32 nIdx = pool.activeList[ni];
                const Entity& enemy = pool.entities[nIdx];
                if (enemy.flags & ENT_FRIENDLY) continue;  // skip other friendlies
                if (enemy.flags & ENT_DEAD) continue;
                if (!(enemy.flags & ENT_ACTIVE)) continue;

                Vec3 toEnemy = enemy.position - e.position;
                f32 eDist = length(toEnemy);
                if (eDist < bestEnemyDist) {
                    bestEnemyDist = eDist;
                    bestEnemyIdx = static_cast<u16>(nIdx);
                    enemyPos = enemy.position;
                }
            }

            e.targetEntityIdx = bestEnemyIdx;

            if (bestEnemyIdx != 0xFFFF) {
                // Chase and attack the nearest hostile enemy
                Vec3 toEnemy = enemyPos - e.position;
                f32 eDist = length(toEnemy);
                Vec3 dirToEnemy = (eDist > 0.001f) ? toEnemy * (1.0f / eDist) : Vec3{0,0,0};

                // Face the enemy
                if (eDist > 0.001f) {
                    e.yaw = atan2f(-dirToEnemy.x, -dirToEnemy.z);
                }

                if (eDist > e.attackRange) {
                    // Chase: move toward enemy on XZ plane
                    Vec3 flatDir = normalize(Vec3{dirToEnemy.x, 0.0f, dirToEnemy.z});
                    e.velocity.x = flatDir.x * e.moveSpeed;
                    e.velocity.z = flatDir.z * e.moveSpeed;
                    entityMoveAndSlide(e, grid, dt);
                } else {
                    // Attack: stand still and swing
                    e.velocity = {0, 0, 0};
                    e.attackTimer -= dt;
                    if (e.attackTimer <= 0.0f) {
                        e.attackTimer = e.attackCooldown;
                        e.attackAnimT = 0.3f;

                        // Deal damage to the enemy entity
                        Entity& target = pool.entities[bestEnemyIdx];
                        if (!(target.flags & ENT_DEAD)) {
                            EntityHandle th = {bestEnemyIdx, target.generation};
                            Combat::applyDamage(pool, th, e.damage);

                            // Random attack speech (1-in-5 chance)
                            if ((std::rand() % 5) == 0) {
                                static const char* attackLines[] = {"Take that!", "Die, beast!", "For glory!"};
                                e.speechText = attackLines[std::rand() % 3];
                                e.speechTimer = 2.5f;
                            }
                        }
                    }
                }

                snapEntityToFloor(e, grid);
            } else {
                // No enemies nearby — follow the player at a comfortable distance
                Vec3 toPlayer = playerEye - e.position;
                f32 pDist = length(toPlayer);

                if (pDist > GameConst::NPC_FOLLOW_DIST) {
                    // Too far from player: move toward them
                    Vec3 flatDir = normalize(Vec3{toPlayer.x, 0.0f, toPlayer.z});
                    e.velocity.x = flatDir.x * e.moveSpeed;
                    e.velocity.z = flatDir.z * e.moveSpeed;
                    e.yaw = atan2f(-flatDir.x, -flatDir.z);
                    entityMoveAndSlide(e, grid, dt);
                } else {
                    // Close enough to player: idle in place
                    e.velocity = {0, 0, 0};
                }

                snapEntityToFloor(e, grid);
            }

            // Speech timer decay (clears bubble when expired)
            if (e.speechTimer > 0.0f) {
                e.speechTimer -= dt;
                if (e.speechTimer <= 0.0f) {
                    e.speechText = nullptr;
                }
            }

            // Low health desperate speech (random ~2-second cadence)
            if (e.health < e.maxHealth * 0.3f && e.health > 0 && e.speechTimer <= 0.0f) {
                if ((std::rand() % 120) == 0) {
                    static const char* hurtLines[] = {"I'm hurt...", "Help!", "Can't... hold on..."};
                    e.speechText = hurtLines[std::rand() % 3];
                    e.speechTimer = 3.0f;
                }
            }

            continue; // skip hostile AI path for friendly NPCs
        }

        // ---------------------------------------------------------------------------
        // Hostile enemy AI — targets friendly NPCs first, then player
        // ---------------------------------------------------------------------------
        Vec3 toPlayer = playerEye - e.position;
        f32  dist     = length(toPlayer);

        // Default target is the player
        Vec3 targetPos = playerEye;
        f32  targetDist = dist;
        bool targetIsNPC = false;

        // Search for closer friendly NPCs to target instead of the player
        {
            f32 bestNpcDist = dist; // only retarget if NPC is closer than the player
            for (u32 ni = 0; ni < pool.activeCount; ni++) {
                u32 nIdx = pool.activeList[ni];
                const Entity& npc = pool.entities[nIdx];
                if (!(npc.flags & ENT_FRIENDLY)) continue;
                if (npc.flags & ENT_DEAD) continue;

                Vec3 toNpc = npc.position - e.position;
                f32 npcDist = length(toNpc);
                if (npcDist < bestNpcDist && npcDist <= e.detectionRange) {
                    bestNpcDist = npcDist;
                    targetPos = npc.position + Vec3{0, npc.halfExtents.y, 0};
                    targetDist = npcDist;
                    targetIsNPC = true;
                    e.targetEntityIdx = static_cast<u16>(nIdx);
                }
            }
            if (!targetIsNPC) {
                e.targetEntityIdx = 0xFFFF;
            }
        }

        // Direction toward current target (NPC or player)
        Vec3 toTarget = targetPos - e.position;
        f32  tDist = length(toTarget);
        Vec3 dirToTarget = (tDist > 0.001f) ? toTarget * (1.0f / tDist) : Vec3{0,0,0};

        bool isBat = (e.flags & ENT_FLYING) != 0;

        // Face toward target (yaw only) — except during flyby
        if (tDist > 0.001f && e.aiState != AIState::FLYBY) {
            e.yaw = atan2f(-dirToTarget.x, -dirToTarget.z);
        }

        switch (e.aiState) {

        case AIState::IDLE: {
            e.velocity = {0, 0, 0};

            // Bats hold position when idle — no position wobble
            if (isBat) {
                (void)0; // wings flap via LimbSystem, body stays still
            }

            // Staggered LOS check — bats check more frequently (every 4 frames)
            e.aiCheckIdx++;
            u16 checkFreq = isBat ? 4 : 8;
            if (e.aiCheckIdx >= checkFreq) {
                e.aiCheckIdx = 0;
                // Trigger on NPC target or player within detection range
                if (targetIsNPC && targetDist <= e.detectionRange) {
                    e.aiState = AIState::CHASE;
                } else if (dist <= e.detectionRange && hasLOS(e, player, grid)) {
                    e.aiState = AIState::CHASE;
                }
            }
        } break;

        case AIState::CHASE: {
            if (isBat) {
                // Fly directly toward target at a slight height offset
                Vec3 flyTarget = targetPos + Vec3{0, 0.5f, 0};
                Vec3 toFly = flyTarget - e.position;
                f32 flyDist = length(toFly);
                if (flyDist > 0.01f) {
                    Vec3 flyDir = toFly * (1.0f / flyDist);
                    e.velocity = flyDir * e.moveSpeed;
                }
            } else {
                // Ground movement: XZ only toward target
                Vec3 flatDir = normalize(Vec3{dirToTarget.x, 0.0f, dirToTarget.z});
                if (lengthSq(flatDir) > 0.001f) {
                    e.velocity.x = flatDir.x * e.moveSpeed;
                    e.velocity.z = flatDir.z * e.moveSpeed;
                }
                snapEntityToFloor(e, grid);
            }

            entityMoveAndSlide(e, grid, dt);

            // Transition to attack if close enough to target
            if (targetDist <= e.attackRange) {
                if (isBat) {
                    // Bats use ATTACK state directly — no complex FLYBY
                    e.aiState = AIState::ATTACK;
                    e.attackTimer = e.attackCooldown * 0.5f;
                } else {
                    e.aiState     = AIState::ATTACK;
                    e.attackTimer = e.attackCooldown * 0.5f;
                }
            }
            // Lost interest: fall back to idle if target is far away
            if (targetDist > e.detectionRange * 1.5f) {
                e.aiState = AIState::IDLE;
            }
        } break;

        case AIState::FLYBY: {
            // Bat swoop attack — fly toward flyby waypoint, damage on contact with player
            Vec3 toFlybyTarget = e.flybyTarget - e.position;
            f32 flybyTargetDist = length(toFlybyTarget);

            if (flybyTargetDist > 0.3f) {
                Vec3 flyDir = toFlybyTarget * (1.0f / flybyTargetDist);
                f32 speed = e.moveSpeed * 2.2f; // aggressive fast dive
                e.velocity = flyDir * speed;

                // Fly straight during swoop — no wobble
            }

            // Face movement direction
            if (lengthSq(e.velocity) > 0.01f) {
                e.yaw = atan2f(-e.velocity.x, -e.velocity.z);
            }

            entityMoveAndSlide(e, grid, dt);

            // Deal damage when passing close to target
            if (dist <= e.attackRange * 0.8f) {
                if (targetIsNPC && e.targetEntityIdx < MAX_ENTITIES) {
                    // Damage the NPC target during flyby
                    Entity& npcTarget = pool.entities[e.targetEntityIdx];
                    if (!(npcTarget.flags & ENT_DEAD)) {
                        EntityHandle th = {e.targetEntityIdx, npcTarget.generation};
                        Combat::applyDamage(pool, th, e.damage);
                        e.attackAnimT = 0.3f;
                    }
                } else if (hasLOS(e, player, grid)) {
                    // Damage the player if no NPC target
                    Combat::applyDamageToPlayer(player, e.damage);
                    e.attackAnimT = 0.3f;
                }

                if (e.attackAnimT > 0.0f) {
                    // After hitting: pull up and circle back
                    Vec3 retreatDir = e.velocity * (-1.0f);
                    if (lengthSq(retreatDir) > 0.01f) retreatDir = normalize(retreatDir);
                    else retreatDir = {0, 1, 0};
                    e.flybyTarget = e.position + retreatDir * 3.0f + Vec3{0, 2.5f, 0};
                    e.flybyTimer = 1.0f; // quick retreat, return to attack faster
                    // Stay in FLYBY to fly to retreat point, then expire to CHASE
                }
            }

            e.flybyTimer -= dt;
            if (flybyTargetDist < 0.3f || e.flybyTimer <= 0.0f) {
                e.aiState = AIState::CHASE;
            }
        } break;

        case AIState::ATTACK: {
            // Stand and attack (ground enemies stop; bats hover in place)
            e.velocity = {0, 0, 0};

            e.attackTimer -= dt;
            if (e.attackTimer <= 0.0f) {
                e.attackTimer = e.attackCooldown;
                e.attackAnimT = 0.3f; // trigger attack animation

                // Damage NPC target if we're targeting one, otherwise damage player
                if (targetDist <= e.attackRange * 1.1f) {
                    if (targetIsNPC && e.targetEntityIdx < MAX_ENTITIES) {
                        Entity& npcTarget = pool.entities[e.targetEntityIdx];
                        if (!(npcTarget.flags & ENT_DEAD)) {
                            EntityHandle th = {e.targetEntityIdx, npcTarget.generation};
                            Combat::applyDamage(pool, th, e.damage);
                        }
                    } else if (hasLOS(e, player, grid)) {
                        Combat::applyDamageToPlayer(player, e.damage);
                    }
                }
            }

            // Transition back to chase if out of range
            if (targetDist > e.attackRange * 1.3f) {
                e.aiState = AIState::CHASE;
            }
        } break;

        case AIState::DORMANT: {
            // Mimic: sits still disguised as a chest until player gets close
            e.velocity = {0, 0, 0};
            if (dist <= GameConst::MIMIC_TRIGGER_DIST) {
                // Player is trying to loot — spring to life!
                e.aiState = AIState::CHASE;
                e.attackAnimT = 0.4f; // surprise attack animation
                e.speechText = "*CHOMP*";
                e.speechTimer = 2.0f;
            }
        } break;

        case AIState::DEAD:
            break;
        }
    }
}
