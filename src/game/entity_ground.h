#pragma once
// entity_ground.h — keeping a ground entity's feet on the floor.
//
// EXTRACTED TO BE TESTABLE. This is one short function that every enemy's position depends on, and
// it lived in enemy_ai.cpp, which pulls in combat, projectiles, squads and boss AI — so the test
// binary could not reach it and the rule that decides where every monster in the game stands had no
// test at all. Same reasoning that moved `atomicReplace` into platform/atomic_file.h: the primitive
// everything depends on is exactly the one that must be pinned.
//
// It is called from half a dozen places per frame, after XZ movement, to stop entities floating or
// sinking. Two early-outs decide whether it does anything, and BOTH are load-bearing — see the
// comments at each, and tests/game/test_entity_ground.cpp for what they cost.

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "world/level_grid.h"

// Snap a ground entity's Y to the floor height of its current grid cell.
//
// THE INVARIANT, which the first version did not have: a ground entity may be ABOVE its floor (a
// pad launch, a vault, a fall) but must never be left INSIDE it. Both early-outs used to `return`
// outright, so either one firing meant no vertical correction of any kind — including when the body
// was already under the world. That is "enemies sometimes drive inside the ground when attacking and
// moving towards the player": a crowd converging on the player shoves centres into rock (early-out
// 2) while movement leaves a scrap of Y velocity (early-out 1), and from that tick nothing pulls
// them back up. Pinned by tests/game/test_entity_ground.cpp, which failed on exactly these cases.
inline void snapEntityToFloor(Entity& e, const LevelGrid& grid) {
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(grid, e.position, gx, gz)) return;   // off-grid: no floor to know

    // Story-aware (see Collision::snapEntityToFloor): select the slab top vs the ground floor from
    // the entity's FEET, so enemies can climb ramps onto a balcony, stand under one, and drop off its
    // edge — the foundation of two-story chase. Identity off platform cells.
    const f32 feetY  = e.position.y - e.halfExtents.y;
    const f32 floorY = LevelGridSystem::effectiveFloorHeight(grid, gx, gz, feetY);
    const f32 restY  = floorY + e.halfExtents.y;

    // AIRBORNE ground enemies keep their arc — one riding a jump-pad launch must not be yanked back
    // down. This is the single choke point: snapEntityToFloor is called from half a dozen places
    // every frame, and any one of them cancelling the arc would make pads silently useless to
    // enemies. Knockback is XZ-only, so a non-zero Y velocity here means "mid-flight".
    //
    // A centre inside a SOLID cell is the other case that must not hard-snap: the cell's floor is
    // not the surface the body is standing on, so forcing the body onto it would teleport an enemy
    // pressed against a wall.
    //
    // Both now CLAMP instead of returning. Clamping is the weakest correction that still holds the
    // invariant — it never moves a body that is already at or above its floor, so the arc and the
    // wall-press are untouched, and it is the only thing standing between a shoved enemy and the
    // inside of the map.
    const bool airborne = !(e.flags & ENT_FLYING) && e.velocity.y != 0.0f;
    if (airborne || LevelGridSystem::isSolid(grid, gx, gz)) {
        if (e.position.y < restY) e.position.y = restY;
        return;
    }
    e.position.y = restY;
}
