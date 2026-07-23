// autoplay_nav.h — pure navigation POLICY for the Autoplay bot (no player mutation here).
//
// Two jobs: (1) the hazard veto — is a one-cell XZ step in `dir` safe given the bot's feet
// height, so combat kiting and travel can never back it into lava, a wall, or off the map; (2)
// descend eligibility — the exact gate updateFloorDoor enforces (engine_update.cpp:2760-2801),
// mirrored so the brain only asks to descend when it actually can. The per-style TRAVEL goal
// (ramp climb / drop-hole pick / causeway) is computed in the engine driver from StoryNav +
// DungeonResult; this file holds the safety check every steering intent passes through.
#pragma once
#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

namespace Autoplay {

// True if stepping one cell along `dir` (XZ heading, need not be unit) from `from` at feet height
// `feetY` is safe: the destination cell is in-bounds, not solid, and not lava-at-feet-height. The
// lava test reuses LevelGridSystem::feetInLava — the SINGLE-SOURCE rule the burn tick uses (level_grid.h:
// "the burn tick and any future lava consumer share one rule"), so the bot vetoes exactly the cells
// that would burn it and NOT the ones it can clear airborne. `lavaFloor` short-circuits the lava
// test entirely on non-Hellforge floors (no lava cells exist there, so the query is pure waste).
inline bool stepAllowed(const LevelGrid& g, Vec3 from, f32 feetY, Vec3 dir, bool lavaFloor) {
    Vec3 flat{dir.x, 0.0f, dir.z};
    if (lengthSq(flat) < 1e-6f) return true;                 // no heading: nothing to veto
    Vec3 to = from + normalize(flat) * g.cellSize;           // one cell ahead along the heading
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, to, gx, gz)) return false;   // off the map edge
    if (LevelGridSystem::isSolid(g, gx, gz)) return false;           // into a wall
    // Airborne over lava is free (feet above the molten surface) — exactly what makes a 1-cell
    // vein jumpable — so probe feetInLava at the DESTINATION with the bot's own feet height.
    if (lavaFloor && LevelGridSystem::feetInLava(g, Vec3{to.x, feetY, to.z})) return false;
    return true;
}

// Inputs the descend gate reasons over, gathered by the engine driver from m_level + boss state.
struct DescendCtx {
    bool doorActive = false;   // m_level.floorDoorActive (false in town / arena / The Source)
    f32  distToDoor = 1e9f;    // metres from the bot to m_level.floorDoorPos
    bool hasBoss    = false;   // m_level.floorHasBoss (exit is sealed on boss floors)
    bool bossAlive  = false;   // floorBossAlive()
};
// Mirror of updateFloorDoor's gate: within 2 m of an active door (the real code tests squared
// distance < 4.0) and not blocked by a live floor boss. The Source portal is a SEPARATE trigger
// the brain simply never requests, so it needs no branch here.
inline bool mayDescend(const DescendCtx& c) {
    if (!c.doorActive) return false;
    if (c.distToDoor > 2.0f) return false;
    if (c.hasBoss && c.bossAlive) return false;
    return true;
}

} // namespace Autoplay
