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

bool Collision::overlapsLedgeAbove(Vec3 feetPos, f32 halfWidth, const LevelGrid& grid) {
    const f32 minX = feetPos.x - halfWidth, maxX = feetPos.x + halfWidth;
    const f32 minZ = feetPos.z - halfWidth, maxZ = feetPos.z + halfWidth;
    s32 cx0, cx1, cz0, cz1;
    cellRange(minX, maxX, grid.cellSize, cx0, cx1);
    cellRange(minZ, maxZ, grid.cellSize, cz0, cz1);
    for (s32 cz = cz0; cz <= cz1; cz++) {
        for (s32 cx = cx0; cx <= cx1; cx++) {
            if (!LevelGridSystem::isInBounds(grid, (u32)cx, (u32)cz)) continue;
            const GridCell& c = LevelGridSystem::getCell(grid, (u32)cx, (u32)cz);
            if (!(c.flags & CELL_LEDGE)) continue;
            if (LevelGridSystem::getFloorHeight(grid, (u32)cx, (u32)cz) > feetPos.y + STEP_UP_HEIGHT)
                return true;
        }
    }
    return false;
}

bool Collision::onJumpPad(Vec3 feetPos, f32 halfWidth, const LevelGrid& grid) {
    const f32 minX = feetPos.x - halfWidth, maxX = feetPos.x + halfWidth;
    const f32 minZ = feetPos.z - halfWidth, maxZ = feetPos.z + halfWidth;
    s32 cx0, cx1, cz0, cz1;
    cellRange(minX, maxX, grid.cellSize, cx0, cx1);
    cellRange(minZ, maxZ, grid.cellSize, cz0, cz1);
    for (s32 cz = cz0; cz <= cz1; cz++) {
        for (s32 cx = cx0; cx <= cx1; cx++) {
            if (!LevelGridSystem::isInBounds(grid, (u32)cx, (u32)cz)) continue;
            const GridCell& c = LevelGridSystem::getCell(grid, (u32)cx, (u32)cz);
            if (!(c.flags & CELL_JUMPPAD)) continue;
            // Only the pad the body is RESTING on counts: its floor must be at (or just below) the
            // feet, within the same STEP_UP_HEIGHT slack the landing snap allows. This rejects a pad
            // cell that sits far below a taller platform the body is standing on (the AABB can span
            // both), so you don't get relaunched off a pad you're not actually touching.
            const f32 fh = LevelGridSystem::getFloorHeight(grid, (u32)cx, (u32)cz);
            if (fh <= feetPos.y + 0.001f && fh >= feetPos.y - STEP_UP_HEIGHT)
                return true;
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

    // --- X axis --- (walls OR a too-high jump-ledge block the step)
    Vec3 tryPos = player.position + Vec3{delta.x, 0.0f, 0.0f};
    if (overlapsGrid(tryPos, grid) || overlapsLedgeAbove(tryPos, PLAYER_HALF_WIDTH, grid)) {
        delta.x         = 0.0f;
        player.velocity.x = 0.0f;
    } else {
        player.position.x = tryPos.x;
    }

    // --- Z axis ---
    tryPos = player.position + Vec3{0.0f, 0.0f, delta.z};
    if (overlapsGrid(tryPos, grid) || overlapsLedgeAbove(tryPos, PLAYER_HALF_WIDTH, grid)) {
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

    // --- Jump pad --- launch a grounded body upward off a CELL_JUMPPAD (Quake/Combat-Hall pad).
    // Fires the moment the body is resting/landing on the pad (onGround, not already rising), so you
    // can't stand on it — you get flung and air-steer the arc. A velocity.y impulse exactly like the
    // jump: every movement path (local predict, server drain, reconcile replay) funnels through here,
    // so it replicates in co-op with no wire change (posY + onGround are snapshotted; see CELL_JUMPPAD).
    if (player.onGround && player.velocity.y <= 0.1f &&
        onJumpPad(player.position, PLAYER_HALF_WIDTH, grid)) {
        player.velocity.y = JUMPPAD_LAUNCH;
        player.onGround   = false;
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

    // --- X axis (grid + entities OR a too-high jump-ledge) ---
    Vec3 tryPos = player.position + Vec3{delta.x, 0.0f, 0.0f};
    if (overlapsWorld(tryPos, grid, obstacles, obstacleCount) ||
        overlapsLedgeAbove(tryPos, PLAYER_HALF_WIDTH, grid)) {
        delta.x           = 0.0f;
        player.velocity.x = 0.0f;
    } else {
        player.position.x = tryPos.x;
    }

    // --- Z axis (grid + entities) ---
    tryPos = player.position + Vec3{0.0f, 0.0f, delta.z};
    if (overlapsWorld(tryPos, grid, obstacles, obstacleCount) ||
        overlapsLedgeAbove(tryPos, PLAYER_HALF_WIDTH, grid)) {
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

    // --- Jump pad --- (see the grid-only overload for the full rationale) launch a grounded body
    // upward off a CELL_JUMPPAD. velocity.y impulse, same replication story as the jump.
    if (player.onGround && player.velocity.y <= 0.1f &&
        onJumpPad(player.position, PLAYER_HALF_WIDTH, grid)) {
        player.velocity.y = JUMPPAD_LAUNCH;
        player.onGround   = false;
    }
}

// ---------------------------------------------------------------------------
// Entity-grid collision helpers (shared by enemy AI and spawn validation)
// ---------------------------------------------------------------------------

bool Collision::entityOverlapsGrid(Vec3 centre, Vec3 halfExtents, const LevelGrid& grid) {
    f32 minX = centre.x - halfExtents.x;
    f32 maxX = centre.x + halfExtents.x;
    f32 minZ = centre.z - halfExtents.z;
    f32 maxZ = centre.z + halfExtents.z;

    s32 cx0 = static_cast<s32>(std::floor(minX / grid.cellSize));
    s32 cx1 = static_cast<s32>(std::floor((maxX - 0.0001f) / grid.cellSize));
    s32 cz0 = static_cast<s32>(std::floor(minZ / grid.cellSize));
    s32 cz1 = static_cast<s32>(std::floor((maxZ - 0.0001f) / grid.cellSize));

    for (s32 z = cz0; z <= cz1; z++) {
        for (s32 x = cx0; x <= cx1; x++) {
            if (x < 0 || z < 0) return true;
            if (!LevelGridSystem::isInBounds(grid, static_cast<u32>(x), static_cast<u32>(z))) return true;
            if (LevelGridSystem::isSolid(grid, static_cast<u32>(x), static_cast<u32>(z))) return true;
        }
    }
    return false;
}

void Collision::snapEntityToFloor(Vec3& position, Vec3 halfExtents, const LevelGrid& grid) {
    u32 gx, gz;
    if (LevelGridSystem::worldToGrid(grid, position, gx, gz) &&
        !LevelGridSystem::isSolid(grid, gx, gz)) {
        f32 floorH = LevelGridSystem::getFloorHeight(grid, gx, gz);
        position.y = floorH + halfExtents.y;
    }
}

// See the header for why a push is validated rather than repaired afterwards.
bool Collision::tryPushXZ(Vec3& position, Vec3 halfExtents, const LevelGrid& grid, f32 dx, f32 dz) {
    Vec3 cand = position;
    cand.x += dx;
    cand.z += dz;
    // Refuse only a push that CREATES an overlap. If the body is already embedded (spawn, teleport,
    // a rebuilt level), let it move and leave the ejection pass to dig it out — otherwise we would
    // lock it inside the wall forever.
    if (!entityOverlapsGrid(position, halfExtents, grid) &&
         entityOverlapsGrid(cand,     halfExtents, grid)) {
        return false;
    }
    position = cand;
    return true;
}

void Collision::ensureNotInWall(Vec3& position, Vec3 halfExtents, const LevelGrid& grid) {
    if (!entityOverlapsGrid(position, halfExtents, grid)) return;

    u32 cx, cz;
    if (!LevelGridSystem::worldToGrid(grid, position, cx, cz)) return;

    // Spiral search: try cell centres in expanding rings until a valid spot is found
    for (u32 radius = 0; radius <= 5; radius++) {
        for (s32 dz = -static_cast<s32>(radius); dz <= static_cast<s32>(radius); dz++) {
            for (s32 dx = -static_cast<s32>(radius); dx <= static_cast<s32>(radius); dx++) {
                if (static_cast<u32>(std::abs(dx)) != radius &&
                    static_cast<u32>(std::abs(dz)) != radius) continue;
                s32 tx = static_cast<s32>(cx) + dx;
                s32 tz = static_cast<s32>(cz) + dz;
                if (tx < 0 || tz < 0) continue;
                if (!LevelGridSystem::isInBounds(grid, static_cast<u32>(tx), static_cast<u32>(tz))) continue;
                if (LevelGridSystem::isSolid(grid, static_cast<u32>(tx), static_cast<u32>(tz))) continue;
                Vec3 candidate = LevelGridSystem::gridToWorld(grid, static_cast<u32>(tx), static_cast<u32>(tz));
                candidate.y = position.y;
                if (!entityOverlapsGrid(candidate, halfExtents, grid)) {
                    position = candidate;
                    snapEntityToFloor(position, halfExtents, grid);
                    return;
                }
            }
        }
    }
}
