#pragma once

#include "core/types.h"
#include "core/math.h"

// Cell flags
static constexpr u8 CELL_SOLID   = 1 << 0;
static constexpr u8 CELL_FLOOR   = 1 << 1;
static constexpr u8 CELL_CEILING = 1 << 2;

struct GridCell {
    u8 flags;            // CELL_SOLID / CELL_FLOOR / CELL_CEILING
    u8 floorHeight;      // quarter-units (multiply by 0.25 for metres)
    u8 ceilingHeight;    // quarter-units
    u8 wallMaterialId;   // material for wall surfaces
    u8 floorMaterialId;  // material for floor surface
    u8 ceilMaterialId;   // material for ceiling surface
};

struct LevelGrid {
    GridCell* cells = nullptr;
    u32 width  = 0;   // X cells
    u32 depth  = 0;   // Z cells
    f32 cellSize = 1.0f;

    // Flow field for NPC pathfinding toward the floor exit.
    // Each cell stores a direction index (0-7) pointing to the next cell
    // toward the exit, or 0xFF if unreachable.  Computed via BFS once
    // per floor after level generation.
    //   Directions: 0=+X, 1=+X+Z, 2=+Z, 3=-X+Z, 4=-X, 5=-X-Z, 6=-Z, 7=+X-Z
    u8* flowDir = nullptr;

    // Clearance field: per-cell Chebyshev (8-connected) step distance to the
    // nearest solid cell, clamped to 255.  Solid cells are 0; a floor cell that
    // touches a wall is 1 (≈0.5 m of slack at cellSize 1.0), and openness grows
    // outward from there.  Built once per floor and used by the pathfinder to
    // keep paths off wall faces (so a wide entity AABB doesn't clip corners) and
    // by the AI to escape if it ever wedges.  See buildClearanceField.
    u8* clearance = nullptr;
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

    // Build BFS flow field from exitWorldPos toward all reachable cells.
    // Each cell gets a direction (0-7) pointing one step closer to the exit.
    void buildFlowField(LevelGrid& grid, Vec3 exitWorldPos);

    // Build the per-cell clearance field (distance to nearest wall). Call once
    // per floor after the grid geometry is final. See LevelGrid::clearance.
    void buildClearanceField(LevelGrid& grid);

    // Clearance (cells-to-nearest-wall) at a grid cell; 0 if out of bounds or
    // before the field is built. Higher = more open space around the cell.
    u8 clearanceAt(const LevelGrid& grid, u32 x, u32 z);

    // Get the world-space direction an NPC at worldPos should move to follow
    // the flow field.  Returns zero vector if already at exit or unreachable.
    Vec3 flowDirection(const LevelGrid& grid, Vec3 worldPos);
}

// Tactical spatial queries for enemy AI
namespace LevelGridQuery {
    // Find nearest walkable cell with cover from threatPos (wall blocks LOS).
    // BFS from 'from', max 8-cell radius. Returns false if none found.
    bool findCoverCell(const LevelGrid& grid, Vec3 from, Vec3 threatPos,
                       Vec3& outPos, f32 maxRadius = 8.0f);

    // Find a walkable cell at ~90-120 deg offset from target-entity line.
    // Used for flanking. preferRight hints search direction.
    bool findFlankCell(const LevelGrid& grid, Vec3 entityPos, Vec3 targetPos,
                       f32 attackRange, bool preferRight, Vec3& outPos);

    // Find doorway cells in a room (perimeter cells adjacent to corridor).
    // Returns count found (up to maxResults).
    u8 findDoorwayCells(const LevelGrid& grid, u32 roomX, u32 roomZ,
                        u32 roomW, u32 roomD, Vec3* outPositions, u8 maxResults = 4);

    // Calculate spread position for SURROUND state.
    Vec3 getSurroundPosition(Vec3 targetPos, u8 slotIndex, u8 totalSlots, f32 radius);
}
