#include "world/level_grid.h"
#include "core/assert.h"
#include "core/log.h"
#include <cstdlib>
#include <cstring>

void LevelGridSystem::init(LevelGrid& grid, u32 width, u32 depth, f32 cellSize) {
    ENGINE_ASSERT(width > 0 && depth > 0, "Grid dimensions must be > 0");
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
