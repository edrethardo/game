// Boss AI header — personality-driven boss behavior system.
// See boss_def.h for BossDef struct and boss_ai.cpp for implementation.

#pragma once

#include "core/types.h"
#include "core/math.h"

struct Entity;
struct EntityPool;
struct ProjectilePool;
struct Player;
struct LevelGrid;
struct BossDef;

namespace BossAI {
    // Per-tick boss update: applies personality, enrage, projectile firing.
    // Called from EnemyAI::update when enemyType == BOSS.
    void update(Entity& e, const BossDef& def,
                EntityPool& pool, ProjectilePool& projectiles,
                Player& player, const LevelGrid& grid, f32 dt);

    // Check if any minion spawned by this boss is still alive (for minion shield).
    bool hasMinionAlive(const EntityPool& pool, u16 bossIdx);

    // Callbacks — set by Engine during init
    void setMagicBurstCallback(void(*cb)(Vec3 pos, f32 r, f32 g, f32 b, u32 count));
    void setDamageNumberCallback(void(*cb)(Vec3 pos, f32 dmg));
}
