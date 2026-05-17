#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "world/level_grid.h"

struct SpatialGrid;  // forward decl for spatial proximity queries

// Projectile behavior flags (stored in projFlags bitmask)
static constexpr u8 PROJ_ORB       = 1 << 0;  // Frozen Orb skill projectile
static constexpr u8 PROJ_ORB_SHARD = 1 << 1;  // Frozen Orb sub-shard
static constexpr u8 PROJ_GRAVITY   = 1 << 2;  // Affected by gravity (arcing trajectory)
static constexpr u8 PROJ_SPLASH    = 1 << 3;  // AoE splash damage on impact
static constexpr u8 PROJ_SPARK     = 1 << 4;  // Lightning bolt visual (jagged line)
static constexpr u8 PROJ_VOID      = 1 << 5;  // Void weapon projectile (purple tint)

static constexpr u32 MAX_PROJECTILES = 4096;

// Active projectile instance. Moves each frame, collides with walls and entities.
// projFlags bits: 0=isOrb (Frozen Orb skill), 1=isOrbShard (sub-projectile)
struct Projectile {
    Vec3 position   = {0,0,0};
    Vec3 velocity   = {0,0,0};
    f32  radius     = 0.15f;
    f32  damage     = 30.0f;
    f32  lifetime   = 3.0f;
    bool active     = false;
    bool fromPlayer = true; // false = enemy projectile
    u8   projFlags  = 0;       // bit 0: isOrb, bit 1: isOrbShard
    f32  subTimer   = 0.0f;    // orb shard spawn interval timer
    f32  orbAngle   = 0.0f;    // current rotation angle for shard spawning
    f32  gravity      = 0.0f;   // downward acceleration in units/s^2 (0 = straight line)
    f32  splashRadius = 0.0f;   // AoE radius on impact (0 = single target)
    f32  splashDamage = 0.0f;   // damage dealt in splash zone
    f32  freezeDuration = 0.0f; // freeze target on hit for this many seconds (0 = no freeze)
    Vec3 lightColor   = {0,0,0}; // dynamic point light color (zero = no light emitted)
    u8   meshId       = 0;     // weapon mesh to render (0 = default cube)

    // On-hit status effect (enemy projectiles apply to player on hit)
    // 0=none, 1=poison, 2=slow, 3=burn, 4=freeze
    u8   onHitEffect    = 0;
    f32  onHitDuration  = 0.0f;
};

struct ProjectilePool {
    Projectile projectiles[MAX_PROJECTILES];
    u32 activeCount = 0;
};

struct Player;

namespace ProjectileSystem {
    // Callback when a splash projectile explodes (for visual effects)
    using SplashCallback = void(*)(Vec3 position, f32 radius);
    void setSplashCallback(SplashCallback cb);

    // Callback when a player projectile hits an entity (for weapon on-hit procs)
    using HitCallback = void(*)(Vec3 position, EntityHandle target);
    void setHitCallback(HitCallback cb);

    // Callback for floating damage numbers when projectile deals damage
    using DamageNumberCallback = void(*)(Vec3 position, f32 damage);
    void setDamageNumberCallback(DamageNumberCallback cb);

    void init(ProjectilePool& pool);

    // Spawn a projectile. Returns the pool slot index (0xFFFF if pool full).
    u16 spawn(ProjectilePool& pool,
              Vec3 origin, Vec3 direction, f32 speed,
              f32 damage, f32 radius, f32 lifetime,
              bool fromPlayer, u8 extraFlags = 0);

    // Update all projectiles: move, collide with grid and entities/player.
    void update(ProjectilePool& pool,
                const LevelGrid& grid,
                EntityPool& entities,
                Player& player,
                f32 dt,
                const SpatialGrid* spatialGrid = nullptr);
}
