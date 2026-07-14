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
    // outT is 0 when the origin is INSIDE the box (point-blank overlap). Boxes entirely behind the
    // origin return false.
    bool rayVsAABB(Vec3 origin, Vec3 direction,
                   const AABB& box, f32& outT, Vec3& outNormal);

    // Nearest hostile entity whose collider the ray passes through (skips dead/friendly/props).
    //
    // Use this for PRECISION shots instead of a narrow queryConeSorted. A cone measures the angle to
    // an entity's CENTRE, so its linear aim tolerance is dist*tan(angle) — it collapses at close
    // range, which is why a 2° cone made a point-blank enemy filling the screen nearly unhittable.
    // A ray-vs-AABB test has the same tolerance at every distance: the size of the target.
    bool rayNearestEntity(const EntityPool& pool,
                          Vec3 origin, Vec3 direction, f32 maxDistance,
                          EntityHandle& outHandle, f32& outT);

    // AABB overlap test
    bool aabbOverlap(const AABB& a, const AABB& b);

    // Cone query: find entities within a cone. Returns count.
    // outHandles and outDistances filled sorted by distance (nearest first).
    u32 queryConeSorted(const EntityPool& pool,
                        Vec3 origin, Vec3 direction,
                        f32 coneAngleCos, f32 maxDistance,
                        EntityHandle* outHandles, f32* outDistances,
                        u32 maxResults,
                        bool horizontalCone = false);  // melee: judge the cone in the XZ plane
}
