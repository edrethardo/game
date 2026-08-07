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
    const bool inRock   = LevelGridSystem::isSolid(grid, gx, gz);

    if (inRock) {
        // A CENTRE INSIDE ROCK IS NOT A HEIGHT. The cell's own floorHeight is the height of the
        // ground under a wall, which is not the surface the body is standing on — and a big body
        // (The Merge Conflict is the widest hitbox in the acts) sits with its centre over a clump
        // while most of it is still on open floor. Clamping to the wall's own floor left exactly
        // those enemies embedded: sunk, and out of attack range in Y, so they stopped dealing damage
        // as well — reported as "the merge conflicts are just sinking in and not even doing damage".
        //
        // Take the HIGHEST floor among the orthogonal neighbours that are actually walkable: that is
        // the surface the body is overhanging, and highest is the safe direction (a body pushed up
        // is visible and falls back, a body left low is inside the map).
        f32 best = -1e9f;
        const s32 dx[4] = {1, -1, 0, 0}, dz[4] = {0, 0, 1, -1};
        for (u32 i = 0; i < 4; i++) {
            const s32 nx = static_cast<s32>(gx) + dx[i], nz = static_cast<s32>(gz) + dz[i];
            if (nx < 0 || nz < 0) continue;
            const u32 ux = static_cast<u32>(nx), uz = static_cast<u32>(nz);
            if (ux >= grid.width || uz >= grid.depth) continue;
            if (LevelGridSystem::isSolid(grid, ux, uz)) continue;
            const f32 h = LevelGridSystem::effectiveFloorHeight(grid, ux, uz, feetY);
            if (h > best) best = h;
        }
        // Walled in on all four sides: nothing to stand on, so only refuse to sink further.
        const f32 target = (best > -1e8f) ? best + e.halfExtents.y : restY;
        if (e.position.y < target) e.position.y = target;
        return;
    }

    if (airborne) {
        // Keep the arc — a pad launch or a vault must not be yanked down — but never below the
        // floor. Clamping is the weakest correction that still holds the invariant.
        if (e.position.y < restY) e.position.y = restY;
        return;
    }
    e.position.y = restY;
}
