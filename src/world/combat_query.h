#pragma once

#include "core/types.h"
#include "core/math.h"
#include "renderer/frustum.h"
#include "game/entity.h"
#include "world/level_grid.h"

struct CombatHit {
    bool  hit      = false;
    Vec3  position = {0,0,0};
    Vec3  normal   = {0,0,0};
    f32   distance = 0.0f;
    enum HitType : u8 { NONE, WORLD, ENTITY } type = NONE;
    EntityHandle entityHandle;
};

namespace CombatQuery {
    // Full raycast: tests grid (DDA) and all active entity AABBs.
    // Returns the closest hit.
    CombatHit raycast(const LevelGrid& grid, const EntityPool& pool,
                      Vec3 origin, Vec3 direction, f32 maxDistance);

    // Ray vs AABB slab test. Returns true if hit, outT = distance along ray.
    bool rayVsAABB(Vec3 origin, Vec3 direction,
                   const AABB& box, f32& outT, Vec3& outNormal);

    // AABB overlap test
    bool aabbOverlap(const AABB& a, const AABB& b);

    // Cone query: find entities within a cone. Returns count.
    // outHandles and outDistances filled sorted by distance (nearest first).
    u32 queryConeSorted(const EntityPool& pool,
                        Vec3 origin, Vec3 direction,
                        f32 coneAngleCos, f32 maxDistance,
                        EntityHandle* outHandles, f32* outDistances,
                        u32 maxResults);
}
