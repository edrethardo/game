#pragma once
// Spatial grid for fast entity proximity queries. Divides the XZ plane into
// fixed-size cells. Each cell holds up to MAX_PER_CELL entity indices.
// Rebuilt every tick from the active entity list — no incremental updates needed.
// Used by ProjectileSystem for collision checks instead of brute-force N×M.

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "core/assert.h"

static constexpr u32 SGRID_SIZE     = 64;   // 64×64 cells
static constexpr f32 SGRID_CELL     = 4.0f; // 4m per cell (covers 256×256m world)
// Max entities tracked per 4m cell. Exceeding it is no longer a correctness problem — the
// remainder goes to the overflow list below and is still returned by every query — so this is now
// purely a memory/perf knob: raising it shrinks the overflow list at 8 KB per extra slot.
static constexpr u32 SGRID_PER_CELL = 16;
static constexpr f32 SGRID_OFFSET   = 128.0f; // world origin offset (positions range ±128m)

// Worst case a single queryNeighbors can return: the 3x3 cell block plus every overflowed entity.
// DERIVED, never hand-typed — a literal is exactly how this broke. The projectile candidate buffers
// were sized 72 under the comment "3x3 cells x 8 per cell max" and stayed 72 when SGRID_PER_CELL was
// raised to 16, so from then on a dense 3x3 block was silently truncated to half its contents on
// every query, on top of the cell overflow below.
static constexpr u32 SGRID_QUERY_MAX = 9 * SGRID_PER_CELL + MAX_ENTITIES;

struct SpatialGrid {
    u8  count[SGRID_SIZE][SGRID_SIZE] = {};
    u16 cells[SGRID_SIZE][SGRID_SIZE][SGRID_PER_CELL] = {};

    // Entities that did not fit their cell, or that stood outside the grid's span. They are NOT
    // dropped: queryNeighbors appends this list to every query, so an entity is never invisible to
    // projectile collision however tightly a swarm packs or however far out it wanders. Returning
    // them for queries anywhere is conservative — the caller runs a precise AABB test on every
    // candidate, so extra candidates cost comparisons and can never produce a wrong hit.
    // Measured before this existed: 5643 overflow events in a 3 h soak, up to 15 at once, each one
    // an enemy that could not be shot until the cluster spread out.
    u16 overflow[MAX_ENTITIES] = {};
    u16 overflowCount = 0;
};

namespace SpatialGridSystem {
    // Rebuild grid from current active entities. Call once per tick.
    inline void rebuild(SpatialGrid& grid, const EntityPool& pool) {
        // Clear counts (faster than memset on the full cells array)
        for (u32 z = 0; z < SGRID_SIZE; z++)
            for (u32 x = 0; x < SGRID_SIZE; x++)
                grid.count[z][x] = 0;
        grid.overflowCount = 0;

        for (u32 a = 0; a < pool.activeCount; a++) {
            u32 idx = pool.activeList[a];
            const Entity& e = pool.entities[idx];
            if (e.flags & ENT_DEAD) continue;

            // Map world position to grid cell
            s32 cx = static_cast<s32>((e.position.x + SGRID_OFFSET) / SGRID_CELL);
            s32 cz = static_cast<s32>((e.position.z + SGRID_OFFSET) / SGRID_CELL);

            const bool inGrid = (cx >= 0 && cx < (s32)SGRID_SIZE &&
                                 cz >= 0 && cz < (s32)SGRID_SIZE);
            // Two ways an entity used to fall out of collision entirely, both silent: its cell was
            // full, or it stood outside the grid's +/-128 m span. Both now park it in the overflow
            // list instead, which every query appends — so "every active entity is a collision
            // candidate" holds unconditionally.
            if (inGrid && grid.count[cz][cx] < SGRID_PER_CELL) {
                u8& cnt = grid.count[cz][cx];
                grid.cells[cz][cx][cnt] = static_cast<u16>(idx);
                cnt++;
            } else if (grid.overflowCount < MAX_ENTITIES) {
                // The bound is the whole pool, so it can only be reached if EVERY entity overflows;
                // it exists to make the array access provably in range, not as a real limit.
                grid.overflow[grid.overflowCount++] = static_cast<u16>(idx);
            }
        }
    }

    // Query all entity indices in a cell and its 8 neighbors (3×3 region).
    // Returns count of indices written to outIndices (up to maxOut).
    inline u32 queryNeighbors(const SpatialGrid& grid, Vec3 pos,
                               u16* outIndices, u32 maxOut) {
        s32 cx = static_cast<s32>((pos.x + SGRID_OFFSET) / SGRID_CELL);
        s32 cz = static_cast<s32>((pos.z + SGRID_OFFSET) / SGRID_CELL);
        u32 out = 0;

        for (s32 dz = -1; dz <= 1; dz++) {
            for (s32 dx = -1; dx <= 1; dx++) {
                s32 qx = cx + dx;
                s32 qz = cz + dz;
                if (qx < 0 || qx >= (s32)SGRID_SIZE || qz < 0 || qz >= (s32)SGRID_SIZE) continue;
                u8 cnt = grid.count[qz][qx];
                for (u8 i = 0; i < cnt && out < maxOut; i++) {
                    outIndices[out++] = grid.cells[qz][qx][i];
                }
            }
        }
        // Everything that could not be bucketed, appended to EVERY query. This is what makes
        // "an active entity is always a collision candidate" true regardless of local density or
        // distance from the origin — without it the list above is just a record of what was lost.
        //
        // Correctness here depends on maxOut being SGRID_QUERY_MAX. Overflow is appended LAST, so a
        // caller passing a smaller buffer would truncate away exactly the entities this list exists
        // to rescue — silently re-creating the unhittable-enemy bug it fixed. Every caller sizes
        // from the constant today; this is what keeps that true when one is added.
        ENGINE_ASSERT(maxOut >= SGRID_QUERY_MAX,
                      "queryNeighbors needs an SGRID_QUERY_MAX buffer or overflow is dropped");
        for (u16 i = 0; i < grid.overflowCount && out < maxOut; i++)
            outIndices[out++] = grid.overflow[i];
        return out;
    }
}
