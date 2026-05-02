#include "game/entity.h"
#include "core/log.h"

void EntitySystem::init(EntityPool& pool) {
    pool.freeCount = MAX_ENTITIES;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        pool.entities[i] = {};
        pool.freeList[i] = static_cast<u16>(MAX_ENTITIES - 1 - i); // stack: pop gives 0 first
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
    e.moveSpeed    = moveSpeed;
    e.detectionRange = detectionRange;
    e.attackRange  = attackRange;
    e.attackCooldown = attackCooldown;
    e.attackTimer  = 0.0f;
    e.damage       = damage;
    e.aiState      = AIState::IDLE;
    e.flashTimer   = 0.0f;
    e.deathTimer   = 0.0f;

    return {idx, e.generation};
}

void EntitySystem::despawn(EntityPool& pool, EntityHandle handle) {
    if (!handleValid(pool, handle)) return;
    Entity& e = pool.entities[handle.index];
    e.flags = 0;
    pool.freeList[pool.freeCount++] = handle.index;
}

void EntitySystem::tickTimers(EntityPool& pool, f32 dt) {
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = pool.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;

        if (e.flashTimer > 0.0f) e.flashTimer -= dt;

        if (e.flags & ENT_DEAD) {
            e.deathTimer -= dt;
            if (e.deathTimer <= 0.0f) {
                // Despawn
                e.flags = 0;
                pool.freeList[pool.freeCount++] = static_cast<u16>(i);
            }
        }
    }
}

u32 EntitySystem::activeCount(const EntityPool& pool) {
    u32 count = 0;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        if (pool.entities[i].flags & ENT_ACTIVE) count++;
    }
    return count;
}
