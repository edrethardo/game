#pragma once

#include "core/types.h"
#include "core/math.h"

// Cell flags
static constexpr u8 CELL_SOLID   = 1 << 0;
static constexpr u8 CELL_FLOOR   = 1 << 1;
static constexpr u8 CELL_CEILING = 1 << 2;
// A raised floor that must be JUMPED onto — collision refuses to walk a body up onto it from more
// than STEP_UP_HEIGHT below (Collision::COLLISION step-up gate), so it reads as a Quake-style ledge
// you hop to rather than a ramp you stroll up. ONLY cells explicitly marked get this; every existing
// level and every walkable tier stays on the unlimited walk-up path, so it is zero-regression.
static constexpr u8 CELL_LEDGE   = 1 << 3;
// A Quake/Combat-Hall JUMP PAD: a walkable floor cell that flings a grounded body upward. Collision
// fires an upward velocity impulse (Collision::JUMPPAD_LAUNCH) whenever a body is resting/landing on
// the cell — you can't stand on it, you get launched, and air-steer the arc onto a higher tier. The
// launch is a pure velocity.y impulse applied inside moveAndSlide (the one path every peer funnels
// through), exactly like the jump, so it replicates in co-op with NO wire change (posY + onGround are
// already snapshotted; the trigger is deterministic geometry on both client and server). Opt-in per
// cell → zero regression. Enemies never jump, so a pad in an enemy route is a dead end — PvP-only.
static constexpr u8 CELL_JUMPPAD = 1 << 4;

// A PLATFORM SLAB: a second walkable story floating above this cell's normal floor. The cell keeps
// its base floor (CELL_FLOOR + floorHeight) as the GROUND story — bodies below walk UNDER the slab
// (its underside is their local ceiling), bodies above walk ON it. Real two-story, unlike
// CELL_LEDGE risers, which are solid underneath. Consumed by exactly three chokes:
// Collision::moveAndSlide (story selection + underside head clamp), Raycast::cast (top/underside/
// rim planes — nothing shoots through a balcony floor), and the level mesher (top/underside/rim
// quads). The grid is rebuilt from the seed on every peer, so slabs replicate in co-op with NO
// wire change — the jump-pad story. Enemies never jump, so AI keeps navigating the ground story
// and simply walks under platforms (PvP-only above, the pads/ledges policy).
static constexpr u8 CELL_PLATFORM = 1 << 5;

// Slab thickness in quarter-units (0.5 m). Underside = platHeight - this, clamped to floorHeight,
// so a low stair step degrades gracefully into riser-like geometry with no dead under-space.
static constexpr u8 PLATFORM_THICKNESS_Q = 2;

// Feet within this many metres below a slab top interact with the SLAB story (walk on / step up);
// further below and they belong to the ground story. Must equal Collision::STEP_UP_HEIGHT (the
// ledge step gate) — collision.cpp static_asserts the pair; it lives here because collision.h
// includes this header, not the other way round.
static constexpr f32 PLATFORM_STEP_TOLERANCE = 0.4f;

// Up to 3 walk-under slabs per cell → 4 walkable stories. platHeight[] is STRICTLY ASCENDING by top
// height; slots >= platCount MUST be zero (canonical byte-form: GridCell is calloc'd per floor and
// never serialized, so the test_level_gen determinism memcmp compares raw bytes). CELL_PLATFORM is
// set iff platCount > 0.
static constexpr u8 MAX_PLATFORMS_PER_CELL = 3;

struct GridCell {
    u8 flags;            // CELL_SOLID / CELL_FLOOR / CELL_CEILING / CELL_PLATFORM / ...
    u8 floorHeight;      // quarter-units (multiply by 0.25 for metres)
    u8 ceilingHeight;    // quarter-units
    u8 wallMaterialId;   // material for wall surfaces
    u8 floorMaterialId;  // material for floor surface
    u8 ceilMaterialId;   // material for ceiling surface
    u8 platCount;                              // number of slabs, 0..MAX_PLATFORMS_PER_CELL
    u8 platHeight[MAX_PLATFORMS_PER_CELL];     // slab TOP surfaces, quarter-units, STRICTLY ASCENDING
    u8 platMaterialId[MAX_PLATFORMS_PER_CELL]; // per-slab top + underside material
};
static_assert(sizeof(GridCell) == 13,
    "GridCell must stay 13 all-u8 bytes: calloc'd per floor, never serialized; a size change silently "
    "breaks the test_level_gen determinism memcmp and any future grid memcpy");

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
    // CELL_PLATFORM (two-story) queries. hasPlatform is the gate; the getters assume it passed.
    bool hasPlatform(const LevelGrid& grid, u32 x, u32 z);
    f32  getPlatformTop(const LevelGrid& grid, u32 x, u32 z);
    f32  getPlatformUnderside(const LevelGrid& grid, u32 x, u32 z);
    // Slab AUTHORERS (write paths, operating on a cell reference). setPlatform = REPLACE-to-single:
    // every legacy single-slab writer routes here so its double-write junctions collapse to one slab
    // and shipped geometry stays byte-identical. Keeps the ascending + canonical-byte-form invariants.
    void setPlatform(GridCell& c, u8 topQ, u8 mat);
    void addPlatform(GridCell& c, u8 topQ, u8 mat);   // ACCUMULATE (FOUR_STORY generator only)
    void removePlatform(GridCell& c, u8 topQ);        // build-time hole puncher (drop-holes)
    u8   platformCount(const LevelGrid& grid, u32 x, u32 z);              // slab count; multi-slab loop bound
    f32  getPlatformTop(const LevelGrid& grid, u32 x, u32 z, u8 i);       // indexed slab top
    f32  getPlatformUnderside(const LevelGrid& grid, u32 x, u32 z, u8 i); // indexed slab underside
    // THE story selector: the walkable floor a body with feet at feetY interacts with in this
    // cell. Slab cells return the slab top for feet within PLATFORM_STEP_TOLERANCE below it
    // (walking the balcony, stepping up a slab stair), else the base floor (walking the arcade
    // beneath). Every consumer that means "the floor under THIS body" must use this, never raw
    // getFloorHeight, or bodies under a balcony get teleport-snapped up through it.
    f32  effectiveFloorHeight(const LevelGrid& grid, u32 x, u32 z, f32 feetY);
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
