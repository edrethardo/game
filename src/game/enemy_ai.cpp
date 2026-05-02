#include "game/enemy_ai.h"
#include "game/player.h"
#include "game/combat.h"
#include "world/raycast.h"
#include "world/level_grid.h"
#include <cmath>

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
    Vec3 playerEye = player.position + Vec3{0, player.eyeHeight, 0};

    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = pool.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;
        if (e.flags & ENT_DEAD) continue;

        Vec3 toPlayer = playerEye - e.position;
        f32  dist     = length(toPlayer);
        Vec3 dirToPlayer = (dist > 0.001f) ? toPlayer * (1.0f / dist) : Vec3{0,0,0};

        // Face toward player (yaw only)
        if (dist > 0.001f) {
            e.yaw = atan2f(-dirToPlayer.x, -dirToPlayer.z);
        }

        switch (e.aiState) {

        case AIState::IDLE: {
            e.velocity = {0, 0, 0};

            // Staggered LOS check: only check every 8 frames
            e.aiCheckIdx++;
            if (e.aiCheckIdx >= 8) {
                e.aiCheckIdx = 0;
                if (dist <= e.detectionRange && hasLOS(e, player, grid)) {
                    e.aiState = AIState::CHASE;
                }
            }
        } break;

        case AIState::CHASE: {
            // Move toward player
            if (e.flags & ENT_FLYING) {
                // Full 3D chase
                e.velocity = dirToPlayer * e.moveSpeed;
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
                e.aiState     = AIState::ATTACK;
                e.attackTimer = e.attackCooldown * 0.5f; // first attack faster
            }
            // Lost interest
            if (dist > e.detectionRange * 1.5f) {
                e.aiState = AIState::IDLE;
            }
        } break;

        case AIState::ATTACK: {
            e.velocity = {0, 0, 0};

            // Flying enemies bob gently
            if (e.flags & ENT_FLYING) {
                e.position.y += sinf(e.attackTimer * 4.0f) * 0.3f * dt;
            }

            e.attackTimer -= dt;
            if (e.attackTimer <= 0.0f) {
                e.attackTimer = e.attackCooldown;

                // Deal damage to player if in range and LOS
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
            // Handled by EntitySystem::tickTimers
            break;
        }
    }
}
