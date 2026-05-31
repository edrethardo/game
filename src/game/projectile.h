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

// Switch: smaller pool for better cache utilization on Tegra X1 (A57 at 1 GHz)
#ifdef __SWITCH__
static constexpr u32 MAX_PROJECTILES = 512;
#else
static constexpr u32 MAX_PROJECTILES = 1024;
#endif

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
    bool isCrit     = false; // player rolled a crit at spawn — applied on direct hit, NOT splash
    u8   ownerSlot  = 0xFF;  // (L8) firing player's slot (0xFF = none); restored into
                             // Combat::s_attackingPlayer around this projectile's damage so a
                             // kill credits the firer even though it resolves frames later
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

    // DEPRECATED (M10/M11, rewrite design doc): the "predicted ghost" fire
    // model where the client spawns a local-only Projectile with predicted=true
    // and matches it to an authoritative server projectile via clientTickLow.
    // The rewrite unifies this: the server spawns the projectile at the lag-comp
    // tick (so it's born where the client launched it), and the client's
    // predicted projectile IS the canonical one until reconciliation arrives.
    // `predicted` and `clientTick` will be replaced by a single source-of-truth
    // projectile flagged with its origin (server-authoritative vs unconfirmed).
    // Do not add new readers of `predicted` outside the existing match-despawn
    // path.
    //
    // Client-side prediction (V2 fire prediction):
    //   predicted=true marks a local ghost spawned by the CLIENT's own handleWeaponFire so
    //     the user sees the projectile leave the wand at click-time instead of waiting the
    //     ~50-100 ms for the authoritative one to arrive via snapshot. The renderer merges
    //     predicted ghosts into m_renderInterp.projectiles each frame (clientNetPost) so they
    //     draw alongside snapshot projectiles, and the matching snapshot projectile despawns
    //     the ghost on arrival (matched by ownerSlot + clientTick low 16 bits).
    //   clientTick is the client's m_clientTick at spawn time (M1.8: was m_serverTick) —
    //     shipped to the server in CL_FIRE_WEAPON, the server stores it on the authoritative
    //     projectile, the snapshot carries its low 16 bits (SnapProjectile.clientTickLow),
    //     and the client uses it to find which local predicted matches. 0 means "no prediction"
    //     (server-spawned by host's own fire, NPC projectile, etc.) — match is skipped.
    bool predicted     = false;
    u32  clientTick    = 0;
    f32  predictedLife = 0.0f;  // seconds since spawn; predicted ghosts despawn at 0.5 s if no match arrived (UDP loss fallback)
};

struct ProjectilePool {
    Projectile projectiles[MAX_PROJECTILES];
    u16 activeList[MAX_PROJECTILES]; // indices of active projectiles (dense)
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
    // extraPlayers/extraPlayerCount: additional local players (split-screen) that enemy
    // projectiles must also collide with — full status + Wanderer Deflect, same as the
    // primary. Mirrors EnemyAI::update's extra-players convention.
    void update(ProjectilePool& pool,
                const LevelGrid& grid,
                EntityPool& entities,
                Player& player,
                f32 dt,
                const SpatialGrid* spatialGrid = nullptr,
                Player** extraPlayers = nullptr,
                u32 extraPlayerCount = 0);
}
