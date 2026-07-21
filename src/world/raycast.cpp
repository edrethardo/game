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

    // Floor/ceiling crossing inside cell (fcx,fcz) within t ∈ (0, tExit]. Returns {.hit=false} if
    // none. Used for the STARTING cell (below) AND each cell the ray enters. Without the starting-
    // cell call, a ray that stays inside one cell (e.g. a projectile's short per-tick cast) never
    // gets a floor/ceiling test, so projectiles passed through the floor/ceiling instead of bouncing.
    auto tryFloorCeil = [&](s32 fcx, s32 fcz, f32 tExit) -> RayHit {
        RayHit none;
        if (dir.y == 0.0f) return none;
        if (dir.y < 0.0f) {
            // Platform slab TOP: a descending ray that starts above it crosses this plane before
            // the base floor. tP <= 0 (origin at/below the top — e.g. under the slab shooting
            // down) skips it and falls through to the base-floor test, which is then correct.
            if (LevelGridSystem::hasPlatform(grid, (u32)fcx, (u32)fcz)) {
                const f32 topH = LevelGridSystem::getPlatformTop(grid, (u32)fcx, (u32)fcz);
                const f32 tP = (topH - origin.y) / dir.y;
                if (tP > 0.0f && tP <= tExit) {
                    Vec3 hp = origin + dir * tP;
                    if (static_cast<s32>(std::floor(hp.x / cs)) == fcx &&
                        static_cast<s32>(std::floor(hp.z / cs)) == fcz) {
                        RayHit hit; hit.hit = true; hit.position = hp; hit.normal = {0.0f, 1.0f, 0.0f};
                        hit.cellX = (u32)fcx; hit.cellZ = (u32)fcz; hit.distance = tP; return hit;
                    }
                }
            }
            f32 floorH = LevelGridSystem::getFloorHeight(grid, (u32)fcx, (u32)fcz);
            f32 tF = (floorH - origin.y) / dir.y;
            if (tF > 0.0f && tF <= tExit) {
                Vec3 hp = origin + dir * tF;
                if (static_cast<s32>(std::floor(hp.x / cs)) == fcx &&
                    static_cast<s32>(std::floor(hp.z / cs)) == fcz) {
                    RayHit hit; hit.hit = true; hit.position = hp; hit.normal = {0.0f, 1.0f, 0.0f};
                    hit.cellX = (u32)fcx; hit.cellZ = (u32)fcz; hit.distance = tF; return hit;
                }
            }
        } else { // dir.y > 0 — ceiling
            // Platform slab UNDERSIDE: a rising ray that starts below it (the arcade shooting up).
            if (LevelGridSystem::hasPlatform(grid, (u32)fcx, (u32)fcz)) {
                const f32 undH = LevelGridSystem::getPlatformUnderside(grid, (u32)fcx, (u32)fcz);
                const f32 tU = (undH - origin.y) / dir.y;
                if (tU > 0.0f && tU <= tExit) {
                    Vec3 hp = origin + dir * tU;
                    if (static_cast<s32>(std::floor(hp.x / cs)) == fcx &&
                        static_cast<s32>(std::floor(hp.z / cs)) == fcz) {
                        RayHit hit; hit.hit = true; hit.position = hp; hit.normal = {0.0f, -1.0f, 0.0f};
                        hit.cellX = (u32)fcx; hit.cellZ = (u32)fcz; hit.distance = tU; return hit;
                    }
                }
            }
            f32 ceilH = LevelGridSystem::getCeilingHeight(grid, (u32)fcx, (u32)fcz);
            f32 tC = (ceilH - origin.y) / dir.y;
            if (tC > 0.0f && tC <= tExit) {
                Vec3 hp = origin + dir * tC;
                if (static_cast<s32>(std::floor(hp.x / cs)) == fcx &&
                    static_cast<s32>(std::floor(hp.z / cs)) == fcz) {
                    RayHit hit; hit.hit = true; hit.position = hp; hit.normal = {0.0f, -1.0f, 0.0f};
                    hit.cellX = (u32)fcx; hit.cellZ = (u32)fcz; hit.distance = tC; return hit;
                }
            }
        }
        return none;
    };

    Vec3 lastNormal = {0, 0, 0};
    f32  t = 0.0f;

    // Starting cell first — the DDA loop only checks cells it ADVANCES into (see lambda note).
    if (cx >= 0 && cz >= 0 && (u32)cx < grid.width && (u32)cz < grid.depth &&
        !LevelGridSystem::isSolid(grid, (u32)cx, (u32)cz)) {
        f32 tExit0 = (tMaxX < tMaxZ) ? tMaxX : tMaxZ;
        if (tExit0 > maxDistance) tExit0 = maxDistance;
        RayHit fc = tryFloorCeil(cx, cz, tExit0);
        if (fc.hit) return fc;
    }

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

        // Platform RIM (per-slab): entering a slab cell with the crossing Y inside ANY slab's side
        // band is a hit on that slab's edge face (the balcony's visible edge). getPlatformUnderside(i)
        // is clamped UP to the next-lower surface, so stacked/thin slab bands never overlap and no
        // phantom rim is emitted in the open story between two slabs (band-subtraction). Strict
        // epsilons let a surface-grazing shot — a sniper firing flat across a slab top — pass.
        if (LevelGridSystem::hasPlatform(grid, (u32)cx, (u32)cz)) {
            const f32 yAt = origin.y + dir.y * t;
            const u8  pc  = LevelGridSystem::platformCount(grid, (u32)cx, (u32)cz);
            for (u32 i = 0; i < pc; ++i) {
                const f32 topH = LevelGridSystem::getPlatformTop(grid, (u32)cx, (u32)cz, i);
                const f32 undH = LevelGridSystem::getPlatformUnderside(grid, (u32)cx, (u32)cz, i);
                if (yAt > undH + 0.0001f && yAt < topH - 0.0001f) {
                    RayHit hit;
                    hit.hit      = true;
                    hit.position = origin + dir * t;
                    hit.normal   = lastNormal;
                    hit.cellX    = (u32)cx;
                    hit.cellZ    = (u32)cz;
                    hit.distance = t;
                    return hit;
                }
            }
        }

        // Empty cell — floor/ceiling crossing within this cell (tNext = when we leave it).
        f32 tNext = (tMaxX < tMaxZ) ? tMaxX : tMaxZ;
        if (tNext > maxDistance) tNext = maxDistance;
        RayHit fc = tryFloorCeil(cx, cz, tNext);
        if (fc.hit) return fc;
    }

    return miss; // no hit within maxDistance
}
