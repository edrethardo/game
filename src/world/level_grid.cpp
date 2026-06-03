#include "world/level_grid.h"
#include "world/raycast.h"
#include "core/assert.h"
#include "core/log.h"
#include <cstdlib>
#include <cstring>
#include <cmath>

void LevelGridSystem::init(LevelGrid& grid, u32 width, u32 depth, f32 cellSize) {
    ENGINE_ASSERT(width > 0 && depth > 0, "Grid dimensions must be > 0");
    // Free previous allocations if re-initializing (floor descent)
    if (grid.cells)     { std::free(grid.cells);     grid.cells     = nullptr; }
    if (grid.flowDir)   { std::free(grid.flowDir);   grid.flowDir   = nullptr; }
    if (grid.clearance) { std::free(grid.clearance); grid.clearance = nullptr; }
    grid.width    = width;
    grid.depth    = depth;
    grid.cellSize = cellSize;
    grid.cells    = static_cast<GridCell*>(std::calloc(width * depth, sizeof(GridCell)));
    ENGINE_ASSERT(grid.cells, "LevelGrid allocation failed");
    LOG_INFO("LevelGrid init: %ux%u cells (%.0f KB)", width, depth,
             (width * depth * sizeof(GridCell)) / 1024.0f);
}

void LevelGridSystem::shutdown(LevelGrid& grid) {
    if (grid.cells) { std::free(grid.cells); grid.cells = nullptr; }
    if (grid.flowDir) { std::free(grid.flowDir); grid.flowDir = nullptr; }
    if (grid.clearance) { std::free(grid.clearance); grid.clearance = nullptr; }
    grid.width = grid.depth = 0;
}

bool LevelGridSystem::isInBounds(const LevelGrid& grid, u32 x, u32 z) {
    return x < grid.width && z < grid.depth;
}

GridCell& LevelGridSystem::getCell(LevelGrid& grid, u32 x, u32 z) {
    ENGINE_ASSERT(isInBounds(grid, x, z), "Grid cell out of bounds");
    return grid.cells[z * grid.width + x];
}

const GridCell& LevelGridSystem::getCell(const LevelGrid& grid, u32 x, u32 z) {
    ENGINE_ASSERT(isInBounds(grid, x, z), "Grid cell out of bounds");
    return grid.cells[z * grid.width + x];
}

bool LevelGridSystem::worldToGrid(const LevelGrid& grid, Vec3 pos, u32& outX, u32& outZ) {
    f32 fx = pos.x / grid.cellSize;
    f32 fz = pos.z / grid.cellSize;
    if (fx < 0 || fz < 0) return false;
    outX = static_cast<u32>(fx);
    outZ = static_cast<u32>(fz);
    return isInBounds(grid, outX, outZ);
}

Vec3 LevelGridSystem::gridToWorld(const LevelGrid& grid, u32 x, u32 z) {
    return {
        (x + 0.5f) * grid.cellSize,
        0.0f,
        (z + 0.5f) * grid.cellSize
    };
}

bool LevelGridSystem::isSolid(const LevelGrid& grid, u32 x, u32 z) {
    if (!isInBounds(grid, x, z)) return true; // treat OOB as solid
    return (grid.cells[z * grid.width + x].flags & CELL_SOLID) != 0;
}

f32 LevelGridSystem::getFloorHeight(const LevelGrid& grid, u32 x, u32 z) {
    if (!isInBounds(grid, x, z)) return 0.0f;
    return grid.cells[z * grid.width + x].floorHeight * 0.25f;
}

f32 LevelGridSystem::getCeilingHeight(const LevelGrid& grid, u32 x, u32 z) {
    if (!isInBounds(grid, x, z)) return 4.0f;
    return grid.cells[z * grid.width + x].ceilingHeight * 0.25f;
}

// 8-directional neighbor offsets: 0=+X, 1=+X+Z, 2=+Z, 3=-X+Z, 4=-X, 5=-X-Z, 6=-Z, 7=+X-Z
static constexpr s32 kDirDx[] = { 1, 1, 0,-1,-1,-1, 0, 1};
static constexpr s32 kDirDz[] = { 0, 1, 1, 1, 0,-1,-1,-1};

void LevelGridSystem::buildFlowField(LevelGrid& grid, Vec3 exitWorldPos) {
    u32 totalCells = grid.width * grid.depth;

    // Allocate or reuse flow field
    if (!grid.flowDir) {
        grid.flowDir = static_cast<u8*>(std::malloc(totalCells));
    }
    std::memset(grid.flowDir, 0xFF, totalCells); // 0xFF = unreachable

    // Find the exit cell
    u32 exitX, exitZ;
    if (!worldToGrid(grid, exitWorldPos, exitX, exitZ)) {
        LOG_WARN("buildFlowField: exit position out of bounds");
        return;
    }

    // BFS from the exit cell outward — each visited cell stores the direction
    // that points TOWARD the exit (reverse of the expansion direction).
    // Use a simple queue backed by a stack-allocated array (grid is small: 48x48 = 2304).
    static constexpr u32 MAX_QUEUE = 48 * 48;
    u32 queue[MAX_QUEUE];
    u32 qHead = 0, qTail = 0;

    // Seed the exit cell
    grid.flowDir[exitZ * grid.width + exitX] = 0xFE; // 0xFE = "at exit"
    queue[qTail++] = exitZ * grid.width + exitX;

    while (qHead < qTail) {
        u32 idx = queue[qHead++];
        u32 cx = idx % grid.width;
        u32 cz = idx / grid.width;

        // Expand to 4 cardinal neighbors only (no diagonals).
        // Diagonal BFS steps cause NPCs to clip wall corners in 1-cell
        // corridors because the entity AABB is nearly 1 cell wide.
        for (u8 d = 0; d < 8; d += 2) { // 0=+X, 2=+Z, 4=-X, 6=-Z
            s32 nx = static_cast<s32>(cx) + kDirDx[d];
            s32 nz = static_cast<s32>(cz) + kDirDz[d];
            if (nx < 0 || nz < 0 || static_cast<u32>(nx) >= grid.width ||
                static_cast<u32>(nz) >= grid.depth) continue;

            u32 nIdx = static_cast<u32>(nz) * grid.width + static_cast<u32>(nx);
            if (grid.flowDir[nIdx] != 0xFF) continue; // already visited
            // Walkable cells have CELL_FLOOR set; everything else is a wall
            if (!(grid.cells[nIdx].flags & CELL_FLOOR)) continue;

            // Store the REVERSE direction (points back toward the exit).
            // d goes from (cx,cz)->(nx,nz); reverse is (d+4)%8.
            grid.flowDir[nIdx] = (d + 4) % 8;
            queue[qTail++] = nIdx;
        }
    }

    u32 reachable = 0;
    for (u32 i = 0; i < totalCells; i++) {
        if (grid.flowDir[i] != 0xFF) reachable++;
    }
    LOG_INFO("Flow field built: %u/%u cells reachable from exit (%u,%u)",
             reachable, totalCells, exitX, exitZ);
}

u8 LevelGridSystem::clearanceAt(const LevelGrid& grid, u32 x, u32 z) {
    if (!grid.clearance || !isInBounds(grid, x, z)) return 0;
    return grid.clearance[z * grid.width + x];
}

void LevelGridSystem::buildClearanceField(LevelGrid& grid) {
    u32 totalCells = grid.width * grid.depth;
    if (totalCells == 0) return;

    if (!grid.clearance) {
        grid.clearance = static_cast<u8*>(std::malloc(totalCells));
    }

    // Multi-source BFS (8-connected) seeded from every solid cell with dist 0.
    // Each floor cell ends up holding its Chebyshev step-distance to the nearest
    // wall, so we get "how many cells of slack on the tightest side." Chebyshev
    // (vs Manhattan) is the right metric: a diagonal wall is one step away, which
    // matches how an axis-aligned AABB actually clips a diagonal corner.
    // Each cell is enqueued at most once (FIFO BFS finalises distance on first
    // visit), so a queue of totalCells entries is sufficient.
    u32* queue = static_cast<u32*>(std::malloc(sizeof(u32) * totalCells));
    if (!queue) return;
    u32 qHead = 0, qTail = 0;

    // Seed: solids are distance 0 and are the BFS frontier.
    for (u32 i = 0; i < totalCells; i++) {
        if (grid.cells[i].flags & CELL_SOLID) {
            grid.clearance[i] = 0;
            queue[qTail++] = i;
        } else {
            grid.clearance[i] = 0xFF; // unvisited sentinel
        }
    }

    while (qHead < qTail) {
        u32 idx = queue[qHead++];
        u32 cx = idx % grid.width;
        u32 cz = idx / grid.width;
        u8  d  = grid.clearance[idx];
        u8  nd = (d < 254) ? static_cast<u8>(d + 1) : 254; // clamp before 0xFF

        // Expand to all 8 neighbours — a diagonally-adjacent wall still pinches
        // a wide AABB, so it must count toward clearance.
        for (u8 dir = 0; dir < 8; dir++) {
            s32 nx = static_cast<s32>(cx) + kDirDx[dir];
            s32 nz = static_cast<s32>(cz) + kDirDz[dir];
            if (nx < 0 || nz < 0 || static_cast<u32>(nx) >= grid.width ||
                static_cast<u32>(nz) >= grid.depth) continue;
            u32 nIdx = static_cast<u32>(nz) * grid.width + static_cast<u32>(nx);
            if (grid.clearance[nIdx] <= nd) continue; // already closer to a wall
            grid.clearance[nIdx] = nd;
            queue[qTail++] = nIdx;
        }
    }

    std::free(queue);
}

Vec3 LevelGridSystem::flowDirection(const LevelGrid& grid, Vec3 worldPos) {
    if (!grid.flowDir) return {0, 0, 0};

    u32 gx, gz;
    if (!worldToGrid(grid, worldPos, gx, gz)) return {0, 0, 0};

    u8 dir = grid.flowDir[gz * grid.width + gx];
    if (dir >= 8) return {0, 0, 0}; // at exit (0xFE) or unreachable (0xFF)

    // Target = centre of the next cell along the flow.  Steering toward
    // the cell centre keeps NPCs away from walls and prevents their AABB
    // from clipping into adjacent solid cells in tight corridors.
    u32 nextX = static_cast<u32>(static_cast<s32>(gx) + kDirDx[dir]);
    u32 nextZ = static_cast<u32>(static_cast<s32>(gz) + kDirDz[dir]);
    f32 targetX = (nextX + 0.5f) * grid.cellSize;
    f32 targetZ = (nextZ + 0.5f) * grid.cellSize;

    f32 dx = targetX - worldPos.x;
    f32 dz = targetZ - worldPos.z;
    f32 len = sqrtf(dx * dx + dz * dz);
    if (len < 0.01f) return {0, 0, 0};
    return {dx / len, 0.0f, dz / len};
}

// ---------------------------------------------------------------------------
// LevelGridQuery — tactical spatial queries used by the enemy AI system.
// These run infrequently (once per AI decision, not every tick) so BFS over
// a bounded 64×64 window is acceptable.
// ---------------------------------------------------------------------------

bool LevelGridQuery::findCoverCell(const LevelGrid& grid, Vec3 from, Vec3 threatPos,
                                   Vec3& outPos, f32 maxRadius) {
    using namespace LevelGridSystem;

    u32 startX, startZ;
    if (!worldToGrid(grid, from, startX, startZ)) return false;

    // Small BFS queue: bounded to 256 entries (plenty for an 8-cell radius).
    static constexpr u32 MAX_Q = 256;
    // Pack (x, z) as u32 to save space in the queue.
    u32 queue[MAX_Q];
    u32 qHead = 0, qTail = 0;

    // Visited buffer: 64×64 centered on start cell.
    // Offset applied so array index = (z - startZ + 32) * 64 + (x - startX + 32).
    static constexpr s32 HALF = 32;
    static constexpr u32 VIS_W = 64;
    bool visited[VIS_W * VIS_W] = {};

    auto visIdx = [&](s32 x, s32 z) -> s32 {
        s32 lx = x - static_cast<s32>(startX) + HALF;
        s32 lz = z - static_cast<s32>(startZ) + HALF;
        if (lx < 0 || lz < 0 || lx >= static_cast<s32>(VIS_W) || lz >= static_cast<s32>(VIS_W))
            return -1;
        return lz * static_cast<s32>(VIS_W) + lx;
    };

    auto enqueue = [&](u32 x, u32 z) {
        s32 vi = visIdx(static_cast<s32>(x), static_cast<s32>(z));
        if (vi < 0 || visited[vi]) return;
        visited[vi] = true;
        if (qTail < MAX_Q) queue[qTail++] = (z << 16) | x;
    };

    enqueue(startX, startZ);

    // Threat eye height: cast rays at mid-body level.
    Vec3 threatEye = {threatPos.x, threatPos.y + 0.5f, threatPos.z};

    while (qHead < qTail) {
        u32 packed = queue[qHead++];
        u32 cx = packed & 0xFFFF;
        u32 cz = packed >> 16;

        // Skip the start cell itself — we're looking for a different position.
        if (cx == startX && cz == startZ) {
            // Still expand neighbors from start.
        } else {
            // Range check: reject cells beyond maxRadius.
            f32 dx = static_cast<f32>(cx) - static_cast<f32>(startX);
            f32 dz = static_cast<f32>(cz) - static_cast<f32>(startZ);
            if (dx * dx + dz * dz > maxRadius * maxRadius) continue;

            // Must be walkable (has floor, not solid).
            if (isSolid(grid, cx, cz) || !(getCell(grid, cx, cz).flags & CELL_FLOOR))
                goto expand;

            {
                // Candidate world position at eye height.
                Vec3 cellWorld = gridToWorld(grid, cx, cz);
                Vec3 candidateEye = {cellWorld.x, cellWorld.y + 0.5f, cellWorld.z};

                // Check LOS from candidate to threat.
                Vec3 toThreat = {threatEye.x - candidateEye.x,
                                 threatEye.y - candidateEye.y,
                                 threatEye.z - candidateEye.z};
                f32 dist = sqrtf(toThreat.x * toThreat.x + toThreat.y * toThreat.y +
                                 toThreat.z * toThreat.z);

                if (dist > 0.01f) {
                    RayHit hit = Raycast::cast(grid, candidateEye, toThreat, dist);
                    // Cover: a wall blocks LOS before reaching the threat (with 0.5 m margin).
                    if (hit.hit && hit.distance < dist - 0.5f) {
                        outPos = cellWorld;
                        return true;
                    }
                }
            }
        }

        expand:
        // 4-cardinal BFS expansion only (avoids diagonal wall-clips).
        static constexpr s32 dx4[] = { 1,-1, 0, 0 };
        static constexpr s32 dz4[] = { 0, 0, 1,-1 };
        for (u32 i = 0; i < 4; i++) {
            s32 nx = static_cast<s32>(cx) + dx4[i];
            s32 nz = static_cast<s32>(cz) + dz4[i];
            if (nx < 0 || nz < 0) continue;
            if (!isInBounds(grid, static_cast<u32>(nx), static_cast<u32>(nz))) continue;
            enqueue(static_cast<u32>(nx), static_cast<u32>(nz));
        }
    }

    return false;
}

bool LevelGridQuery::findFlankCell(const LevelGrid& grid, Vec3 entityPos, Vec3 targetPos,
                                   f32 attackRange, bool preferRight, Vec3& outPos) {
    using namespace LevelGridSystem;

    // Angle from target back to entity (base direction for offset candidates).
    f32 baseAngle = atan2f(entityPos.z - targetPos.z, entityPos.x - targetPos.x);

    // Try offsets at 90–120° from the target→entity line. Check preferred
    // side first, then mirror. Angles in radians.
    static constexpr f32 kOffsets[] = {
        1.5708f,  // 90°
        1.8326f,  // 105°
        2.0944f,  // 120°
        1.3090f,  // 75°
        1.0472f,  // 60°
    };
    static constexpr u32 kNumOffsets = 5;

    // Build candidate angle list: preferred side first, then mirror.
    f32 candidates[kNumOffsets * 2];
    for (u32 i = 0; i < kNumOffsets; i++) {
        f32 sign = preferRight ? -1.0f : 1.0f; // right = clockwise = negative in XZ
        candidates[i]              = baseAngle + sign * kOffsets[i];
        candidates[kNumOffsets + i] = baseAngle - sign * kOffsets[i];
    }

    Vec3 targetEye = {targetPos.x, targetPos.y + 0.5f, targetPos.z};

    for (u32 i = 0; i < kNumOffsets * 2; i++) {
        f32 angle = candidates[i];
        Vec3 candidate = {
            targetPos.x + cosf(angle) * attackRange,
            targetPos.y,
            targetPos.z + sinf(angle) * attackRange
        };

        u32 gx, gz;
        if (!worldToGrid(grid, candidate, gx, gz)) continue;
        if (isSolid(grid, gx, gz)) continue;
        if (!(getCell(grid, gx, gz).flags & CELL_FLOOR)) continue;

        // Confirm LOS from candidate to target so the entity can actually attack.
        Vec3 candidateEye = {candidate.x, candidate.y + 0.5f, candidate.z};
        Vec3 toTarget = {targetEye.x - candidateEye.x,
                         targetEye.y - candidateEye.y,
                         targetEye.z - candidateEye.z};
        f32 dist = sqrtf(toTarget.x * toTarget.x + toTarget.y * toTarget.y +
                         toTarget.z * toTarget.z);

        if (dist < 0.01f) continue;
        RayHit hit = Raycast::cast(grid, candidateEye, toTarget, dist);
        if (!hit.hit) {
            // Clear LOS to target — valid flank position.
            outPos = gridToWorld(grid, gx, gz);
            return true;
        }
    }

    return false;
}

u8 LevelGridQuery::findDoorwayCells(const LevelGrid& grid, u32 roomX, u32 roomZ,
                                    u32 roomW, u32 roomD,
                                    Vec3* outPositions, u8 maxResults) {
    using namespace LevelGridSystem;

    u8 found = 0;

    // Iterate over all perimeter cells of the room AABB.
    // A doorway = non-solid perimeter cell that has at least one non-solid
    // neighbor outside the room bounds (corridor/hallway connection).
    for (u32 z = roomZ; z < roomZ + roomD && found < maxResults; z++) {
        for (u32 x = roomX; x < roomX + roomW && found < maxResults; x++) {
            // Only process perimeter cells.
            bool onPerimeter = (x == roomX || x == roomX + roomW - 1 ||
                                z == roomZ || z == roomZ + roomD - 1);
            if (!onPerimeter) continue;
            if (!isInBounds(grid, x, z)) continue;
            if (isSolid(grid, x, z)) continue; // solid perimeter = wall, not doorway

            // Check if any orthogonal neighbor lies outside the room and is walkable.
            static constexpr s32 dx4[] = { 1,-1, 0, 0 };
            static constexpr s32 dz4[] = { 0, 0, 1,-1 };
            for (u32 d = 0; d < 4; d++) {
                s32 nx = static_cast<s32>(x) + dx4[d];
                s32 nz = static_cast<s32>(z) + dz4[d];
                if (nx < 0 || nz < 0) continue;
                u32 unx = static_cast<u32>(nx);
                u32 unz = static_cast<u32>(nz);

                // Must be outside the room footprint.
                bool outsideRoom = (unx < roomX || unx >= roomX + roomW ||
                                    unz < roomZ || unz >= roomZ + roomD);
                if (!outsideRoom) continue;
                if (!isInBounds(grid, unx, unz)) continue;

                // Outside neighbor is walkable → this perimeter cell is a doorway.
                if (!isSolid(grid, unx, unz) && (getCell(grid, unx, unz).flags & CELL_FLOOR)) {
                    outPositions[found++] = gridToWorld(grid, x, z);
                    break; // one match per perimeter cell is enough
                }
            }
        }
    }

    return found;
}

Vec3 LevelGridQuery::getSurroundPosition(Vec3 targetPos, u8 slotIndex, u8 totalSlots,
                                         f32 radius) {
    // Evenly distribute slots around the target on the XZ plane.
    // totalSlots == 0 guard prevents divide-by-zero.
    if (totalSlots == 0) return targetPos;
    f32 angle = (2.0f * 3.14159265f * static_cast<f32>(slotIndex)) /
                static_cast<f32>(totalSlots);
    return {
        targetPos.x + cosf(angle) * radius,
        targetPos.y,
        targetPos.z + sinf(angle) * radius
    };
}
