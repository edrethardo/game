#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "game/weapon.h"
#include "world/level_grid.h"

struct Player;
struct ProjectilePool;
struct ParticlePool;
struct ScreenShake;

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
    // Optional damageOrigin enables directional checks (e.g. shield bearer frontal reduction).
    void applyDamage(EntityPool& pool, EntityHandle target, f32 damage,
                     const Vec3* damageOrigin = nullptr, bool isCrit = false);

    // Transition an entity to its death state and fire the death callback (loot
    // drop, squad alert, death procs). Use this for damage that bypasses
    // applyDamage — environmental/DoT sources (poison, burn, scorch) — so those
    // kills still drop loot. No-op if the entity is already dead. Does NOT spawn a
    // damage number (DoTs tick every frame and would otherwise spam).
    void killEntity(EntityPool& pool, EntityHandle target);

    // Apply damage to the player. Optional attackerPos enables directional indicator.
    // attackerIdx: entity pool index of the attacker (0xFFFF if unknown/environmental).
    // Used by dodge-through detection to fire riposte counter-hits.
    void applyDamageToPlayer(Player& player, f32 damage, const Vec3* attackerPos = nullptr,
                             u16 attackerIdx = 0xFFFF);

    // Dodge-through callback: called when damage is blocked during a dodge roll
    using DodgeThroughCallback = void(*)(u16 attackerIdx, Vec3 attackerPos);
    void setDodgeThroughCallback(DodgeThroughCallback cb);

    // Execute a melee attack (cone check, damage all in cone).
    // Crit is rolled internally from weapon.critChance — see combat.cpp.
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

    // Damage number callback — auto-fires on every applyDamage call.
    // isCrit/isKill are threaded through so the renderer can style those hits distinctly.
    using DamageNumberCallback = void(*)(Vec3 position, f32 amount, bool isCrit, bool isKill);
    void setDamageNumberCallback(DamageNumberCallback cb);
    // Manually spawn a floating damage number (for skills that bypass applyDamage).
    // Crits are not applicable at this call site, so isCrit/isKill are always false.
    void spawnDamageNumber(Vec3 position, f32 amount);

    // Death callback — called when an entity dies, before pool cleanup
    using DeathCallback = void(*)(EntityPool& pool, u16 entityIndex, Vec3 position);
    void setDeathCallback(DeathCallback cb);

    // (L8) Player slot currently credited as the attacker. The engine sets it around each
    // player's weapon fire (and projectile.cpp restores each projectile's ownerSlot around
    // its damage); killEntity stamps it onto Entity::killerSlot so loot can be reserved to
    // the killer. 0xFF = none/environmental (free-for-all drop).
    void setAttackingPlayer(u8 slot);
    u8   getAttackingPlayer();

    // Perfect block callback — called when player executes a perfect block
    using PerfectBlockCallback = void(*)(Player& player);
    void setPerfectBlockCallback(PerfectBlockCallback cb);

    // Wire in the particle pool and screen shake so combat events emit visual FX.
    // Both pointers are stored as file-scope statics in combat.cpp.
    void setFXTargets(ParticlePool* particles, ScreenShake* shake);
}
