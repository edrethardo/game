#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"
#include "game/entity.h"

// Minimap with fog-of-war. Renders a top-down view of the dungeon
// in the top-right corner of the screen. Cells visited by the player
// or friendly NPCs are revealed (NPC-revealed cells shown dimmer).

static constexpr u32 MAX_MINIMAP_CELLS  = 64 * 64;
static constexpr f32 MINIMAP_REVEAL_RADIUS = 6.0f;  // cells around player
static constexpr f32 NPC_REVEAL_RADIUS     = 4.0f;  // cells around each NPC

namespace Minimap {
    void init(u32 gridWidth, u32 gridDepth);
    void shutdown();

    // Update fog-of-war. NPCs also reveal cells (shown dimmer than player-explored).
    void updateVisited(const LevelGrid& grid, Vec3 playerPos,
                       const EntityPool& entities);

    // Render minimap with NPC dots + optional remote co-op player dots.
    // otherPlayers/otherActive are parallel arrays of length otherPlayerCount (may be
    // null / 0 in singleplayer); each active slot draws as a distinct cyan dot. The local
    // player is always the green dot+arrow — callers pass their REMOTE players here with
    // the local slot(s) marked inactive so the local marker is never duplicated.
    void draw(u32 screenWidth, u32 screenHeight,
              const LevelGrid& grid, Vec3 playerPos, f32 playerYaw,
              const EntityPool& entities,
              const Vec3* otherPlayers = nullptr, const bool* otherActive = nullptr,
              u32 otherPlayerCount = 0);
}
