# Four-Story "Descent" Dungeon Floors — Design

**Status:** Design (approved in brainstorming; ready for implementation plan)
**Date:** 2026-07-20
**Scope:** A new procedural floor style — a plain dungeon floor that is *four walkable stories tall*,
traversed by a one-way, drop-only descent. Requires generalizing the engine's two-story walk-under slab
system to N stories (up to 4). Seed-built and server-authoritative, so **no save-format and no wire-protocol
change**.

---

## 1. Goal & experience

A dungeon floor stacked four stories deep on one footprint. You **spawn on the top level (L3)** and work
down to the **exit on the bottom level (L0)**. The **only** way between levels is **dropping** through holes
in the floor (or off an edge) — there are no ramps, no stairs, and no way back up. Loot and enemies are
spread **evenly** across all four levels, so each level is its own real fight and the depth is about
*vertical navigation*, not a risk/reward gradient. Diving past a fight is a genuine tactical **escape**
(you leave those enemies behind and land among fresh ones) and a **commitment** (you cannot return for loot
you skipped).

This is deliberately *not* a themed "vault" and *not* a single-room arena — it is an ordinary dungeon floor
that happens to be tall.

### Locked decisions (from the user)

1. **True 4-story stack** — four walkable floors dead-stacked at the same footprint (not a terraced pit).
   This requires the multi-slab engine foundation (Part A).
2. **One-way descent, drop-only** — spawn L3, exit L0; the only inter-level link is a drop-hole/edge; no
   ramps, no stairs, no ascending.
3. **Flat seeding** — loot and enemies even across all four levels.
4. **Plain dungeon floor** — no vault/jackpot framing.
5. **Parked for later:** the "Skylight Wells" (C2) concept is reserved for a future **PvP** map, not this
   PvE floor. (Blueprint saved in the brainstorm session.)

### Why a foundation change is required

Today the engine is strictly two-story: `GridCell` carries a **single** `platHeight`/`platMaterialId` gated
by the `CELL_PLATFORM` flag — a ground floor plus at most one walk-under slab. Four walkable surfaces on one
footprint (ground + three slabs) needs the slab system generalized from one slab per cell to up to three.

---

## 2. Architecture

Two parts, built **foundation-first** (Part A ships as a zero-new-gameplay, fully-tested engine
generalization before any 4-story map code exists):

- **Part A — Multi-story tile foundation.** Generalize `CELL_PLATFORM` to carry up to 3 slabs per cell. The
  whole change funnels through the single story selector `effectiveFloorHeight`, so once that returns
  "highest slab top within step tolerance at/below the feet, else the base floor," every downstream consumer
  (collision landing/snap, both `snapEntityToFloor` twins, the head-clamp, raycast, mesher) inherits 4-story
  footing and clean drop-through-a-hole with near-zero call-site churn.
- **Part B — The `FOUR_STORY` generator + wiring.** A new drop-only generator built on Part A, plus the
  spawn/exit/seeding wiring.

### Data-flow / determinism invariant

`GridCell` is `calloc`'d per floor and **never serialized** — verified: `sizeof(GridCell)` appears only in
`level_grid.cpp` (the calloc + a log line) and `tests/world/test_level_gen.cpp:89` (the determinism
`memcmp`); no reference in persistence or net code, and snapshots carry player/entity/projectile state,
never grid cells. The generator uses `GenRNG` + integer quarter-units only (no float/trig), so host and
client carve **byte-identical** grids. Therefore the whole feature replicates in co-op with **no
`PROTOCOL_VERSION` bump and no `SAVE_VERSION` bump.**

---

## 3. Part A — Multi-story tile foundation

### 3.1 `GridCell` (src/world/level_grid.h)

Replace the two scalar slab fields with a small fixed array, keeping the struct **all-`u8`** (no padding, so
`calloc`-zeroed grids stay byte-comparable for the determinism `memcmp`):

```cpp
static constexpr u8 MAX_PLATFORMS_PER_CELL = 3;   // up to 3 slabs → 4 walkable stories

struct GridCell {
    u8 flags;
    u8 floorHeight;
    u8 ceilingHeight;
    u8 wallMaterialId;
    u8 floorMaterialId;
    u8 ceilMaterialId;
    u8 platCount;                               // number of slabs, 0..MAX_PLATFORMS_PER_CELL
    u8 platHeight[MAX_PLATFORMS_PER_CELL];      // slab TOP surfaces, quarter-units, STRICTLY ASCENDING
    u8 platMaterialId[MAX_PLATFORMS_PER_CELL];
};
static_assert(sizeof(GridCell) == 13,
    "GridCell must stay 13 all-u8 bytes: calloc'd per floor, never serialized; a size change silently "
    "breaks the test_level_gen determinism memcmp and any future grid memcpy");
```

Grows 8 → 13 bytes; a single live grid is ~25 KB — trivial.

**Invariants (documented in the header):**
- **(a) Ascending:** `platHeight[0..platCount-1]` is strictly ascending by top height (raycast/mesher/
  head-clamp depend on it).
- **(b) Flag gate:** `CELL_PLATFORM` is set iff `platCount > 0` — the fast short-circuit for every slab
  branch.
- **(c) Canonical byte-form:** slots at index `>= platCount` **must be zero** — the authorers never leave
  stale bytes — so two logically-identical cells reached by different edit orders compare byte-equal and the
  determinism `memcmp` (and any future grid hash) stays valid.
- **(d) Spacing:** same-cell slab tops must differ by `> PLATFORM_STEP_TOLERANCE` (else a body resting on the
  lower slab teleports up onto the higher via `effectiveFloorHeight`). The 12/24/36 qu (3 m) spacing is
  safe; a debug-build assert in `addPlatform` enforces it.

The existing `static_assert(PLATFORM_STEP_TOLERANCE == STEP_UP_HEIGHT)` in `collision.cpp` is unaffected.

### 3.2 Authorers (src/world/level_grid.{h,cpp})

Three write paths — the migration hinges on using the right one:

- **`setPlatform(GridCell& c, u8 topQ, u8 mat)` — REPLACE-to-single.** Byte-identical to the old scalar
  `c.platHeight=q; c.platMaterialId=m` (last-write-wins). Sets `platCount=1`, fills slot 0, sets
  `CELL_PLATFORM`, zeroes slots [1..2]. **Every existing single-slab writer migrates to this** (see 3.7) so
  their double-write junctions collapse to exactly one slab and shipped geometry stays byte-identical.
- **`addPlatform(GridCell& c, u8 topQ, u8 mat)` — ACCUMULATE.** Sorted-insert keeping `platHeight[]`
  strictly ascending; de-dup (a slab already at `topQ` overwrites its material, no second entry); sets
  `CELL_PLATFORM`; bumps `platCount`; no-op at `MAX_PLATFORMS_PER_CELL`; never writes a slot `>= platCount`;
  debug-asserts invariant (d). **Reserved for the `FOUR_STORY` generator only.**
- **`removePlatform(GridCell& c, u8 topQ)` — hole puncher (build-time).** Finds the slab at `topQ`, shifts
  strictly-higher entries down one index (preserving ascending order), decrements `platCount`, zeroes the
  vacated top slot, clears `CELL_PLATFORM` only at count 0. No-op if none matches.

Query helpers:
- `hasPlatform(grid,x,z)` — **unchanged signature**; new impl `in-bounds && platCount>0 && !(flags&CELL_SOLID)`.
- `platformCount(grid,x,z)` — **new**; the loop bound for every multi-slab consumer.
- `getPlatformTop(grid,x,z)` — highest top (`platHeight[platCount-1]*0.25`), plus an **indexed overload**
  `getPlatformTop(grid,x,z,i)`.
- `getPlatformUnderside(grid,x,z,i)` — `max(platHeight[i]-PLATFORM_THICKNESS_Q, i>0 ? platHeight[i-1] :
  floorHeight) * 0.25` — clamped **down to the next-lower surface** (previous slab top, else the base
  floor), so a lower slab's thickness band can never poke into an upper slab's underside. Thin no-index
  wrapper → `i=0`.

### 3.3 The generalization — `effectiveFloorHeight` (src/world/level_grid.cpp)

**Unchanged signature.** New body:

```cpp
f32 best = getFloorHeight(x,z);
if (!hasPlatform(x,z)) return best;
for (u8 i = 0; i < platCount; i++) {
    f32 top = platHeight[i] * 0.25f;
    if (feetY >= top - PLATFORM_STEP_TOLERANCE && top > best) best = top;
}
return best;   // highest surface among {floor} ∪ {slab tops} at/below feet + tolerance
```

This one function drives collision landing/snap, both `snapEntityToFloor` twins, and the story selection —
so a body over a hole (that cell missing the relevant slab) naturally resolves to the next intact surface
below (a fall), and a body on a slab stays on it (step-up tolerance).

### 3.4 Collision (src/world/collision.cpp)

Both `moveAndSlide` overloads share identical Y logic (grid-only ~145-277 and entity-obstacle ~282-407) —
change both, or extract a shared static Y helper so they can't diverge.

- **`overlapsPlatformBand`** loops `i in [0,platformCount)` per overlapped cell; blocks the XZ step if **any**
  slab band is clipped (feet below `top[i]-STEP_UP_HEIGHT` **and** head above `underside[i]`).
- **Under-slab head clamp** (~212-234 grid, ~347-368 entity): loop all slabs the rising body overlaps; clamp
  `tryPos.y` to the **MINIMUM** qualifying `underside[i]-PLAYER_HEIGHT` (**running min, not last-wins**) among
  slabs the body started fully below, and set `velocity.y=0` once — so a body under L1 bonks the L1 underside
  and never pops onto L2.
- **Free-fall landing:** the floor-snap **UP** loop (keyed on the **pre-move `preFeetY`**) is the mechanism
  that lands a faller on a slab (slabs aren't `CELL_SOLID`, so a fall reaches the else-branch and only the
  snap catches it). It **must stay velocity-agnostic.** The player already free-falls under `GRAVITY=-40`;
  peak speed on a single 3 m dive is ~15 m/s ≈ 0.26 m/tick ≪ 3 m spacing, so no slab is tunneled.

### 3.5 Raycast (src/world/raycast.cpp)

`Raycast::cast` public signature unchanged. The single-slab tests become slab loops in **both** slab-testing
sites (the `tryFloorCeil` lambda ~53-108 **and** the second block ~177-196):
- **Descending (dir.y<0):** explicit **top-down** loop `i = platformCount-1 .. 0`; for each slab compute the
  plane crossing and, if in-cell and in-range, return it (`+Y` normal); otherwise **continue to the next-lower
  slab**; test the base floor only after all slabs miss. (A naive "test the highest, else fall to base floor"
  would thread a ray through a slab it should hit — pinned by test.)
- **Rising (dir.y>0):** **bottom-up** underside loop.
- **Rim:** per-slab side faces, keeping the band-subtraction so stacked/thin slabs don't emit phantom rims.

This is what lets a hole-sniper's ray thread a hole to the level below while a solid slab blocks a shot into
a floor.

### 3.6 Mesher (src/world/level_mesh.cpp)

Wrap the `CELL_PLATFORM` block (~350-426) in `for (u8 s=0; s<platformCount; ++s)`, emitting each slab's
top/underside/rim using the indexed getters and `cell.platMaterialId[s]` (line ~353). Keep the two-strip rim
band-subtraction, nested per-slab, subtracting a neighbour slab whose band overlaps ours (render-only; a bug
is a see-through edge, not a desync). Skip the `CELL_CEILING` quad for `FOUR_STORY` interior cells (occluded
by three slabs — drops a full overdraw layer). Replace the **silent** `pushQuad` scratch-overflow return
(~line 61) with a **one-shot `LOG_WARN` counter** — a dropped slab quad is an invisible floor the player
falls through. **Do not** bump `SCRATCH_VERTS/INDICES` (12288/16384): the worst same-material fully-slabbed
bucket needs only ~7168/10752; doubling would add ~7 MB transient during `buildAll` (Switch static-init
pressure).

### 3.7 Migration (mandatory, the highest risk)

Existing single-slab writers do scalar **overwrite** (last-write-wins) and **do** write different heights to
the same cell at junctions — verified in the arena: perimeter bands at qu12, then corner ramps at qu1..N
overwriting them; VERTICAL_HALL ramp tops abut balconies. Routing these through `addPlatform` (accumulate)
would fabricate a **phantom second slab** (extra collision band, extra rim, wrong `effectiveFloorHeight`),
silently altering shipped geometry — and the determinism `memcmp` would **not** catch it (still
deterministic).

**Fix:** route **every** existing single-slab writer through `setPlatform` (replace):
- `carveVerticalHall`'s `slab()` lambda (level_gen.cpp) — `LevelGridSystem::setPlatform(c, q, floorMat)`.
- `engine_arena.cpp`'s `plat()` lambda — replace `c.flags=CELL_FLOOR|CELL_PLATFORM; c.platHeight=topQ;
  c.platMaterialId=plank;` with `c.flags=CELL_FLOOR; LevelGridSystem::setPlatform(c, topQ, plank);` while
  **keeping** the `floorMaterialId=sand`/`wallMaterialId=stone` lines.
- The four test slab-writers: `test_platform.cpp` (`setPlat` + `plat` lambdas), `test_vertical_hall.cpp`,
  `test_story_nav.cpp:45`, `test_teleport_dest.cpp:140`. (A bare `c.platHeight=q` now leaves `platCount==0`,
  so `hasPlatform` returns false and the slab silently vanishes — every test writer must use the authorer.)
- **Regression test:** assert `platCount <= 1` on every cell of a generated `VERTICAL_HALL` grid **and** an
  arena grid, and re-run `test_vertical_hall` unchanged.

---

## 4. Part B — The `FOUR_STORY` "Descent" generator

### 4.1 New types (src/world/level_gen.h)

- `LayoutStyle::FOUR_STORY` appended before `COUNT`; `styleName` → `"descent"`.
- `DungeonResult` gains the drop-hole record:
  ```cpp
  struct DropHole { Vec3 pos; f32 surfaceY; };   // pos.xz = centre; surfaceY = the slab top it pierces
  static constexpr u32 MAX_DROP_HOLES = 32;
  DropHole dropHoles[MAX_DROP_HOLES] = {};
  u8 dropHoleCount = 0;
  ```

### 4.2 `carveFourStory(grid, rng, result, forcedSpawn, forcedExit)`

Deterministic (GenRNG + integer quarter-units only). Constants: `FS_L1_Q=12, FS_L2_Q=24, FS_L3_Q=36` (tops
3/6/9 m); `FS_CEIL_Q=48` (12 m — must clear L3 @ 9 m + a 1.8 m body; the existing `VH_CEIL=8` is too low).

- **Step 0 — guard:** `if (W<40 || D<40) return;` — leaves `roomCount==0` so `generate()`'s `<5`-room check
  re-carves BSP (the existing degenerate fallback). `startGame` forces `gridSize>=44` for the style, so this
  only fires on a mis-sized dev call.
- **Step 1 — L0:** `carveArea(grid, 1,1, W-2,D-2, 0, FS_CEIL_Q*0.25, wallMat, 1,2)` — the fully-connected
  ground story (exit reachability is trivial). Capture `floorMat`.
- **Step 2 — full slabs:** for every interior cell, `addPlatform(cell,12,..); addPlatform(cell,24,..);
  addPlatform(cell,36,..)` → every interior cell carries `platCount==3 {12,24,36}`.
- **Step 3 — offset drop-holes (the core):** partition the interior into 4 quadrants (NW/NE/SE/SW). Assign
  holes to **disjoint quadrant sets across adjacent levels** so a dive always lands on the next intact slab
  (never an express shaft): **L3 → {NW,SE}, L2 → {NE,SW}, L1 → {NW,SE}**. An L3 hole (NW/SE) sits over intact
  L2 (holes in NE/SW) → land on L2; an L2 hole over intact L1 → land on L1; an L1 hole → land on L0. L3 and
  L1 share {NW,SE} but are **not adjacent** (intact L2 between), so no column ever pierces two adjacent
  levels. For each level and each assigned quadrant: place the **first hole unconditionally** (guarantees
  `>=1` hole/level = reachability), then `rng.range(0,2)` extra. Each hole is a rect of GenRNG origin+size,
  `w,h in [2,3]` (**`>=2` wide** so a 0.6 m player AABB falls cleanly — a 1-cell hole lets the floor-snap
  re-grab the player onto a neighbour slab and it never falls), kept **strictly inside** its quadrant with a
  `>=1`-cell margin (prevents a hole spilling across a boundary and column-aligning with an adjacent-level
  hole), and clear of the spawn/exit pads. Punch = `removePlatform(cell, L)` per cell. Record each hole.
- **Step 4 — rooms (flat seeding):** tile each level into a small grid of `DungeonRoom`s (2×2 or 2×3 per
  level → 8-24 rooms ≤ `MAX_DUNGEON_ROOMS=32`) via `addRoom` with `room.floorHeight = level metres`
  (0/3/6/9). These drive per-level enemy/loot/decor/light spread.
- **Step 5 — endpoints:** pick an L3 cell in a quadrant with **no** L3 hole, **validate** it carries a real
  L3 slab (nudge to an adjacent slabbed cell if it landed on a hole), `spawnBalconyPos = centre @ y=9`; pick
  a far-side L0 cell, `exitBalconyPos = centre @ y=0`; `forcedSpawn` = the L3 room containing spawn;
  `forcedExit` = the L0 room; `spawnOnUpper=true`; `portalCount` stays 0. No pillars/cover in v1
  (full-footprint slabs leave no bare cell for the VH pillar pattern — deferred).

### 4.3 `finalizeDungeon` floor-aware adjacency (mandatory)

Today's bbox adjacency is `xOverlap && zOverlap` with **no floorHeight** — so the four stacked same-XZ rooms
all mark mutually adjacent, and `spawnFloorEnemies` (which skips spawn + adjacent + 2-hop rooms) then places
**zero enemies** on the entire floor. Fix: add `&& fabsf(ri.floorHeight - rj.floorHeight) < 1.5f` to the
adjacency test. **Verified a no-op for every existing style** (all raised floors are 0.5 m: BSP/HUB/gauntlet)
and it cleanly separates the 3 m stories. Regression-pinned (0 vs 0.5 → adjacent; 0 vs 3 → not).

### 4.4 `pickLayoutStyle` (src/world/level_gen.cpp)

Extend the per-tier weights (`kWeights[5][5] → [5][6]`) with a `FOUR_STORY` column, and add the **non-boss
remap** `if (style==FOUR_STORY && (floor<6 || floor%5==0)) style=BSP_ROOMS;` (mirrors VERTICAL_HALL) —
**mandatory**, because the boss-arena expansion rewrites `floorHeight` and rebuilds the mesh, which would
corrupt the stacked slabs.

---

## 5. Part C — Engine wiring & seeding

- **Spawn L3 / exit L0.** Add `static bool usesBalconyEndpoints(LayoutStyle s){ return s==VERTICAL_HALL ||
  s==FOUR_STORY; }` and use it at the three existing VERTICAL_HALL sites in `engine_startgame.cpp`
  (grid-force `>=44`; `spawnBalconyPos → spawnPos`; `exitBalconyPos → m_level.floorDoorPos`). The player
  free-falls L3→…→L0 via holes; `buildFlowField` runs from the L0 door.
- **Flat loot/enemy spread.** Because `carveFourStory` emits per-level rooms at `floorHeight` 0/3/6/9, the
  existing room-keyed seeders spread content across all four stories for free (an enemy at
  `room.floorHeight + halfExtents.y` lands on that level's slab and the story-snap holds it). **Guard:**
  `spawnFloorEnemies` rejects a candidate cell whose `effectiveFloorHeight(room.floorHeight)` isn't within
  tolerance of `room.floorHeight` (a hole cell), so nothing seeds onto a hole and instantly snaps down.
- **Snipers on slabs.** `spawnFloorNests` no-ops (`portalCount==0`). A new `spawnFloorHoleSnipers(dungeon,
  tier)` (called for `FOUR_STORY`) seats 1-2 ranged defs **at the hole edge** (adjacent slab cell, raised
  eye, no ground snap, facing the hole). Their multi-slab raycast LOS is blocked by intact slabs and threads
  the hole, so they plunge-fire at **angled** targets one level below. (A target straight under the hole edge
  is a brief self-blocked blind spot — you can't shoot through your own floor; the AI LOS gate simply
  withholds fire until a clear line exists. Normal upper-level room seeding also contributes ranged threat.)
- **Enemy footing/fall — no AI code change.** `snapEntityToFloor` inherits multi-slab footing via
  `effectiveFloorHeight`; an enemy whose centre crosses a hole teleport-snaps one story down (satisfying
  "some fall after the player; none climb up"). Active drop-hole-seek pursuit is deferred (§7).
- **Dev door `--fourstory`.** `m_forceFourStory` in `engine.h`; `fourStory` in `LaunchOptions` +
  parse in `launch_options.cpp` + apply in `engine_launch.cpp` (all mirror `--vhall`). Normal PvE floor, so
  no sentinel/arena wiring caveats apply.

---

## 6. Testing strategy

Test-first per phase; foundation goes green before any 4-story code exists.

**Authorers/getters:** `setPlatform` → `platCount==1` + zeroed trailing slots + replaces on second call;
`addPlatform` out-of-order tops stay ascending, de-dup overwrites material; `removePlatform(middle)` shifts
down + zeroes the vacated slot + clears the flag only at 0. **Regression:** generated VERTICAL_HALL + arena
grids have `platCount<=1` everywhere.

**`effectiveFloorHeight` over a 3-slab stack:** feet 9.0→9, 8.9→9 (step band), 8.5→6, 6.0→6, 5.5→3, 3.0→3,
0.0→0; a hole cell (missing 36) with feet 9.0 → 6. Regression: the 2-story balcony expectations
(3.0→3.0, 2.7→3.0, 2.5→0.0) stay byte-identical.

**Collision:** free-fall through a 2×2 L3 hole lands on L2 (6.0), `onGround`, never dips below 6; stacked
L3+L2 hole → L1; **sticky-lip** (walk into a 2×2 hole with horizontal velocity still completes the fall);
high-velocity single tick (`velocity.y=-30`) still lands on the top slab; head-clamp running-min under
stacked slabs in **both** overloads; `overlapsPlatformBand` blocks into any band, passes under/over.

**Raycast:** descend through an L3 hole over intact L2 → hits 6.0; through an all-holed column → L0; nearest
of {12,24} from y=8 → 6.0 (top-down order); highest-slab crossing exits cell → still hits the lower in-cell
slab; **sniper contract** (edge eye → offset lower-level target reaches the lower surface); rising from y=0.5
under {12,24} → 2.5; RIM band hits.

**Entity snap (both twins):** feet near 0/3/6/9 stand on that story; step off an L2 edge over an L1-highest
cell → 3.0; over a full hole → 0.0; never up; 2-story balcony-edge drop pinned.

**Generator (`tests/world/test_four_story.cpp`):** determinism (same seed → byte-identical grid + equal
room/spawn/exit/hole counts); every upper level has `>=1` hole; holes `>=2` wide + inside quadrant +
margin; **quadrant disjointness** (no adjacent-level hole cell overlaps column-wise → max one story/dive);
reachability BFS L3-spawn → L0/exit; spawn cell carries a real L3 slab; `portalCount==0`, `spawnOnUpper`;
`<40`-grid → BSP fallback. Add `FOUR_STORY` to `test_level_gen` `kAllStyles`; finalize adjacency regression
(0-vs-3 not adjacent, 0-vs-0.5 adjacent).

**Compile tripwires:** `static_assert(sizeof(GridCell)==13)` and the existing `PLATFORM_STEP_TOLERANCE ==
STEP_UP_HEIGHT`.

**Mesher headroom:** a worst-case fully-slabbed same-material section does not fire the `pushQuad` overflow
warn against the current 12288/16384 scratch.

---

## 7. Risks & open issues

**High:** the slab-writer migration (§3.7) — mitigated by `setPlatform` + the `platCount<=1` regression.
**Medium:** raycast descending loop control (explicit top-down continue); the floor-snap sticky-lip
(**reject** the tempting velocity-gated fix — it would break every landing; handled geometrically by
`>=2×2` holes + the 0.4 m `preFeetY` gate); canonical byte-form (zero trailing slots); the sniper self-block
(seat at the edge, accept the straight-down blind spot); the zero-enemy floor (the finalize fix).
**Determinism/perf:** integer/GenRNG only; slab loops are `<=3` over the few cells a ray/body touches; fill
is the real budget risk (up to ~8 stacked opaque layers looking down a hole) — mitigated by the
`CELL_CEILING` skip; a one-time GPU capture on a `--fourstory` floor is the remaining check.

**Deferred to v2 (open issues):**
- **Enemy drop-hole SEEK pursuit** — omitted by the agreed design ("some fall after you; none climb up").
  Full-footprint slabs can park an enemy on the intact slab above a player who dropped away. If wanted:
  a pure `StoryNav::nearestDropHole(holes,count,from,feetY)` + a CHASE else-branch (holes already recorded).
- **Smooth enemy fall** — descent is an instant teleport-snap (entity move has no gravity), a fast vertical
  lerp on remote clients (matches VERTICAL_HALL); a real velocity-based fall needs a snapshot field.
- **`story_nav.h onUpperStory`** reads the highest slab, so it classifies L1/L2 as "ground" — harmless in v1
  (`FOUR_STORY portalCount=0`); needs a `feetY`-resolving overload when down-pursuit lands.
- **Upper-level cover/pillars** — omitted (full slabs leave no bare cell); add floor-to-ceiling columns
  piercing all stories, off holes and pads, if LOS-breaking cover is wanted.
- **Cosmetic budgets** — point-light cap reached sooner (lower levels may be dim); walkable-surface guard
  applied to enemies only (decor floating over a hole is a minor float).
- **`pickLayoutStyle` weight-table growth** shifts existing (seed, floor) → non-boss layouts on this build
  (harmless; floors are seed-regenerated, not saved) — note in the changelog, exactly as VERTICAL_HALL was.

---

## 8. Build order (phased; each phase ends green)

0. **Foundation, no new behavior:** GridCell + `static_assert`; getters + `effectiveFloorHeight`; the three
   authorers; migrate all single-slab writers to `setPlatform`. Full suite green + the `platCount<=1`
   regression — proves zero regression before any 4-story code.
1. **Collision:** multi-slab band + running-min head-clamp (both overloads); leave the snap-UP loop
   velocity-agnostic. Collision tests green.
2. **Raycast:** top-down/bottom-up slab loops + rim (both slab-testing sites). Raycast tests green.
3. **Mesher:** per-slab quads + rim subtraction + the overflow warn + `CELL_CEILING` skip. Determinism
   `memcmp` stays green (render-only).
4. **Generator:** `dropHoles`; `LayoutStyle::FOUR_STORY`; `carveFourStory`; `generate()` dispatch;
   `pickLayoutStyle` weights + non-boss remap; the finalize adjacency fix; `test_four_story.cpp` + kAllStyles
   + adjacency regression. Green.
5. **Engine wiring:** `usesBalconyEndpoints`; `--fourstory`; the `spawnFloorEnemies` guard;
   `spawnFloorHoleSnipers`. Manual `--fourstory` playtest (spawn L3, dive each hole to land one level down,
   reach L0; snipers plunge-fire; enemies fall/never climb; host+client identical grids).
6. **Docs:** CLAUDE.md (a `FOUR_STORY` "Descent" paragraph beside VERTICAL_HALL; `MAX_PLATFORMS_PER_CELL`,
   setPlatform-vs-addPlatform, no save/wire change), `engine-reference` (13-byte GridCell + invariants, the
   `effectiveFloorHeight` rule, `dropHoles`, `--fourstory`), `engine-how-to` (adding a stacked-slab style +
   the holes-`>=2×2` / offset-per-level / non-boss / setPlatform pitfalls).
