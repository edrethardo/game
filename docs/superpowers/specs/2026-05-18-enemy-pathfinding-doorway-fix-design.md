# Fix NPC/Enemy Pathfinding and Doorway Sticking

## Context

All NPCs (enemies and friendlies) frequently get stuck in doorways and run into walls. Root causes:

1. **No pathfinding during CHASE** — enemies walk directly at the player. When LOS is blocked, they use sinusoidal drift (random wiggle) instead of real navigation. A* pathfinding exists but is only used for FLANK/RETREAT.
2. **No entity-entity separation** — entities pile up in tight spaces. No collision or pushing between entities.
3. **1-cell-wide doorways** — where corridors (2 cells wide) connect to rooms, the connection is only 1 cell (1.0m). Entities are 0.8m wide, so only one fits at a time.
4. **No stuck detection for hostiles** — friendly NPCs have stuck recovery (teleport after 0.5s), but hostile enemies have nothing.
5. **Drone/turret teleport range too short** — 15m causes drones to snap back too aggressively.

## Changes

### 1. A* pathfinding fallback in CHASE state (`enemy_ai.cpp`)

When `hasDirectLOS` is false during CHASE, use A* instead of sinusoidal drift:

- Call `Pathfinder::findPath()` to compute a path from current position to target
- Store in existing `e.pathWaypoints[6]` / `e.pathLen` / `e.pathIdx`
- Walk toward waypoints sequentially at chase speed
- Recalculate path every 2 seconds (reuse `e.tacticalTimer`) or when path is exhausted
- When LOS becomes clear again, drop the path and switch to direct movement
- Applies to ALL ground entities in CHASE (enemies and friendlies)

### 2. Entity-entity separation force (`enemy_ai.cpp`)

Add a soft repulsion pass after the per-entity AI loop:

- For each pair of active entities within 1.2m of each other, apply a gentle push apart
- Push magnitude: `0.5 * (1.2 - dist) / 1.2 * dt` — stronger when closer, zero at boundary
- Direction: along the line connecting their centers (XZ plane only)
- Skip pairs where both are drones (`npcClass == NONE && ENT_FRIENDLY`) — drones should swarm
- Skip dead entities
- O(n^2) but with MAX_ENTITIES=128 and early distance culling, well within budget

### 3. Stuck detection for hostile enemies (`enemy_ai.cpp`)

Extend the existing friendly-only stuck detection to all entities:

- Track movement: if entity moves < 0.05m in 0.5s while in CHASE, flag stuck
- Recovery: attempt A* path to target. If that fails, teleport to nearest walkable cell center
- Reuse existing `e.stuckTimer` field (already on all Entity, currently only used by friendlies)
- Add a `e.lastSeenPos` update for hostiles too (currently only tracked for friendlies)

### 4. Widen doorway connections to 2 cells (`level_gen.cpp`)

In `carveLCorridor()` and `carveCorridor()`, ensure where corridors meet room perimeters the opening is 2 cells wide instead of 1. Carve an extra adjacent cell at each room entry point.

### 5. Increase drone/turret teleport range (`enemy_ai.cpp`)

Change the teleport threshold from `15.0f` to `25.0f` in the drone section.

## Files to Modify

| File | Change |
|------|--------|
| `src/game/enemy_ai.cpp` | A* fallback in CHASE, entity separation pass, hostile stuck detection, drone teleport range |
| `src/world/level_gen.cpp` | Widen corridor-to-room connections |

## Verification

1. `cmake --build build` compiles
2. Spawn enemies with F4 near a doorway — they should path through without jamming
3. Multiple enemies chasing through corridors should spread out, not stack
4. Enemies around corners should navigate via A* instead of walking into walls
5. Drones/turrets stay with player at longer range before teleporting
