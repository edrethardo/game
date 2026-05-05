#include "game/entity.h"
#include "game/game_constants.h"
#include "core/log.h"

void EntitySystem::init(EntityPool& pool) {
    pool.freeCount = MAX_ENTITIES;
    pool.activeCount = 0;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        pool.entities[i] = {};
        pool.freeList[i] = static_cast<u16>(MAX_ENTITIES - 1 - i);
    }
    LOG_INFO("EntityPool initialized (%u slots)", MAX_ENTITIES);
}

EntityHandle EntitySystem::spawn(EntityPool& pool, Vec3 position, Vec3 halfExtents,
                                  bool flying, f32 health, f32 moveSpeed,
                                  f32 detectionRange, f32 attackRange,
                                  f32 attackCooldown, f32 damage)
{
    if (pool.freeCount == 0) {
        LOG_WARN("EntityPool: no free slots");
        return {};
    }

    u16 idx = pool.freeList[--pool.freeCount];
    Entity& e = pool.entities[idx];
    e.generation++;
    e.flags        = ENT_ACTIVE | (flying ? ENT_FLYING : 0);
    e.position     = position;
    e.velocity     = {0,0,0};
    e.yaw          = 0.0f;
    e.halfExtents  = halfExtents;
    e.health       = health;
    e.maxHealth    = health;
    e.moveSpeed    = moveSpeed * GameConst::SPEED_MULT;
    e.detectionRange = detectionRange;
    e.attackRange  = attackRange;
    e.attackCooldown = attackCooldown;
    e.attackTimer  = 0.0f;
    e.damage       = damage;
    e.aiState      = AIState::IDLE;
    e.flashTimer   = 0.0f;
    e.deathTimer   = 0.0f;

    // Add to active list
    pool.activeList[pool.activeCount++] = idx;

    return {idx, e.generation};
}

static void removeFromActiveList(EntityPool& pool, u16 idx) {
    for (u32 i = 0; i < pool.activeCount; i++) {
        if (pool.activeList[i] == idx) {
            // Swap with last
            pool.activeList[i] = pool.activeList[pool.activeCount - 1];
            pool.activeCount--;
            return;
        }
    }
}

void EntitySystem::despawn(EntityPool& pool, EntityHandle handle) {
    if (!handleValid(pool, handle)) return;
    Entity& e = pool.entities[handle.index];
    e.flags = 0;
    removeFromActiveList(pool, handle.index);
    pool.freeList[pool.freeCount++] = handle.index;
}

void EntitySystem::tickTimers(EntityPool& pool, f32 dt) {
    for (u32 a = 0; a < pool.activeCount; ) {
        u32 i = pool.activeList[a];
        Entity& e = pool.entities[i];

        if (e.flashTimer > 0.0f) e.flashTimer -= dt;

        // Tick status effects (poison/burn deal DoT, freeze checked at move time)
        if (e.poisonTimer > 0.0f) {
            e.poisonTimer -= dt;
            e.health -= e.poisonDps * dt;
            if (e.health <= 0.0f && !(e.flags & ENT_DEAD)) {
                e.health = 0.0f;
                e.flags |= ENT_DEAD;
                e.aiState = AIState::DEAD;
                e.deathTimer = 1.0f;
                e.velocity = {0,0,0};
            }
        }
        if (e.burnTimer > 0.0f) {
            e.burnTimer -= dt;
            e.health -= e.burnDps * dt;
            if (e.health <= 0.0f && !(e.flags & ENT_DEAD)) {
                e.health = 0.0f;
                e.flags |= ENT_DEAD;
                e.aiState = AIState::DEAD;
                e.deathTimer = 1.0f;
                e.velocity = {0,0,0};
            }
        }
        if (e.freezeTimer > 0.0f) e.freezeTimer -= dt;

        if (e.flags & ENT_DEAD) {
            e.deathTimer -= dt;
            if (e.deathTimer <= 0.0f) {
                e.flags = 0;
                pool.freeList[pool.freeCount++] = static_cast<u16>(i);
                // Swap-remove from active list
                pool.activeList[a] = pool.activeList[pool.activeCount - 1];
                pool.activeCount--;
                continue; // re-check same index
            }
        }
        a++;
    }
}

u32 EntitySystem::activeCount(const EntityPool& pool) {
    return pool.activeCount;
}
