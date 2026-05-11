#include "world/collision.h"
#include "game/player.h"
#include "core/math.h"
#include <cmath>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Returns the AABB of the player given a feet position.
static void playerAABB(const Vec3& feetPos,
                       f32& outMinX, f32& outMaxX,
                       f32& outMinY, f32& outMaxY,
                       f32& outMinZ, f32& outMaxZ)
{
    outMinX = feetPos.x - PLAYER_HALF_WIDTH;
    outMaxX = feetPos.x + PLAYER_HALF_WIDTH;
    outMinY = feetPos.y;
    outMaxY = feetPos.y + PLAYER_HEIGHT;
    outMinZ = feetPos.z - PLAYER_HALF_WIDTH;
    outMaxZ = feetPos.z + PLAYER_HALF_WIDTH;
}

// Returns the range of grid cells that overlap an interval [mn, mx] in one axis.
static void cellRange(f32 mn, f32 mx, f32 cellSize, s32& outMin, s32& outMax) {
    outMin = static_cast<s32>(std::floor(mn / cellSize));
    outMax = static_cast<s32>(std::floor((mx - 0.0001f) / cellSize));
}

// Check whether the player AABB (given feet position) overlaps any solid cell
// in the XZ plane (height test uses floor/ceiling too).
static bool overlapsGrid(const Vec3& feetPos, const LevelGrid& grid) {
    f32 minX, maxX, minY, maxY, minZ, maxZ;
    playerAABB(feetPos, minX, maxX, minY, maxY, minZ, maxZ);

    s32 cx0, cx1, cz0, cz1;
    cellRange(minX, maxX, grid.cellSize, cx0, cx1);
    cellRange(minZ, maxZ, grid.cellSize, cz0, cz1);

    for (s32 cz = cz0; cz <= cz1; cz++) {
        for (s32 cx = cx0; cx <= cx1; cx++) {
            // Treat OOB as solid
            if (!LevelGridSystem::isInBounds(grid, (u32)cx, (u32)cz)) return true;
            if (LevelGridSystem::isSolid(grid, (u32)cx, (u32)cz))     return true;
        }
    }
    return false;
}

// Checks whether the player AABB (given feet position) overlaps any entity
// obstacle in the XZ plane. Y axis is ignored — entities don't block jumping.
static bool overlapsAnyObstacle(const Vec3& feetPos,
                                const CollisionObstacle* obs, u32 count) {
    f32 pMinX = feetPos.x - PLAYER_HALF_WIDTH;
    f32 pMaxX = feetPos.x + PLAYER_HALF_WIDTH;
    f32 pMinZ = feetPos.z - PLAYER_HALF_WIDTH;
    f32 pMaxZ = feetPos.z + PLAYER_HALF_WIDTH;
    for (u32 i = 0; i < count; i++) {
        f32 eMinX = obs[i].position.x - obs[i].halfExtents.x;
        f32 eMaxX = obs[i].position.x + obs[i].halfExtents.x;
        f32 eMinZ = obs[i].position.z - obs[i].halfExtents.z;
        f32 eMaxZ = obs[i].position.z + obs[i].halfExtents.z;
        if (pMaxX > eMinX && pMinX < eMaxX &&
            pMaxZ > eMinZ && pMinZ < eMaxZ) {
            return true;
        }
    }
    return false;
}

// Combined check: overlaps grid OR any entity obstacle (XZ only).
static bool overlapsWorld(const Vec3& feetPos, const LevelGrid& grid,
                          const CollisionObstacle* obs, u32 obsCount) {
    return overlapsGrid(feetPos, grid) || overlapsAnyObstacle(feetPos, obs, obsCount);
}

// ---------------------------------------------------------------------------
// Axis-separated sweep collision
// ---------------------------------------------------------------------------
void Collision::moveAndSlide(Player& player, const LevelGrid& grid, f32 dt) {
    // Apply gravity
    if (!player.onGround) {
        player.velocity.y += GRAVITY * dt;
    }

    Vec3 delta = player.velocity * dt;

    // --- X axis ---
    Vec3 tryPos = player.position + Vec3{delta.x, 0.0f, 0.0f};
    if (overlapsGrid(tryPos, grid)) {
        delta.x         = 0.0f;
        player.velocity.x = 0.0f;
    } else {
        player.position.x = tryPos.x;
    }

    // --- Z axis ---
    tryPos = player.position + Vec3{0.0f, 0.0f, delta.z};
    if (overlapsGrid(tryPos, grid)) {
        delta.z         = 0.0f;
        player.velocity.z = 0.0f;
    } else {
        player.position.z = tryPos.z;
    }

    // --- Y axis ---
    player.onGround = false;

    tryPos = player.position + Vec3{0.0f, delta.y, 0.0f};
    if (overlapsGrid(tryPos, grid)) {
        if (delta.y < 0.0f) {
            // Landing — snap feet to the grid floor
            // Find the highest floor height among overlapping cells
            f32 minX, maxX, minY, maxY, minZ, maxZ;
            playerAABB(tryPos, minX, maxX, minY, maxY, minZ, maxZ);
            s32 cx0, cx1, cz0, cz1;
            cellRange(minX, maxX, grid.cellSize, cx0, cx1);
            cellRange(minZ, maxZ, grid.cellSize, cz0, cz1);

            f32 highestFloor = player.position.y; // fallback: stay put
            for (s32 cz = cz0; cz <= cz1; cz++) {
                for (s32 cx = cx0; cx <= cx1; cx++) {
                    if (!LevelGridSystem::isInBounds(grid, (u32)cx, (u32)cz)) continue;
                    if (!LevelGridSystem::isSolid(grid, (u32)cx, (u32)cz)) {
                        // Open cell — player stands on its floor
                        f32 fh = LevelGridSystem::getFloorHeight(grid, (u32)cx, (u32)cz);
                        if (fh > highestFloor) highestFloor = fh;
                    }
                }
            }
            player.position.y = highestFloor;
            player.velocity.y = 0.0f;
            player.onGround   = true;
        } else {
            // Head hit ceiling
            player.velocity.y = 0.0f;
        }
    } else {
        player.position.y = tryPos.y;

        // Check if still standing on something
        Vec3 groundCheck = player.position + Vec3{0.0f, -0.05f, 0.0f};
        if (overlapsGrid(groundCheck, grid)) {
            player.onGround = true;
        }
    }

    // --- Floor height snap (prevent sinking through floor due to floor height data) ---
    // Walk through overlapping open cells and push up if below their floor
    {
        f32 minX, maxX, minY, maxY, minZ, maxZ;
        playerAABB(player.position, minX, maxX, minY, maxY, minZ, maxZ);
        s32 cx0, cx1, cz0, cz1;
        cellRange(minX, maxX, grid.cellSize, cx0, cx1);
        cellRange(minZ, maxZ, grid.cellSize, cz0, cz1);

        for (s32 cz = cz0; cz <= cz1; cz++) {
            for (s32 cx = cx0; cx <= cx1; cx++) {
                if (!LevelGridSystem::isInBounds(grid, (u32)cx, (u32)cz)) continue;
                if (LevelGridSystem::isSolid(grid, (u32)cx, (u32)cz))     continue;
                f32 fh = LevelGridSystem::getFloorHeight(grid, (u32)cx, (u32)cz);
                if (player.position.y < fh) {
                    player.position.y = fh;
                    if (player.velocity.y < 0.0f) player.velocity.y = 0.0f;
                    player.onGround = true;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Overload: grid + entity obstacle collision
// ---------------------------------------------------------------------------
void Collision::moveAndSlide(Player& player, const LevelGrid& grid, f32 dt,
                             const CollisionObstacle* obstacles, u32 obstacleCount) {
    // Apply gravity
    if (!player.onGround) {
        player.velocity.y += GRAVITY * dt;
    }

    Vec3 delta = player.velocity * dt;

    // --- X axis (grid + entities) ---
    Vec3 tryPos = player.position + Vec3{delta.x, 0.0f, 0.0f};
    if (overlapsWorld(tryPos, grid, obstacles, obstacleCount)) {
        delta.x           = 0.0f;
        player.velocity.x = 0.0f;
    } else {
        player.position.x = tryPos.x;
    }

    // --- Z axis (grid + entities) ---
    tryPos = player.position + Vec3{0.0f, 0.0f, delta.z};
    if (overlapsWorld(tryPos, grid, obstacles, obstacleCount)) {
        delta.z           = 0.0f;
        player.velocity.z = 0.0f;
    } else {
        player.position.z = tryPos.z;
    }

    // --- Y axis (grid only — entities don't block vertical movement) ---
    player.onGround = false;

    tryPos = player.position + Vec3{0.0f, delta.y, 0.0f};
    if (overlapsGrid(tryPos, grid)) {
        if (delta.y < 0.0f) {
            f32 minX, maxX, minY, maxY, minZ, maxZ;
            playerAABB(tryPos, minX, maxX, minY, maxY, minZ, maxZ);
            s32 cx0, cx1, cz0, cz1;
            cellRange(minX, maxX, grid.cellSize, cx0, cx1);
            cellRange(minZ, maxZ, grid.cellSize, cz0, cz1);

            f32 highestFloor = player.position.y;
            for (s32 cz = cz0; cz <= cz1; cz++) {
                for (s32 cx = cx0; cx <= cx1; cx++) {
                    if (!LevelGridSystem::isInBounds(grid, (u32)cx, (u32)cz)) continue;
                    if (!LevelGridSystem::isSolid(grid, (u32)cx, (u32)cz)) {
                        f32 fh = LevelGridSystem::getFloorHeight(grid, (u32)cx, (u32)cz);
                        if (fh > highestFloor) highestFloor = fh;
                    }
                }
            }
            player.position.y = highestFloor;
            player.velocity.y = 0.0f;
            player.onGround   = true;
        } else {
            player.velocity.y = 0.0f;
        }
    } else {
        player.position.y = tryPos.y;

        Vec3 groundCheck = player.position + Vec3{0.0f, -0.05f, 0.0f};
        if (overlapsGrid(groundCheck, grid)) {
            player.onGround = true;
        }
    }

    // --- Floor height snap ---
    {
        f32 minX, maxX, minY, maxY, minZ, maxZ;
        playerAABB(player.position, minX, maxX, minY, maxY, minZ, maxZ);
        s32 cx0, cx1, cz0, cz1;
        cellRange(minX, maxX, grid.cellSize, cx0, cx1);
        cellRange(minZ, maxZ, grid.cellSize, cz0, cz1);

        for (s32 cz = cz0; cz <= cz1; cz++) {
            for (s32 cx = cx0; cx <= cx1; cx++) {
                if (!LevelGridSystem::isInBounds(grid, (u32)cx, (u32)cz)) continue;
                if (LevelGridSystem::isSolid(grid, (u32)cx, (u32)cz))     continue;
                f32 fh = LevelGridSystem::getFloorHeight(grid, (u32)cx, (u32)cz);
                if (player.position.y < fh) {
                    player.position.y = fh;
                    if (player.velocity.y < 0.0f) player.velocity.y = 0.0f;
                    player.onGround = true;
                }
            }
        }
    }
}
