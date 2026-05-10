# Tactical Enemy AI Design

## Problem

Enemies are predictable (chase-attack loop), uncoordinated (act independently), and don't punish passive play. Players can kite, pick enemies off individually, and ignore dungeon layout as a tactical factor.

## Goal

Make enemies smart enough to make the player sweat from floor 1. Enemies should use the dungeon layout against the player, coordinate in groups, and punish predictable/passive player behavior.

## Design

Three new systems layered on the existing FSM. Each is independent and testable.

---

## 1. Grid A* Pathfinder

**Files:** `src/world/pathfinder.h`, `src/world/pathfinder.cpp`

Grid-based A* operating on `LevelGrid` cell coordinates. Enables enemies to navigate around obstacles, take alternate corridors, and reach tactical positions.

### Interface

```cpp
namespace Pathfinder {
    // Find path from start to goal on the grid. Returns waypoint count (0 = no path).
    // Writes up to maxWaypoints world-space positions into outPath.
    // maxSearch bounds the open set to prevent runaway on large maps.
    u8 findPath(const LevelGrid& grid, Vec3 start, Vec3 goal,
                Vec3* outPath, u8 maxWaypoints = 6, u16 maxSearch = 256);
}
```

### Behavior

- Operates on cell centers (integer grid coords → world position conversion)
- 4-connected neighbors (no diagonals — matches the blocky dungeon aesthetic)
- Heuristic: Manhattan distance
- Max search: 256 cells (~16 cell radius). Beyond that, fall back to flow field.
- Cells with `CELL_SOLID` flag are impassable. Flying entities ignore floor/ceiling height checks but still respect solid walls.

### Scheduling

- Each entity re-paths every 0.5 seconds (30 frames at 60 FPS)
- Staggered by entity index: `if ((frameCount + entity.index) % 30 == 0) repath()`
- At 128 entities max, this means ~4 A* queries per frame
- Path stored on Entity: `Vec3 pathWaypoints[6]; u8 pathLen; u8 pathIdx;`

### Fallback

If A* fails (no path within maxSearch), enemy falls back to direct chase with wall sliding (current behavior). This handles edge cases like enemies trapped in dead ends.

---

## 2. Squad Coordinator

**Files:** `src/game/squad.h`, `src/game/squad.cpp`

Room-based coordination that assigns tactical roles to groups of enemies.

### Data

```cpp
enum SquadRole : u8 { ROLE_NONE, ROLE_RUSH, ROLE_FLANK, ROLE_HOLD, ROLE_HARASS };

struct Squad {
    u16 roomIdx;                      // BSP room this squad belongs to
    u16 memberIndices[8];             // entity pool indices
    u8  memberCount;
    u8  roles[8];                     // SquadRole per member
    bool alerted;                     // has this squad detected the player?
    f32 reassignTimer;                // countdown to next role reassignment
};
```

### Constants

- `MAX_SQUADS = 32` (one per dungeon room, matches `MAX_DUNGEON_ROOMS`)
- Reassignment interval: 3 seconds, or immediately when a member dies
- Max members per squad: 8

### Role Assignment Logic

When a squad is alerted (any member detects the player):

1. Count melee vs ranged vs flying members
2. Assign 1-2 melee as `ROLE_RUSH` (closest to player)
3. If 2+ melee remain, assign 1 as `ROLE_FLANK` (furthest from player, has alternate path)
4. Ranged enemies get `ROLE_HOLD` (stay at range, prefer doorway positions)
5. Flying enemies get `ROLE_HARASS` (orbit and dive, never commit)

On member death: promote next FLANK to RUSH if no rushers remain. Re-evaluate after 3s.

### Alert Propagation

- When one enemy gains LOS to player, all enemies in the same room are alerted (instant)
- Adjacent rooms are alerted after a 2-second delay (simulates enemies hearing combat)
- Alert propagation uses room adjacency from the BSP tree

### Integration

- `Squad::update()` called once per frame from the game loop (after entity AI update)
- Squads are rebuilt when a new floor generates (`Squad::rebuild(rooms, entities)`)
- Entity reads its role from `entity.squadRole` to determine FSM behavior

---

## 3. Tactical FSM States

**Modified file:** `src/game/enemy_ai.cpp`
**Modified file:** `src/game/entity.h` (new AIState values + fields)

### New States

| State | Entry Condition | Behavior | Exit |
|-------|----------------|----------|------|
| `FLANK` | Role=FLANK, squad alerted | A* to flank cell (90-120° from player-facing). Move at full speed. | Arrive at flank cell → ATTACK |
| `RETREAT` | HP < 30% of max | A* to nearest cover cell (wall between self and player). Hold 1.5s. | Timer expires → CHASE |
| `AMBUSH` | Role=HOLD, doorway cell available | Move to doorway, face corridor, wait motionless. | Player enters 4-unit range + LOS → ATTACK (burst) |
| `STRAFE` | Ranged + in ATTACK state + target in range | Sidestep perpendicular to aim direction. Change direction every 1-2s randomly. Fire while moving. | Target closes to < 50% of attack range → CHASE |
| `SURROUND` | 2+ melee RUSH enemies on same target | Each picks an angle (evenly divided around target). Move to that position. | In position + cooldown ready → ATTACK |

### Modified Existing States

**CHASE:**
- Anti-kite: if target distance hasn't decreased for 2s, trigger sprint burst (1.5× speed, 1s duration, 2s cooldown)
- If role=FLANK, transition to FLANK instead of direct chase

**ATTACK:**
- Ranged enemies: lead shots by predicting target velocity (`targetPos + targetVel * dist / projSpeed`)
- After 2 attacks, melee enemies with role=RUSH have 30% chance to trigger a short RETREAT (feint)

### Entity Struct Extensions

```cpp
// Pathfinding
Vec3 pathWaypoints[6];
u8   pathLen = 0;
u8   pathIdx = 0;

// Squad
u8   squadRole = 0;       // SquadRole enum
u16  squadId = 0xFFFF;    // which squad (0xFFFF = unassigned)

// Tactical timers
f32  tacticalTimer = 0.0f;  // multi-purpose: re-flank interval, retreat hold, ambush patience
f32  sprintTimer = 0.0f;    // anti-kite sprint burst cooldown
f32  kiteTimer = 0.0f;      // time target has been maintaining distance
bool hasRetreated = false;  // prevent retreat looping (reset on re-engage)
```

---

## 4. Spatial Queries

**Added to:** `src/world/level_grid.h`, `src/world/level_grid.cpp`

Cheap local queries enemies use to find tactical positions.

### Functions

```cpp
namespace LevelGridQuery {
    // Find nearest walkable cell with cover from threatPos (wall blocks LOS).
    // BFS outward from 'from', max 8-cell radius. Returns false if none found.
    bool findCoverCell(const LevelGrid& grid, Vec3 from, Vec3 threatPos,
                       Vec3& outPos, f32 maxRadius = 8.0f);

    // Find a walkable cell at roughly 90-120° offset from the target-entity line.
    // Used for flanking positions. preferRight hints initial search direction.
    bool findFlankCell(const LevelGrid& grid, Vec3 entityPos, Vec3 targetPos,
                       f32 attackRange, bool preferRight, Vec3& outPos);

    // Find doorway cells in a room (cells adjacent to corridor with 1 solid neighbor).
    // Writes up to maxResults positions. Returns count found.
    u8 findDoorwayCells(const LevelGrid& grid, u16 roomIdx,
                        Vec3* outPositions, u8 maxResults = 4);

    // Calculate ideal spread angle for SURROUND state.
    // Returns world position at 'radius' from target, at evenly-distributed angle.
    Vec3 getSurroundPosition(Vec3 targetPos, u8 slotIndex, u8 totalSlots, f32 radius);
}
```

### Implementation Notes

- `findCoverCell`: BFS with early termination. For each candidate, single raycast to threatPos. First cell where ray hits a wall = cover. Bounded to 8-cell radius (~50 cells checked worst case).
- `findFlankCell`: Arc search at attackRange distance from target. Check 30° increments from 60° to 150° (both sides). First walkable cell with LOS to target wins.
- `findDoorwayCells`: Scan room perimeter cells. A doorway cell has ≤2 solid orthogonal neighbors and connects to a non-room cell (corridor). Pre-computable at level gen time for zero runtime cost.
- `getSurroundPosition`: Pure math — `targetPos + Vec3(cos(angle), 0, sin(angle)) * radius` where angle = `2π * slotIndex / totalSlots`.

---

## 5. Player Experience Summary

**Before:** 3 skeletons spot you, all chase in a line, you backpedal and swing. They die.

**After:** 3 skeletons spot you. One charges head-on. A second disappears down a side corridor and appears behind you. The third waits at the doorway to the next room — if you retreat, it's ready. The charger occasionally backs off after swinging (feint), and if you try to kite, it sprint-bursts to close the gap. The ranged bone mage in the back is firing where you're going to be, not where you are.

---

## 6. Performance Budget

- A* queries: ~4 per frame (staggered 0.5s per entity, 128 max entities)
- Squad update: 1 pass over active squads per frame (max 32 squads, trivial)
- Spatial queries: triggered on state entry only (not per-frame), bounded BFS
- Extra per-entity memory: ~80 bytes (path cache + tactical fields)
- No heap allocation — all fixed arrays

---

## 7. Files Modified/Created

| Action | Path | Purpose |
|--------|------|---------|
| Create | `src/world/pathfinder.h` | A* interface |
| Create | `src/world/pathfinder.cpp` | A* implementation |
| Create | `src/game/squad.h` | Squad coordinator data + interface |
| Create | `src/game/squad.cpp` | Role assignment, alert propagation |
| Modify | `src/world/level_grid.h` | Add spatial query declarations |
| Modify | `src/world/level_grid.cpp` | Add spatial query implementations |
| Modify | `src/game/entity.h` | New AIState values, path/squad/tactical fields |
| Modify | `src/game/enemy_ai.cpp` | New tactical states, sprint burst, projectile leading |
| Modify | `src/engine/engine.cpp` | Wire squad system into game loop, init squads on floor gen |

---

## 8. Verification

- **Unit test pathfinder:** Generate a known grid, verify A* finds shortest path around walls
- **Visual test (F1 debug draw):** Draw entity paths, flank targets, cover cells, squad roles as colored lines/markers
- **Gameplay test:** Enter a room with 3+ enemies, verify they don't all chase in a line. Verify flanker takes alternate route. Verify retreating enemy seeks cover. Verify ranged enemies lead shots.
- **Performance test (F3 profiler):** AI frame time stays under 2ms with 128 active entities
- **Edge cases:** Dead-end rooms (no flank path available — falls back to RUSH). Single-enemy rooms (no squad coordination needed, behaves as current). Boss rooms (boss keeps existing ability system, squad coordinates adds around boss).
