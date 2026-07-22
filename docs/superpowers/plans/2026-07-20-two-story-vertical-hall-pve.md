# Two-Story "Vertical Hall" PvE Floors — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new PvE layout style, `VERTICAL_HALL` — a chain of two-story chambers (ground pit + walk-under `CELL_PLATFORM` balcony joined by walkable ramps) with ranged "sniper nest" enemies on the balconies and the floor exit on the OPPOSITE story from the player's spawn, so sometimes you ascend and sometimes you descend. Enemies chase across both stories: they drop off edges to follow you down (free) and climb ramps to follow you up (ramp-portal routing).

**Architecture:** The two-story primitives (`CELL_PLATFORM` slab, story selection via `effectiveFloorHeight`, slab-aware collision/raycast/mesher) already ship and are proven by the Arena. This feature (a) makes *entity* movement story-aware (the player already is), (b) adds a `carveVerticalHall` generator that reuses the Arena's slab/ramp vocabulary procedurally, (c) routes cross-story chase through recorded ramp "portals" without touching the core `Pathfinder`, and (d) places nests + the opposite-story exit. Everything is **seed-built + server-authoritative**, so it replicates in co-op with **no wire/save change** (the jump-pad/arena story).

**Tech Stack:** C++17, doctest (`tests/`), CMake. No new dependencies, no new assets (slabs/ramps are level geometry using existing materials).

**Repo rules that bind this plan:** **no-unprompted-commits** — the commit steps run only under the user's standing per-task authorization; otherwise leave changes uncommitted and say so. `GridCell` and `Entity` are NOT serialized (grid rebuilt from seed; entities snapshot-replicated), so growing them is save/wire-safe. Every code change carries inline "why" comments; keep `CLAUDE.md` + `engine-reference`/`engine-how-to` skills in sync.

---

## Design decisions (locked, from brainstorming)

1. **Full two-story chase** — enemies pursue up AND down (not static-nest only).
2. **Vertical hall shape** — a serpentine chain of two-story chambers (models `carveGauntlet`), spawn forced to the first, exit to the last.
3. **Enemy descent = free-fall/drop** (v1: instant story-snap down; graceful gravity is Phase 4 polish). **Enemy ascent = walkable ramps** via ramp-portal routing.
4. **Frequency** — `VERTICAL_HALL` in the weighted `pickLayoutStyle` rotation from **floor 6+**, modest weight.
5. **Exit on the opposite story** — a per-seed coin-flip puts the player's spawn on the ground (→ exit on a balcony, ascend) or on a balcony (→ exit on the ground, descend).
6. **Player-only shortcut** — 0-1 `CELL_JUMPPAD`/`CELL_LEDGE` per chamber (enemies can't use it).
7. **Non-boss floors only** — boss floors keep their own arena geometry.

---

## File map

| File | Change |
|---|---|
| `src/game/enemy_ai.cpp` | `snapEntityToFloor` → story-aware (`effectiveFloorHeight`); Phase-4 gravity |
| `src/world/collision.cpp` / `.h` | `Collision::snapEntityToFloor` → story-aware; entity underside/band helpers (Phase 4) |
| `src/world/level_gen.h` | `LayoutStyle::VERTICAL_HALL`; `DungeonResult` gains `StoryPortal portals[]` + `bool spawnOnUpper` |
| `src/world/level_gen.cpp` | `carveVerticalHall`, `styleName`, `pickLayoutStyle` wiring, `generate` dispatch |
| `src/engine/engine_startgame.cpp` | spawn/exit story coin-flip (override `spawnPos.y` + `doorY`) |
| `src/engine/engine_spawn.cpp` | `spawnFloorNests` — ranged enemies on balcony cells |
| `src/engine/engine.h` | decl `spawnFloorNests`; store `m_level` story-portal access if needed |
| `src/game/enemy_ai_states.cpp` | CHASE cross-story routing (target story vs mine → nearest portal) |
| `src/game/enemy_ai.cpp` / `enemy_ai_internal.h` | `entityStory()` helper + portal lookup |
| `tests/world/test_platform.cpp` | entity story-snap cases (climb ramp / drop off edge / stand under slab) |
| `tests/world/test_vertical_hall.cpp` | **new** — gen validity (rooms≥5, spawn/exit reachable, balcony+ramp+portal present) |
| `tests/CMakeLists.txt` | add `world/test_vertical_hall.cpp` |
| `CLAUDE.md`, `.claude/skills/engine-reference/SKILL.md`, `.claude/skills/engine-how-to/SKILL.md` | doc sync |

---

# PHASE 1 — Story-aware entity movement (foundation)

*Outcome: an enemy physically stands on a slab when its feet reach the slab top (walking a ramp), stays on the ground story when under a balcony, and instantly drops to the ground when it walks off a balcony edge. Zero regression on single-story floors (a cell with no `CELL_PLATFORM` returns `getFloorHeight` unchanged).*

### Task 1.1: Make `snapEntityToFloor` story-aware

**Files:**
- Modify: `src/game/enemy_ai.cpp:76-83`
- Modify: `src/world/collision.cpp:434-441` (the `Vec3&` twin used by `ensureNotInWall`)
- Test: `tests/world/test_platform.cpp`

- [ ] **Step 1: Write failing entity tests** — append to `tests/world/test_platform.cpp`, reusing its `BalconyRoom` helper (a 12×12 grid with a slab band z=1..2, top 3.0 m). Entities are CENTRE-based, so feet = `position.y - halfExtents.y`.

```cpp
#include "game/enemy_ai_internal.h"   // snapEntityToFloor

static Entity groundEnemy(Vec3 centre) {
    Entity e{}; e.halfExtents = {0.35f, 0.5f, 0.35f}; e.position = centre; return e;
}

TEST_CASE("snapEntityToFloor: enemy under a balcony stays on the GROUND story") {
    BalconyRoom r;
    Entity e = groundEnemy({5.5f, 0.5f, 1.5f});   // feet ~0, beneath the z=1..2 slab
    snapEntityToFloor(e, r.grid);
    CHECK(e.position.y == doctest::Approx(0.5f));  // ground floor 0 + halfExtents.y
}

TEST_CASE("snapEntityToFloor: enemy with feet near the slab top stands ON the slab") {
    BalconyRoom r;
    Entity e = groundEnemy({5.5f, 3.5f, 1.5f});    // feet ~3.0 = slab top, on a slab cell
    snapEntityToFloor(e, r.grid);
    CHECK(e.position.y == doctest::Approx(3.5f));  // slab top 3.0 + halfExtents.y
}

TEST_CASE("snapEntityToFloor: walking off the balcony edge drops to the ground story") {
    BalconyRoom r;
    // feet high (was on balcony) but now over a NON-slab cell (z=5) → drops to ground
    Entity e = groundEnemy({5.5f, 3.5f, 5.5f});
    snapEntityToFloor(e, r.grid);
    CHECK(e.position.y == doctest::Approx(0.5f));
}
```

Run: `./build/tests/dungeon_tests -tc="*snapEntityToFloor*"` → FAIL (current impl uses raw `getFloorHeight`, so case 2 pins to 0.5 and case 3 stays at 3.5).

- [ ] **Step 2: Implement** — `enemy_ai.cpp:76-83`:

```cpp
void snapEntityToFloor(Entity& e, const LevelGrid& grid) {
    u32 gx, gz;
    if (LevelGridSystem::worldToGrid(grid, e.position, gx, gz) &&
        !LevelGridSystem::isSolid(grid, gx, gz)) {
        // Story-aware: pick the slab top vs the ground floor from the entity's FEET, exactly like
        // the player's collision does — so an enemy that walked up a ramp stands ON the balcony, one
        // beneath it stays under, and one that steps off the edge (feet now over a non-slab cell)
        // drops to the ground story. On single-story cells effectiveFloorHeight == getFloorHeight.
        const f32 feetY = e.position.y - e.halfExtents.y;
        e.position.y = LevelGridSystem::effectiveFloorHeight(grid, gx, gz, feetY) + e.halfExtents.y;
    }
}
```

And `collision.cpp:434-441` (`Collision::snapEntityToFloor(Vec3& pos, Vec3 halfExtents, grid)`) identically, using `pos.y - halfExtents.y` for feet. (Both must change or `ensureNotInWall` re-pins a slab-standing enemy to the ground.)

- [ ] **Step 3: Run** — `-tc="*snapEntityToFloor*"` PASS; then the full suite (no single-story regression, since `effectiveFloorHeight` is identity off platform cells).
- [ ] **Step 4: Commit** (if authorized): `feat(ai): story-aware entity floor-snap for two-story levels`.

> **Gotcha:** the tolerance that makes ramp-climbing work is `PLATFORM_STEP_TOLERANCE = 0.4f` (== `STEP_UP_HEIGHT`). Ramp steps MUST be ≤ 0.25 m (one quarter-unit) per cell so `feetY >= top - tolerance` holds each step — the Arena's ramps already are (Task 2.2 matches).

---

# PHASE 2 — VERTICAL_HALL geometry

*Outcome: a seed-built floor of chained two-story chambers with balconies, ramps, and the exit on the opposite story from spawn. Playable to walk through (with Phase 1, you can climb ramps and the exit is reachable). Nests + chase come in Phases 3-4.*

### Task 2.1: Register the layout style

**Files:** `src/world/level_gen.h` (enum ~L34), `src/world/level_gen.cpp` (`styleName` ~L800, `pickLayoutStyle` ~L810, `generate` dispatch ~L840)

- [ ] **Step 1:** `level_gen.h` — add before `COUNT`:
```cpp
enum struct LayoutStyle : u8 { BSP_ROOMS = 0, CAVERN, GAUNTLET, HUB, VERTICAL_HALL, COUNT };
```
- [ ] **Step 2:** `level_gen.cpp` `styleName` — add `case LayoutStyle::VERTICAL_HALL: return "vertical";`.
- [ ] **Step 3:** `pickLayoutStyle` — widen `kWeights` to **5 columns** (each row still summing to 100) and gate to floor ≥ 6. The 5th column is `VERTICAL_HALL`; keep it modest (0 on the tutorial tier so floors 4-5 never roll it):
```cpp
static constexpr u8 kWeights[5][5] = {
    {50, 12, 12, 12, 14},   // 4-10  (VERTICAL_HALL only rolls when floor>=6, gated below)
    {30, 12, 18, 25, 15},   // 11-20
    {22, 40,  8, 18, 12},   // 21-30
    {22, 12, 36, 18, 12},   // 31-40
    {22, 22, 18, 26, 12},   // 41-50
};
u32 acc = 0;
for (u32 s = 0; s < 5; s++) {
    acc += kWeights[tier][s];
    if (roll < acc) {
        LayoutStyle st = static_cast<LayoutStyle>(s);
        // VERTICAL_HALL is a NON-BOSS style (decision 7): boss floors expand a room into a boss
        // arena + rebuild the mesh, which would stomp balcony cells. Milestone bosses land every
        // 5th floor (see spawnFloorBoss); fall back to BSP on those and on the tutorial floors 4-5.
        if (st == LayoutStyle::VERTICAL_HALL && (floor < 6 || floor % 5 == 0))
            return LayoutStyle::BSP_ROOMS;
        return st;
    }
}
```
(Confirm the boss-floor predicate against `spawnFloorBoss` — if it's not "every 5th floor", use that helper instead of `floor % 5 == 0`.)
- [ ] **Step 4:** `generate` switch — add `case LayoutStyle::VERTICAL_HALL: carveVerticalHall(grid, rng, result, forcedSpawn, forcedExit); break;` (same signature shape as `carveGauntlet`).
- [ ] **Step 5:** Build; run the existing pathfinder/level tests (no behavior change yet — `carveVerticalHall` is added in 2.2).

### Task 2.2: `carveVerticalHall` — the generator

**Files:** `src/world/level_gen.cpp` (new static fn, model on `carveGauntlet` ~L585), `src/world/level_gen.h` (`DungeonResult` additions), `tests/world/test_vertical_hall.cpp` (new), `tests/CMakeLists.txt`

- [ ] **Step 1:** `level_gen.h` — extend `DungeonResult` (NOT serialized) so the AI + placement can find stories/ramps:
```cpp
struct StoryPortal { Vec3 lowPos; Vec3 highPos; };   // ramp base (ground) ↔ top (balcony), world coords
static constexpr u32 MAX_STORY_PORTALS = 16;
struct DungeonResult {
    // ...existing fields...
    StoryPortal portals[MAX_STORY_PORTALS] = {};
    u8   portalCount   = 0;
    bool spawnOnUpper  = false;   // VERTICAL_HALL coin-flip: true ⇒ spawn on a balcony, exit on ground (descend)
    // Balcony cell of the spawn room and the exit room (for placement); {0,0,0} if none.
    Vec3 spawnBalconyPos = {};
    Vec3 exitBalconyPos  = {};
};
```

- [ ] **Step 2: Write the failing gen test** — `tests/world/test_vertical_hall.cpp` (register in `tests/CMakeLists.txt` after `world/test_pathfinder.cpp`; `level_gen.cpp` + `level_grid.cpp` are already linked):
```cpp
// test_vertical_hall.cpp — VERTICAL_HALL invariants: enough rooms to avoid the BSP fallback,
// a reachable spawn+exit, and real two-story content (balcony slabs, walkable ramps, portals).
#include <doctest/doctest.h>
#include "world/level_gen.h"
#include "world/level_grid.h"

static u32 countFlag(const LevelGrid& g, u8 flag) {
    u32 n = 0; for (u32 i = 0; i < g.width * g.depth; i++) if (g.cells[i].flags & flag) n++; return n;
}

TEST_CASE("VERTICAL_HALL generates a valid two-story floor across many seeds") {
    for (u32 seed = 1; seed <= 64; seed++) {
        LevelGrid g; LevelGridSystem::init(g, 40, 40, 1.0f);
        DungeonResult r = LevelGen::generate(g, seed, 40, 40, LevelGen::LayoutStyle::VERTICAL_HALL);
        CHECK(r.roomCount >= 5);                 // never falls back to BSP
        CHECK(countFlag(g, CELL_PLATFORM) > 0);  // has balconies
        CHECK(r.portalCount > 0);                // has ramp portals for the chase
        CHECK(r.spawnRoomIdx != r.exitRoomIdx);
        // If we spawn on a balcony, the spawn room MUST have a ramp down or the player is stuck.
        if (r.spawnOnUpper) CHECK(lengthSq(r.spawnBalconyPos) > 0.0f);
        LevelGridSystem::shutdown(g);
    }
}
```
Run → FAIL (no `carveVerticalHall` yet).

- [ ] **Step 3: Implement `carveVerticalHall`** (static, `level_gen.cpp`). Structure, using the existing helpers `carveArea`, `carveLCorridor`, `addAdjacency`, and slab/ramp primitives adapted from the Arena (`engine_arena.cpp:149-196`):

  1. **Chain of 5-6 chambers** along a serpentine axis (mirror `carveGauntlet`'s room chain so `roomCount ≥ 5` and spawn/exit force to first/last). Each chamber is a `carveArea` room (≈10×10), ceiling raised to ≥ 20 qu (5 m) so a 3 m balcony fits. Record each as a `DungeonRoom`.
  2. **Per chamber, stamp a balcony band** along one wall: for the cells in the band set `flags = CELL_FLOOR | CELL_PLATFORM; platHeight = 12; platMaterialId = c.floorMaterialId` (reuse the room's own floor material — the floor retheme in `startGame` rewrites it per biome anyway; the Arena hardcodes a plank id only because it isn't rethemed). Keep the band ≤ half the room so the ground pit stays open beneath.
  3. **Stamp a walkable ramp** from the pit up to the balcony using **graduated `CELL_PLATFORM` slabs stepping one quarter-unit per cell** (Arena stairwell pattern, `engine_arena.cpp:179-196`): `platHeight` goes `1,2,…,12` across ~12 cells (0.25 m steps — climbable, story-snap-safe). Record a `StoryPortal{ lowPos = ramp base cell centre @ ground, highPos = ramp top cell centre @ balcony top }` in `result.portals[]`. **Every chamber that has a balcony MUST get at least one ramp** — especially the spawn room when `spawnOnUpper` (or a balcony-spawned player is stuck).
  4. **Optional player-only shortcut** (0-1 per chamber, RNG-gated): a `CELL_JUMPPAD` tile in the pit (`flags |= CELL_JUMPPAD`) OR a `CELL_LEDGE` riser — enemies can't use it (they never jump), the player can. Do NOT record it as a portal.
  5. **Corridors** between chambers via `carveLCorridor` + `addAdjacency`.
  6. **Coin-flip** `result.spawnOnUpper = (rng.next() & 1)`. Record `result.spawnBalconyPos` (a balcony cell centre of the spawn room) and `result.exitBalconyPos` (of the exit room) for Task 2.3.
  7. Set `forcedSpawn = firstChamberIdx; forcedExit = lastChamberIdx;`.

  **Determinism:** use ONLY `GenRNG` integer ops (no libm) — host + client must build byte-identical grids (`level_gen.cpp:9-11`). Heights are quarter-units; a balcony world-Y = `platHeight * 0.25f`.

- [ ] **Step 4:** Run `-tc="*VERTICAL_HALL*"` → PASS; full suite green.
- [ ] **Step 5: Commit** (if authorized): `feat(worldgen): VERTICAL_HALL two-story layout style`.

> **Room-count trap:** non-BSP styles with `roomCount < 5` are silently replaced by BSP (`generate` fallback, `level_gen.cpp:859`). The chained-chamber shape must register ≥ 5 rooms — the gen test pins this across 64 seeds.
> **Mesh/clearance ordering:** `carveVerticalHall` runs inside `generate`, BEFORE `buildClearanceField`/`buildFlowField`/mesh build in `startGame`, so the platforms are meshed and the ground-story clearance field is correct for free.

### Task 2.3: Exit + spawn on opposite stories

**Files:** `src/engine/engine_startgame.cpp` (spawn ~L539/811/888, exit door ~L681-693)

- [ ] **Step 1:** After `generate` returns (`startGame` ~L539), apply the coin-flip to the spawn Y for VERTICAL_HALL only:
```cpp
Vec3 spawnPos = dungeon.spawnPos;
if (layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL && dungeon.spawnOnUpper &&
    lengthSq(dungeon.spawnBalconyPos) > 0.0f) {
    spawnPos = dungeon.spawnBalconyPos;   // start on the balcony (→ descend to a ground exit)
}
```
- [ ] **Step 2:** At the exit-door block (`~L687`), when VERTICAL_HALL, set `doorY` (and X/Z) to the OPPOSITE story of spawn:
```cpp
if (layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL) {
    if (dungeon.spawnOnUpper) {
        // spawn upper ⇒ exit on the GROUND of the exit room (already the default doorY = floorHeight)
    } else if (lengthSq(dungeon.exitBalconyPos) > 0.0f) {
        m_level.floorDoorPos = dungeon.exitBalconyPos;   // spawn ground ⇒ exit on the balcony (ascend)
        m_level.floorDoorActive = true;
        LevelGridSystem::buildFlowField(m_level.grid, m_level.floorDoorPos);
    }
}
```
- [ ] **Step 3: Manual/runtime check** — launch a floor-6+ seed that rolls VERTICAL_HALL (temporarily force `pickLayoutStyle` to return it, or add a `--vhall` dev flag mirroring `--arena`): confirm you spawn on one story and the exit portal is on the other, the portal beam renders at the right height, and the interaction prompt only fires when you're within 2 m (3D) — i.e. after you've traversed. Revert the force.

> The exit portal render (`engine_render_effects.cpp:313+`) and the `lengthSq < 4.0f` proximity gate (`engine_update.cpp:2191, 2751`) are 3D — a balcony exit "just works" and is only reachable once you're on its story. No new interaction code needed.

---

# PHASE 3 — Cross-story chase (the "full chase")

*Outcome: an enemy whose target is on a DIFFERENT story routes to the nearest ramp portal and climbs, instead of milling under the balcony. Dropping down already works from Phase 1 (the 2D path toward the target's XZ walks the enemy off the balcony edge, and the story-snap drops it). This phase adds only the CLIMB.*

### Task 3.1: Story membership + portal lookup helpers

**Files:** `src/game/enemy_ai.cpp` / `enemy_ai_internal.h`; the dungeon (with `portals`) is already passed to `EnemyAI::update` (`&m_level.dungeon`) — thread it to `updateHostileStates`.

- [ ] **Step 1:** Add pure helpers (`enemy_ai_internal.h`). Note the caller passes the body's actual **feet** Y (enemy feet = `position.y - halfExtents.y`; the PLAYER's `position.y` IS its feet — player collision is feet-based, recon §1 — so pass `targetPlayer->position.y`, NOT the eye `targetPos.y`):
```cpp
// Which story a body's FEET are on in cell(xzPos): true = upper (slab top), false = ground.
inline bool onUpperStory(const LevelGrid& g, Vec3 xzPos, f32 feetY) {
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, xzPos, gx, gz)) return false;
    if (!LevelGridSystem::hasPlatform(g, gx, gz)) return false;
    return feetY >= LevelGridSystem::getPlatformTop(g, gx, gz) - PLATFORM_STEP_TOLERANCE;
}
// Nearest portal END on the SAME story as `from`, whose OTHER end is on the story of `toStoryUpper`.
// Returns the XZ goal to walk to (the ramp foot on my story), or `from` if none.
Vec3 nearestPortalGoal(const DungeonResult& d, Vec3 from, bool fromUpper, bool toUpper);
```
`nearestPortalGoal` (impl in `enemy_ai.cpp`): scan `d.portals[0..portalCount)`; the near end is `fromUpper ? highPos : lowPos`; pick the closest (XZ) whose crossing takes you toward `toUpper`; return that near-end XZ (keep the enemy's own Y).

- [ ] **Step 2:** Write a unit test (`tests/world/test_vertical_hall.cpp` or a new `test_story_nav.cpp`): build a `DungeonResult` with one portal (low @ (2,0,2), high @ (2,3,4)); assert `nearestPortalGoal(..., fromUpper=false, toUpper=true)` returns ≈ the low end, and the reverse returns the high end.

### Task 3.2: Wire cross-story routing into CHASE

**Files:** `src/game/enemy_ai_states.cpp` (CHASE ground branch ~L314-363), signature threading for `dungeon`.

- [ ] **Step 1:** In the CHASE ground branch, BEFORE computing `chaseGoal`, compare stories and redirect if they differ:
```cpp
bool myUpper     = onUpperStory(grid, e.position, e.position.y - e.halfExtents.y);
bool targetUpper = onUpperStory(grid, targetPlayer->position, targetPlayer->position.y); // player pos == feet
Vec3 chaseGoal;
if (myUpper != targetUpper && dungeon && dungeon->portalCount > 0) {
    // Different stories: head to the ramp foot on MY story; the story-snap carries me up/down it.
    // (Descending doesn't strictly need this — walking toward the target's XZ drops me off the edge —
    //  but routing to a ramp is cleaner and avoids suicidal ledge-drops onto hazards.)
    chaseGoal = nearestPortalGoal(*dungeon, e.position, myUpper, targetUpper);
} else {
    chaseGoal = encircleGoal(e, pool, squads, grid, targetPos, targetDist);
}
```
The existing `hasWidthLOS`-gated beeline/A* follow (`enemy_ai_states.cpp:326-361`) then paths to `chaseGoal` unchanged — once the enemy reaches the ramp foot and climbs (story-snap), `myUpper` flips and the next tick chases the target directly.

- [ ] **Step 2:** Thread `const DungeonResult* dungeon` from `EnemyAI::update` (already has `&m_level.dungeon`) through `updateHostileStates` into the CHASE code (add the param; `nullptr` on floors without portals ⇒ behaves exactly as today).

- [ ] **Step 3: Runtime check (SP)** — force a VERTICAL_HALL floor: stand on a balcony; a melee enemy on the ground routes to a ramp and climbs to you. Drop to the pit; it walks off the edge / down a ramp and follows. A sniper on a far nest holds and fires. No stuck-milling under the balcony.

> **Why not `(x,z,story)` A*:** the core `Pathfinder` node is 2D `(x,z)` with no story field, used by EVERY floor; adding a story dimension touches the closed/open dedup, neighbor gen, and the `Vec3` waypoint contract — high regression risk for all content. Portal-routing layers on top and is `nullptr`-inert off vertical-hall floors. If playtests show routing gaps (e.g. multi-ramp chambers), escalate to a hierarchical region graph — but only then.

---

# PHASE 4 — Nests, polish, docs

### Task 4.1: `spawnFloorNests` — snipers on the balconies

**Files:** `src/engine/engine_spawn.cpp` (model on `spawnBredEnemy` ~L1367 + `spawnFloorEnemies` ~L313), `src/engine/engine.h` (decl), `src/engine/engine_startgame.cpp` (call after `spawnFloorEnemies` ~L622)

- [ ] **Step 1:** `Engine::spawnFloorNests(const DungeonResult& dungeon)` — for each recorded balcony region (from `portals[].highPos` or a per-chamber balcony centre), pick a **ranged** tier def (`collectTierDefs` filtered to `attackRange > 5` — e.g. Sniper Imp / Bone Archer / Bone Mage) and spawn 1-2 at the balcony cell with an EXPLICIT balcony Y (`platHeight * 0.25 + def.halfExtents.y`, NOT the ground `getFloorHeight` snap). Reuse the `spawnBredEnemy` body but pass the balcony Y and skip the ground snap. Floor-scale HP/damage identically.
- [ ] **Step 2:** Call it in `startGame` right after `spawnFloorEnemies(dungeon, currentTier)` and BEFORE `SquadSystem::rebuild`, gated `if (layoutStyle == VERTICAL_HALL)`.
- [ ] **Step 3: Runtime check** — nests occupy the balconies, fire down at the player, and (Phase 3) can leave to chase if you climb to them.

### Task 4.2 (optional polish): graceful enemy gravity

*If instant story-snap-down reads as teleporty in playtests.* Add gravity to ground enemies in `entityMoveAndSlide` (`enemy_ai.cpp:196`): when a non-flying enemy's feet exceed its cell's `effectiveFloorHeight`, integrate `velocity.y -= GRAVITY*dt` and land at the effective floor (mirror the flyer branch). Keep the story-snap as the backstop. Test: an enemy stepped off a 3 m balcony falls over several ticks and lands at y≈halfExtents.y.

### Task 4.3: Docs

- [ ] Update `CLAUDE.md` (a `VERTICAL_HALL` note under level gen / the two-story PvE story), `engine-reference` (LayoutStyle list, `DungeonResult.portals`, story-snap change), `engine-how-to` (how to tune the hall + the "enemies now traverse stories on VERTICAL_HALL" gotcha).

---

## Verification

1. **Unit** — `test_platform.cpp` entity story-snap (climb/under/drop); `test_vertical_hall.cpp` (rooms≥5, balconies, portals, spawn≠exit across 64 seeds); `nearestPortalGoal`. `./build/tests/dungeon_tests`.
2. **Build** — Debug + Release clean, zero new warnings; full suite green.
3. **Runtime (SP)** — a floor-6+ VERTICAL_HALL: real two stories, ramps climbable, exit on the opposite story (both ascend and descend variants across seeds), nests hold + fire, a melee enemy climbs a ramp to chase you up and drops off to chase you down.
4. **Runtime (co-op)** — host + client on the same VERTICAL_HALL seed: the guest sees identical geometry (no wire change), enemy climbs/drops replicate with no rubber-band, nest fire lands.
5. **Regression** — play several non-VERTICAL_HALL floors (rooms/cavern/gauntlet/hub) + a boss floor: enemy movement, snapping, and pathing are byte-for-byte unchanged (story helpers are inert off platform cells / `nullptr` dungeon).
6. **Perf** — floor 40+ VERTICAL_HALL: draw calls within the 300-500 Switch budget; story helpers add only a per-CHASE-tick story compare + a ≤16-portal scan.

## Risks

- **Ramp geometry tuning** — steps must stay ≤ 0.25 m/cell or the story-snap tolerance won't carry a body up; the balcony ceiling must clear 3 m. Pinned by the gen test's presence checks, but *feel* needs a playtest.
- **Portal routing gaps** — with multiple ramps per chamber, `nearestPortalGoal` may pick a suboptimal ramp; acceptable for v1 (enemy still climbs), escalate to a region graph only if playtests demand.
- **Instant drop-down** looks teleporty until Task 4.2; ship Phase 1-3 first, add gravity if it reads badly.
- **Difficulty spike** — plunging sniper fire on exposed ramps can be brutal; the nest count/def mix is the tuning dial (Task 4.1), needs a playtest, not a green suite.
- **`spawnOnUpper` reachability** — a spawn-on-balcony start must have a ramp down within its chamber, or the player is stuck; the gen test should additionally assert the spawn room has a portal (add to Task 2.2).
