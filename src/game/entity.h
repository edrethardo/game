#pragma once

#include "core/types.h"
#include "core/math.h"
#include "renderer/frustum.h"

static constexpr u32 MAX_ENTITIES = 128;

enum EntityFlags : u8 {
    ENT_ACTIVE  = 1 << 0,
    ENT_FLYING  = 1 << 1,
    ENT_DEAD    = 1 << 2,
};

enum struct AIState : u8 {
    IDLE,
    CHASE,
    ATTACK,
    FLYBY,   // bat swoops past player to attack from behind
    DEAD,
};

struct Entity {
    // Identity
    u16 generation = 0;
    u8  flags      = 0;

    // Transform
    Vec3 position  = {0,0,0};   // centre of AABB
    Vec3 velocity  = {0,0,0};
    f32  yaw       = 0.0f;

    // Collision
    Vec3 halfExtents = {0.4f, 0.5f, 0.4f};

    // Combat
    f32  health         = 50.0f;
    f32  maxHealth      = 50.0f;
    f32  attackRange    = 2.0f;
    f32  attackCooldown = 1.0f;
    f32  attackTimer    = 0.0f;
    f32  damage         = 10.0f;
    f32  moveSpeed      = 3.0f;
    f32  detectionRange = 15.0f;

    // AI
    AIState aiState    = AIState::IDLE;
    u16     aiCheckIdx = 0;  // staggered LOS frame counter
    Vec3    flybyTarget = {0,0,0};  // waypoint for FLYBY state
    f32     flybyTimer  = 0.0f;     // time left in flyby maneuver

    // Rendering
    u8  meshId     = 0;  // index into Engine::m_meshDefs
    u8  materialId = 0;  // index into MaterialSystem

    // Animation
    f32  animTimer    = 0.0f;  // continuous timer for procedural animation
    f32  attackAnimT  = 0.0f;  // brief attack animation countdown

    // Feedback
    f32  flashTimer = 0.0f;
    f32  deathTimer = 0.0f;
};

struct EntityHandle {
    u16 index      = 0xFFFF;
    u16 generation = 0;
};

struct EntityPool {
    Entity entities[MAX_ENTITIES];
    u16    freeList[MAX_ENTITIES];
    u32    freeCount = 0;

    // Active entity indices for fast iteration
    u32    activeList[MAX_ENTITIES];
    u32    activeCount = 0;
};

inline bool handleValid(const EntityPool& pool, EntityHandle h) {
    return h.index < MAX_ENTITIES &&
           (pool.entities[h.index].flags & ENT_ACTIVE) &&
           pool.entities[h.index].generation == h.generation;
}

inline Entity* handleGet(EntityPool& pool, EntityHandle h) {
    if (!handleValid(pool, h)) return nullptr;
    return &pool.entities[h.index];
}

inline AABB entityAABB(const Entity& e) {
    return { e.position - e.halfExtents, e.position + e.halfExtents };
}

namespace EntitySystem {
    void init(EntityPool& pool);

    EntityHandle spawn(EntityPool& pool, Vec3 position, Vec3 halfExtents,
                       bool flying, f32 health, f32 moveSpeed,
                       f32 detectionRange, f32 attackRange,
                       f32 attackCooldown, f32 damage);

    void despawn(EntityPool& pool, EntityHandle handle);

    // Tick timers (flash, death). Called from engine update.
    void tickTimers(EntityPool& pool, f32 dt);

    // Count currently active (including dying)
    u32 activeCount(const EntityPool& pool);
}
