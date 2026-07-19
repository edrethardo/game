#include "world/combat_query.h"
#include "world/raycast.h"
#include <cmath>
#include <cfloat>

// -----------------------------------------------------------------------
// Ray vs AABB slab test
// -----------------------------------------------------------------------
bool CombatQuery::rayVsAABB(Vec3 origin, Vec3 dir,
                             const AABB& box, f32& outT, Vec3& outNormal)
{
    f32 tmin = -FLT_MAX;
    f32 tmax =  FLT_MAX;
    Vec3 nmin = {0,0,0};

    // Per-axis slab
    f32 o[3] = {origin.x, origin.y, origin.z};
    f32 d[3] = {dir.x, dir.y, dir.z};
    f32 mn[3] = {box.min.x, box.min.y, box.min.z};
    f32 mx[3] = {box.max.x, box.max.y, box.max.z};

    for (int i = 0; i < 3; i++) {
        if (std::fabs(d[i]) < 1e-8f) {
            // Parallel to slab — miss if origin outside
            if (o[i] < mn[i] || o[i] > mx[i]) return false;
        } else {
            f32 invD = 1.0f / d[i];
            f32 t1 = (mn[i] - o[i]) * invD;
            f32 t2 = (mx[i] - o[i]) * invD;

            Vec3 n = {0,0,0};
            if (i == 0) n.x = -1.0f;
            else if (i == 1) n.y = -1.0f;
            else n.z = -1.0f;

            if (t1 > t2) {
                f32 tmp = t1; t1 = t2; t2 = tmp;
                if (i == 0) n.x = 1.0f;
                else if (i == 1) n.y = 1.0f;
                else n.z = 1.0f;
            }

            if (t1 > tmin) { tmin = t1; nmin = n; }
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }

    // The box is entirely BEHIND the ray origin. Without this, tmin and tmax are both negative,
    // `tmin > tmax` is false, and the "starts inside box" branch below happily reports a hit at
    // t = 0 — i.e. the ray hits things behind you. (CombatQuery::raycast only got away with it
    // because its `t > 0.01f` guard threw those away, along with genuine point-blank overlaps.)
    if (tmax < 0.0f) return false;

    if (tmin < 0.0f) {
        // Ray starts inside the box (point-blank overlap) — the surface is at the origin.
        outT = 0.0f;
        outNormal = {0,0,0};
        return true;
    }

    outT = tmin;
    outNormal = nmin;
    return true;
}

// Nearest hostile entity whose collider the ray actually passes through.
//
// This is the correct primitive for a PRECISION shot, and the reason is geometric: a cone test
// (queryConeSorted) measures the angle to an entity's CENTRE, so its *linear* tolerance is
// dist * tan(coneAngle) — it shrinks to nothing as the target gets closer. Headshot's 2° cone
// allowed ~70 cm of aim error at 20 m but only ~5 cm at 1.5 m, so a point-blank enemy filling the
// screen was nearly impossible to hit. A ray-vs-AABB test has the SAME tolerance at every range:
// the size of the target. Aim anywhere on the body and you hit it.
bool CombatQuery::rayNearestEntity(const EntityPool& pool,
                                    Vec3 origin, Vec3 direction, f32 maxDistance,
                                    EntityHandle& outHandle, f32& outT)
{
    bool  found = false;
    f32   bestT = maxDistance;
    for (u32 a = 0; a < pool.activeCount; a++) {
        const u32 i = pool.activeList[a];
        const Entity& e = pool.entities[i];
        if (e.flags & ENT_DEAD)     continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;   // decorations are not targets
        if (e.flags & ENT_BURROWED) continue;           // still underground — not a target

        f32 t; Vec3 n;
        if (!rayVsAABB(origin, direction, entityAABB(e), t, n)) continue;
        if (t > maxDistance) continue;
        if (t > bestT && found) continue;

        bestT = t;
        outHandle = { static_cast<u16>(i), e.generation };
        outT   = t;
        found  = true;
    }
    return found;
}

// -----------------------------------------------------------------------
// AABB overlap
// -----------------------------------------------------------------------
bool CombatQuery::aabbOverlap(const AABB& a, const AABB& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x &&
           a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

// -----------------------------------------------------------------------
// Entity-aware raycast
// -----------------------------------------------------------------------
CombatHit CombatQuery::raycast(const LevelGrid& grid, const EntityPool& pool,
                                Vec3 origin, Vec3 direction, f32 maxDistance)
{
    CombatHit result;

    // Normalize direction
    f32 len2 = lengthSq(direction);
    if (len2 < 1e-10f) return result;
    Vec3 dir = direction * (1.0f / std::sqrt(len2));

    // 1. World raycast via DDA
    RayHit worldHit = Raycast::cast(grid, origin, dir, maxDistance);
    f32 bestDist = worldHit.hit ? worldHit.distance : maxDistance;

    if (worldHit.hit) {
        result.hit      = true;
        result.position = worldHit.position;
        result.normal   = worldHit.normal;
        result.distance = worldHit.distance;
        result.type     = CombatHit::WORLD;
    }

    // 2. Test all active entities — inflate hitboxes slightly for forgiving hitscan
    static constexpr f32 HITSCAN_PADDING = 0.15f;
    for (u32 a = 0; a < pool.activeCount; a++) {
        u32 i = pool.activeList[a];
        const Entity& e = pool.entities[i];
        if (e.flags & ENT_DEAD) continue;
        if (e.flags & ENT_FRIENDLY) continue;  // don't shoot allies
        if (e.enemyType == EnemyType::PROP) continue;  // skip decorations
        if (e.flags & ENT_BURROWED) continue;          // still underground — not a target

        AABB box = entityAABB(e);
        // Pad the AABB so near-misses still register
        box.min = box.min - Vec3{HITSCAN_PADDING, HITSCAN_PADDING, HITSCAN_PADDING};
        box.max = box.max + Vec3{HITSCAN_PADDING, HITSCAN_PADDING, HITSCAN_PADDING};
        f32 t;
        Vec3 n;
        if (rayVsAABB(origin, dir, box, t, n) && t < bestDist && t > 0.01f) {
            bestDist         = t;
            result.hit       = true;
            result.position  = origin + dir * t;
            result.normal    = n;
            result.distance  = t;
            result.type      = CombatHit::ENTITY;
            result.entityHandle = {static_cast<u16>(i), e.generation};
        }
    }

    return result;
}

// -----------------------------------------------------------------------
// Cone query (sorted by distance)
// -----------------------------------------------------------------------
u32 CombatQuery::queryConeSorted(const EntityPool& pool,
                                  Vec3 origin, Vec3 direction,
                                  f32 coneAngleCos, f32 maxDistance,
                                  EntityHandle* outHandles, f32* outDistances,
                                  u32 maxResults,
                                  bool horizontalCone,
                                  const LevelGrid* losGrid)
{
    u32 count = 0;

    for (u32 a = 0; a < pool.activeCount; a++) {
        if (count >= maxResults) break;
        u32 i = pool.activeList[a];
        const Entity& e = pool.entities[i];
        if (e.flags & ENT_DEAD) continue;
        if (e.flags & ENT_FRIENDLY) continue;  // don't hit allies with melee cone
        if (e.enemyType == EnemyType::PROP) continue;  // skip decorations
        if (e.flags & ENT_BURROWED) continue;          // still underground — not a target

        Vec3 toEntity = e.position - origin;
        f32 dist = length(toEntity);
        if (dist > maxDistance) continue;

        // Cone test. Melee (horizontalCone) judges the arc in the XZ plane: a
        // point-blank enemy's centre sits well below the eye, so the 3D
        // eye->centre vector points downward and would wrongly fail the cone.
        // A swing is a horizontal arc, so drop the vertical component. Other
        // callers keep the original 3D test. Very-close cases skip the check
        // entirely to avoid degenerate normalization.
        if (horizontalCone) {
            Vec3 toXZ   = {toEntity.x, 0.0f, toEntity.z};
            Vec3 dirXZ  = {direction.x, 0.0f, direction.z};
            f32  lenXZ  = length(toXZ);
            f32  dirLen = length(dirXZ);
            if (lenXZ >= 0.5f && dirLen > 1e-4f) {
                f32 d = dot(toXZ * (1.0f / lenXZ), dirXZ * (1.0f / dirLen));
                if (d < coneAngleCos) continue;
            }
        } else if (dist >= 0.5f) {
            f32 d = dot(toEntity * (1.0f / dist), direction);
            if (d < coneAngleCos) continue;
        }

        // Line-of-sight gate (melee only — losGrid non-null). Cast the full 3D ray from the swing
        // origin to the entity centre; a wall/floor/ceiling closer than the entity blocks the hit.
        // The DDA is slab-aware, so an enemy on a platform above/below is occluded by that slab even
        // though the melee cone above is judged horizontally. Point-blank overlaps (dist tiny) skip
        // the test to avoid a degenerate zero-length ray.
        if (losGrid && dist > 0.01f) {
            Vec3 losDir = toEntity * (1.0f / dist);
            RayHit los = Raycast::cast(*losGrid, origin, losDir, dist);
            if (los.hit && los.distance < dist - 0.05f) continue;
        }

        // Insert sorted by distance (insertion sort, small N)
        u32 insertAt = count;
        for (u32 j = 0; j < count; j++) {
            if (dist < outDistances[j]) { insertAt = j; break; }
        }
        // Shift
        if (count < maxResults) {
            for (u32 j = count; j > insertAt; j--) {
                outHandles[j]   = outHandles[j-1];
                outDistances[j] = outDistances[j-1];
            }
            outHandles[insertAt]   = {static_cast<u16>(i), e.generation};
            outDistances[insertAt] = dist;
            count++;
        } else if (insertAt < count) {
            for (u32 j = count - 1; j > insertAt; j--) {
                outHandles[j]   = outHandles[j-1];
                outDistances[j] = outDistances[j-1];
            }
            outHandles[insertAt]   = {static_cast<u16>(i), e.generation};
            outDistances[insertAt] = dist;
        }
    }

    return count;
}
