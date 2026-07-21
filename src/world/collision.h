#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

struct Player;

// Player collider half-extents (metres)
static constexpr f32 PLAYER_HALF_WIDTH  = 0.3f;  // 0.6m total, fits through 1m cells
static constexpr f32 PLAYER_HEIGHT      = 1.8f;
static constexpr f32 PLAYER_HALF_HEIGHT = PLAYER_HEIGHT * 0.5f;

// Gravity is integrated EXACTLY ONCE per tick, here in moveAndSlide (the physics integrator, which
// also zeroes velocity.y on landing / ceiling). It used to be applied a SECOND time inside
// applyMovement (player.cpp) every airborne tick, so the real, felt gravity was -40 while this
// constant said -20 — the jump reached only ~0.8 m / 0.4 s instead of the 1.6 m / 0.8 s the pair
// (JUMP_SPEED 8, GRAVITY -20) described. The redundant application is gone; -40 here preserves the
// low-and-tight arc players actually had (apex v²/2|g| = 0.8 m, air time 2v/|g| = 0.4 s). Player-
// only: particles and PROJ_GRAVITY projectiles carry their own constants.
static constexpr f32 GRAVITY      = -40.0f; // m/s²  (single-applied; see note above)
static constexpr f32 JUMP_SPEED   =   8.0f; // m/s   (apex 0.8 m, air time 0.4 s at GRAVITY -40)

// Max height a body may WALK up onto a CELL_LEDGE cell. A ledge whose floor sits more than this above
// the body's feet is refused (treated as a wall) until the body jumps high enough to clear it — the
// Quake-style "hop onto the platform" gate. Only CELL_LEDGE cells are gated; plain raised floors keep
// the unlimited walk-up. 0.4 m < the 0.8 m jump apex, so a ledge up to ~0.75 m is reachable by jumping.
static constexpr f32 STEP_UP_HEIGHT = 0.4f;

// Upward launch speed of a CELL_JUMPPAD (m/s). Apex v²/2|g| = 17²/80 = 3.6 m — over 4× the 0.8 m walk
// jump, enough to reach the 3.0 m arena crown when launched from the 1.5 m tower ring, and to pop onto
// the 1.5 m tier from the ground pads. Just under the 4 m perimeter-wall height, so a ground launch
// never clears the arena. Applied as a velocity.y impulse (like JUMP_SPEED) inside moveAndSlide, so it
// replicates in co-op with no wire change — see CELL_JUMPPAD.
static constexpr f32 JUMPPAD_LAUNCH = 17.0f;

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

    // True if an AABB (feetPos, halfWidth in XZ, feet at feetPos.y) would enter a CELL_LEDGE cell
    // whose floor sits more than STEP_UP_HEIGHT above the feet — a Quake-style ledge that must be
    // JUMPED onto. moveAndSlide / entityMoveAndSlide treat that as a wall so a body can't stroll up
    // it; once a jump lifts the feet within STEP_UP_HEIGHT of the ledge floor the move is allowed.
    bool overlapsLedgeAbove(Vec3 feetPos, f32 halfWidth, const LevelGrid& grid);

    // True if a body with feet at feetPos is standing ON a CELL_JUMPPAD cell — any overlapped pad
    // cell whose floor sits within STEP_UP_HEIGHT below the feet (i.e. the pad the body is resting on,
    // not one under a higher platform it's on). moveAndSlide calls this at the end of a grounded step
    // and, if true, replaces velocity.y with JUMPPAD_LAUNCH (the Quake launch). See CELL_JUMPPAD.
    // Launch speed (m/s) of the CELL_JUMPPAD the body is RESTING on, or 0 if it is not on one.
    // Reads GridCell::jumpPadQ so a map can author stronger pads than JUMPPAD_LAUNCH.
    f32 jumpPadSpeed(Vec3 feetPos, f32 halfWidth, const LevelGrid& grid);

    // True if an AABB (feet at feetPos, halfWidth in XZ, PLAYER_HEIGHT tall) would CLIP a
    // CELL_PLATFORM slab band [underside, top]: the body is neither stepping ONTO the slab (feet
    // within STEP_UP_HEIGHT of the top — slab stairs, jump landings) nor passing fully BENEATH it
    // (head clear of the underside). moveAndSlide treats that as a wall on the X/Z axes, so a
    // too-high slab can't be walked up onto and a body can't wedge into a slab edge mid-jump.
    bool overlapsPlatformBand(Vec3 feetPos, f32 halfWidth, const LevelGrid& grid);

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
