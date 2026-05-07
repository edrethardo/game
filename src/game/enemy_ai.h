#pragma once

#include "core/types.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "world/level_grid.h"

struct Player;

namespace EnemyAI {
    // Update all enemy AI: FSM transitions, movement, attacks.
    // Additional player targets can be passed for co-op/multiplayer — each enemy
    // targets the nearest player. Damage is applied to the targeted player.
    static constexpr u32 MAX_AI_TARGETS = 4;
    void update(EntityPool& pool, const LevelGrid& grid,
                Player& player, ProjectilePool& projectiles, f32 dt,
                Player** extraPlayers = nullptr, u32 extraPlayerCount = 0);
}
