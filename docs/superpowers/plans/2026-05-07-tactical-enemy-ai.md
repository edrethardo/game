# Tactical Enemy AI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make enemies coordinate in squads, use A* pathfinding for flanking/retreating, and punish passive play with new tactical FSM states.

**Architecture:** Three layered systems on the existing FSM: (1) Grid A* pathfinder for smart navigation, (2) Room-based squad coordinator for role assignment, (3) New tactical FSM states (FLANK, RETREAT, AMBUSH, STRAFE, SURROUND) plus anti-kiting and projectile leading.

**Tech Stack:** C++17, fixed-size arrays (no heap in hot paths), staggered per-frame scheduling, existing `LevelGrid` cell system.

---

## File Structure

| Action | Path | Responsibility |
|--------|------|----------------|
| Create | `src/world/pathfinder.h` | A* interface: `findPath()` declaration |
| Create | `src/world/pathfinder.cpp` | A* implementation: open/closed sets, neighbor expansion |
| Create | `src/game/squad.h` | Squad data structures, coordinator interface |
| Create | `src/game/squad.cpp` | Role assignment logic, alert propagation |
| Modify | `src/world/level_grid.h` | Add spatial query declarations (`findCoverCell`, etc.) |
| Modify | `src/world/level_grid.cpp` | Add spatial query implementations |
| Modify | `src/game/entity.h` | New AIState values + path/squad/tactical fields on Entity |
| Modify | `src/game/enemy_ai.h` | Updated `update()` signature (accepts SquadPool) |
| Modify | `src/game/enemy_ai.cpp` | New tactical states, sprint burst, projectile leading |
| Modify | `src/engine/engine.cpp` | Wire squad system init/update, pass squads to AI |

---

### Task 1: A* Pathfinder

**Files:**
- Create: `src/world/pathfinder.h`
- Create: `src/world/pathfinder.cpp`
- Modify: `src/CMakeLists.txt` (add new source)

- [ ] **Step 1: Create pathfinder header**

```cpp
// src/world/pathfinder.h
#pragma once

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

static constexpr u8 MAX_PATH_WAYPOINTS = 6;
static constexpr u16 MAX_ASTAR_SEARCH = 256;

namespace Pathfinder {
    // Find path from start to goal. Returns number of waypoints written (0 = no path).
    // outPath receives world-space positions. Searches up to maxSearch cells.
    u8 findPath(const LevelGrid& grid, Vec3 start, Vec3 goal,
                Vec3* outPath, u8 maxWaypoints = MAX_PATH_WAYPOINTS,
                u16 maxSearch = MAX_ASTAR_SEARCH);
}
```

- [ ] **Step 2: Create pathfinder implementation**

```cpp
// src/world/pathfinder.cpp
// Grid-based A* pathfinder for enemy tactical navigation.
// Operates on LevelGrid cell coordinates with Manhattan heuristic.
// Bounded search (MAX_ASTAR_SEARCH) prevents runaway on large maps.

#include "world/pathfinder.h"
#include <cstring>

struct AStarNode {
    u16 x, z;
    u16 parentIdx;   // index into closed list
    u16 g;           // cost from start
    u16 f;           // g + heuristic
};

// Fixed-size open/closed sets — no heap allocation
static AStarNode s_open[MAX_ASTAR_SEARCH];
static AStarNode s_closed[MAX_ASTAR_SEARCH];
static u16 s_openCount;
static u16 s_closedCount;

static u16 manhattan(u16 ax, u16 az, u16 bx, u16 bz) {
    u16 dx = (ax > bx) ? (ax - bx) : (bx - ax);
    u16 dz = (az > bz) ? (az - bz) : (bz - az);
    return dx + dz;
}

static s16 findInClosed(u16 x, u16 z) {
    for (u16 i = 0; i < s_closedCount; i++) {
        if (s_closed[i].x == x && s_closed[i].z == z) return static_cast<s16>(i);
    }
    return -1;
}

static s16 findInOpen(u16 x, u16 z) {
    for (u16 i = 0; i < s_openCount; i++) {
        if (s_open[i].x == x && s_open[i].z == z) return static_cast<s16>(i);
    }
    return -1;
}

// Pop lowest-f node from open set
static AStarNode popBestOpen() {
    u16 bestIdx = 0;
    for (u16 i = 1; i < s_openCount; i++) {
        if (s_open[i].f < s_open[bestIdx].f) bestIdx = i;
    }
    AStarNode best = s_open[bestIdx];
    s_open[bestIdx] = s_open[--s_openCount]; // swap-remove
    return best;
}

u8 Pathfinder::findPath(const LevelGrid& grid, Vec3 start, Vec3 goal,
                         Vec3* outPath, u8 maxWaypoints, u16 maxSearch) {
    u32 sx, sz, gx, gz;
    if (!LevelGridSystem::worldToGrid(grid, start, sx, sz)) return 0;
    if (!LevelGridSystem::worldToGrid(grid, goal, gx, gz)) return 0;

    // Goal in solid cell — no path
    if (LevelGridSystem::isSolid(grid, gx, gz)) return 0;

    // Already at goal
    if (sx == gx && sz == gz) return 0;

    s_openCount = 0;
    s_closedCount = 0;

    u16 startX = static_cast<u16>(sx), startZ = static_cast<u16>(sz);
    u16 goalX = static_cast<u16>(gx), goalZ = static_cast<u16>(gz);

    // Seed open set
    s_open[s_openCount++] = {startX, startZ, 0xFFFF, 0, manhattan(startX, startZ, goalX, goalZ)};

    // 4-connected neighbors (no diagonals)
    static constexpr s16 dx[4] = {1, -1, 0, 0};
    static constexpr s16 dz[4] = {0, 0, 1, -1};

    while (s_openCount > 0 && s_closedCount < maxSearch) {
        AStarNode current = popBestOpen();

        // Goal reached — reconstruct path
        if (current.x == goalX && current.z == goalZ) {
            // Trace back through closed list
            Vec3 trace[MAX_ASTAR_SEARCH];
            u16 traceLen = 0;
            trace[traceLen++] = LevelGridSystem::gridToWorld(grid, current.x, current.z);
            u16 parentIdx = current.parentIdx;
            while (parentIdx != 0xFFFF && traceLen < MAX_ASTAR_SEARCH) {
                trace[traceLen++] = LevelGridSystem::gridToWorld(grid, s_closed[parentIdx].x, s_closed[parentIdx].z);
                parentIdx = s_closed[parentIdx].parentIdx;
            }

            // Reverse into output, skipping start, taking every Nth for compression
            u8 written = 0;
            u16 step = (traceLen > maxWaypoints) ? (traceLen / maxWaypoints) : 1;
            for (s16 i = static_cast<s16>(traceLen) - 2; i >= 0 && written < maxWaypoints; i -= step) {
                outPath[written++] = trace[i];
            }
            // Ensure goal is always the last waypoint
            if (written > 0) {
                outPath[written - 1] = LevelGridSystem::gridToWorld(grid, goalX, goalZ);
            }
            return written;
        }

        // Add to closed
        u16 closedIdx = s_closedCount;
        s_closed[s_closedCount++] = current;

        // Expand neighbors
        for (u8 d = 0; d < 4; d++) {
            s16 nx = static_cast<s16>(current.x) + dx[d];
            s16 nz = static_cast<s16>(current.z) + dz[d];
            if (nx < 0 || nz < 0) continue;
            u16 ux = static_cast<u16>(nx), uz = static_cast<u16>(nz);
            if (!LevelGridSystem::isInBounds(grid, ux, uz)) continue;
            if (LevelGridSystem::isSolid(grid, ux, uz)) continue;
            if (findInClosed(ux, uz) >= 0) continue;

            u16 newG = current.g + 1;
            s16 openIdx = findInOpen(ux, uz);
            if (openIdx >= 0) {
                if (newG < s_open[openIdx].g) {
                    s_open[openIdx].g = newG;
                    s_open[openIdx].f = newG + manhattan(ux, uz, goalX, goalZ);
                    s_open[openIdx].parentIdx = closedIdx;
                }
            } else if (s_openCount < MAX_ASTAR_SEARCH) {
                s_open[s_openCount++] = {ux, uz, closedIdx, newG,
                    static_cast<u16>(newG + manhattan(ux, uz, goalX, goalZ))};
            }
        }
    }

    return 0; // no path found within budget
}
```

- [ ] **Step 3: Add pathfinder.cpp to CMakeLists.txt**

In `src/CMakeLists.txt`, add `world/pathfinder.cpp` to the source list alongside other `world/` files.

- [ ] **Step 4: Build and verify compilation**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build`
Expected: Clean compilation, no errors.

- [ ] **Step 5: Commit**

```bash
git add src/world/pathfinder.h src/world/pathfinder.cpp src/CMakeLists.txt
git commit -m "feat: add grid A* pathfinder for enemy tactical navigation"
```

---

### Task 2: Spatial Queries

**Files:**
- Modify: `src/world/level_grid.h`
- Modify: `src/world/level_grid.cpp`

- [ ] **Step 1: Add spatial query declarations to level_grid.h**

Append before the closing of the file:

```cpp
// Tactical spatial queries for enemy AI
namespace LevelGridQuery {
    // Find nearest walkable cell with cover from threatPos (wall blocks LOS).
    // BFS from 'from', max 8-cell radius. Returns false if none found.
    bool findCoverCell(const LevelGrid& grid, Vec3 from, Vec3 threatPos,
                       Vec3& outPos, f32 maxRadius = 8.0f);

    // Find a walkable cell at ~90-120 deg offset from target-entity line.
    // Used for flanking. preferRight hints search direction.
    bool findFlankCell(const LevelGrid& grid, Vec3 entityPos, Vec3 targetPos,
                       f32 attackRange, bool preferRight, Vec3& outPos);

    // Find doorway cells in a room (adjacent to corridor, 1 solid orthogonal neighbor).
    // Returns count found (up to maxResults).
    u8 findDoorwayCells(const LevelGrid& grid, u32 roomX, u32 roomZ,
                        u32 roomW, u32 roomD, Vec3* outPositions, u8 maxResults = 4);

    // Calculate spread position for SURROUND state.
    Vec3 getSurroundPosition(Vec3 targetPos, u8 slotIndex, u8 totalSlots, f32 radius);
}
```

- [ ] **Step 2: Implement spatial queries in level_grid.cpp**

Append to end of `src/world/level_grid.cpp`:

```cpp
// ---------------------------------------------------------------------------
// Tactical spatial queries
// ---------------------------------------------------------------------------
#include "world/raycast.h"
#include <cmath>

bool LevelGridQuery::findCoverCell(const LevelGrid& grid, Vec3 from, Vec3 threatPos,
                                    Vec3& outPos, f32 maxRadius) {
    u32 startX, startZ;
    if (!LevelGridSystem::worldToGrid(grid, from, startX, startZ)) return false;

    // BFS outward from start cell
    struct QEntry { u16 x, z; };
    static QEntry queue[256];
    static bool visited[256 * 256]; // TODO: use smaller bounded array
    u16 qHead = 0, qTail = 0;

    // Use a fixed visited array bounded by search radius
    u16 maxCells = static_cast<u16>(maxRadius * 2 + 1);
    u16 offsetX = static_cast<u16>(startX) - static_cast<u16>(maxRadius);
    u16 offsetZ = static_cast<u16>(startZ) - static_cast<u16>(maxRadius);

    // Simple bounded BFS with inline visited tracking
    static u8 visitedBuf[64 * 64]; // 64x64 max search area
    memset(visitedBuf, 0, sizeof(visitedBuf));

    auto localIdx = [&](u16 x, u16 z) -> s32 {
        s32 lx = static_cast<s32>(x) - static_cast<s32>(startX) + 32;
        s32 lz = static_cast<s32>(z) - static_cast<s32>(startZ) + 32;
        if (lx < 0 || lx >= 64 || lz < 0 || lz >= 64) return -1;
        return lz * 64 + lx;
    };

    queue[qTail++] = {static_cast<u16>(startX), static_cast<u16>(startZ)};
    s32 startLocalIdx = localIdx(static_cast<u16>(startX), static_cast<u16>(startZ));
    if (startLocalIdx >= 0) visitedBuf[startLocalIdx] = 1;

    static constexpr s16 dx[4] = {1, -1, 0, 0};
    static constexpr s16 dz[4] = {0, 0, 1, -1};

    while (qHead < qTail && qHead < 256) {
        QEntry cur = queue[qHead++];
        Vec3 cellWorld = LevelGridSystem::gridToWorld(grid, cur.x, cur.z);

        // Distance check
        f32 dist = length(cellWorld - from);
        if (dist > maxRadius) continue;

        // Check if this cell provides cover (wall blocks LOS from threat)
        if (cur.x != startX || cur.z != startZ) {
            Vec3 eyePos = cellWorld + Vec3{0, 0.5f, 0};
            Vec3 threatEye = threatPos + Vec3{0, 0.5f, 0};
            // Raycast from this cell to threat — if it hits a wall, we have cover
            Vec3 dir = threatEye - eyePos;
            f32 maxDist = length(dir);
            if (maxDist > 0.1f) {
                dir = dir * (1.0f / maxDist);
                f32 hitDist = 0.0f;
                Vec3 hitNorm;
                bool hit = Raycast::cast(grid, eyePos, dir, maxDist, hitDist, hitNorm);
                if (hit && hitDist < maxDist - 0.5f) {
                    outPos = cellWorld;
                    return true;
                }
            }
        }

        // Expand neighbors
        for (u8 d = 0; d < 4; d++) {
            s16 nx = static_cast<s16>(cur.x) + dx[d];
            s16 nz = static_cast<s16>(cur.z) + dz[d];
            if (nx < 0 || nz < 0) continue;
            u16 ux = static_cast<u16>(nx), uz = static_cast<u16>(nz);
            if (!LevelGridSystem::isInBounds(grid, ux, uz)) continue;
            if (LevelGridSystem::isSolid(grid, ux, uz)) continue;
            s32 li = localIdx(ux, uz);
            if (li < 0 || visitedBuf[li]) continue;
            visitedBuf[li] = 1;
            if (qTail < 256) queue[qTail++] = {ux, uz};
        }
    }
    return false;
}

bool LevelGridQuery::findFlankCell(const LevelGrid& grid, Vec3 entityPos, Vec3 targetPos,
                                    f32 attackRange, bool preferRight, Vec3& outPos) {
    // Find a cell at ~90 degrees from the target→entity line, at attack range distance
    Vec3 toEntity = entityPos - targetPos;
    f32 angle = atan2f(toEntity.z, toEntity.x);

    // Try angles at 90, 105, 120, 75, 60 degrees offset
    static constexpr f32 offsets[] = {1.57f, 1.83f, 2.09f, 1.31f, 1.05f};
    for (u8 i = 0; i < 5; i++) {
        f32 sign = preferRight ? 1.0f : -1.0f;
        f32 testAngle = angle + offsets[i] * sign;
        Vec3 candidate = targetPos + Vec3{cosf(testAngle) * attackRange, 0, sinf(testAngle) * attackRange};

        u32 gx, gz;
        if (!LevelGridSystem::worldToGrid(grid, candidate, gx, gz)) continue;
        if (LevelGridSystem::isSolid(grid, gx, gz)) continue;

        // Verify LOS from flank position to target (can attack from there)
        Vec3 dir = targetPos - candidate;
        f32 dist = length(dir);
        if (dist < 0.1f) continue;
        dir = dir * (1.0f / dist);
        f32 hitDist = 0.0f;
        Vec3 hitNorm;
        bool blocked = Raycast::cast(grid, candidate + Vec3{0, 0.5f, 0}, dir, dist, hitDist, hitNorm);
        if (!blocked || hitDist >= dist - 0.5f) {
            outPos = candidate;
            return true;
        }
    }
    // Try the other side if preferred side failed
    for (u8 i = 0; i < 5; i++) {
        f32 sign = preferRight ? -1.0f : 1.0f;
        f32 testAngle = angle + offsets[i] * sign;
        Vec3 candidate = targetPos + Vec3{cosf(testAngle) * attackRange, 0, sinf(testAngle) * attackRange};

        u32 gx, gz;
        if (!LevelGridSystem::worldToGrid(grid, candidate, gx, gz)) continue;
        if (LevelGridSystem::isSolid(grid, gx, gz)) continue;

        Vec3 dir = targetPos - candidate;
        f32 dist = length(dir);
        if (dist < 0.1f) continue;
        dir = dir * (1.0f / dist);
        f32 hitDist = 0.0f;
        Vec3 hitNorm;
        bool blocked = Raycast::cast(grid, candidate + Vec3{0, 0.5f, 0}, dir, dist, hitDist, hitNorm);
        if (!blocked || hitDist >= dist - 0.5f) {
            outPos = candidate;
            return true;
        }
    }
    return false;
}

u8 LevelGridQuery::findDoorwayCells(const LevelGrid& grid, u32 roomX, u32 roomZ,
                                     u32 roomW, u32 roomD, Vec3* outPositions, u8 maxResults) {
    u8 found = 0;
    // Scan room perimeter — doorways are non-solid cells on the edge adjacent to corridor
    for (u32 x = roomX; x < roomX + roomW && found < maxResults; x++) {
        for (u32 z = roomZ; z < roomZ + roomD && found < maxResults; z++) {
            // Only check perimeter cells
            if (x != roomX && x != roomX + roomW - 1 && z != roomZ && z != roomZ + roomD - 1) continue;
            if (!LevelGridSystem::isInBounds(grid, x, z)) continue;
            if (LevelGridSystem::isSolid(grid, x, z)) continue;

            // Check if adjacent to a non-room walkable cell (corridor)
            static constexpr s16 dx[4] = {1, -1, 0, 0};
            static constexpr s16 dz[4] = {0, 0, 1, -1};
            for (u8 d = 0; d < 4; d++) {
                s32 nx = static_cast<s32>(x) + dx[d];
                s32 nz = static_cast<s32>(z) + dz[d];
                if (nx < 0 || nz < 0) continue;
                u32 ux = static_cast<u32>(nx), uz = static_cast<u32>(nz);
                if (!LevelGridSystem::isInBounds(grid, ux, uz)) continue;
                if (LevelGridSystem::isSolid(grid, ux, uz)) continue;
                // Neighbor is walkable but outside room bounds — it's a corridor connection
                if (ux < roomX || ux >= roomX + roomW || uz < roomZ || uz >= roomZ + roomD) {
                    outPositions[found++] = LevelGridSystem::gridToWorld(grid, x, z);
                    break;
                }
            }
        }
    }
    return found;
}

Vec3 LevelGridQuery::getSurroundPosition(Vec3 targetPos, u8 slotIndex, u8 totalSlots, f32 radius) {
    f32 angle = (6.2832f * static_cast<f32>(slotIndex)) / static_cast<f32>(totalSlots);
    return targetPos + Vec3{cosf(angle) * radius, 0.0f, sinf(angle) * radius};
}
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build build`
Expected: Clean compilation.

- [ ] **Step 4: Commit**

```bash
git add src/world/level_grid.h src/world/level_grid.cpp
git commit -m "feat: add spatial queries for enemy tactical positioning"
```

---

### Task 3: Entity Struct Extensions

**Files:**
- Modify: `src/game/entity.h`

- [ ] **Step 1: Add new AIState values**

In the `AIState` enum (line 18), add after `DORMANT`:

```cpp
enum struct AIState : u8 {
    IDLE,
    CHASE,
    ATTACK,
    FLYBY,
    DORMANT,
    FLANK,     // circling to player's side/rear via A* path
    RETREAT,   // falling back to cover (low HP or post-attack feint)
    AMBUSH,    // holding doorway position, waiting for player
    STRAFE,    // ranged: sidestepping while firing
    SURROUND,  // melee: spreading to surround target
    DEAD,
};
```

- [ ] **Step 2: Add squad role enum and tactical fields to Entity**

Add after the `EnemyType` enum (before struct Entity):

```cpp
enum struct SquadRole : u8 {
    ROLE_NONE = 0,
    ROLE_RUSH,    // charge head-on
    ROLE_FLANK,   // circle to side/rear
    ROLE_HOLD,    // ranged: hold doorway/distance
    ROLE_HARASS,  // flying: orbit and dive
};
```

Add these fields to `struct Entity` after the `stuckTimer` field (line 82):

```cpp
    // Pathfinding (A* waypoint cache)
    Vec3 pathWaypoints[6] = {};
    u8   pathLen = 0;
    u8   pathIdx = 0;

    // Squad coordination
    SquadRole squadRole = SquadRole::ROLE_NONE;
    u16 squadId = 0xFFFF;  // room-based squad (0xFFFF = unassigned)

    // Tactical state
    f32  tacticalTimer = 0.0f;  // multi-purpose: re-flank, retreat hold, ambush wait
    f32  sprintTimer   = 0.0f;  // anti-kite sprint burst cooldown
    f32  kiteTimer     = 0.0f;  // how long target has maintained distance
    bool hasRetreated  = false; // prevent retreat loop (reset on re-engage)
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build build`
Expected: Clean compilation. Existing code continues to work since new fields have defaults.

- [ ] **Step 4: Commit**

```bash
git add src/game/entity.h
git commit -m "feat: add tactical AI states and pathfinding fields to Entity"
```

---

### Task 4: Squad Coordinator

**Files:**
- Create: `src/game/squad.h`
- Create: `src/game/squad.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Create squad header**

```cpp
// src/game/squad.h
#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "world/level_grid.h"
#include "world/level_gen.h"

static constexpr u32 MAX_SQUADS = 32;
static constexpr u8  MAX_SQUAD_MEMBERS = 8;
static constexpr f32 SQUAD_REASSIGN_INTERVAL = 3.0f;
static constexpr f32 SQUAD_ADJACENT_ALERT_DELAY = 2.0f;

struct Squad {
    u16  roomIdx;                         // BSP room index
    u16  memberIndices[MAX_SQUAD_MEMBERS]; // entity pool indices
    u8   memberCount;
    bool alerted;                          // has detected the player?
    f32  alertTimer;                       // countdown for adjacent-room alert propagation
    f32  reassignTimer;                    // countdown to next role reassignment
};

struct SquadPool {
    Squad squads[MAX_SQUADS];
    u32   squadCount;
};

namespace SquadSystem {
    // Rebuild squads from current room layout and entity positions.
    // Called once per floor generation.
    void rebuild(SquadPool& pool, const DungeonResult& dungeon,
                 EntityPool& entities);

    // Per-frame update: handle alert propagation and role reassignment.
    void update(SquadPool& pool, const DungeonResult& dungeon,
                EntityPool& entities, Vec3 playerPos, f32 dt);

    // Called when an entity detects the player — alerts its squad.
    void alertSquad(SquadPool& pool, u16 entityIndex, EntityPool& entities);

    // Called when a squad member dies — reassign roles.
    void onMemberDeath(SquadPool& pool, u16 entityIndex, EntityPool& entities);
}
```

- [ ] **Step 2: Create squad implementation**

```cpp
// src/game/squad.cpp
// Room-based squad coordinator: assigns tactical roles to enemy groups.
// Each dungeon room maps to one squad. When any member detects the player,
// the squad is alerted and roles are assigned based on enemy type/position.

#include "game/squad.h"
#include <cstring>
#include <cmath>

// Determine which room an entity is in based on position
static s32 findRoomForEntity(const DungeonResult& dungeon, Vec3 pos) {
    for (u32 r = 0; r < dungeon.roomCount; r++) {
        const DungeonRoom& room = dungeon.rooms[r];
        if (pos.x >= static_cast<f32>(room.x) &&
            pos.x < static_cast<f32>(room.x + room.w) &&
            pos.z >= static_cast<f32>(room.z) &&
            pos.z < static_cast<f32>(room.z + room.d)) {
            return static_cast<s32>(r);
        }
    }
    return -1; // in a corridor
}

static void assignRoles(Squad& squad, EntityPool& entities, Vec3 playerPos) {
    if (!squad.alerted || squad.memberCount == 0) return;

    // Classify members
    u8 rushCount = 0;
    for (u8 m = 0; m < squad.memberCount; m++) {
        Entity& e = entities.entities[squad.memberIndices[m]];
        if (e.flags & ENT_DEAD) continue;

        if (e.flags & ENT_FLYING) {
            e.squadRole = SquadRole::ROLE_HARASS;
        } else if (e.attackRange > 5.0f) {
            e.squadRole = SquadRole::ROLE_HOLD;
        } else {
            // Melee: first 2 rush, rest flank
            if (rushCount < 2) {
                e.squadRole = SquadRole::ROLE_RUSH;
                rushCount++;
            } else {
                e.squadRole = SquadRole::ROLE_FLANK;
            }
        }
    }
}

void SquadSystem::rebuild(SquadPool& pool, const DungeonResult& dungeon,
                           EntityPool& entities) {
    memset(&pool, 0, sizeof(SquadPool));
    pool.squadCount = dungeon.roomCount;

    // Initialize squads — one per room
    for (u32 r = 0; r < dungeon.roomCount && r < MAX_SQUADS; r++) {
        pool.squads[r].roomIdx = static_cast<u16>(r);
        pool.squads[r].memberCount = 0;
        pool.squads[r].alerted = false;
        pool.squads[r].alertTimer = 0.0f;
        pool.squads[r].reassignTimer = 0.0f;
    }

    // Assign entities to squads based on position
    for (u32 i = 0; i < entities.activeCount; i++) {
        u32 idx = entities.activeList[i];
        Entity& e = entities.entities[idx];
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.flags & ENT_DEAD) continue;

        s32 roomIdx = findRoomForEntity(dungeon, e.position);
        if (roomIdx < 0 || roomIdx >= static_cast<s32>(pool.squadCount)) continue;

        Squad& squad = pool.squads[roomIdx];
        if (squad.memberCount < MAX_SQUAD_MEMBERS) {
            squad.memberIndices[squad.memberCount++] = static_cast<u16>(idx);
            e.squadId = static_cast<u16>(roomIdx);
        }
    }
}

void SquadSystem::update(SquadPool& pool, const DungeonResult& dungeon,
                          EntityPool& entities, Vec3 playerPos, f32 dt) {
    for (u32 s = 0; s < pool.squadCount; s++) {
        Squad& squad = pool.squads[s];
        if (squad.memberCount == 0) continue;

        // Alert propagation timer (adjacent rooms hear combat after delay)
        if (squad.alertTimer > 0.0f) {
            squad.alertTimer -= dt;
            if (squad.alertTimer <= 0.0f) {
                squad.alerted = true;
                assignRoles(squad, entities, playerPos);
            }
        }

        // Periodic role reassignment
        if (squad.alerted) {
            squad.reassignTimer -= dt;
            if (squad.reassignTimer <= 0.0f) {
                squad.reassignTimer = SQUAD_REASSIGN_INTERVAL;
                assignRoles(squad, entities, playerPos);
            }
        }
    }
}

void SquadSystem::alertSquad(SquadPool& pool, u16 entityIndex, EntityPool& entities) {
    Entity& e = entities.entities[entityIndex];
    if (e.squadId == 0xFFFF || e.squadId >= pool.squadCount) return;

    Squad& squad = pool.squads[e.squadId];
    if (squad.alerted) return; // already alerted

    squad.alerted = true;
    squad.reassignTimer = SQUAD_REASSIGN_INTERVAL;

    // Get player position from entity's target direction for role assignment
    Vec3 playerPos = e.lastSeenPos; // approximate
    assignRoles(squad, entities, playerPos);

    // Alert adjacent rooms after a delay
    // Adjacent = rooms whose bounds are within 3 cells of this room
    const DungeonRoom& thisRoom = {/* accessed via squad.roomIdx from dungeon */};
    // (Adjacency check requires dungeon reference — handled in update loop)
    for (u32 s = 0; s < pool.squadCount; s++) {
        if (s == e.squadId) continue;
        if (!pool.squads[s].alerted && pool.squads[s].alertTimer <= 0.0f) {
            pool.squads[s].alertTimer = SQUAD_ADJACENT_ALERT_DELAY;
        }
    }
}

void SquadSystem::onMemberDeath(SquadPool& pool, u16 entityIndex, EntityPool& entities) {
    Entity& e = entities.entities[entityIndex];
    if (e.squadId == 0xFFFF || e.squadId >= pool.squadCount) return;

    Squad& squad = pool.squads[e.squadId];

    // Remove from member list
    for (u8 m = 0; m < squad.memberCount; m++) {
        if (squad.memberIndices[m] == entityIndex) {
            squad.memberIndices[m] = squad.memberIndices[--squad.memberCount];
            break;
        }
    }

    // Force immediate reassignment
    squad.reassignTimer = 0.0f;
}
```

- [ ] **Step 3: Add squad.cpp to CMakeLists.txt**

Add `game/squad.cpp` to the source list.

- [ ] **Step 4: Build and verify**

Run: `cmake --build build`
Expected: Clean compilation.

- [ ] **Step 5: Commit**

```bash
git add src/game/squad.h src/game/squad.cpp src/CMakeLists.txt
git commit -m "feat: add squad coordinator for room-based enemy role assignment"
```

---

### Task 5: Tactical FSM States in enemy_ai.cpp

**Files:**
- Modify: `src/game/enemy_ai.h`
- Modify: `src/game/enemy_ai.cpp`

- [ ] **Step 1: Update enemy_ai.h to include squad and pathfinder**

```cpp
// src/game/enemy_ai.h
#pragma once

#include "core/types.h"
#include "game/entity.h"
#include "game/squad.h"
#include "game/projectile.h"
#include "world/level_grid.h"

struct Player;

namespace EnemyAI {
    static constexpr u32 MAX_AI_TARGETS = 4;
    void update(EntityPool& pool, const LevelGrid& grid,
                Player& player, ProjectilePool& projectiles, f32 dt,
                SquadPool* squads = nullptr,
                Player** extraPlayers = nullptr, u32 extraPlayerCount = 0);
}
```

- [ ] **Step 2: Add new tactical state handlers in enemy_ai.cpp**

Add these new case blocks inside the `switch (e.aiState)` statement in `EnemyAI::update`, after the existing DORMANT case and before the DEAD case. Also add the anti-kite logic to CHASE and projectile leading to ATTACK.

**Insert after `case AIState::DORMANT:` block (after line 1366):**

```cpp
        case AIState::FLANK: {
            // Follow A* path to flank position
            if (e.pathIdx < e.pathLen) {
                Vec3 wp = e.pathWaypoints[e.pathIdx];
                Vec3 toWP = wp - e.position;
                f32 wpDist = length(Vec3{toWP.x, 0, toWP.z});
                if (wpDist < 0.5f) {
                    e.pathIdx++;
                } else {
                    Vec3 moveDir = normalize(Vec3{toWP.x, 0, toWP.z});
                    e.velocity.x = moveDir.x * effectiveSpeed * 1.2f;
                    e.velocity.z = moveDir.z * effectiveSpeed * 1.2f;
                }
            } else {
                // Arrived at flank position — attack
                e.aiState = AIState::ATTACK;
                e.attackTimer = 0.1f;
                e.hasRetreated = false;
            }
            entityMoveAndSlide(e, grid, dt);
            if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);

            // Re-flank every 4 seconds if still in FLANK
            e.tacticalTimer -= dt;
            if (e.tacticalTimer <= 0.0f) {
                e.aiState = AIState::CHASE; // re-evaluate
            }
        } break;

        case AIState::RETREAT: {
            // Follow path to cover cell
            if (e.pathIdx < e.pathLen) {
                Vec3 wp = e.pathWaypoints[e.pathIdx];
                Vec3 toWP = wp - e.position;
                f32 wpDist = length(Vec3{toWP.x, 0, toWP.z});
                if (wpDist < 0.5f) {
                    e.pathIdx++;
                } else {
                    Vec3 moveDir = normalize(Vec3{toWP.x, 0, toWP.z});
                    e.velocity.x = moveDir.x * effectiveSpeed * 1.4f; // run fast
                    e.velocity.z = moveDir.z * effectiveSpeed * 1.4f;
                }
            } else {
                // At cover — hold position briefly
                e.velocity = {0, 0, 0};
            }
            entityMoveAndSlide(e, grid, dt);
            if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);

            // Hold in cover for 1.5 seconds then re-engage
            e.tacticalTimer -= dt;
            if (e.tacticalTimer <= 0.0f) {
                e.aiState = AIState::CHASE;
                e.hasRetreated = true;
            }
        } break;

        case AIState::AMBUSH: {
            // Wait motionless at doorway until player enters kill zone
            e.velocity = {0, 0, 0};
            // Face toward room interior (toward expected player approach)
            Vec3 toTarget = targetPos - e.position;
            if (lengthSq(toTarget) > 0.01f) {
                e.yaw = atan2f(-toTarget.x, -toTarget.z);
            }
            // Trigger when player is close and has LOS
            if (dist <= 4.0f && hasLOS(e, player, grid)) {
                e.aiState = AIState::ATTACK;
                e.attackTimer = 0.0f; // immediate attack burst
            }
        } break;

        case AIState::STRAFE: {
            // Sidestep perpendicular to aim direction while firing
            Vec3 toTarget = targetPos - e.position;
            f32 targetLen = length(Vec3{toTarget.x, 0, toTarget.z});
            if (targetLen > 0.01f) {
                Vec3 forward = Vec3{toTarget.x, 0, toTarget.z} * (1.0f / targetLen);
                // Lateral direction (alternates based on tacticalTimer)
                f32 side = (e.tacticalTimer > 0.0f) ? 1.0f : -1.0f;
                Vec3 lateral = {-forward.z * side, 0, forward.x * side};
                e.velocity.x = lateral.x * effectiveSpeed * 0.7f;
                e.velocity.z = lateral.z * effectiveSpeed * 0.7f;
                e.yaw = atan2f(-forward.x, -forward.z);
            }

            entityMoveAndSlide(e, grid, dt);
            if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);

            // Change strafe direction every 1-2 seconds
            e.tacticalTimer -= dt;
            if (e.tacticalTimer <= -1.5f) e.tacticalTimer = 1.0f + (std::rand() % 100) * 0.01f;

            // Fire while strafing (same attack logic as ATTACK state)
            e.attackTimer -= dt;
            if (e.attackTimer <= 0.0f && hasLOSToPoint(
                    e.position + Vec3{0, e.halfExtents.y, 0}, targetPos, grid)) {
                e.attackTimer = e.attackCooldown;
                e.attackAnimT = 0.3f;
                Vec3 atkOrigin = e.position + Vec3{0, e.halfExtents.y, 0};
                // Lead the shot: predict target position
                Vec3 predictedPos = targetPos + targetVel * (dist / 14.0f);
                Vec3 atkDir = normalize(predictedPos - atkOrigin);
                f32 projSpeed = 14.0f;
                f32 projRadius = 0.08f;
                ProjectileSystem::spawn(projectiles, atkOrigin,
                    atkDir, projSpeed, e.damage, projRadius, 3.0f, false);
            }

            // Exit if player closes distance
            if (targetDist < e.attackRange * 0.5f) {
                e.aiState = AIState::CHASE;
            }
        } break;

        case AIState::SURROUND: {
            // Move to assigned spread position around target
            Vec3 goalPos = LevelGridQuery::getSurroundPosition(
                targetPos, static_cast<u8>(i % 6), 4, e.attackRange * 0.8f);
            Vec3 toGoal = goalPos - e.position;
            f32 goalDist = length(Vec3{toGoal.x, 0, toGoal.z});

            if (goalDist > 0.5f) {
                Vec3 moveDir = normalize(Vec3{toGoal.x, 0, toGoal.z});
                e.velocity.x = moveDir.x * effectiveSpeed;
                e.velocity.z = moveDir.z * effectiveSpeed;
            } else {
                // In position — attack
                e.aiState = AIState::ATTACK;
                e.attackTimer = 0.2f;
            }
            entityMoveAndSlide(e, grid, dt);
            if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);
        } break;
```

- [ ] **Step 3: Add anti-kite sprint burst to CHASE state**

In the CHASE state's ground movement section (around line 1186), after the `e.velocity.x = flatDir.x * speed;` block, add:

```cpp
                        // Anti-kite: if target keeps distance for 2s, sprint burst
                        if (targetDist > e.attackRange * 1.5f &&
                            targetDist < e.detectionRange) {
                            e.kiteTimer += dt;
                            if (e.kiteTimer > 2.0f && e.sprintTimer <= 0.0f) {
                                speed = effectiveSpeed * 2.0f; // sprint burst
                                e.sprintTimer = 3.0f;          // 3s cooldown
                                e.kiteTimer = 0.0f;
                                e.velocity.x = flatDir.x * speed;
                                e.velocity.z = flatDir.z * speed;
                            }
                        } else {
                            e.kiteTimer = 0.0f;
                        }
                        e.sprintTimer -= dt;
```

- [ ] **Step 4: Add projectile leading to ATTACK state**

In the ranged attack section (around line 1323), replace the direction calculation:

```cpp
                        // Lead the shot: predict where target will be
                        Vec3 atkOrigin = e.position + Vec3{0, e.halfExtents.y, 0};
                        f32 projSpeed = 14.0f;
                        f32 projRadius = 0.08f;
                        if (e.flags & ENT_FLYING) { projSpeed = 10.0f; projRadius = 0.06f; }
                        f32 timeToHit = dist / projSpeed;
                        Vec3 predictedPos = targetPos + targetVel * timeToHit;
                        Vec3 atkDir = normalize(predictedPos - atkOrigin);
                        ProjectileSystem::spawn(projectiles, atkOrigin,
                            atkDir, projSpeed, e.damage, projRadius, 3.0f, false);
```

- [ ] **Step 5: Add squad-aware state transitions at the top of the CHASE case**

At the beginning of the CHASE state (after `case AIState::CHASE: {`), add role-based branching:

```cpp
            // Squad role overrides: redirect to tactical state if appropriate
            if (e.squadRole == SquadRole::ROLE_FLANK && !(e.flags & ENT_FLYING)) {
                // Repath to flank position every 4s
                if (e.pathLen == 0 || e.tacticalTimer <= 0.0f) {
                    Vec3 flankPos;
                    bool preferRight = (i % 2 == 0);
                    if (LevelGridQuery::findFlankCell(grid, e.position, targetPos,
                            e.attackRange, preferRight, flankPos)) {
                        e.pathLen = Pathfinder::findPath(grid, e.position, flankPos,
                            e.pathWaypoints);
                        e.pathIdx = 0;
                        if (e.pathLen > 0) {
                            e.aiState = AIState::FLANK;
                            e.tacticalTimer = 4.0f;
                            break;
                        }
                    }
                }
            }
            if (e.squadRole == SquadRole::ROLE_HOLD && e.attackRange > 5.0f) {
                // Ranged enemies strafe instead of standing still
                if (targetDist <= e.attackRange && hasLOSToPoint(
                        e.position + Vec3{0, e.halfExtents.y, 0}, targetPos, grid)) {
                    e.aiState = AIState::STRAFE;
                    e.tacticalTimer = 1.0f;
                    break;
                }
            }
```

- [ ] **Step 6: Add retreat trigger in ATTACK state**

After the attack execution (around line 1348), add:

```cpp
            // Retreat when low HP
            if (e.health < e.maxHealth * 0.3f && !e.hasRetreated &&
                !(e.flags & ENT_FLYING) && e.enemyType != EnemyType::BOSS) {
                Vec3 coverPos;
                if (LevelGridQuery::findCoverCell(grid, e.position, targetPos, coverPos)) {
                    e.pathLen = Pathfinder::findPath(grid, e.position, coverPos,
                        e.pathWaypoints);
                    e.pathIdx = 0;
                    if (e.pathLen > 0) {
                        e.aiState = AIState::RETREAT;
                        e.tacticalTimer = 1.5f;
                        break;
                    }
                }
            }
```

- [ ] **Step 7: Add include for pathfinder at the top of enemy_ai.cpp**

```cpp
#include "world/pathfinder.h"
#include "game/squad.h"
```

- [ ] **Step 8: Add `targetVel` calculation near the top of the per-entity loop**

Near where `targetPos` and `dist` are computed (early in the loop body), add:

```cpp
            // Target velocity for projectile leading
            Vec3 targetVel = targetIsNPC ?
                pool.entities[e.targetEntityIdx].velocity :
                player.velocity;
```

- [ ] **Step 9: Alert squad when enemy detects player**

In the IDLE → CHASE transition (around line 1084), after setting `e.aiState = AIState::CHASE`, add:

```cpp
                    // Alert squad
                    if (squads) SquadSystem::alertSquad(*squads, static_cast<u16>(i), pool);
```

- [ ] **Step 10: Build and verify**

Run: `cmake --build build`
Expected: Clean compilation. Game still runs (new states aren't triggered without squads wired in).

- [ ] **Step 11: Commit**

```bash
git add src/game/enemy_ai.h src/game/enemy_ai.cpp
git commit -m "feat: add tactical FSM states, anti-kiting, and projectile leading"
```

---

### Task 6: Wire Into Engine Game Loop

**Files:**
- Modify: `src/engine/engine.cpp`
- Modify: `src/engine/engine.h` (add SquadPool member)

- [ ] **Step 1: Add SquadPool to Engine class**

In `src/engine/engine.h`, add member (alongside existing pools):

```cpp
#include "game/squad.h"

// Inside class Engine:
    SquadPool m_squads;
```

- [ ] **Step 2: Initialize squads after floor generation**

In `engine.cpp`, after enemy spawning completes (after the spawn loop that places enemies in rooms), add:

```cpp
    // Build squad assignments for the new floor
    SquadSystem::rebuild(m_squads, dungeon, m_entities);
```

Find the location by searching for where `dungeon.rooms[r]` is used in the spawn loop (around line 1424-1490).

- [ ] **Step 3: Update squads each frame**

In the game update function (inside `singleplayerUpdate` or equivalent, after `EnemyAI::update` is called), add:

```cpp
    SquadSystem::update(m_squads, m_dungeon, m_entities, playerWorldPos, dt);
```

- [ ] **Step 4: Pass squads to EnemyAI::update**

Change the existing `EnemyAI::update` call to pass the squad pool:

```cpp
    EnemyAI::update(m_entities, m_grid, m_localPlayer, m_projectiles, dt,
                    &m_squads, extraPlayers, extraPlayerCount);
```

- [ ] **Step 5: Call onMemberDeath in the death callback**

In the entity death callback (set up in `Engine::init`, around the `Combat::setDeathCallback` area), add:

```cpp
    SquadSystem::onMemberDeath(s_engine->m_squads, handle.index, s_engine->m_entities);
```

- [ ] **Step 6: Build and verify**

Run: `cmake --build build`
Expected: Clean compilation. Run the game — enemies should now exhibit tactical behavior (flanking, strafing, retreat).

- [ ] **Step 7: Commit**

```bash
git add src/engine/engine.h src/engine/engine.cpp
git commit -m "feat: wire squad system and tactical AI into game loop"
```

---

### Task 7: Debug Visualization

**Files:**
- Modify: `src/renderer/debug_draw.h` (if needed)
- Modify: `src/engine/engine.cpp` (render pass)

- [ ] **Step 1: Add debug draw for AI paths and roles**

In the debug render section (guarded by F1 toggle), add visualization of enemy paths and squad roles:

```cpp
    // Draw enemy A* paths and squad roles when debug overlay is active
    if (m_debugDraw) {
        for (u32 i = 0; i < m_entities.activeCount; i++) {
            u32 idx = m_entities.activeList[i];
            const Entity& e = m_entities.entities[idx];
            if (e.flags & ENT_DEAD) continue;
            if (e.flags & ENT_FRIENDLY) continue;

            // Draw path waypoints as lines
            if (e.pathLen > 0) {
                Vec3 prev = e.position;
                for (u8 p = e.pathIdx; p < e.pathLen; p++) {
                    Vec3 color = {0.2f, 0.9f, 0.2f}; // green path
                    DebugDraw::line(prev, e.pathWaypoints[p], color);
                    prev = e.pathWaypoints[p];
                }
            }

            // Color-code by squad role
            Vec3 roleColor = {1, 1, 1};
            switch (e.squadRole) {
                case SquadRole::ROLE_RUSH:   roleColor = {1.0f, 0.2f, 0.2f}; break; // red
                case SquadRole::ROLE_FLANK:  roleColor = {1.0f, 0.8f, 0.0f}; break; // yellow
                case SquadRole::ROLE_HOLD:   roleColor = {0.2f, 0.5f, 1.0f}; break; // blue
                case SquadRole::ROLE_HARASS: roleColor = {0.8f, 0.2f, 1.0f}; break; // purple
                default: break;
            }
            DebugDraw::line(e.position, e.position + Vec3{0, 1.5f, 0}, roleColor);
        }
    }
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build`
Expected: Clean. Press F1 in-game to see colored role indicators and path lines.

- [ ] **Step 3: Commit**

```bash
git add src/engine/engine.cpp
git commit -m "feat: add debug visualization for enemy paths and squad roles"
```

---

### Task 8: Switch Build Verification

- [ ] **Step 1: Build for Switch and deploy**

```bash
cmake --build build 2>&1 | grep "error:" | head -3
echo "---"
docker run --rm -u "$(id -u):$(id -g)" -v /home/aaron/game:/game -w /game devkitpro/devkita64 \
    bash -c "source /opt/devkitpro/switchvars.sh && cmake --build build-switch 2>&1" | tail -5
```

Expected: Both builds succeed.

- [ ] **Step 2: Playtest checklist**

Verify in-game:
- [ ] Enter a room with 3+ enemies — they don't all chase in a line
- [ ] At least one enemy takes a side corridor (FLANK role)
- [ ] Ranged enemies sidestep while shooting (STRAFE)
- [ ] Kiting backward for 2+ seconds triggers enemy sprint burst
- [ ] Low-HP enemies retreat behind walls
- [ ] Ranged projectiles lead the player's movement
- [ ] F1 debug shows path lines and role colors
- [ ] Performance stays at 60 FPS (check F3 profiler)

- [ ] **Step 3: Final commit if any fixes needed**

```bash
git add -u
git commit -m "fix: address playtest issues in tactical AI"
```
