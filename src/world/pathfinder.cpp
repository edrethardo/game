// pathfinder.cpp — Grid A* pathfinder for enemy tactical navigation.
// Implements a fixed-budget A* search over the dungeon grid using
// 4-connected neighbors and a Manhattan distance heuristic.
// All open/closed sets are fixed-size stack arrays (no heap).
// Produces up to MAX_PATH_WAYPOINTS world-space waypoints by subsampling
// the raw node path; the goal cell is always the final waypoint.

#include "world/pathfinder.h"
#include <cstring>  // memset

namespace Pathfinder {

// Internal node stored in open and closed lists.
struct AStarNode {
    u16 x, z;
    u16 parentIdx;  // index into closed list; 0xFFFF = no parent (start node)
    u16 g;          // cost from start (grid steps)
    u16 f;          // g + heuristic (Manhattan distance to goal)
};

static inline u16 manhattan(u16 ax, u16 az, u16 bx, u16 bz) {
    // Cast to s32 to handle subtraction safely before taking abs.
    s32 dx = static_cast<s32>(ax) - static_cast<s32>(bx);
    s32 dz = static_cast<s32>(az) - static_cast<s32>(bz);
    if (dx < 0) dx = -dx;
    if (dz < 0) dz = -dz;
    return static_cast<u16>(dx + dz);
}

u8 findPath(const LevelGrid& grid, Vec3 start, Vec3 goal,
            Vec3* outPath, u8 maxWaypoints, u16 maxSearch)
{
    // Convert world positions to grid coords; bail if either is out of bounds.
    u32 sx, sz, gx, gz;
    if (!LevelGridSystem::worldToGrid(grid, start, sx, sz)) return 0;
    if (!LevelGridSystem::worldToGrid(grid, goal,  gx, gz)) return 0;

    // Trivial case: already at goal.
    if (sx == gx && sz == gz) {
        if (maxWaypoints > 0) { outPath[0] = goal; return 1; }
        return 0;
    }

    // Fixed-size open and closed lists — no heap.
    AStarNode openList[MAX_ASTAR_SEARCH];
    AStarNode closedList[MAX_ASTAR_SEARCH];
    u16 openCount   = 0;
    u16 closedCount = 0;

    // Seed open list with the start node.
    openList[openCount++] = AStarNode{
        static_cast<u16>(sx), static_cast<u16>(sz),
        0xFFFFu,  // no parent
        0,
        manhattan(static_cast<u16>(sx), static_cast<u16>(sz),
                  static_cast<u16>(gx), static_cast<u16>(gz))
    };

    // 4-connected neighbor offsets: +X, -X, +Z, -Z.
    const s32 dx[4] = { 1, -1,  0,  0 };
    const s32 dz[4] = { 0,  0,  1, -1 };

    while (openCount > 0 && closedCount < maxSearch) {
        // Pick the open node with the lowest f score (linear scan — open list
        // stays small enough for this to be cheaper than a heap).
        u16 bestIdx = 0;
        for (u16 i = 1; i < openCount; ++i) {
            if (openList[i].f < openList[bestIdx].f) bestIdx = i;
        }

        AStarNode current = openList[bestIdx];
        // Remove current from open list by swapping with the last element.
        openList[bestIdx] = openList[--openCount];

        // Move current to closed list.
        u16 currentClosedIdx = closedCount;
        if (closedCount >= MAX_ASTAR_SEARCH) break;  // exhausted budget
        closedList[closedCount++] = current;

        // Goal reached — trace back through closed list and build waypoint array.
        if (current.x == static_cast<u16>(gx) &&
            current.z == static_cast<u16>(gz))
        {
            // Count path length by walking parent chain.
            u16 pathLen = 0;
            u16 idx = currentClosedIdx;
            while (idx != 0xFFFFu) {
                ++pathLen;
                idx = closedList[idx].parentIdx;
            }

            if (pathLen == 0 || maxWaypoints == 0) return 0;

            // Sample every Nth node so we stay within maxWaypoints.
            // Always include the goal (index 0 in reverse order = first visited).
            u16 step = (pathLen > maxWaypoints) ? (pathLen / maxWaypoints) : 1;

            // Collect sampled indices (in reverse path order: goal → start).
            u16 sampledIndices[MAX_PATH_WAYPOINTS];
            u8  sampledCount = 0;

            idx = currentClosedIdx;
            u16 nodePos = 0;  // position along reversed path (0 = goal)
            while (idx != 0xFFFFu && sampledCount < maxWaypoints) {
                // Always keep goal (nodePos==0) and every step-th node after.
                if (nodePos == 0 || (nodePos % step) == 0) {
                    sampledIndices[sampledCount++] = idx;
                }
                idx = closedList[idx].parentIdx;
                ++nodePos;
            }

            // Ensure the goal is always the last waypoint presented to the caller
            // (outPath[0] = first step from start, outPath[n-1] = goal).
            // sampledIndices[0] is the goal; reverse into outPath.
            for (u8 i = 0; i < sampledCount; ++i) {
                u16 si = sampledIndices[sampledCount - 1u - i];
                outPath[i] = LevelGridSystem::gridToWorld(
                    grid, closedList[si].x, closedList[si].z);
            }

            // Override the very last entry with the exact goal world position
            // so entities don't stop slightly off-center from the target.
            outPath[sampledCount - 1u] = goal;

            return sampledCount;
        }

        // Expand 4-connected neighbors.
        for (int d = 0; d < 4; ++d) {
            s32 nx = static_cast<s32>(current.x) + dx[d];
            s32 nz = static_cast<s32>(current.z) + dz[d];

            if (nx < 0 || nz < 0) continue;
            u32 unx = static_cast<u32>(nx);
            u32 unz = static_cast<u32>(nz);

            if (!LevelGridSystem::isInBounds(grid, unx, unz)) continue;
            if (LevelGridSystem::isSolid(grid, unx, unz))     continue;

            u16 ng = current.g + 1u;
            u16 nh = manhattan(static_cast<u16>(unx), static_cast<u16>(unz),
                               static_cast<u16>(gx),  static_cast<u16>(gz));
            u16 nf = ng + nh;

            // Skip if already in closed list with a lower or equal g.
            bool inClosed = false;
            for (u16 c = 0; c < closedCount; ++c) {
                if (closedList[c].x == static_cast<u16>(unx) &&
                    closedList[c].z == static_cast<u16>(unz))
                {
                    inClosed = true;
                    break;
                }
            }
            if (inClosed) continue;

            // If already in open list with a better or equal g, skip.
            bool dominated = false;
            for (u16 o = 0; o < openCount; ++o) {
                if (openList[o].x == static_cast<u16>(unx) &&
                    openList[o].z == static_cast<u16>(unz))
                {
                    if (openList[o].g <= ng) {
                        dominated = true;
                    } else {
                        // Found a better path to this open node; update it.
                        openList[o].g         = ng;
                        openList[o].f         = nf;
                        openList[o].parentIdx = currentClosedIdx;
                        dominated = true;  // handled; don't add duplicate
                    }
                    break;
                }
            }
            if (dominated) continue;

            // Add neighbor to open list if budget allows.
            if (openCount < MAX_ASTAR_SEARCH) {
                openList[openCount++] = AStarNode{
                    static_cast<u16>(unx), static_cast<u16>(unz),
                    currentClosedIdx, ng, nf
                };
            }
        }
    }

    return 0;  // no path found within budget
}

} // namespace Pathfinder
