// autoplay_vhall.h — the VERTICAL_HALL two-story travel field for the Autoplay bot.
//
// VHALL is a TWO-STORY floor: the bot spawns on one story and the exit door is on the other, reached
// by a graduated-slab ramp. Every earlier attempt steered with the flat 2D flow field
// (LevelGridSystem::flowDirection) plus a pile of per-tick ramp heuristics (commit a ramp, latch
// "crossed", beeline the door, pulse a jump). They all fought the same root problem: the flat field
// treats each XZ cell as ONE node, so it CANNOT represent two stories — a balcony cell and the ground
// beneath it are the same node — and it routes a bot that has just climbed to the balcony straight
// back off the open inner edge to the ground ("drops afterward"). No heuristic on top of a 2D field
// fixes a 3D topology.
//
// This is the conceptual fix, and it mirrors what DescentField did for FOUR_STORY: a BFS flow field
// over the real topology. Here the nodes are (cell, STORY) pairs — ground (story 0) and upper
// (story 1) — and two nodes are adjacent only when you could actually step between their walkable
// surfaces:
//   * A step from (cell,s) to a 4-connected (neighbour, ns) — ANY story ns — is legal when the
//     neighbour node is walkable AND its surface height is within one step (PLATFORM_STEP_TOLERANCE)
//     of ours, which the collision step-up/step-down handles. Cross-story steps are what model
//     MOUNTING the ramp: the open GROUND beside a ramp FOOT (0 m) connects to that foot's UPPER node
//     (slab ~0.25 m), and from the foot the graduated ramp is one continuous height-continuous run up
//     to the balcony. A balcony (3 m) is NOT within a step of the void/ground (0 m) beside it, so the
//     field can never route the bot off a balcony edge.
//   * A GROUND node is excluded where a slab roofs it below head height — the body cannot fit under
//     the low part of a ramp, and routing it there wedges it. The high arcades under the balconies
//     (slab ~3 m) stay walkable ground.
// Seeded from the exit door's (cell, story) and flooded outward, the field routes the whole journey —
// ground spawn -> ramp foot -> up the ramp -> across the balcony -> door — as ONE field, with no
// story heuristics and no way to be steered off an edge.
//
// Like DescentField (and the exit field it descends from) it expands 4-CONNECTED and the readout
// steers at the next cell's CENTRE — the two anti-wall-hug rules — so the bot rides the middle of the
// 2-wide ramp instead of drifting off its side.
#pragma once
#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

namespace Autoplay {

// Per-(cell,story) flow field. Owned by the engine driver (one per run), rebuilt on demand. `dir`
// holds 2*width*depth entries: index = story*(width*depth) + z*width + x. Encoding matches the exit
// field: 0-7 = step direction toward the door, 0xFE = at the door, 0xFF = unreachable.
struct VHallField {
    u8* dir    = nullptr;    // 2*width*depth, or null before the first build
    u32 cap    = 0;          // allocated entry count (grown, never shrunk)
    u32 width  = 0, depth = 0;
    u32 stamp  = 0xFFFFFFFFu; // floor identity, so a new floor can never reuse a stale field
    f32 doorY  = 0.0f;       // the door story this field was seeded for (rebuild if it changes)
    bool valid = false;
};

// Rebuild the field if it is stale for (doorPos, stamp, grid size); a no-op when already current, so
// it is safe to call every tick. Returns `valid`.
bool ensureVHallField(VHallField& f, const LevelGrid& g, Vec3 doorPos, u32 stamp);

// Unit XZ heading toward the door from the bot's current (cell, story), or {0,0,0} at the door / off
// the field / on an unreachable node. The bot's story is read from its feet height vs the cell's slab
// (the same rule collision uses), so a bot mid-ramp is treated as upper and routed along the ramp.
Vec3 vhallDirection(const VHallField& f, const LevelGrid& g, Vec3 pos);

// True when the bot's (cell, story) node is the door itself (field code 0xFE) — it has arrived.
bool atVHallGoal(const VHallField& f, const LevelGrid& g, Vec3 pos);

void freeVHallField(VHallField& f);

} // namespace Autoplay
