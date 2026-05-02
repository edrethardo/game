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
