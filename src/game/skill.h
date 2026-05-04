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

struct Player; // forward decl

namespace SkillSystem {
    void init();

    // Tick cooldowns, energy regen, pending meteors
    void update(SkillState& ss, f32 dt);

    // Try to activate the player's current skill (returns true if activated)
    bool tryActivate(SkillState& ss, const SkillDef* skillDefs, u32 skillDefCount,
                     Vec3 eyePos, Vec3 forward, f32 yaw,
                     ProjectilePool& projectiles, EntityPool& entities,
                     const LevelGrid& grid, Player& player);

    // Update orb projectiles (spawn shards) -- called from projectile update or engine update
    void updateOrbProjectiles(ProjectilePool& pool, const SkillDef* skillDefs, u32 skillDefCount, f32 dt);

    // Update pending meteors
    void updateMeteors(EntityPool& entities, f32 dt);

    // Get the SkillDef for a given SkillId (returns nullptr if not found)
    const SkillDef* findSkillDef(const SkillDef* defs, u32 count, SkillId id);

    // Visual FX callbacks — set by Engine to trigger skill effects
    using NovaCallback = void(*)(Vec3 position, f32 radius, Vec3 color);
    using DashCallback = void(*)(Vec3 start, Vec3 end);
    // Scorch callback — creates a persistent ground fire zone (position, radius, duration)
    using ScorchCallback = void(*)(Vec3 position, f32 radius, f32 duration, f32 dps);
    void setNovaCallback(NovaCallback cb);
    void setDashCallback(DashCallback cb);
    void setScorchCallback(ScorchCallback cb);
}
