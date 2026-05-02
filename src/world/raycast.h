#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

struct RayHit {
    bool hit       = false;
    Vec3 position  = {0,0,0}; // exact world-space hit point
    Vec3 normal    = {0,0,0}; // outward face normal
    u32  cellX     = 0;
    u32  cellZ     = 0;
    f32  distance  = 0.0f;
};

namespace Raycast {
    // 2D DDA on XZ plane with floor/ceiling height checks.
    // direction need not be normalized but should be non-zero.
    RayHit cast(const LevelGrid& grid,
                Vec3 origin, Vec3 direction,
                f32 maxDistance = 100.0f);
}
