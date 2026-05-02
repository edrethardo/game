#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "world/level_grid.h"

static constexpr u32 MAX_PROJECTILES = 128;

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
};

struct ProjectilePool {
    Projectile projectiles[MAX_PROJECTILES];
    u32 activeCount = 0;
};

struct Player;

namespace ProjectileSystem {
    void init(ProjectilePool& pool);

    void spawn(ProjectilePool& pool,
               Vec3 origin, Vec3 direction, f32 speed,
               f32 damage, f32 radius, f32 lifetime,
               bool fromPlayer);

    // Update all projectiles: move, collide with grid and entities/player.
    void update(ProjectilePool& pool,
                const LevelGrid& grid,
                EntityPool& entities,
                Player& player,
                f32 dt);
}
