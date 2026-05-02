#include "world/raycast.h"
#include <cmath>
#include <cfloat>

RayHit Raycast::cast(const LevelGrid& grid,
                     Vec3 origin, Vec3 direction,
                     f32 maxDistance)
{
    RayHit miss;

    f32 cs = grid.cellSize;
    if (cs <= 0.0f) return miss;

    // Normalize direction (we need the 3D direction for the hit point)
    f32 len2 = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
    if (len2 < 1e-10f) return miss;
    f32 invLen = 1.0f / std::sqrt(len2);
    Vec3 dir = direction * invLen;

    // Starting cell
    s32 cx = static_cast<s32>(std::floor(origin.x / cs));
    s32 cz = static_cast<s32>(std::floor(origin.z / cs));

    // Step direction per axis
    s32 stepX = (dir.x > 0) ? 1 : -1;
    s32 stepZ = (dir.z > 0) ? 1 : -1;

    // DDA: distance to cross next cell boundary in each axis
    // tDeltaX = how much t to traverse one full cell width in X
    f32 tDeltaX = (dir.x != 0.0f) ? std::fabs(cs / dir.x) : FLT_MAX;
    f32 tDeltaZ = (dir.z != 0.0f) ? std::fabs(cs / dir.z) : FLT_MAX;

    // Initial tMax: t to reach the first X and Z boundary
    f32 tMaxX, tMaxZ;
    if (dir.x > 0.0f)
        tMaxX = ((std::floor(origin.x / cs) + 1.0f) * cs - origin.x) / dir.x;
    else if (dir.x < 0.0f)
        tMaxX = (std::floor(origin.x / cs) * cs - origin.x) / dir.x;
    else
        tMaxX = FLT_MAX;

    if (dir.z > 0.0f)
        tMaxZ = ((std::floor(origin.z / cs) + 1.0f) * cs - origin.z) / dir.z;
    else if (dir.z < 0.0f)
        tMaxZ = (std::floor(origin.z / cs) * cs - origin.z) / dir.z;
    else
        tMaxZ = FLT_MAX;

    Vec3 lastNormal = {0, 0, 0};
    f32  t = 0.0f;

    while (t < maxDistance) {
        // Advance to next cell
        if (tMaxX < tMaxZ) {
            t          = tMaxX;
            cx        += stepX;
            tMaxX     += tDeltaX;
            lastNormal = {static_cast<f32>(-stepX), 0.0f, 0.0f};
        } else {
            t          = tMaxZ;
            cz        += stepZ;
            tMaxZ     += tDeltaZ;
            lastNormal = {0.0f, 0.0f, static_cast<f32>(-stepZ)};
        }

        if (t > maxDistance) break;

        // OOB = solid wall
        if (cx < 0 || cz < 0 ||
            (u32)cx >= grid.width || (u32)cz >= grid.depth)
        {
            RayHit hit;
            hit.hit      = true;
            hit.position = origin + dir * t;
            hit.normal   = lastNormal;
            hit.cellX    = 0;
            hit.cellZ    = 0;
            hit.distance = t;
            return hit;
        }

        if (LevelGridSystem::isSolid(grid, (u32)cx, (u32)cz)) {
            // Check height: the ray must actually pass through this cell's height range
            // to count as a wall hit (avoids false hits on very tall/short walls)
            Vec3 hitPos = origin + dir * t;
            f32  hitY   = hitPos.y;

            // We compare against the adjacent (open) cell's floor/ceiling
            // Since we're entering from the side, the wall face spans from
            // some floor to some ceiling. For solid cells, they block full height.
            // Accept if the ray y at the crossing is within [0, some_max].
            // Simple approach: always accept solid cell hits.
            RayHit hit;
            hit.hit      = true;
            hit.position = hitPos;
            hit.normal   = lastNormal;
            hit.cellX    = (u32)cx;
            hit.cellZ    = (u32)cz;
            hit.distance = t;
            (void)hitY;
            return hit;
        }

        // Empty cell — check floor/ceiling intersection for looking up/down
        if (dir.y != 0.0f) {
            f32 floorH = LevelGridSystem::getFloorHeight(grid, (u32)cx, (u32)cz);
            f32 ceilH  = LevelGridSystem::getCeilingHeight(grid, (u32)cx, (u32)cz);

            // Floor hit (ray going down)
            if (dir.y < 0.0f) {
                f32 tFloor = (floorH - origin.y) / dir.y;
                if (tFloor > 0.0f && tFloor < t && tFloor <= maxDistance) {
                    Vec3 hitPos = origin + dir * tFloor;
                    // Check the hit position is inside this cell
                    s32 hcx = static_cast<s32>(std::floor(hitPos.x / cs));
                    s32 hcz = static_cast<s32>(std::floor(hitPos.z / cs));
                    if (hcx == cx && hcz == cz) {
                        RayHit hit;
                        hit.hit      = true;
                        hit.position = hitPos;
                        hit.normal   = {0.0f, 1.0f, 0.0f};
                        hit.cellX    = (u32)cx;
                        hit.cellZ    = (u32)cz;
                        hit.distance = tFloor;
                        return hit;
                    }
                }
            }

            // Ceiling hit (ray going up)
            if (dir.y > 0.0f) {
                f32 tCeil = (ceilH - origin.y) / dir.y;
                if (tCeil > 0.0f && tCeil < t && tCeil <= maxDistance) {
                    Vec3 hitPos = origin + dir * tCeil;
                    s32 hcx = static_cast<s32>(std::floor(hitPos.x / cs));
                    s32 hcz = static_cast<s32>(std::floor(hitPos.z / cs));
                    if (hcx == cx && hcz == cz) {
                        RayHit hit;
                        hit.hit      = true;
                        hit.position = hitPos;
                        hit.normal   = {0.0f, -1.0f, 0.0f};
                        hit.cellX    = (u32)cx;
                        hit.cellZ    = (u32)cz;
                        hit.distance = tCeil;
                        return hit;
                    }
                }
            }
        }
    }

    return miss; // no hit within maxDistance
}
