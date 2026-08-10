#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"
#include "game/entity.h"
#include "game/shrine.h"   // WorldItemPool + Shrine::buffOf/colorOf (shrine icons)

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

    // Render minimap with NPC dots + optional remote co-op player dots + shrine icons.
    // otherPlayers/otherActive are parallel arrays of length otherPlayerCount (may be
    // null / 0 in singleplayer); each active slot draws as a distinct cyan dot. The local
    // player is always the green dot+arrow — callers pass their REMOTE players here with
    // the local slot(s) marked inactive so the local marker is never duplicated.
    //
    // worldItems (optional): shrines in it are drawn as colour-coded diamonds, but ONLY where the
    // fog of war has been lifted — the reveal test reads the visited mask, which lives in
    // minimap.cpp, so it is deliberately not something a caller can forget to apply. Activating a
    // shrine frees its world slot, so a used shrine drops off the map by itself. Clients mirror the
    // server's world items every frame, so guests see shrines on the same terms as the host.
    void draw(u32 screenWidth, u32 screenHeight,
              const LevelGrid& grid, Vec3 playerPos, f32 playerYaw,
              const EntityPool& entities,
              const Vec3* otherPlayers = nullptr, const bool* otherActive = nullptr,
              u32 otherPlayerCount = 0,
              const WorldItemPool* worldItems = nullptr,
              // The overworld sentinel floor we are standing on (0 = not in a zone). Only used to
              // decide which zone-gate icon to draw: a cave mouth is a cave mouth from BOTH sides,
              // so the glyph needs to know where you are, not just where the gate leads.
              u8 zoneFloor = 0,
              // Which Cairn Stones (quest 56) the local character has aligned — bit N = stone N,
              // matching the ordinal each stone carries in its ItemInstance. Passed in rather than
              // read here because quest progress is per-character engine state and the minimap is a
              // renderer: the caller already holds the one authority (Engine::cairnAlignedMask).
              u8 cairnMask = 0);
}
