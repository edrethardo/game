#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

// Room data exposed for enemy/item spawning after generation
struct DungeonRoom {
    u32 x, z, w, d;     // grid cell position and dimensions
    f32 floorHeight;     // floor height in meters
    u8  wallMat;         // wall material ID

    // Rooms connected via corridors (indices into DungeonResult::rooms).
    // Populated by LevelGen::generate after all rooms and corridors are carved.
    u16 adjacentRooms[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    u8  adjacentCount = 0;
};

static constexpr u32 MAX_DUNGEON_ROOMS = 32;

// A ramp linking the two stories of a VERTICAL_HALL chamber: the ramp foot (ground story) and its
// top (balcony story), in world coords. The enemy cross-story chase routes to the near end
// (nearestPortalGoal). Empty/unused for every other layout style.
struct StoryPortal {
    Vec3 lowPos;   // ramp foot @ ground
    Vec3 highPos;  // ramp top  @ balcony
};
static constexpr u32 MAX_STORY_PORTALS = 16;

// A FOUR_STORY drop-hole: a punched gap in a slab you fall through to the level below. pos.xz = hole
// centre; pos.y == surfaceY == the slab TOP it pierces (world metres). Zero/unused for other styles;
// consumed by spawnFloorHoleSnipers (ranged seats at the edge) + enemy-fall AI.
struct DropHole { Vec3 pos; f32 surfaceY; };
static constexpr u32 MAX_DROP_HOLES = 32;

struct DungeonResult {
    Vec3 spawnPos;                         // player spawn (world coords)
    DungeonRoom rooms[MAX_DUNGEON_ROOMS];  // generated rooms
    u32 roomCount;                         // number of rooms created
    u32 spawnRoomIdx;                      // dead-end room chosen for player spawn
    u32 exitRoomIdx;                       // farthest room from spawn for exit portal

    // VERTICAL_HALL two-story metadata (zero/unused for other styles).
    StoryPortal portals[MAX_STORY_PORTALS] = {};
    u8   portalCount     = 0;
    bool spawnOnUpper    = false;   // coin-flip: spawn on a balcony (⇒ exit on the ground: DESCEND)
    Vec3 spawnBalconyPos = {};      // balcony centre of the spawn chamber (world)
    Vec3 exitBalconyPos  = {};      // balcony centre of the exit chamber (world)

    // FOUR_STORY "Descent" drop-holes (zero/unused for other styles).
    static constexpr u32 MAX_DROP_HOLES = ::MAX_DROP_HOLES;
    DropHole dropHoles[MAX_DROP_HOLES] = {};
    u8       dropHoleCount = 0;
};

namespace LevelGen {
    // Structural layout algorithms. Every style fills the same DungeonResult
    // contract (rect rooms, adjacency, reachable spawn->exit), so all downstream
    // consumers (enemy/chest/boss/light/event placement, squads, exit portal)
    // work unchanged — the styles differ only in how the space is carved.
    enum struct LayoutStyle : u8 {
        BSP_ROOMS = 0,  // classic: BSP partition rooms + L-corridors (the original)
        CAVERN,         // cellular-automata cave with carved rect clearings as rooms
        GAUNTLET,       // serpentine chain of arenas — one long fight toward the exit
        HUB,            // grand central chamber, spoke corridors to perimeter vaults
        VERTICAL_HALL,  // two-story chambers: ground pit + walk-under balcony, ramps, opposite-story exit
        FOUR_STORY,     // four dead-stacked walkable stories on one footprint; one-way drop-only descent
        COUNT
    };

    // Human-readable style name for logs.
    const char* styleName(LayoutStyle style);

    // Deterministic per-floor style pick — host and client call this with the same
    // (dungeonSeed, floor), so both sides always agree. Floors 1-3 (the tiny
    // tutorial grids) are always classic BSP; deeper floors mix styles with
    // per-tier weights (caves peak in the Spider Caverns, gauntlets in the
    // Hellforge, hubs in the Catacombs/Void).
    LayoutStyle pickLayoutStyle(u32 seed, u32 floor);

    // Legacy test dungeon (kept for fallback)
    Vec3 generateTestDungeon(LevelGrid& grid);

    // Procedural dungeon generation. Always produces a valid result — spawn is
    // the room with fewest corridor exits, exit is the farthest room. A style
    // that degenerates on a given seed/size falls back to BSP_ROOMS internally
    // (deterministically), so the caller never sees an unplayable floor.
    DungeonResult generate(LevelGrid& grid, u32 seed, u32 gridWidth = 48, u32 gridDepth = 48,
                           LayoutStyle style = LayoutStyle::BSP_ROOMS);
}
