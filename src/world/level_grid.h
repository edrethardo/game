#pragma once

#include "core/types.h"
#include "core/math.h"

// Cell flags
static constexpr u8 CELL_SOLID   = 1 << 0;
static constexpr u8 CELL_FLOOR   = 1 << 1;
static constexpr u8 CELL_CEILING = 1 << 2;

struct GridCell {
    u8 flags;          // CELL_SOLID / CELL_FLOOR / CELL_CEILING
    u8 floorHeight;    // quarter-units (multiply by 0.25 for metres)
    u8 ceilingHeight;  // quarter-units
    u8 materialId;     // texture index for meshing
};

struct LevelGrid {
    GridCell* cells = nullptr;
    u32 width  = 0;   // X cells
    u32 depth  = 0;   // Z cells
    f32 cellSize = 1.0f;
};

namespace LevelGridSystem {
    void init(LevelGrid& grid, u32 width, u32 depth, f32 cellSize = 1.0f);
    void shutdown(LevelGrid& grid);

    GridCell&       getCell(LevelGrid& grid, u32 x, u32 z);
    const GridCell& getCell(const LevelGrid& grid, u32 x, u32 z);

    bool worldToGrid(const LevelGrid& grid, Vec3 worldPos, u32& outX, u32& outZ);
    Vec3 gridToWorld(const LevelGrid& grid, u32 x, u32 z);   // cell centre

    bool isSolid(const LevelGrid& grid, u32 x, u32 z);
    f32  getFloorHeight(const LevelGrid& grid, u32 x, u32 z);
    f32  getCeilingHeight(const LevelGrid& grid, u32 x, u32 z);
    bool isInBounds(const LevelGrid& grid, u32 x, u32 z);
}
