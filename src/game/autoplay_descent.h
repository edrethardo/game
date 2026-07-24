// autoplay_descent.h — the FOUR_STORY "Descent" travel field for the Autoplay bot.
//
// On a Descent floor the exit is three stories below the spawn and the only way down is to walk into
// a hole punched in your own story's slab. Steering to one is a MAZE routing problem, and the two
// cheaper answers both failed live:
//
//   * A straight-line bearing at the nearest hole (v1). The floor is a braided recursive-backtracker
//     maze of 3-wide corridors, so the bearing points through walls most of the time; the hazard
//     veto's +-45/+-90 detour fan then scrapes the bot along whatever it hit. Measured over a 150 s
//     trace: the distance to the chosen hole GREW on 61 samples and shrank on 53 — a random walk.
//   * A*, re-planned on a throttle (v2). Correct in principle — the four stories share one
//     full-height wall skeleton, so plain 2D A* is exact on any story — but Pathfinder::findPath
//     gives up after MAX_ASTAR_SEARCH (256) closed cells, which on a maze of 3-wide corridors is
//     barely a dozen cells of real travel. A hole across the floor simply returned nothing, the code
//     fell back to the raw bearing, and the bot was back to walking into walls. Stepped shorter legs
//     papered over it without fixing the wall-pointing fallback, and the goal thrashed between holes
//     as "nearest" changed underfoot.
//
// So this is a BFS FLOW FIELD, the same shape as LevelGridSystem::buildFlowField but seeded from
// every clean drop hole on ONE story instead of from the exit. That buys three things at once: the
// direction it hands back is derived from a route that exists, so it can never point into a wall;
// it is defined for EVERY reachable cell, so there is no "no plan" state where the bot stands and
// stares; and it costs one ~2k-cell BFS per story change (three per floor) instead of an A* search
// every half second.
//
// Like the exit field it expands 4-CONNECTED and steers toward the next cell's CENTRE — both are
// deliberate anti-wall-hugging measures inherited from that code, and they are what keeps a 0.6 m
// body off the corners of a 1-cell-thick wall.
#pragma once
#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"
#include "world/level_gen.h"

namespace Autoplay {

// Per-cell descent field. Owned by the engine driver (one per run), rebuilt on demand. The `dir`
// encoding matches LevelGrid::flowDir exactly: 0-7 = step direction, 0xFE = standing on a hole,
// 0xFF = unreachable.
struct DescentField {
    u8* dir    = nullptr;    // width*depth cells, or null before the first build
    u32 cap    = 0;          // allocated cell count (grown, never shrunk)
    u32 width  = 0, depth = 0;
    f32 storyY = 1e9f;       // the story this field routes on
    u32 stamp  = 0xFFFFFFFFu;// floor identity, so a new floor can never reuse a stale field
    bool valid = false;      // false when the story has no way down (L0) — caller keeps the exit field
    // True when this story offered ONLY return-lift holes, so the field had to seed padded ones.
    // The driver reads it to stand its pad-avoidance veto down: otherwise it would refuse the last
    // step into the single way down, and the bot would circle a hole it is not allowed to enter.
    bool paddedOnly = false;
};

// Rebuild the field if it is stale for (storyY, stamp, grid size); a no-op when it is already
// current, so this is safe to call every tick. Returns `valid`: false means this story has no drop
// holes at all, which on the Descent means we are on L0 and the ordinary exit flow field is the
// right heading.
bool ensureDescentField(DescentField& f, const LevelGrid& g, const DungeonResult& d,
                        f32 storyY, u32 stamp);

// Unit XZ heading toward the nearest way down, or {0,0,0} when the field is invalid, the position is
// off-grid, or this cell could not be reached from any hole (a sealed pocket, or a jump-pad cell —
// those are excluded from the field on purpose, see the .cpp).
Vec3 descentDirection(const DescentField& f, const LevelGrid& g, Vec3 pos);

// True when the bot is standing on a drop hole's own cell (field code 0xFE) — it is about to fall,
// and the driver can stop steering.
bool atDescentGoal(const DescentField& f, const LevelGrid& g, Vec3 pos);

void freeDescentField(DescentField& f);

} // namespace Autoplay
