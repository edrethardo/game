#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/item.h"
#include "game/projectile.h"
#include "game/entity.h"
#include "world/level_grid.h"

// Pending meteor (delayed AoE)
static constexpr u32 MAX_PENDING_METEORS = 4;
struct PendingMeteor {
    Vec3 position = {0,0,0};
    f32  damage   = 0.0f;
    f32  radius   = 0.0f;
    f32  timer    = 0.0f;
    bool active   = false;
};

struct Player;     // forward decl
struct ParticlePool;
struct ScreenShake;

namespace SkillSystem {
    void init();

    // Set skill power scaling (0.0 = base, 1.0 = max). Called by engine before
    // tryActivate — 0.0 for class skills, scaled by item level for legendary skills.
    void setSkillPower(f32 power);

    // Set class skill damage multiplier (scales with effective floor).
    // Called by engine before class skill activation. Item skills use 1.0.
    void setClassDamageMult(f32 mult);

    // Tick cooldowns, energy regen, pending meteors
    void update(SkillState& ss, f32 dt);

    // Try to activate the player's current skill (returns true if activated)
    bool tryActivate(SkillState& ss, const SkillDef* skillDefs, u32 skillDefCount,
                     Vec3 eyePos, Vec3 forward, f32 yaw,
                     ProjectilePool& projectiles, EntityPool& entities,
                     const LevelGrid& grid, Player& player,
                     f32 cooldownReduction = 0.0f);

    // Update orb projectiles (spawn shards) -- called from projectile update or engine update
    void updateOrbProjectiles(ProjectilePool& pool, const SkillDef* skillDefs, u32 skillDefCount, f32 dt);

    // Update pending meteors
    void updateMeteors(EntityPool& entities, f32 dt);

    // Get the SkillDef for a given SkillId (returns nullptr if not found)
    const SkillDef* findSkillDef(const SkillDef* defs, u32 count, SkillId id);

    // Visual FX callbacks — set by Engine to trigger skill effects
    using NovaCallback = void(*)(Vec3 position, f32 radius, Vec3 color);
    using DashCallback = void(*)(Vec3 start, Vec3 end);
    using ScorchCallback = void(*)(Vec3 position, f32 radius, f32 duration, f32 dps);
    // Drone spawn callback — engine handles entity creation with proper mesh/material
    // type: 0=combat drone (spider), 1=swarm drone (bat), 2=turret
    using DroneSpawnCallback = void(*)(Vec3 position, u8 type);
    // Chain lightning visual — receives array of bounce positions
    using ChainCallback = void(*)(const Vec3* points, u8 count);
    void setNovaCallback(NovaCallback cb);
    void setDashCallback(DashCallback cb);
    void setScorchCallback(ScorchCallback cb);
    void setDroneSpawnCallback(DroneSpawnCallback cb);
    void setChainCallback(ChainCallback cb);
    void setBoltMeshId(u8 meshId, u8 matId);

    // Wire in the particle pool and screen shake for skill activation FX.
    void setFXTargets(ParticlePool* particles, ScreenShake* shake);
}
