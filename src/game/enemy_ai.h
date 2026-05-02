#pragma once

#include "core/types.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "world/level_grid.h"

struct Player;

namespace EnemyAI {
    // Update all enemy AI: FSM transitions, movement, attacks.
    void update(EntityPool& pool, const LevelGrid& grid,
                Player& player, ProjectilePool& projectiles, f32 dt);
}
