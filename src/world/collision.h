#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

struct Player;

// Player collider half-extents (metres)
static constexpr f32 PLAYER_HALF_WIDTH  = 0.3f;  // 0.6m total, fits through 1m cells
static constexpr f32 PLAYER_HEIGHT      = 1.8f;
static constexpr f32 PLAYER_HALF_HEIGHT = PLAYER_HEIGHT * 0.5f;

static constexpr f32 GRAVITY      = -20.0f; // m/s²
static constexpr f32 JUMP_SPEED   =   8.0f; // m/s

namespace Collision {
    // Moves player.position by player.velocity*dt against the level grid.
    // Updates player.onGround, zeroes blocked velocity components.
    void moveAndSlide(Player& player, const LevelGrid& grid, f32 dt);
}
