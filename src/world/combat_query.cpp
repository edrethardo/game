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

    if (tmin < 0.0f) {
        // Ray starts inside box
        outT = 0.0f;
        outNormal = {0,0,0};
        return true;
    }

    outT = tmin;
    outNormal = nmin;
    return true;
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
                                  u32 maxResults)
{
    u32 count = 0;

    for (u32 a = 0; a < pool.activeCount; a++) {
        if (count >= maxResults) break;
        u32 i = pool.activeList[a];
        const Entity& e = pool.entities[i];
        if (e.flags & ENT_DEAD) continue;
        if (e.flags & ENT_FRIENDLY) continue;  // don't hit allies with melee cone
        if (e.enemyType == EnemyType::PROP) continue;  // skip decorations

        Vec3 toEntity = e.position - origin;
        f32 dist = length(toEntity);
        if (dist > maxDistance) continue;

        // Very close entities (< 0.5m) always hit — skip cone check to avoid
        // degenerate normalization and allow hitting enemies in melee range
        if (dist >= 0.5f) {
            f32 d = dot(toEntity * (1.0f / dist), direction);
            if (d < coneAngleCos) continue;
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
