// pathfinder.cpp — Grid A* pathfinder for enemy tactical navigation.
//
// Searches the dungeon grid with a fixed node budget and returns a short list of
// world-space waypoints. The search is clearance-aware (see bodyRadius in the
// header): it is 8-connected with corner-cut prevention, biases paths away from
// wall faces via the LevelGrid clearance field, and string-pulls the raw cell
// path into long straight runs the entity AABB can actually traverse. That last
// step is what stops wide enemies from clipping inside corners while following a
// cell-center waypoint chain. All open/closed/scratch sets are fixed-size stack
// arrays — no heap.

#include "world/pathfinder.h"
#include <cmath>
#include <cstring>  // memset

namespace Pathfinder {

// Step costs: cardinal moves are 10, diagonals 14 (≈10·√2). A wall-hugging cell
// (clearance at the minimum the body needs) costs a little extra so the search
// rounds corners through open space when it has the option — neutral inside a
// tight corridor where every cell hugs a wall.
static constexpr u16 COST_CARDINAL = 10;
static constexpr u16 COST_DIAGONAL = 14;
static constexpr u16 COST_WALL_HUG = 6;

// Internal node stored in open and closed lists.
struct AStarNode {
    u16 x, z;
    u16 parentIdx;  // index into closed list; 0xFFFF = no parent (start node)
    u16 g;          // accumulated cost from start
    u16 f;          // g + heuristic
};

// Octile distance heuristic (admissible for 8-connected movement).
static inline u16 octile(u16 ax, u16 az, u16 bx, u16 bz) {
    s32 dx = static_cast<s32>(ax) - static_cast<s32>(bx); if (dx < 0) dx = -dx;
    s32 dz = static_cast<s32>(az) - static_cast<s32>(bz); if (dz < 0) dz = -dz;
    s32 lo = dx < dz ? dx : dz;
    // 10·(dx+dz) − (2·10 − 14)·min = diagonal shortcut credited at 14 per step.
    return static_cast<u16>(COST_CARDINAL * (dx + dz) - (2 * COST_CARDINAL - COST_DIAGONAL) * lo);
}

// Minimum clearance (in cells) a body of the given radius needs in a cell: the
// cell centre must sit more than `radius` from the nearest wall face, and at
// cellSize 1.0 a clearance of 1 already gives 0.5 m of slack. 0 ⇒ no constraint.
static inline u8 minClearanceFor(f32 bodyRadius, f32 cellSize) {
    if (bodyRadius <= 0.0f) return 0;
    s32 c = static_cast<s32>(std::ceil(bodyRadius / cellSize + 0.5f));
    return static_cast<u8>(c < 1 ? 1 : c);
}

// A cell is walkable for the body if it is in-bounds, not solid, and (when the
// clearance field exists) has at least minClr clearance.
static inline bool isWalkable(const LevelGrid& grid, u32 x, u32 z, u8 minClr) {
    if (!LevelGridSystem::isInBounds(grid, x, z)) return false;
    if (LevelGridSystem::isSolid(grid, x, z))     return false;
    if (minClr > 0 && grid.clearance &&
        LevelGridSystem::clearanceAt(grid, x, z) < minClr) return false;
    return true;
}

// Width-aware straight-line test between two world points: can a body of the
// given radius walk directly from a to b without any part of its AABB entering a
// wall? Samples the segment and checks the four AABB corners at each sample.
// Used by string-pulling to collapse the cell path into straight runs.
static bool segmentTraversable(const LevelGrid& grid, Vec3 a, Vec3 b,
                               f32 radius, u8 minClr) {
    f32 dx = b.x - a.x, dz = b.z - a.z;
    f32 dist = std::sqrt(dx * dx + dz * dz);
    if (dist < 1e-4f) return true;
    f32 step = grid.cellSize * 0.25f;            // fine enough to never tunnel
    s32 n = static_cast<s32>(dist / step) + 1;
    f32 inv = 1.0f / static_cast<f32>(n);
    f32 r = radius > 0.0f ? radius : 0.0f;
    for (s32 i = 0; i <= n; i++) {
        f32 t = static_cast<f32>(i) * inv;
        f32 px = a.x + dx * t;
        f32 pz = a.z + dz * t;
        // Check the AABB footprint (centre and the four offset corners).
        const f32 ox[5] = { 0,  r,  r, -r, -r };
        const f32 oz[5] = { 0,  r, -r,  r, -r };
        for (u8 k = 0; k < 5; k++) {
            u32 gx, gz;
            if (!LevelGridSystem::worldToGrid(grid, {px + ox[k], 0, pz + oz[k]}, gx, gz))
                return false;
            if (!isWalkable(grid, gx, gz, minClr)) return false;
        }
    }
    return true;
}

u8 findPath(const LevelGrid& grid, Vec3 start, Vec3 goal,
            Vec3* outPath, u8 maxWaypoints, f32 bodyRadius, u16 maxSearch)
{
    if (maxWaypoints == 0) return 0;
    if (maxSearch > MAX_ASTAR_SEARCH) maxSearch = MAX_ASTAR_SEARCH;
    const u8 minClr = minClearanceFor(bodyRadius, grid.cellSize);

    // Convert world positions to grid coords; bail if either is out of bounds.
    u32 sx, sz, gx, gz;
    if (!LevelGridSystem::worldToGrid(grid, start, sx, sz)) return 0;
    if (!LevelGridSystem::worldToGrid(grid, goal,  gx, gz)) return 0;

    // Trivial case: already in the goal cell.
    if (sx == gx && sz == gz) { outPath[0] = goal; return 1; }

    // Fixed-size open and closed lists — no heap.
    AStarNode openList[MAX_ASTAR_SEARCH];
    AStarNode closedList[MAX_ASTAR_SEARCH];
    u16 openCount = 0, closedCount = 0;

    openList[openCount++] = AStarNode{
        static_cast<u16>(sx), static_cast<u16>(sz), 0xFFFFu, 0,
        octile(static_cast<u16>(sx), static_cast<u16>(sz),
               static_cast<u16>(gx), static_cast<u16>(gz))
    };

    // 8-connected neighbour offsets: 4 cardinal then 4 diagonal.
    const s32 dx8[8] = { 1, -1,  0,  0,  1,  1, -1, -1 };
    const s32 dz8[8] = { 0,  0,  1, -1,  1, -1,  1, -1 };

    u16 goalClosedIdx = 0xFFFFu;

    while (openCount > 0 && closedCount < maxSearch) {
        // Pick the open node with the lowest f (linear scan — open list stays
        // small enough that this beats a heap).
        u16 bestIdx = 0;
        for (u16 i = 1; i < openCount; ++i)
            if (openList[i].f < openList[bestIdx].f) bestIdx = i;

        AStarNode current = openList[bestIdx];
        openList[bestIdx] = openList[--openCount];

        u16 currentClosedIdx = closedCount;
        if (closedCount >= MAX_ASTAR_SEARCH) break;
        closedList[closedCount++] = current;

        if (current.x == static_cast<u16>(gx) && current.z == static_cast<u16>(gz)) {
            goalClosedIdx = currentClosedIdx;
            break;
        }

        for (int d = 0; d < 8; ++d) {
            s32 nx = static_cast<s32>(current.x) + dx8[d];
            s32 nz = static_cast<s32>(current.z) + dz8[d];
            if (nx < 0 || nz < 0) continue;
            u32 unx = static_cast<u32>(nx), unz = static_cast<u32>(nz);
            if (!isWalkable(grid, unx, unz, minClr)) continue;

            // Corner-cut prevention: a diagonal step is only legal if BOTH shared
            // orthogonal cells are also walkable, otherwise a wide body would
            // scrape the wall it's cutting past.
            if (d >= 4) {
                if (!isWalkable(grid, static_cast<u32>(current.x) + dx8[d], current.z, minClr) ||
                    !isWalkable(grid, current.x, static_cast<u32>(current.z) + dz8[d], minClr))
                    continue;
            }

            u16 stepCost = (d < 4) ? COST_CARDINAL : COST_DIAGONAL;
            // Bias away from wall-hugging cells where the map allows it.
            if (minClr > 0 && grid.clearance &&
                LevelGridSystem::clearanceAt(grid, unx, unz) <= minClr)
                stepCost += COST_WALL_HUG;

            u16 ng = current.g + stepCost;
            u16 nf = ng + octile(static_cast<u16>(unx), static_cast<u16>(unz),
                                 static_cast<u16>(gx), static_cast<u16>(gz));

            // Skip if already closed (first close is optimal under a consistent h).
            bool inClosed = false;
            for (u16 c = 0; c < closedCount; ++c)
                if (closedList[c].x == unx && closedList[c].z == unz) { inClosed = true; break; }
            if (inClosed) continue;

            // If already open with a ≤ g, skip; otherwise relax it.
            bool dominated = false;
            for (u16 o = 0; o < openCount; ++o) {
                if (openList[o].x == unx && openList[o].z == unz) {
                    if (openList[o].g <= ng) { dominated = true; }
                    else { openList[o].g = ng; openList[o].f = nf;
                           openList[o].parentIdx = currentClosedIdx; dominated = true; }
                    break;
                }
            }
            if (dominated) continue;

            if (openCount < MAX_ASTAR_SEARCH)
                openList[openCount++] = AStarNode{
                    static_cast<u16>(unx), static_cast<u16>(unz),
                    currentClosedIdx, ng, nf };
        }
    }

    if (goalClosedIdx == 0xFFFFu) return 0;  // no path within budget

    // Reconstruct the cell path into world points, start → goal order.
    // rawPts[0] is the entity's real start position (not its cell centre) so the
    // first string-pull anchor is exact.
    Vec3 rawPts[MAX_ASTAR_SEARCH + 1];
    u16  rawCount = 0;
    // Walk the parent chain (goal → start) into a temporary, then reverse.
    u16 chain[MAX_ASTAR_SEARCH];
    u16 chainLen = 0;
    for (u16 idx = goalClosedIdx; idx != 0xFFFFu && chainLen < MAX_ASTAR_SEARCH;
         idx = closedList[idx].parentIdx)
        chain[chainLen++] = idx;
    // chain is goal..start; emit start..goal.
    rawPts[rawCount++] = start;
    for (s32 i = chainLen - 2; i >= 0; --i)  // skip start cell (i=chainLen-1); use real start
        rawPts[rawCount++] = LevelGridSystem::gridToWorld(
            grid, closedList[chain[i]].x, closedList[chain[i]].z);
    rawPts[rawCount - 1] = goal;             // exact goal as the final point

    // String-pull: keep a waypoint only where the straight run from the current
    // anchor to the next point would clip a wall. Yields long diagonal legs
    // instead of a cell-center staircase.
    Vec3 pulled[MAX_ASTAR_SEARCH + 1];
    u16  pulledCount = 0;
    u16 anchor = 0;
    for (u16 i = 2; i < rawCount; ++i) {
        if (!segmentTraversable(grid, rawPts[anchor], rawPts[i], bodyRadius, minClr)) {
            pulled[pulledCount++] = rawPts[i - 1];
            anchor = i - 1;
        }
    }
    pulled[pulledCount++] = rawPts[rawCount - 1]; // goal always last

    // Compress to maxWaypoints by even subsampling if the pulled path is still
    // long (rare). The goal is always preserved as the final entry.
    if (pulledCount <= maxWaypoints) {
        for (u16 i = 0; i < pulledCount; ++i) outPath[i] = pulled[i];
        return static_cast<u8>(pulledCount);
    }
    u16 step = pulledCount / maxWaypoints;
    if (step < 1) step = 1;
    u8 out = 0;
    for (u16 i = 0; i < pulledCount && out < maxWaypoints - 1; i += step)
        outPath[out++] = pulled[i];
    outPath[out++] = pulled[pulledCount - 1];
    return out;
}

} // namespace Pathfinder
