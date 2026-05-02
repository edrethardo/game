#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

// Minimap with fog-of-war. Renders a top-down view of the dungeon
// in the top-right corner of the screen. Only cells the player has
// visited (or nearby) are revealed.

static constexpr u32 MAX_MINIMAP_CELLS  = 64 * 64; // max grid size supported
static constexpr f32 MINIMAP_REVEAL_RADIUS = 6.0f;  // cells around player to reveal

namespace Minimap {
    // Initialize minimap state for a new level. Call after level generation.
    void init(u32 gridWidth, u32 gridDepth);
    void shutdown();

    // Update fog-of-war based on player position. Call each frame.
    void updateVisited(const LevelGrid& grid, Vec3 playerPos);

    // Render the minimap. Draws in top-right corner of screen.
    // playerPos: world position for centering the player marker
    // playerYaw: player facing direction for the arrow indicator
    void draw(u32 screenWidth, u32 screenHeight,
              const LevelGrid& grid, Vec3 playerPos, f32 playerYaw);
}
