#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "game/weapon.h"
#include "world/level_grid.h"

struct Player;
struct ProjectilePool;

// Combat hit result from last player attack (for feedback)
struct AttackResult {
    bool  didFire     = false;
    bool  hitEntity   = false;
    bool  hitWorld    = false;
    Vec3  hitPosition = {0,0,0};
    Vec3  hitNormal   = {0,0,0};
    f32   hitDistance  = 0.0f;
    u32   entitiesHit = 0; // melee can hit multiple
};

namespace Combat {
    // Apply damage to an entity. Handles health, flash, death transition.
    void applyDamage(EntityPool& pool, EntityHandle target, f32 damage);

    // Apply damage to the player.
    void applyDamageToPlayer(Player& player, f32 damage);

    // Execute a melee attack (cone check, damage all in cone).
    AttackResult fireMelee(const WeaponDef& weapon,
                           Vec3 eyePos, Vec3 forward,
                           EntityPool& pool);

    // Execute a hitscan attack (raycast, damage first entity hit).
    AttackResult fireHitscan(const WeaponDef& weapon,
                             Vec3 eyePos, Vec3 forward,
                             const LevelGrid& grid,
                             EntityPool& pool);

    // Spawn a projectile (returns true if spawned).
    bool fireProjectile(const WeaponDef& weapon,
                        Vec3 eyePos, Vec3 forward,
                        ProjectilePool& projectiles);
}
