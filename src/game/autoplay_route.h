// autoplay_route.h — a general WALL-AWARE flow field to an arbitrary GOAL, for the Autoplay bot.
//
// This is the conceptual fix for "the bot walks into a wall trying to reach X". Every travel goal the
// bot doesn't already have a proper field for (the boss, chiefly) used to be steered with a STRAIGHT
// BEARING (`flowDir = normalize(goal - pos)`) plus a ±45/±90 hazard-detour fan. A straight bearing
// cannot route around a wall — the fan only dodges the single cell dead ahead — so the bot jams into
// whatever wall sits between it and the goal (reported across builds: a Sorcerer on Sethrak, a Warrior
// with a cleaver in a boss room, the same with a shrine). The exit field, DescentField and VHallField
// never had this problem because they are BFS FLOW FIELDS: wall-aware by construction, defined on every
// reachable cell, and read by steering at the next cell's CENTRE (so a 0.3 m body never hugs a wall).
//
// RouteField is that same primitive, generalised to seed from ONE arbitrary goal cell instead of the
// exit / drop holes. It routes the bot to a MOVING goal (the boss) by rebuilding when the goal changes
// cell — a ~2k-cell BFS a few times a second, far cheaper than an A* search per tick and with no cell
// cap (A* gave up after MAX_ASTAR_SEARCH, so a boss across the floor returned nothing and the code fell
// back to the straight bearing). It reuses the exact 4-connected BFS + string-pulled cell-centre
// readout the other fields use, so a body follows the middle of a corridor around every corner.
#pragma once
#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

namespace Autoplay {

// Per-goal flow field, owned by the engine driver. `dir` holds width*depth cells: 0-7 = step toward
// the goal, 0xFE = at the goal cell, 0xFF = unreachable. Rebuilt when the goal cell, floor, or grid
// size changes — so it tracks a moving goal without a rebuild every tick.
struct RouteField {
    u8* dir   = nullptr;
    u32 cap   = 0;                  // allocated cells (grown, never shrunk)
    u32 width = 0, depth = 0;
    s32 goalX = -1, goalZ = -1;     // the seeded goal cell (rebuild when it moves)
    u32 stamp = 0xFFFFFFFFu;        // floor identity, so a new floor can't reuse a stale field
    bool valid = false;
};

// Rebuild the field if it is stale for (goal cell, stamp, grid size); a no-op when already current, so
// it is safe to call every tick. Returns `valid` (false when the goal is off-grid / in a wall).
bool ensureRouteField(RouteField& f, const LevelGrid& g, Vec3 goalPos, u32 stamp);

// Unit XZ heading toward the goal from `pos`, string-pulled to the farthest cell-centre reachable in a
// clear straight run (turns the 4-connected staircase into the diagonal a body can actually walk), or
// {0,0,0} at the goal / off-field / on an unreachable cell (caller keeps its fallback heading then).
Vec3 routeDirection(const RouteField& f, const LevelGrid& g, Vec3 pos);

void freeRouteField(RouteField& f);

} // namespace Autoplay
