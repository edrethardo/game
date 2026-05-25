#pragma once
// Spatial grid for fast entity proximity queries. Divides the XZ plane into
// fixed-size cells. Each cell holds up to MAX_PER_CELL entity indices.
// Rebuilt every tick from the active entity list — no incremental updates needed.
// Used by ProjectileSystem for collision checks instead of brute-force N×M.

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#ifdef ENGINE_DEBUG
#include "core/log.h"  // overflow diagnostics, debug builds only
#endif

static constexpr u32 SGRID_SIZE     = 64;   // 64×64 cells
static constexpr f32 SGRID_CELL     = 4.0f; // 4m per cell (covers 256×256m world)
// Max entities tracked per 4m cell. Entities beyond this in one cell are invisible
// to queryNeighbors (projectile collision), so a too-low cap drops hits in dense
// swarms (e.g. F4/F5 debug spawns, boss minion clusters). Debug builds log overflow.
static constexpr u32 SGRID_PER_CELL = 16;
static constexpr f32 SGRID_OFFSET   = 128.0f; // world origin offset (positions range ±128m)

struct SpatialGrid {
    u8  count[SGRID_SIZE][SGRID_SIZE] = {};
    u16 cells[SGRID_SIZE][SGRID_SIZE][SGRID_PER_CELL] = {};
};

namespace SpatialGridSystem {
    // Rebuild grid from current active entities. Call once per tick.
    inline void rebuild(SpatialGrid& grid, const EntityPool& pool) {
        // Clear counts (faster than memset on the full cells array)
        for (u32 z = 0; z < SGRID_SIZE; z++)
            for (u32 x = 0; x < SGRID_SIZE; x++)
                grid.count[z][x] = 0;

#ifdef ENGINE_DEBUG
        u32 dropped = 0;  // entities that overflowed a full cell this rebuild
#endif
        for (u32 a = 0; a < pool.activeCount; a++) {
            u32 idx = pool.activeList[a];
            const Entity& e = pool.entities[idx];
            if (e.flags & ENT_DEAD) continue;

            // Map world position to grid cell
            s32 cx = static_cast<s32>((e.position.x + SGRID_OFFSET) / SGRID_CELL);
            s32 cz = static_cast<s32>((e.position.z + SGRID_OFFSET) / SGRID_CELL);
            if (cx < 0 || cx >= (s32)SGRID_SIZE || cz < 0 || cz >= (s32)SGRID_SIZE) continue;

            u8& cnt = grid.count[cz][cx];
            if (cnt < SGRID_PER_CELL) {
                grid.cells[cz][cx][cnt] = static_cast<u16>(idx);
                cnt++;
            }
#ifdef ENGINE_DEBUG
            else { dropped++; }
#endif
        }
#ifdef ENGINE_DEBUG
        // If this fires, raise SGRID_PER_CELL: dropped entities can't be hit by
        // projectiles (queryNeighbors never returns them) until the cluster spreads.
        if (dropped > 0)
            LOG_WARN("SpatialGrid: %u entities dropped from full cells (raise SGRID_PER_CELL)", dropped);
#endif
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
        return out;
    }
}
