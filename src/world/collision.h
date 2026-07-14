#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

struct Player;

// Player collider half-extents (metres)
static constexpr f32 PLAYER_HALF_WIDTH  = 0.3f;  // 0.6m total, fits through 1m cells
static constexpr f32 PLAYER_HEIGHT      = 1.8f;
static constexpr f32 PLAYER_HALF_HEIGHT = PLAYER_HEIGHT * 0.5f;

static constexpr f32 GRAVITY      = -20.0f; // m/s²
static constexpr f32 JUMP_SPEED   =   8.0f; // m/s

// Axis-aligned obstacle for entity collision during player movement.
// Built from active entities each frame and passed to moveAndSlide.
struct CollisionObstacle {
    Vec3 position;     // centre of AABB
    Vec3 halfExtents;
};

namespace Collision {
    // Moves player.position by player.velocity*dt against the level grid.
    // Updates player.onGround, zeroes blocked velocity components.
    void moveAndSlide(Player& player, const LevelGrid& grid, f32 dt);

    // Overload that also blocks XZ movement against entity obstacles.
    // Player slides along entity AABBs the same way they slide along walls.
    void moveAndSlide(Player& player, const LevelGrid& grid, f32 dt,
                      const CollisionObstacle* obstacles, u32 obstacleCount);

    // Check if an AABB (centre ± halfExtents) overlaps any solid grid cell.
    bool entityOverlapsGrid(Vec3 centre, Vec3 halfExtents, const LevelGrid& grid);

    // Snap an entity's Y to the floor of its current cell (if walkable).
    void snapEntityToFloor(Vec3& position, Vec3 halfExtents, const LevelGrid& grid);

    // Nudge an entity out of walls by spiral-searching nearby cell centres.
    // Call after spawning to guarantee the entity isn't embedded in geometry.
    void ensureNotInWall(Vec3& position, Vec3 halfExtents, const LevelGrid& grid);

    // Apply an XZ displacement to `position`, but ONLY if doing so would not push the body INTO
    // solid geometry. Returns true if the push was applied.
    //
    // This is the rule for every push that moves a body without going through moveAndSlide —
    // enemy-vs-player crowding, co-op partner separation. Those used to write position directly and
    // rely on a wall push-out afterwards to repair the damage, which cannot work: once a body has
    // been shoved inside a wall, the ejection pass must GUESS an exit axis, and while the push keeps
    // being re-applied every frame (an enemy leaning on a cornered player) push and eject fight each
    // other — the player jitters, and through a thin wall they pop out the far side. Validate the
    // push instead of repairing it.
    //
    // A push made while ALREADY overlapping geometry is allowed through: refusing it would strand a
    // body that spawned or teleported inside a wall. This function only refuses pushes that would
    // CREATE an overlap.
    bool tryPushXZ(Vec3& position, Vec3 halfExtents, const LevelGrid& grid, f32 dx, f32 dz);
}
