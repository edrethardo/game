#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "game/weapon.h"
#include "world/level_grid.h"

struct Player;
struct ProjectilePool;

// Result of a player attack. Melee can hit multiple entities (cone query);
// hitscan hits first entity or wall (raycast); projectile spawns are fire-and-forget.
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

    // Apply damage to the player. Optional attackerPos enables directional indicator.
    void applyDamageToPlayer(Player& player, f32 damage, const Vec3* attackerPos = nullptr);

    // Execute a melee attack (cone check, damage all in cone).
    AttackResult fireMelee(const WeaponDef& weapon,
                           Vec3 eyePos, Vec3 forward,
                           EntityPool& pool);

    // Execute a hitscan attack (raycast, damage first entity hit).
    AttackResult fireHitscan(const WeaponDef& weapon,
                             Vec3 eyePos, Vec3 forward,
                             const LevelGrid& grid,
                             EntityPool& pool);

    // Spawn a projectile. Returns pool slot index (0xFFFF if full).
    u16 fireProjectile(const WeaponDef& weapon,
                       Vec3 eyePos, Vec3 forward,
                       ProjectilePool& projectiles,
                       u8 extraFlags = 0);

    // Spawn a projectile with gravity and/or splash behavior (for molotov etc.)
    u16 fireProjectile(const WeaponDef& weapon,
                       Vec3 eyePos, Vec3 forward,
                       ProjectilePool& projectiles,
                       f32 gravity, f32 splashRadius, f32 splashDamage);

    // Death callback — called when an entity dies, before pool cleanup
    using DeathCallback = void(*)(EntityPool& pool, u16 entityIndex, Vec3 position);
    void setDeathCallback(DeathCallback cb);

    // Perfect block callback — called when player executes a perfect block
    using PerfectBlockCallback = void(*)(Player& player);
    void setPerfectBlockCallback(PerfectBlockCallback cb);
}
