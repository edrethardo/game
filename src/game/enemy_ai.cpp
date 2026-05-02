#include "game/enemy_ai.h"
#include "game/player.h"
#include "game/combat.h"
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

        Vec3 toPlayer = playerEye - e.position;
        f32  dist     = length(toPlayer);
        Vec3 dirToPlayer = (dist > 0.001f) ? toPlayer * (1.0f / dist) : Vec3{0,0,0};

        bool isBat = (e.flags & ENT_FLYING) != 0;

        // Face toward player (yaw only) — except during flyby
        if (dist > 0.001f && e.aiState != AIState::FLYBY) {
            e.yaw = atan2f(-dirToPlayer.x, -dirToPlayer.z);
        }

        switch (e.aiState) {

        case AIState::IDLE: {
            e.velocity = {0, 0, 0};

            // Bats hover erratically even when idle
            if (isBat) {
                // More erratic idle hovering
                e.position.y += sinf(e.animTimer * 4.0f) * 0.4f * dt;
                e.position.x += sinf(e.animTimer * 2.3f) * 0.2f * dt;
                e.position.z += cosf(e.animTimer * 1.9f) * 0.15f * dt;
            }

            // Staggered LOS check — bats check more frequently (every 4 frames)
            e.aiCheckIdx++;
            u16 checkFreq = isBat ? 4 : 8;
            if (e.aiCheckIdx >= checkFreq) {
                e.aiCheckIdx = 0;
                if (dist <= e.detectionRange && hasLOS(e, player, grid)) {
                    e.aiState = AIState::CHASE;
                }
            }
        } break;

        case AIState::CHASE: {
            if (isBat) {
                // Aggressive flying chase — target position above the player
                Vec3 abovePlayer = playerEye + Vec3{0, 1.8f, 0}; // hover well above head
                Vec3 toAbove = abovePlayer - e.position;
                f32 aboveDist = length(toAbove);
                Vec3 dirAbove = (aboveDist > 0.01f) ? toAbove * (1.0f / aboveDist) : Vec3{0,1,0};

                // Faster erratic weave
                f32 wobbleX = sinf(e.animTimer * 7.0f) * e.moveSpeed * 0.5f;
                f32 wobbleY = sinf(e.animTimer * 4.0f) * e.moveSpeed * 0.2f;

                e.velocity = dirAbove * e.moveSpeed;
                Vec3 perp = {-dirAbove.z, 0.0f, dirAbove.x};
                e.velocity.x += perp.x * wobbleX;
                e.velocity.z += perp.z * wobbleX;
                e.velocity.y += wobbleY;
            } else {
                // Ground movement: XZ only
                Vec3 flatDir = normalize(Vec3{dirToPlayer.x, 0.0f, dirToPlayer.z});
                if (lengthSq(flatDir) > 0.001f) {
                    e.velocity.x = flatDir.x * e.moveSpeed;
                    e.velocity.z = flatDir.z * e.moveSpeed;
                }
                // Snap Y to floor
                u32 gx, gz;
                if (LevelGridSystem::worldToGrid(grid, e.position, gx, gz) &&
                    !LevelGridSystem::isSolid(grid, gx, gz)) {
                    f32 floorH = LevelGridSystem::getFloorHeight(grid, gx, gz);
                    e.position.y = floorH + e.halfExtents.y;
                }
            }

            entityMoveAndSlide(e, grid, dt);

            // Transition to attack if close enough
            if (dist <= e.attackRange) {
                if (isBat) {
                    // Aggressive bat attacks — more dive attacks, faster timers
                    if ((std::rand() % 100) < 40) {
                        // FLYBY: fast swoop past player to behind them
                        Vec3 playerFwd = {-sinf(player.yaw), 0.0f, -cosf(player.yaw)};
                        e.flybyTarget = playerEye + playerFwd * 5.0f + Vec3{0, 2.0f, 0};
                        e.flybyTimer = 1.8f;
                        e.aiState = AIState::FLYBY;
                    } else {
                        // Direct dive attack: steep dive from above
                        e.flybyTarget = playerEye + Vec3{0, -0.5f, 0};
                        e.flybyTimer = 1.0f;
                        e.aiState = AIState::FLYBY;
                    }
                    e.attackAnimT = 0.4f; // longer swipe animation
                } else {
                    e.aiState     = AIState::ATTACK;
                    e.attackTimer = e.attackCooldown * 0.5f;
                }
            }
            // Lost interest
            if (dist > e.detectionRange * 1.5f) {
                e.aiState = AIState::IDLE;
            }
        } break;

        case AIState::FLYBY: {
            // Bat swoop attack — fly toward target, damage on contact
            Vec3 toTarget = e.flybyTarget - e.position;
            f32 targetDist = length(toTarget);

            if (targetDist > 0.3f) {
                Vec3 flyDir = toTarget * (1.0f / targetDist);
                f32 speed = e.moveSpeed * 2.2f; // aggressive fast dive
                e.velocity = flyDir * speed;

                // Wobble during swoop
                e.velocity.x += sinf(e.flybyTimer * 10.0f) * e.moveSpeed * 0.2f;
                e.velocity.y += sinf(e.flybyTimer * 7.0f) * 0.3f;
            }

            // Face movement direction
            if (lengthSq(e.velocity) > 0.01f) {
                e.yaw = atan2f(-e.velocity.x, -e.velocity.z);
            }

            entityMoveAndSlide(e, grid, dt);

            // Deal damage when passing close to player
            if (dist <= e.attackRange * 0.8f && hasLOS(e, player, grid)) {
                Combat::applyDamageToPlayer(player, e.damage);
                e.attackAnimT = 0.3f;

                // After hitting: pull up and circle back
                // Pick a retreat point above and behind the bat's current direction
                Vec3 retreatDir = e.velocity * (-1.0f);
                if (lengthSq(retreatDir) > 0.01f) retreatDir = normalize(retreatDir);
                else retreatDir = {0, 1, 0};
                e.flybyTarget = e.position + retreatDir * 3.0f + Vec3{0, 2.5f, 0};
                e.flybyTimer = 1.0f; // quick retreat, return to attack faster
                // Stay in FLYBY to fly to retreat point, then will expire to CHASE
            }

            e.flybyTimer -= dt;
            if (targetDist < 0.3f || e.flybyTimer <= 0.0f) {
                e.aiState = AIState::CHASE;
            }
        } break;

        case AIState::ATTACK: {
            // Ground enemies only — bats never enter this state
            e.velocity = {0, 0, 0};

            e.attackTimer -= dt;
            if (e.attackTimer <= 0.0f) {
                e.attackTimer = e.attackCooldown;
                e.attackAnimT = 0.3f; // trigger attack animation

                if (dist <= e.attackRange * 1.1f && hasLOS(e, player, grid)) {
                    Combat::applyDamageToPlayer(player, e.damage);
                }
            }

            // Transition back to chase if out of range
            if (dist > e.attackRange * 1.3f) {
                e.aiState = AIState::CHASE;
            }
        } break;

        case AIState::DEAD:
            break;
        }
    }
}
