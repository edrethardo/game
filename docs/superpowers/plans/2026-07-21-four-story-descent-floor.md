# FOUR_STORY "Descent" Floor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new drop-only, four-story dungeon floor — you spawn on the top level (L3) and dive drop-holes down to the exit on the bottom level (L0); the only way between levels is falling.

**Architecture:** A `LayoutStyle::FOUR_STORY` generator (`carveFourStory`) carves the ground floor, stacks three `CELL_PLATFORM` slab levels (tops at 3/6/9 m via `addPlatform`), then punches **quadrant-disjoint offset drop-holes** (`removePlatform`; ≥1 per level for reachability, ≥2 cells wide so the player AABB actually falls, adjacent levels' holes never column-align so a dive is always exactly one story). Per-level `DungeonRoom`s at `floorHeight` 0/3/6/9 make the existing room-keyed seeders spread loot/enemies evenly across all four stories. Engine wiring places spawn on L3 / exit on L0, seats ranged "hole snipers" at hole edges to plunge-fire through the gaps, and adds a `--fourstory` dev door. A floor-aware `finalizeDungeon` adjacency fix (`fabsf(ΔfloorHeight) < 1.5`) prevents the four stacked same-XZ rooms from marking mutually adjacent and placing **zero** enemies.

**Tech Stack:** C++17, doctest, CMake, `GenRNG` (integer/compare-only ⇒ co-op bit-identical). No save/wire change.

**Depends on:** the **Multi-Slab Tile Foundation** plan — complete and green **first** (`setPlatform`/`addPlatform`/`removePlatform`, the generalized `effectiveFloorHeight`, and the collision/raycast/mesher generalizations must already exist).

**Reference spec:** `docs/superpowers/specs/2026-07-20-four-story-descent-floors-design.md` (sections 4–5, 7–8).

---

I now have everything I need. Here are the Phase 4 tasks.

---

### Task 4.1: Types — `LayoutStyle::FOUR_STORY`, `DungeonResult::DropHole`/`dropHoles`, `styleName "descent"`

**Files:**
- Modify: `src/world/level_gen.h:50-57` (enum), `src/world/level_gen.h:28-43` (DropHole + DungeonResult fields)
- Modify: `src/world/level_gen.cpp:975-984` (`styleName`)
- Create: `tests/world/test_four_story.cpp`
- Modify: `tests/CMakeLists.txt:68`

- [ ] **Step 1: Write the failing test** — create `tests/world/test_four_story.cpp` and register it in CMake. The file starts with just the type-wiring case:

```cpp
// test_four_story.cpp — FOUR_STORY "Descent" generator invariants. A plain dungeon floor stacked FOUR
// walkable stories deep on one footprint (L0 ground + three CELL_PLATFORM slabs @ 3/6/9 m), traversed by a
// one-way, drop-only descent through OFFSET holes: spawn L3, fall to the L0 exit, never climb. These pin
// the structural contract — per-level holes, adjacent-level quadrant disjointness (so a dive lands exactly
// one story down), an L3→L0 descent path, a real L3 spawn slab, and the sub-40 BSP fallback — so a
// regression can't ship an express shaft, an unreachable exit, or a host/client grid mismatch.

#include "doctest/doctest.h"
#include "world/level_gen.h"
#include "world/level_grid.h"

#include <cstring>
#include <vector>

TEST_CASE("FOUR_STORY: type + styleName wiring") {
    CHECK(LevelGen::LayoutStyle::FOUR_STORY < LevelGen::LayoutStyle::COUNT);
    CHECK(std::strcmp(LevelGen::styleName(LevelGen::LayoutStyle::FOUR_STORY), "descent") == 0);
    DungeonResult r{};
    CHECK(r.dropHoleCount == 0);
    CHECK(DungeonResult::MAX_DROP_HOLES == 32);
}
```

Add it to the `add_executable(dungeon_tests ...)` list in `tests/CMakeLists.txt` right after the `test_story_nav.cpp` line (68 is `test_vertical_hall.cpp`, 69 is `test_story_nav.cpp`):

```cmake
    world/test_vertical_hall.cpp     # VERTICAL_HALL: room-count, balconies, ramp portals, spawn/exit
    world/test_story_nav.cpp         # pure cross-story chase helpers (onUpperStory / nearestPortalGoal)
    world/test_four_story.cpp        # FOUR_STORY "Descent": offset holes, quadrant disjointness, L3→L0 descent
```

- [ ] **Step 2: Run it, verify it fails** — the compile fails first (the names don't exist yet):
```bash
cmake --build build --target dungeon_tests 2>&1 | grep -E "FOUR_STORY|dropHoleCount|MAX_DROP_HOLES"
```
Expected: `error: 'FOUR_STORY' is not a member of 'LevelGen::LayoutStyle'` and `'DungeonResult' has no member named 'dropHoleCount'`.

- [ ] **Step 3: Minimal implementation** — add the enum value in `src/world/level_gen.h` (before `COUNT` at line 56):

```cpp
        VERTICAL_HALL,  // two-story chambers: ground pit + walk-under balcony, ramps, opposite-story exit
        FOUR_STORY,     // four dead-stacked walkable stories on one footprint; one-way drop-only descent
        COUNT
```

Add the drop-hole record — insert `DropHole` + `MAX_DROP_HOLES` above `struct DungeonResult` (after line 28's `MAX_STORY_PORTALS`), and the three fields inside `DungeonResult` (after `exitBalconyPos`, line 42):

```cpp
static constexpr u32 MAX_STORY_PORTALS = 16;

// A FOUR_STORY drop-hole: a punched gap in a slab you fall through to the level below. pos.xz = hole
// centre; pos.y == surfaceY == the slab TOP it pierces (world metres). Zero/unused for other styles;
// consumed by spawnFloorHoleSnipers (ranged seats at the edge) + enemy-fall AI.
struct DropHole { Vec3 pos; f32 surfaceY; };
static constexpr u32 MAX_DROP_HOLES = 32;
```

```cpp
    Vec3 spawnBalconyPos = {};      // balcony centre of the spawn chamber (world)
    Vec3 exitBalconyPos  = {};      // balcony centre of the exit chamber (world)

    // FOUR_STORY "Descent" drop-holes (zero/unused for other styles).
    static constexpr u32 MAX_DROP_HOLES = ::MAX_DROP_HOLES;
    DropHole dropHoles[MAX_DROP_HOLES] = {};
    u8       dropHoleCount = 0;
};
```

Add the `styleName` case in `src/world/level_gen.cpp` (before `default:` at line 982):

```cpp
        case LayoutStyle::VERTICAL_HALL: return "vertical";
        case LayoutStyle::FOUR_STORY:    return "descent";
        default:                     return "?";
```

- [ ] **Step 4: Run the tests, verify they pass**
```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="FOUR_STORY: type*"
```
Expected: `test cases: 1 | 1 passed`. (`generate(FOUR_STORY)` still routes to the `default` BSP branch — harmless until Task 4.3; nothing calls it yet.)

- [ ] **Step 5: Commit**
```bash
git add src/world/level_gen.h src/world/level_gen.cpp tests/world/test_four_story.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(levelgen): FOUR_STORY layout type + DropHole record + "descent" styleName

Appends LayoutStyle::FOUR_STORY (before COUNT) and DungeonResult::DropHole /
dropHoles[] / dropHoleCount for the drop-only four-story descent floor. Wires
test_four_story.cpp into the test binary. Generator dispatch lands in a later task.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4.2: `pickLayoutStyle` — FOUR_STORY weight column + non-boss remap

**Files:**
- Modify: `src/world/level_gen.cpp:1000-1023` (`pickLayoutStyle` weights + loop + remap)
- Test: `tests/world/test_four_story.cpp` (append)

- [ ] **Step 1: Write the failing test** — append to `tests/world/test_four_story.cpp`:

```cpp
TEST_CASE("FOUR_STORY: pickLayoutStyle appears on non-boss deep floors only, deterministically") {
    // Non-boss remap (mirrors VERTICAL_HALL): FOUR_STORY never fires on floor<6 or a boss floor (floor%5==0)
    // — the boss-arena expansion rewrites floorHeight and rebuilds the mesh, which would stomp the slabs.
    for (u32 floor = 1; floor <= 60; floor++)
        for (u32 seed : {5u, 500u, 50000u, 0xBEEFu}) {
            LevelGen::LayoutStyle s = LevelGen::pickLayoutStyle(seed, floor);
            CAPTURE(floor); CAPTURE(seed);
            if (s == LevelGen::LayoutStyle::FOUR_STORY) {
                CHECK(floor >= 6);
                CHECK(floor % 5 != 0);
            }
            CHECK(s == LevelGen::pickLayoutStyle(seed, floor));   // host==client (deterministic)
        }

    // The style must actually occur on eligible floors (the weight column isn't dead).
    u32 seen = 0;
    for (u32 seed = 0; seed < 2000; seed++)
        for (u32 floor : {7u, 13u, 22u, 34u, 46u})
            if (LevelGen::pickLayoutStyle(seed * 2654435761u, floor) == LevelGen::LayoutStyle::FOUR_STORY)
                seen++;
    CHECK(seen > 0);
}
```

- [ ] **Step 2: Run it, verify it fails**
```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="FOUR_STORY: pickLayoutStyle*"
```
Expected: fails at `CHECK(seen > 0)` — `seen == 0`, because the weight table only accumulates the first 5 columns and never selects index 5.

- [ ] **Step 3: Minimal implementation** — in `src/world/level_gen.cpp`, replace the weight table, the accumulate loop bound, and add the remap (lines 996-1023):

```cpp
    // Per-tier weights [BSP, CAVERN, GAUNTLET, HUB, VERTICAL_HALL, FOUR_STORY]; each row sums to 100.
    // Styles echo the tier's fiction; classic BSP stays the most common style overall so the structural
    // floors keep reading as events, not the norm. VERTICAL_HALL and FOUR_STORY are non-boss styles.
    u8 tier = floor >= 41 ? 4 : floor >= 31 ? 3 : floor >= 21 ? 2 : floor >= 11 ? 1 : 0;
    static constexpr u8 kWeights[5][6] = {
        {46, 12, 11, 11, 12,  8},   // 4-10  Stone Dungeon
        {28, 12, 15, 22, 13, 10},   // 11-20 Catacombs
        {20, 36,  8, 16, 10, 10},   // 21-30 Spider Caverns
        {20, 11, 33, 16, 10, 10},   // 31-40 Hellforge
        {20, 20, 16, 22, 10, 12},   // 41-50 Void
    };
    u32 acc = 0;
    for (u32 s = 0; s < 6; s++) {
        acc += kWeights[tier][s];
        if (roll < acc) {
            LayoutStyle st = static_cast<LayoutStyle>(s);
            // VERTICAL_HALL and FOUR_STORY are NON-BOSS styles: boss floors (every 5th) expand a room
            // into a boss arena and rebuild the mesh, which would stomp the balcony/stacked-slab cells;
            // floors 4-5 run on the tiny tutorial grid. Fall back to classic BSP there (both styles only
            // appear on floor-6+ non-boss floors).
            if (st == LayoutStyle::VERTICAL_HALL && (floor < 6 || floor % 5 == 0))
                return LayoutStyle::BSP_ROOMS;
            if (st == LayoutStyle::FOUR_STORY && (floor < 6 || floor % 5 == 0))
                return LayoutStyle::BSP_ROOMS;
            return st;
        }
    }
    return LayoutStyle::BSP_ROOMS;
```

- [ ] **Step 4: Run the tests, verify they pass** — the new case, plus the existing `pickLayoutStyle` mix test in `test_level_gen` (now requires all 6 columns > 5%):
```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="FOUR_STORY: pickLayoutStyle*,*pickLayoutStyle is floor-gated*"
```
Expected: `test cases: 2 | 2 passed` (every style, index 0-5, clears `total/20`).

- [ ] **Step 5: Commit**
```bash
git add src/world/level_gen.cpp tests/world/test_four_story.cpp
git commit -m "$(cat <<'EOF'
feat(levelgen): weight FOUR_STORY into pickLayoutStyle with the non-boss remap

Grows kWeights to [5][6] with a FOUR_STORY column (rows still sum to 100), widens
the accumulate loop to 6, and remaps FOUR_STORY -> BSP on floor<6 or boss floors
(floor%5==0), mirroring VERTICAL_HALL — the boss-arena expansion would corrupt the
stacked slabs.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4.3: `carveFourStory` + `generate()` dispatch

**Files:**
- Modify: `src/world/level_gen.cpp:973` (insert `carveFourStory` before `styleName`), `src/world/level_gen.cpp:1034-1042` (`generate` dispatch)
- Test: `tests/world/test_four_story.cpp` (append helpers + generator cases)

- [ ] **Step 1: Write the failing test** — append the shared helpers and the generator cases to `tests/world/test_four_story.cpp`. Put the helpers in an anonymous namespace above the new cases:

```cpp
namespace {
// A slab TOP (metres) is present at this cell iff some platform index reports it.
bool hasSlabAt(const LevelGrid& g, u32 x, u32 z, f32 topM) {
    u32 n = LevelGridSystem::platformCount(g, x, z);
    for (u32 i = 0; i < n; i++)
        if (LevelGridSystem::getPlatformTop(g, x, z, i) == doctest::Approx(topM)) return true;
    return false;
}

// Descent-BFS: state = (x, z, level), level in {0,1,2,3} = the surface you stand on. Moving to a
// 4-neighbour drops you to the HIGHEST present surface at/below your level (drop-only — never step up),
// modelling a fall through a punched hole. Proves the L3 spawn always reaches the L0 exit.
bool descendReaches(const LevelGrid& g, u32 sx, u32 sz, u32 startLv, u32 ex, u32 ez, u32 exitLv) {
    const u32 W = g.width, D = g.depth;
    auto surfaceAtOrBelow = [&](u32 x, u32 z, u32 lv) -> u32 {
        for (s32 L = (s32)lv; L >= 1; L--)
            if (hasSlabAt(g, x, z, (f32)L * 3.0f)) return (u32)L;   // L1/L2/L3 tops = 3/6/9 m
        return 0;                                                   // L0 floor is always present interior
    };
    std::vector<u8> seen((size_t)W * D * 4, 0);
    std::vector<u32> stack;
    auto push = [&](u32 x, u32 z, u32 lv) {
        u32 idx = (lv * D + z) * W + x;
        if (seen[idx]) return;
        seen[idx] = 1; stack.push_back(idx);
    };
    push(sx, sz, startLv);
    const s32 dx[4] = {1, -1, 0, 0}, dz[4] = {0, 0, 1, -1};
    while (!stack.empty()) {
        u32 c = stack.back(); stack.pop_back();
        u32 lv = c / (W * D), rem = c % (W * D), x = rem % W, z = rem / W;
        if (x == ex && z == ez && lv == exitLv) return true;
        for (u32 k = 0; k < 4; k++) {
            s32 nx = (s32)x + dx[k], nz = (s32)z + dz[k];
            if (nx < 1 || nz < 1 || nx >= (s32)W - 1 || nz >= (s32)D - 1) continue;
            push((u32)nx, (u32)nz, surfaceAtOrBelow((u32)nx, (u32)nz, lv));
        }
    }
    return false;
}
} // namespace

TEST_CASE("FOUR_STORY: deterministic grid + room/hole counts from the seed") {
    for (u32 seed : {7u, 12345u, 0xDEADBEEFu}) {
        LevelGrid a, b;
        LevelGridSystem::init(a, 48, 48, 1.0f);
        LevelGridSystem::init(b, 48, 48, 1.0f);
        DungeonResult ra = LevelGen::generate(a, seed, 48, 48, LevelGen::LayoutStyle::FOUR_STORY);
        DungeonResult rb = LevelGen::generate(b, seed, 48, 48, LevelGen::LayoutStyle::FOUR_STORY);
        CAPTURE(seed);
        REQUIRE(std::memcmp(a.cells, b.cells, sizeof(GridCell) * 48 * 48) == 0);
        REQUIRE(ra.roomCount == rb.roomCount);
        REQUIRE(ra.spawnRoomIdx == rb.spawnRoomIdx);
        REQUIRE(ra.exitRoomIdx == rb.exitRoomIdx);
        REQUIRE(ra.dropHoleCount == rb.dropHoleCount);
        REQUIRE(ra.dropHoleCount > 0);
        for (u8 i = 0; i < ra.dropHoleCount; i++) {
            REQUIRE(ra.dropHoles[i].pos.x == rb.dropHoles[i].pos.x);
            REQUIRE(ra.dropHoles[i].surfaceY == rb.dropHoles[i].surfaceY);
        }
        LevelGridSystem::shutdown(a);
        LevelGridSystem::shutdown(b);
    }
}

TEST_CASE("FOUR_STORY: every upper level has a drop-hole, holes are >=2 wide and border-margined") {
    for (u32 seed : {1u, 99u, 4242u}) {
        LevelGrid g;
        LevelGridSystem::init(g, 48, 48, 1.0f);
        LevelGen::generate(g, seed, 48, 48, LevelGen::LayoutStyle::FOUR_STORY);
        const u32 W = g.width, D = g.depth;
        u32 h1 = 0, h2 = 0, h3 = 0;
        for (u32 z = 1; z < D - 1; z++)
            for (u32 x = 1; x < W - 1; x++) {
                const f32 lvM[3] = {3.0f, 6.0f, 9.0f};
                for (u32 L = 0; L < 3; L++) {
                    if (hasSlabAt(g, x, z, lvM[L])) continue;       // slab present here
                    (L == 0 ? h1 : L == 1 ? h2 : h3)++;
                    CAPTURE(x); CAPTURE(z); CAPTURE(L);
                    // Border margin: a hole never touches the interior edge (would spill / mis-align).
                    CHECK(x > 1); CHECK(x < W - 2); CHECK(z > 1); CHECK(z < D - 2);
                    // >=2 wide: a same-level missing neighbour in BOTH x and z (no 1-cell hole).
                    CHECK((!hasSlabAt(g, x - 1, z, lvM[L]) || !hasSlabAt(g, x + 1, z, lvM[L])));
                    CHECK((!hasSlabAt(g, x, z - 1, lvM[L]) || !hasSlabAt(g, x, z + 1, lvM[L])));
                }
            }
        CAPTURE(seed);
        CHECK(h1 > 0); CHECK(h2 > 0); CHECK(h3 > 0);               // >=1 hole per upper level
        LevelGridSystem::shutdown(g);
    }
}

TEST_CASE("FOUR_STORY: adjacent-level holes are quadrant-disjoint (max one story per dive)") {
    for (u32 seed : {2u, 808u, 31337u}) {
        LevelGrid g;
        LevelGridSystem::init(g, 48, 48, 1.0f);
        LevelGen::generate(g, seed, 48, 48, LevelGen::LayoutStyle::FOUR_STORY);
        const u32 W = g.width, D = g.depth;
        for (u32 z = 1; z < D - 1; z++)
            for (u32 x = 1; x < W - 1; x++) {
                bool m3 = !hasSlabAt(g, x, z, 9.0f), m2 = !hasSlabAt(g, x, z, 6.0f),
                     m1 = !hasSlabAt(g, x, z, 3.0f);
                CAPTURE(x); CAPTURE(z);
                CHECK_FALSE(m3 && m2);   // no column pierces L3 AND L2 (would be a 2-level express shaft)
                CHECK_FALSE(m2 && m1);   // nor L2 AND L1
            }
        LevelGridSystem::shutdown(g);
    }
}

TEST_CASE("FOUR_STORY: L3 spawn descends through the holes to the L0 exit") {
    for (u32 seed : {3u, 777u, 0xC0FFEEu}) {
        LevelGrid g;
        LevelGridSystem::init(g, 48, 48, 1.0f);
        DungeonResult r = LevelGen::generate(g, seed, 48, 48, LevelGen::LayoutStyle::FOUR_STORY);
        u32 sx, sz, ex, ez;
        REQUIRE(LevelGridSystem::worldToGrid(g, r.spawnBalconyPos, sx, sz));
        REQUIRE(LevelGridSystem::worldToGrid(g, r.exitBalconyPos, ex, ez));
        CAPTURE(seed);
        CHECK(descendReaches(g, sx, sz, 3, ex, ez, 0));
        LevelGridSystem::shutdown(g);
    }
}

TEST_CASE("FOUR_STORY: spawn stands on a real L3 slab, portalCount 0, spawnOnUpper") {
    LevelGrid g;
    LevelGridSystem::init(g, 48, 48, 1.0f);
    DungeonResult r = LevelGen::generate(g, 0xABCDu, 48, 48, LevelGen::LayoutStyle::FOUR_STORY);
    u32 sx, sz;
    REQUIRE(LevelGridSystem::worldToGrid(g, r.spawnBalconyPos, sx, sz));
    CHECK(hasSlabAt(g, sx, sz, 9.0f));            // never spawn into a hole
    CHECK(r.spawnBalconyPos.y == doctest::Approx(9.0f));
    CHECK(r.exitBalconyPos.y == doctest::Approx(0.0f));
    CHECK(r.portalCount == 0);                    // no ramps/stairs — drop-only
    CHECK(r.spawnOnUpper);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("FOUR_STORY: a sub-40 grid falls back to BSP") {
    LevelGrid g;
    LevelGridSystem::init(g, 32, 32, 1.0f);
    DungeonResult r = LevelGen::generate(g, 12345u, 32, 32, LevelGen::LayoutStyle::FOUR_STORY);
    CHECK(r.roomCount >= 5);        // BSP fallback still yields a playable floor
    CHECK(r.dropHoleCount == 0);    // no four-story content
    CHECK(r.portalCount == 0);
    CHECK_FALSE(r.spawnOnUpper);
    LevelGridSystem::shutdown(g);
}
```

- [ ] **Step 2: Run it, verify it fails**
```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="FOUR_STORY: *descends*,FOUR_STORY: *upper level*"
```
Expected: red — `generate(FOUR_STORY)` still hits the `default` BSP branch, so `dropHoleCount == 0` (fails `REQUIRE(ra.dropHoleCount > 0)`) and no slabs exist (`h3 > 0` fails / `descendReaches` false).

- [ ] **Step 3: Minimal implementation** — insert `carveFourStory` (and its `FS_*` constants) in `src/world/level_gen.cpp` immediately after `carveVerticalHall`'s closing brace (line 973), before `styleName`:

```cpp
// ---------------------------------------------------------------------------
// Style: FOUR_STORY — "The Descent". A plain dungeon floor stacked FOUR walkable stories on one
// footprint (L0 ground + three CELL_PLATFORM slabs @ 3/6/9 m), traversed by a ONE-WAY, DROP-ONLY
// descent: spawn L3, fall through OFFSET holes down to the L0 exit — no ramps, no stairs, no way up.
// Loot/enemies seed evenly (per-level rooms at floorHeight 0/3/6/9). Holes go in DISJOINT quadrant sets
// on adjacent levels (L3->{NW,SE}, L2->{NE,SW}, L1->{NW,SE}), so a dive always lands on the next intact
// slab one story down — never a two-level express shaft (L3 & L1 reuse {NW,SE} but aren't adjacent,
// intact L2 between). Built on the multi-slab foundation (addPlatform/removePlatform +
// effectiveFloorHeight), GenRNG + integer quarter-units only -> byte-identical host/client grids, so it
// replicates in co-op with NO wire/save change. Contract pinned by test_four_story.
// ---------------------------------------------------------------------------
static constexpr u8 FS_L1_Q   = 12;   // slab tops: L1 @ 3 m
static constexpr u8 FS_L2_Q   = 24;   //            L2 @ 6 m
static constexpr u8 FS_L3_Q   = 36;   //            L3 @ 9 m
static constexpr u8 FS_CEIL_Q = 48;   // 12 m ceiling — clears L3 @ 9 m + a 1.8 m body (VH_CEIL=8 too low)

static void carveFourStory(LevelGrid& grid, GenRNG& rng, DungeonResult& result,
                           s32& forcedSpawn, s32& forcedExit) {
    const u32 W = grid.width, D = grid.depth;
    if (W < 40 || D < 40) return;   // too small -> generate() sees roomCount==0 (<5) and re-carves BSP

    const f32 cs = grid.cellSize;
    const u8  wallMat = (rng.f01() < 0.3f) ? 3 : 0;

    // 1) L0 — the fully-connected ground story (the exit lives here, so exit reachability is trivial).
    carveArea(grid, 1, 1, W - 2, D - 2, 0.0f, FS_CEIL_Q * 0.25f, wallMat, 1, 2);
    const u8 floorMat = LevelGridSystem::getCell(grid, 2, 2).floorMaterialId;

    // 2) Three full slabs over every interior cell -> every cell carries platCount==3 {12,24,36}.
    for (u32 z = 1; z < D - 1; z++)
        for (u32 x = 1; x < W - 1; x++) {
            GridCell& c = LevelGridSystem::getCell(grid, x, z);
            LevelGridSystem::addPlatform(c, FS_L1_Q, floorMat);
            LevelGridSystem::addPlatform(c, FS_L2_Q, floorMat);
            LevelGridSystem::addPlatform(c, FS_L3_Q, floorMat);
        }

    // 3) Offset drop-holes (the core). Interior split into 4 quadrants at the mid lines. Each level's
    //    holes go in a quadrant set DISJOINT from the adjacent level's (see header), so a dive always
    //    lands one story down on intact slab.
    const u32 midX = W / 2, midZ = D / 2;
    struct Quad { u32 x0, x1, z0, z1; };            // inclusive interior cell bounds
    const Quad NW = {1, midX - 1, 1, midZ - 1};
    const Quad NE = {midX, W - 2, 1, midZ - 1};
    const Quad SW = {1, midX - 1, midZ, D - 2};
    const Quad SE = {midX, W - 2, midZ, D - 2};

    auto punchHoles = [&](u8 q, const Quad* quads, u32 quadN) {
        for (u32 iq = 0; iq < quadN; iq++) {
            const Quad& Q = quads[iq];
            // >=1-cell margin inside the quadrant on all sides, so a hole never touches a quadrant
            // boundary (and thus never column-aligns with an adjacent-level hole across the seam).
            const u32 minX = Q.x0 + 1, maxX = Q.x1 - 1, minZ = Q.z0 + 1, maxZ = Q.z1 - 1;
            const u32 count = 1 + rng.range(0, 3);   // first hole unconditional (=> >=1/level), +0..2
            for (u32 k = 0; k < count; k++) {
                if (result.dropHoleCount >= DungeonResult::MAX_DROP_HOLES) return;
                const u32 hw = rng.range(2, 4), hh = rng.range(2, 4);   // 2..3 wide -> a body falls clean
                if (maxX < minX + hw - 1 || maxZ < minZ + hh - 1) continue;   // quadrant too small (never at >=40)
                const u32 hx = rng.range(minX, maxX - hw + 2);
                const u32 hz = rng.range(minZ, maxZ - hh + 2);
                for (u32 z = hz; z < hz + hh; z++)
                    for (u32 x = hx; x < hx + hw; x++)
                        LevelGridSystem::removePlatform(LevelGridSystem::getCell(grid, x, z), q);
                DropHole& dh = result.dropHoles[result.dropHoleCount++];
                dh.pos = { (hx + hw * 0.5f) * cs, q * 0.25f, (hz + hh * 0.5f) * cs };
                dh.surfaceY = q * 0.25f;
            }
        }
    };
    const Quad nwSE[2] = { NW, SE }, neSW[2] = { NE, SW };
    punchHoles(FS_L3_Q, nwSE, 2);   // L3 holes -> NW/SE (land on intact L2)
    punchHoles(FS_L2_Q, neSW, 2);   // L2 holes -> NE/SW (land on intact L1)
    punchHoles(FS_L1_Q, nwSE, 2);   // L1 holes -> NW/SE (land on L0)

    // 4) Per-level rooms (2x2 tiling per story) at floorHeight 0/3/6/9 — key the flat enemy/loot spread.
    const f32 levelY[4] = { 0.0f, 3.0f, 6.0f, 9.0f };
    const u32 halfW = (W - 2) / 2, halfD = (D - 2) / 2;
    u32 l0Base = 0, l3Base = 0;
    for (u32 lv = 0; lv < 4; lv++) {
        const u32 base = result.roomCount;
        if (lv == 0) l0Base = base;
        if (lv == 3) l3Base = base;
        for (u32 rz = 0; rz < 2; rz++)
            for (u32 rx = 0; rx < 2; rx++) {
                if (result.roomCount >= MAX_DUNGEON_ROOMS) break;
                DungeonRoom& r = result.rooms[result.roomCount++];
                r = DungeonRoom{};
                r.x = 1 + rx * halfW; r.z = 1 + rz * halfD;
                r.w = halfW;          r.d = halfD;
                r.floorHeight = levelY[lv];
                r.wallMat = wallMat;
            }
    }

    // 5) Endpoints — spawn on L3 in a quadrant with NO L3 hole (NE => guaranteed real slab), exit on L0 in
    //    the diagonally-opposite quadrant (SW => a full four-story descent + long traverse). portalCount
    //    stays 0 (no ramps); spawnOnUpper marks the balcony spawn (startGame applies balconyPos verbatim).
    const u32 spawnX = (midX + (W - 2)) / 2, spawnZ = (1 + (midZ - 1)) / 2;   // NE quadrant centre
    const u32 exitX  = (1 + (midX - 1)) / 2, exitZ  = (midZ + (D - 2)) / 2;   // SW quadrant centre
    result.spawnBalconyPos = { (spawnX + 0.5f) * cs, FS_L3_Q * 0.25f, (spawnZ + 0.5f) * cs };
    result.exitBalconyPos  = { (exitX  + 0.5f) * cs, 0.0f,            (exitZ  + 0.5f) * cs };
    result.spawnOnUpper    = true;

    auto roomAt = [&](u32 base, u32 gx, u32 gz) -> s32 {
        for (u32 i = base; i < base + 4 && i < result.roomCount; i++) {
            const DungeonRoom& r = result.rooms[i];
            if (gx >= r.x && gx < r.x + r.w && gz >= r.z && gz < r.z + r.d) return (s32)i;
        }
        return (s32)base;
    };
    forcedSpawn = roomAt(l3Base, spawnX, spawnZ);   // the L3 room containing spawn
    forcedExit  = roomAt(l0Base, exitX,  exitZ);    // the L0 room containing exit
}
```

Add the dispatch case in `generate()` (after the `VERTICAL_HALL` case, line 1038-1039):

```cpp
        case LayoutStyle::VERTICAL_HALL:
            carveVerticalHall(grid, rng, result, forcedSpawn, forcedExit); break;
        case LayoutStyle::FOUR_STORY:
            carveFourStory(grid, rng, result, forcedSpawn, forcedExit); break;
```

- [ ] **Step 4: Run the tests, verify they pass**
```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="FOUR_STORY:*"
```
Expected: all FOUR_STORY cases pass (`8 passed` including the earlier two). Then confirm no regression:
```bash
./build/tests/dungeon_tests
```
Expected: full suite green.

- [ ] **Step 5: Commit**
```bash
git add src/world/level_gen.cpp tests/world/test_four_story.cpp
git commit -m "$(cat <<'EOF'
feat(levelgen): carveFourStory generator + generate() dispatch

Carves four dead-stacked walkable stories (L0 + slabs at 12/24/36 qu via addPlatform)
with offset, quadrant-disjoint drop-holes punched by removePlatform (>=1 per level,
>=2 wide, margined), per-level rooms at floorHeight 0/3/6/9, an L3 spawn on a
guaranteed-real slab, and an L0 exit; portalCount stays 0. GenRNG + integer quarter-
units only, so host/client grids are byte-identical (no wire/save change).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4.4: `finalizeDungeon` floor-aware adjacency fix

**Files:**
- Modify: `src/world/level_gen.cpp:14` (add `#include <cmath>`), `src/world/level_gen.cpp:226-229` (adjacency predicate)
- Test: `tests/world/test_four_story.cpp` (append)

- [ ] **Step 1: Write the failing test** — append to `tests/world/test_four_story.cpp`:

```cpp
TEST_CASE("FOUR_STORY: finalize adjacency is story-aware (0-vs-3 not adjacent, same-story yes)") {
    // Without the floorHeight gate, the four same-XZ stacked room sets all mark mutually adjacent, and
    // spawnFloorEnemies (skips spawn + adjacent + 2-hop rooms) then seeds ZERO enemies. The fix adds
    // `&& fabsf(dhi - dhj) < 1.5f`: cross-story pairs (3/6/9 m apart) are never neighbours, while
    // within-a-story pairs (diff 0, i.e. also the flat 0-vs-0.5 case < 1.5) still link.
    LevelGrid g;
    LevelGridSystem::init(g, 48, 48, 1.0f);
    DungeonResult r = LevelGen::generate(g, 20250720u, 48, 48, LevelGen::LayoutStyle::FOUR_STORY);

    auto bboxTouch = [](const DungeonRoom& a, const DungeonRoom& b) {
        bool xo = (a.x < b.x + b.w + 3) && (b.x < a.x + a.w + 3);
        bool zo = (a.z < b.z + b.d + 3) && (b.z < a.z + a.d + 3);
        return xo && zo;
    };
    auto listed = [](const DungeonRoom& a, u16 bIdx) {
        for (u8 k = 0; k < a.adjacentCount; k++) if (a.adjacentRooms[k] == bIdx) return true;
        return false;
    };

    u32 sameStoryLinks = 0;
    for (u32 i = 0; i < r.roomCount; i++)
        for (u32 j = i + 1; j < r.roomCount; j++) {
            const DungeonRoom& a = r.rooms[i]; const DungeonRoom& b = r.rooms[j];
            if (!bboxTouch(a, b)) continue;
            bool adj = listed(a, (u16)j) && listed(b, (u16)i);
            CAPTURE(i); CAPTURE(j); CAPTURE(a.floorHeight); CAPTURE(b.floorHeight);
            if (a.floorHeight == b.floorHeight) { CHECK(adj); sameStoryLinks++; }  // 0-vs-0 (=> 0-vs-0.5): adjacent
            else                                 CHECK_FALSE(adj);                  // 0-vs-3/6/9: NOT adjacent
        }
    CHECK(sameStoryLinks > 0);   // the <1.5 m branch actually fires
    LevelGridSystem::shutdown(g);
}
```

- [ ] **Step 2: Run it, verify it fails**
```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="FOUR_STORY: finalize adjacency*"
```
Expected: red at a `CHECK_FALSE(adj)` — the stacked same-XZ rooms 3/6/9 m apart currently mark each other adjacent.

- [ ] **Step 3: Minimal implementation** — add the include near the top of `src/world/level_gen.cpp` (after line 14's `#include "core/log.h"`):

```cpp
#include "core/log.h"
#include <cmath>
#include <cstring>
```

Add the floorHeight predicate to the bbox-adjacency test (lines 226-229):

```cpp
            bool xOverlap = (ri.x < rj.x + rj.w + 3) && (rj.x < ri.x + ri.w + 3);
            bool zOverlap = (ri.z < rj.z + rj.d + 3) && (rj.z < ri.z + ri.d + 3);
            // Rooms on different STORIES (FOUR_STORY stacks four same-XZ room sets 3 m apart) are never
            // neighbours — else all four mark mutually adjacent and spawnFloorEnemies (skips spawn +
            // adjacent + 2-hop) seeds ZERO enemies. No-op for flat styles (every raised floor is 0.5 m).
            if (xOverlap && zOverlap && fabsf(ri.floorHeight - rj.floorHeight) < 1.5f)
                addAdjacency(ri, static_cast<u16>(i), rj, static_cast<u16>(j));
```

- [ ] **Step 4: Run the tests, verify they pass**
```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="FOUR_STORY: finalize adjacency*"
```
Expected: `1 passed`. Then confirm the fix is a no-op for existing styles:
```bash
./build/tests/dungeon_tests
```
Expected: full suite green (the flat-style adjacency tests in `test_level_gen`/`test_vertical_hall` are unchanged).

- [ ] **Step 5: Commit**
```bash
git add src/world/level_gen.cpp tests/world/test_four_story.cpp
git commit -m "$(cat <<'EOF'
fix(levelgen): make finalizeDungeon bbox-adjacency story-aware

Adds `&& fabsf(ri.floorHeight - rj.floorHeight) < 1.5f` to the room-adjacency test so
FOUR_STORY's four stacked same-XZ room sets (3/6/9 m apart) no longer mark mutually
adjacent — without which spawnFloorEnemies (skips spawn + adjacent + 2-hop) placed
zero enemies. No-op for every existing style (all raised floors are 0.5 m).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4.5: Register FOUR_STORY in the generic `test_level_gen` contract (`kAllStyles`)

**Files:**
- Modify: `tests/world/test_level_gen.cpp:71-76` (`kAllStyles`)

- [ ] **Step 1: Write the failing test** — extend the shared style list so the five generic contract cases (determinism, reachability, in-bounds rects, room-count floor, spawn-inside-room) now also run against FOUR_STORY across sizes 24/32/40/48. Edit `tests/world/test_level_gen.cpp`:

```cpp
constexpr LevelGen::LayoutStyle kAllStyles[] = {
    LevelGen::LayoutStyle::BSP_ROOMS,
    LevelGen::LayoutStyle::CAVERN,
    LevelGen::LayoutStyle::GAUNTLET,
    LevelGen::LayoutStyle::HUB,
    LevelGen::LayoutStyle::VERTICAL_HALL,
    LevelGen::LayoutStyle::FOUR_STORY,
};
```

(Note: this list currently omits VERTICAL_HALL too; add both so the contract covers every non-BSP style. The `VERTICAL_HALL falls back on a 32-grid` behaviour is exercised by sizes 24/32 here — both styles fall back to BSP below 40, which the reachability/room-count asserts already tolerate.)

- [ ] **Step 2: Run it, verify it fails (or confirm it exposes no contract gap)** — the generic contract now drives FOUR_STORY; a carve bug would surface here:
```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="LevelGen:*"
```
Expected: green — `carveFourStory` already honours the contract (all-non-solid interior => every room center is 4-connected-reachable; 16 rooms >= 5; rects in-bounds at 48; spawn room center resolves inside its room; deterministic). If any `LevelGen:` case goes red, that is a real `carveFourStory` contract gap to fix in Step 3.

- [ ] **Step 3: Minimal implementation** — none expected. If Step 2 surfaced a red case (e.g. a room rect leaking past cell 47 at size 48, or a reachability miss), fix it in `carveFourStory` (`src/world/level_gen.cpp`) — the half-tile room dims must satisfy `r.x + r.w <= size - 1` at every tested size, and every room center must land on non-solid interior floor. Re-run Step 2 until green.

- [ ] **Step 4: Run the tests, verify they pass**
```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests
```
Expected: full suite green — the generic `LevelGen:` contract (all sizes/seeds) now covers FOUR_STORY and VERTICAL_HALL alongside the dedicated `FOUR_STORY:` cases.

- [ ] **Step 5: Commit**
```bash
git add tests/world/test_level_gen.cpp
git commit -m "$(cat <<'EOF'
test(levelgen): cover FOUR_STORY (and VERTICAL_HALL) in the generic contract

Adds both non-BSP styles to test_level_gen's kAllStyles so determinism,
spawn->every-room reachability, in-bounds room rects, the >=5-room floor, and
spawn-inside-room now guard FOUR_STORY across sizes 24/32/40/48. Sub-40 grids fall
back to BSP, which the existing asserts already tolerate.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

Based on the spec and the real files, here are the Phase 5 tasks.

---

### Task 5.1: `--fourstory` dev door

**Files:**
- Modify: `src/engine/launch_options.h` (add `fourStory` field after `verticalHall`, `launch_options.h:53`)
- Modify: `src/engine/launch_options.cpp` (add `--fourstory` parse branch after the `--vhall` branch, `launch_options.cpp:148-150`)
- Modify: `src/engine/engine.h` (add `m_forceFourStory` after `m_forceVerticalHall`, `engine.h:337`)
- Modify: `src/engine/engine_launch.cpp` (apply `m_forceFourStory = opt.fourStory` after the `--vhall` apply, `engine_launch.cpp:73`)

- [ ] **Step 1: No practical unit test — say so, define the manual check.** `parseLaunchArgs` lives in `launch_options.cpp`, which is **not** in the `dungeon_tests` link list (convention list: only `level_gen/level_grid/collision/raycast` are linked). Linking it drags in `net/net.h` (ENet) + `game/item.h` transitively — impractical for a one-flag parse. Verification is a compile + boot check: after implementation, `./build/dungeon_game --new warrior --floor 6 --fourstory` boots with **no** `Unknown launch arg '--fourstory'` warning in the log. (Mirrors `--vhall`: a game-jump modifier, so `--fourstory` alone with no `--new/--load` correctly falls back to the menu via the existing `active && save==NONE` guard.)
- [ ] **Step 2: Establish a green baseline** — build the game as-is so you start from a known-good tree:
  ```bash
  cmake --build build
  ```
  Expected: links cleanly, no errors.
- [ ] **Step 3: Minimal implementation** — four edits, each mirroring the `--vhall` plumbing.

  `src/engine/launch_options.h` — add after the `verticalHall` field (line 53):
  ```cpp
      bool fourStory   = false;                  // --fourstory: force the four-story FOUR_STORY "Descent" layout on non-boss floors (dev)
  ```

  `src/engine/launch_options.cpp` — add a branch after the `--vhall` branch (after line 150):
  ```cpp
          } else if (ieq(a, "--fourstory")) {
              opt.fourStory = true;      // modifier on normal play (needs --new/--load); mirrors --vhall
              opt.active    = true;
  ```

  `src/engine/engine.h` — add after `m_forceVerticalHall` (line 337):
  ```cpp
      bool m_forceFourStory    = false; // dev (--fourstory): force the four-story FOUR_STORY on non-boss floors
  ```

  `src/engine/engine_launch.cpp` — add after `m_forceVerticalHall = opt.verticalHall;` (line 73):
  ```cpp
      // Dev door (--fourstory): force the four-story FOUR_STORY "Descent" layout on every non-boss floor
      // so it is playtestable without waiting for its weighted roll (see startGame). Mirrors --vhall.
      m_forceFourStory = opt.fourStory;
  ```
- [ ] **Step 4: Rebuild + run the manual check:**
  ```bash
  cmake --build build && ./build/dungeon_game --new warrior --floor 6 --fourstory
  ```
  Expected: game boots into `IN_GAME`; the log has **no** `Unknown launch arg` line. (The floor is still an ordinary layout at this point — `startGame` doesn't consume the flag until Task 5.4; this step only proves the flag parses and plumbs through.)
- [ ] **Step 5: Commit:**
  ```bash
  git add src/engine/launch_options.h src/engine/launch_options.cpp src/engine/engine.h src/engine/engine_launch.cpp
  git commit -m "$(cat <<'EOF'
feat(launch): --fourstory dev door for the four-story FOUR_STORY layout

Mirrors --vhall: LaunchOptions.fourStory + parse in launch_options.cpp,
m_forceFourStory in engine.h, applied in engine_launch.cpp. startGame
consumes it in a follow-up.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
  ```

---

### Task 5.2: `spawnFloorHoleSnipers` — ranged snipers at drop-hole edges

**Files:**
- Modify: `src/engine/engine.h` (declare `spawnFloorHoleSnipers` after `spawnFloorNests`, `engine.h:1277`)
- Modify: `src/engine/engine_spawn.cpp` (define it after `spawnFloorNests`, `engine_spawn.cpp:1408-1464`)

- [ ] **Step 1: No practical unit test — say so, define the manual check.** This is an `Engine::` member: exercising it needs a fully-linked `Engine` (SDL/GL/net/audio) **and** a live `DungeonResult` carrying `dropHoles` — neither is available in `dungeon_tests`. The raycast "thread-a-hole" contract it relies on is already pinned at the raycast level (Phase 2 `test_platform`/raycast tests); the seating itself is engine glue. Verification here is compile+link only; observable behavior (snipers on slab edges plunge-firing down a hole) is validated in the Task 5.4 playtest. The function is defined-but-not-called this task (the call site lands in 5.4) — member functions are never warned unused, so the tree stays green.
- [ ] **Step 2: Establish a green baseline:**
  ```bash
  cmake --build build
  ```
  Expected: links cleanly.
- [ ] **Step 3: Minimal implementation** — modeled byte-for-byte on `spawnFloorNests` (the pinned model), iterating `dungeon.dropHoles` instead of `portals`.

  `src/engine/engine.h` — declare after the `spawnFloorNests` declaration (line 1277):
  ```cpp
      // FOUR_STORY only: seat ranged snipers at drop-hole EDGES (an adjacent intact-slab cell at the
      // hole's surface story, raised eye, NO ground snap). Their multi-slab raycast LOS threads the hole
      // to plunge-fire one story down. spawnFloorNests no-ops for FOUR_STORY (portalCount==0). See startGame.
      void spawnFloorHoleSnipers(const DungeonResult& dungeon, u8 tier);
  ```

  `src/engine/engine_spawn.cpp` — define after `spawnFloorNests` closes (after line 1464):
  ```cpp
  // FOUR_STORY "Descent": seat ranged snipers at DROP-HOLE edges. Unlike spawnFloorNests (which rides
  // StoryPortal ramp tops and no-ops here because FOUR_STORY has portalCount==0), this rides the recorded
  // dropHoles: a sniper stands on the intact slab just OFF a hole, at that hole's surface story, and its
  // multi-slab raycast LOS threads the hole to plunge-fire one level down (a solid slab blocks a shot into
  // a floor). No ground snap — the hole's surfaceY is the authoritative footing.
  void Engine::spawnFloorHoleSnipers(const DungeonResult& dungeon, u8 tier) {
      if (dungeon.dropHoleCount == 0) return;

      // Ranged defs of this tier (grounded preferred so they sit ON the slab; flyers fall back). Same
      // selection as spawnFloorNests so the two sniper sources read identically.
      const EnemyDef* tierDefs[MAX_ENEMY_DEFS];
      u32 tierCount = collectTierDefs(m_enemyDefs, tier, tierDefs, MAX_ENEMY_DEFS);
      const EnemyDef* ranged[MAX_ENEMY_DEFS];
      u32 rangedCount = 0;
      for (u32 i = 0; i < tierCount; i++)
          if (tierDefs[i]->attackRange > 5.0f && !tierDefs[i]->flying) ranged[rangedCount++] = tierDefs[i];
      if (rangedCount == 0)
          for (u32 i = 0; i < tierCount; i++)
              if (tierDefs[i]->attackRange > 5.0f) ranged[rangedCount++] = tierDefs[i];
      if (rangedCount == 0) return;   // no ranged enemies this tier → no hole snipers

      const u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
      const f32 hpMult  = GameConst::floorHealthMult(effectiveFloor);
      const f32 dmgMult = GameConst::floorDamageMult(effectiveFloor)
                          * GameConst::difficultyDamageBump(m_difficulty);
      const f32 cs = m_level.grid.cellSize;

      for (u8 hIdx = 0; hIdx < dungeon.dropHoleCount; hIdx++) {
          if (m_entities.activeCount >= MAX_ENTITIES - 2) break;
          const DungeonResult::DropHole& hole = dungeon.dropHoles[hIdx];
          const u32 nest = 1 + (static_cast<u32>(std::rand()) & 1u);   // 1-2 snipers per hole
          for (u32 k = 0; k < nest; k++) {
              if (m_entities.activeCount >= MAX_ENTITIES - 2) break;
              const EnemyDef& def = *ranged[static_cast<u32>(std::rand()) % rangedCount];
              // One cell off the hole's +X edge (holes are >=2 wide, so +1.5 cells clears them onto the
              // intact adjacent slab), spread along +Z; feet at the hole surface story, centre raised.
              Vec3 pos = { hole.pos.x + cs * 1.5f,
                           hole.surfaceY + def.halfExtents.y,
                           hole.pos.z + cs * static_cast<f32>(k) };
              EntityHandle h = EntitySystem::spawn(m_entities, pos, def.halfExtents, def.flying,
                  def.health, def.moveSpeed, def.detectionRange, def.attackRange, def.attackCooldown, def.damage);
              Entity* ent = handleGet(m_entities, h);
              if (!ent) break;
              ent->meshId       = def.meshId;
              ent->materialId   = def.materialId;
              ent->enemyType    = def.enemyType;
              ent->enemyRole    = def.role;
              ent->aiPreference = def.aiPreference;
              const ptrdiff_t slot = &def - m_enemyDefs.defs;
              ent->enemyDefIdx = (slot >= 0 && slot < static_cast<ptrdiff_t>(m_enemyDefs.count))
                               ? static_cast<u8>(slot) : 0xFF;
              ent->baseMoveSpeed      = ent->moveSpeed;
              ent->baseAttackCooldown = ent->attackCooldown;
              ent->level = static_cast<u16>(effectiveFloor);
              ent->health   *= hpMult;
              ent->maxHealth = ent->health;
              ent->damage   *= dmgMult;
              ent->onHitEffect   = def.onHitEffect;
              ent->onHitDuration = def.onHitDuration;
              ent->onHitDps      = def.onHitDps * dmgMult;
              // NO ensureNotInWall / ground snap: it would pull the sniper down to the ground story. The
              // hole surface Y is authoritative; the story-aware snap keeps it on the intact slab.
          }
      }
  }
  ```
- [ ] **Step 4: Rebuild the game, verify it links:**
  ```bash
  cmake --build build
  ```
  Expected: compiles and links (function defined, referenced by the linker as an `Engine` member; behavior deferred to Task 5.4's call site + playtest).
- [ ] **Step 5: Commit:**
  ```bash
  git add src/engine/engine.h src/engine/engine_spawn.cpp
  git commit -m "$(cat <<'EOF'
feat(spawn): spawnFloorHoleSnipers — ranged snipers at FOUR_STORY drop-hole edges

Models spawnFloorNests but rides dungeon.dropHoles: seats 1-2 ranged defs on
the intact slab off each hole's +X edge, at the hole surface story, no ground
snap so the story-aware snap holds them on the slab and their multi-slab LOS
plunge-fires one level down. Call site + playtest land next.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
  ```

---

### Task 5.3: `spawnFloorEnemies` walkable-surface guard (reject spawns over drop-holes)

**Files:**
- Test: `tests/world/test_four_story.cpp` (append one `TEST_CASE`; created + linked in Phase 4. It already includes `world/level_gen.h` → `world/level_grid.h`; add `#include <cmath>` at the top if absent.)
- Modify: `src/engine/engine_spawn.cpp` (add the guard lambda in `spawnFloorEnemies` after `detectMult`, `engine_spawn.cpp:322-323`; extend both candidate-cell validity checks, JSON path `engine_spawn.cpp:403-408` and fallback path `engine_spawn.cpp:516-521`)

- [ ] **Step 1: Write the test** — a regression pin of the exact predicate the inline guard reuses, against the real (already-linked) `level_grid` code. Append to `tests/world/test_four_story.cpp`:
  ```cpp
  TEST_CASE("FourStory spawnFloorEnemies guard: punched drop-hole cell fails the walkable-surface test") {
      // Pins the exact predicate spawnFloorEnemies uses to reject a spawn cell over a hole:
      //   accept iff |effectiveFloorHeight(grid, gx, gz, room.floorHeight) - room.floorHeight| < PLATFORM_STEP_TOLERANCE
      LevelGrid grid;
      LevelGridSystem::init(grid, 8, 8, 1.0f);
      // Solid-walled interior carrying the full 3-slab stack (L1=12, L2=24, L3=36 quarter-units).
      for (u32 z = 1; z < 7; z++)
          for (u32 x = 1; x < 7; x++) {
              GridCell& c = LevelGridSystem::getCell(grid, x, z);
              c.flags = CELL_FLOOR;
              c.floorHeight = 0;
              LevelGridSystem::addPlatform(c, 12, 1);   // FS_L1_Q
              LevelGridSystem::addPlatform(c, 24, 1);   // FS_L2_Q
              LevelGridSystem::addPlatform(c, 36, 1);   // FS_L3_Q
          }
      // An L3 room seeds enemies at room.floorHeight = 9.0 m (FS_L3_Q * 0.25).
      const f32 roomFloorY = 36 * 0.25f;   // 9.0 m
      // Intact cell (4,4): highest slab top == 9.0 → within tolerance → ACCEPTED.
      const f32 effIntact = LevelGridSystem::effectiveFloorHeight(grid, 4, 4, roomFloorY);
      CHECK(effIntact == doctest::Approx(roomFloorY));
      CHECK(std::fabs(effIntact - roomFloorY) < PLATFORM_STEP_TOLERANCE);
      // Punch the L3 slab out of (3,3) — a drop-hole cell on the L3 story.
      LevelGridSystem::removePlatform(LevelGridSystem::getCell(grid, 3, 3), 36);
      // Hole cell: no L3 slab, so the story selector drops to L2 (6.0 m) → NOT within tolerance → REJECTED.
      const f32 effHole = LevelGridSystem::effectiveFloorHeight(grid, 3, 3, roomFloorY);
      CHECK(effHole == doctest::Approx(24 * 0.25f));   // 6.0 m
      CHECK(std::fabs(effHole - roomFloorY) >= PLATFORM_STEP_TOLERANCE);
  }
  ```
- [ ] **Step 2: Run it** — note the honest state:
  ```bash
  cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*walkable-surface*"
  ```
  Expected: **PASSES immediately.** This is a regression pin, not a fail-first test: `effectiveFloorHeight` is already multi-slab from the Phase 0 foundation, and the spec keeps the guard **inline** in an `Engine` member (not unit-linkable) while forbidding inventing a new named predicate API — so the doctest locks the exact numeric condition the inline guard depends on. The guard's *integration* is verified by the Task 5.4 playtest ("no enemy stands over a hole"). If it fails, the Phase 0/4 foundation regressed — fix that before proceeding.
- [ ] **Step 3: Minimal implementation** — add the guard lambda, then extend both validity checks.

  `src/engine/engine_spawn.cpp` — add after the `detectMult` definition (after line 323):
  ```cpp
      // FOUR_STORY guard: a stacked-slab floor has drop-holes punched through upper stories. A spawn cell
      // over a hole has no slab at the room's story, so effectiveFloorHeight resolves to a LOWER surface —
      // seeding there would instantly snap the enemy down a level. Treat it like a wall (reject → nudge to
      // room centre). No-op on flat/2-story floors (every cell resolves to room.floorHeight within tolerance).
      auto notWalkableAtStory = [&](u32 gx, u32 gz, f32 storyY) -> bool {
          return std::fabs(LevelGridSystem::effectiveFloorHeight(m_level.grid, gx, gz, storyY) - storyY)
                 >= PLATFORM_STEP_TOLERANCE;
      };
  ```

  JSON path — replace the validity check at lines 403-408:
  ```cpp
                  if (LevelGridSystem::worldToGrid(m_level.grid, spawnPos, spGx, spGz) &&
                      (LevelGridSystem::isSolid(m_level.grid, spGx, spGz) ||
                       notWalkableAtStory(spGx, spGz, room.floorHeight))) {
                      ex = (room.x + room.w * 0.5f) * m_level.grid.cellSize;
                      ez = (room.z + room.d * 0.5f) * m_level.grid.cellSize;
                      spawnPos = {ex, spawnY, ez};
                  }
  ```

  Fallback path — replace the validity check at lines 516-521:
  ```cpp
                  if (LevelGridSystem::worldToGrid(m_level.grid, spawnPos, spGx, spGz) &&
                      (LevelGridSystem::isSolid(m_level.grid, spGx, spGz) ||
                       notWalkableAtStory(spGx, spGz, room.floorHeight))) {
                      ex = (room.x + room.w * 0.5f) * m_level.grid.cellSize;
                      ez = (room.z + room.d * 0.5f) * m_level.grid.cellSize;
                      spawnPos = {ex, spawnY, ez};
                  }
  ```
- [ ] **Step 4: Rebuild + run the suite, verify green** — the predicate pin stays green and the game builds with the guard:
  ```bash
  cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests
  cmake --build build
  ```
  Expected: full suite green (no regression — the guard is a no-op on every non-stacked style), game links. Integration confirmed in Task 5.4's playtest.
- [ ] **Step 5: Commit:**
  ```bash
  git add tests/world/test_four_story.cpp src/engine/engine_spawn.cpp
  git commit -m "$(cat <<'EOF'
feat(spawn): reject enemy spawns over FOUR_STORY drop-holes (walkable-surface guard)

spawnFloorEnemies now treats a candidate cell whose effectiveFloorHeight at the
room story is not within PLATFORM_STEP_TOLERANCE of room.floorHeight (a punched
hole) like a wall — nudges to room centre so nothing seeds onto a hole and
instantly snaps down. No-op on flat/2-story floors. Predicate regression-pinned.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
  ```

---

### Task 5.4: `usesBalconyEndpoints` wiring + FOUR_STORY spawn-L3/exit-L0 + hole-sniper call (integration + playtest acceptance)

**Files:**
- Modify: `src/engine/engine_startgame.cpp` (add `usesBalconyEndpoints` static helper near the top, after the `extern` block `engine_startgame.cpp:64`; add FOUR_STORY dev-door force after the `--vhall` force `engine_startgame.cpp:534-535`; grid-force `engine_startgame.cpp:538`; spawn override `engine_startgame.cpp:551-554`; sniper call after `engine_startgame.cpp:640-641`; exit override `engine_startgame.cpp:712-715`)

- [ ] **Step 1: No practical unit test — say so, define the acceptance check.** These edits are all inside `Engine::startGame` (needs the full engine + a live world) and a file-`static` helper (not externally linkable). No doctest can reach them. This is the phase's exit gate, so Step 4 is the full manual `--fourstory` playtest from spec §8 (spawn L3, dive each hole to land one level down, reach L0; snipers plunge-fire; enemies fall/never climb; host+client identical grids). Dependencies from Tasks 5.1/5.2/5.3 must be committed first (`m_forceFourStory`, `spawnFloorHoleSnipers`, the enemy guard).
- [ ] **Step 2: Establish a green baseline:**
  ```bash
  cmake --build build
  ```
  Expected: links cleanly (Tasks 5.1-5.3 already in tree).
- [ ] **Step 3: Minimal implementation** — one new helper + five wiring edits (all mirror the existing VERTICAL_HALL sites).

  Add the static helper after the `extern` block near the top of `src/engine/engine_startgame.cpp` (after line 64, before `rollNpcEquipment`):
  ```cpp
  // True for the layout styles whose spawn/exit sit on an explicit balcony/slab position
  // (spawnBalconyPos/exitBalconyPos) instead of the plain room centre: the two-story VERTICAL_HALL
  // and the four-story FOUR_STORY "Descent". Gates the grid-force + endpoint overrides below so both
  // stacked-slab styles share one code path (add a style here and it inherits balcony endpoints).
  static bool usesBalconyEndpoints(LevelGen::LayoutStyle s) {
      return s == LevelGen::LayoutStyle::VERTICAL_HALL ||
             s == LevelGen::LayoutStyle::FOUR_STORY;
  }
  ```

  Edit A — add the FOUR_STORY dev-door force right after the `--vhall` force (after line 535, before the grid-force comment at line 536):
  ```cpp
      // Dev door (--fourstory): force the four-story FOUR_STORY "Descent" on any non-boss floor (bosses
      // land every 5th floor and would stomp the stacked slabs) — mirrors the --vhall door above.
      if (m_forceFourStory && m_level.currentFloor % 5 != 0)
          layoutStyle = LevelGen::LayoutStyle::FOUR_STORY;
  ```

  Edit B — grid-force, replace line 538:
  ```cpp
      // Both stacked-slab styles need a large grid so the upper stories are real floors, not a strip.
      if (usesBalconyEndpoints(layoutStyle) && gridSize < 44) gridSize = 44;
  ```

  Edit C — spawn override, replace lines 551-554:
  ```cpp
      if (usesBalconyEndpoints(layoutStyle) &&
          lengthSq(dungeon.spawnBalconyPos) > 0.0f) {
          spawnPos = dungeon.spawnBalconyPos;   // VERTICAL_HALL entrance slab / FOUR_STORY L3 spawn (y=9)
      }
  ```

  Edit D — sniper call, replace lines 640-641:
  ```cpp
      if (layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL)
          spawnFloorNests(dungeon, currentTier);
      // FOUR_STORY: spawnFloorNests no-ops (portalCount==0); seat snipers at the drop-hole edges instead.
      else if (layoutStyle == LevelGen::LayoutStyle::FOUR_STORY)
          spawnFloorHoleSnipers(dungeon, currentTier);
  ```

  Edit E — exit override, replace lines 712-715:
  ```cpp
          if (usesBalconyEndpoints(layoutStyle) &&
              lengthSq(dungeon.exitBalconyPos) > 0.0f) {
              m_level.floorDoorPos = dungeon.exitBalconyPos;   // VH far-side slab / FOUR_STORY L0 exit (y=0)
          }
  ```
- [ ] **Step 4: Rebuild + run the full `--fourstory` playtest acceptance** (spec §8 phase 5):
  ```bash
  cmake --build build && ./build/dungeon_game --new warrior --floor 6 --fourstory
  ```
  Confirm, in order:
  1. **Spawn on L3** — you start elevated, looking down; the log prints `Floor 6 exit portal at (x, 0.0, z)` (exit at ground story y=0 while you are at y≈9).
  2. **Descent works** — walk into each drop-hole (≥2×2, so you fall cleanly) and land exactly **one** story below each time (9→6→3→0), never an express-shaft through two levels; edges also drop you.
  3. **Reach L0 / exit** — you can path across L0 to the exit portal and it triggers a descend.
  4. **No enemy over a hole** (Task 5.3 guard) — no enemy is standing mid-air / snapping down at floor entry; enemies exist on all four stories.
  5. **Hole snipers** (Task 5.2) — ranged enemies sit at hole edges and plunge-fire at angled targets one level down; a target straight under the edge is a brief self-blocked blind spot (expected — you can't shoot through your own floor).
  6. **Enemies fall, none climb** — an enemy whose centre crosses a hole teleport-snaps down a story; none ascend.
  7. **Host/client identical grids (co-op determinism)** — in two terminals:
     ```bash
     ./build/dungeon_game --host --new warrior --floor 6 --fourstory
     ./build/dungeon_game --join 127.0.0.1 --new ranger
     ```
     Both see the same slab/hole layout, spawn, and exit; the joiner descends the same holes. (Seed-built + server-authoritative → no PROTOCOL bump.)
  8. **GPU fill sanity** (spec §7) — no `pushQuad` overflow warning in the log; frame time stays within budget while looking straight down a hole (worst-case stacked overdraw). Use `--screenshot-interval 5` or the F9 net-graph if you want a capture.
- [ ] **Step 5: Commit:**
  ```bash
  git add src/engine/engine_startgame.cpp
  git commit -m "$(cat <<'EOF'
feat(startgame): wire FOUR_STORY spawn L3/exit L0 via usesBalconyEndpoints + hole snipers

usesBalconyEndpoints(style) folds VERTICAL_HALL + FOUR_STORY through one path at
the three balcony sites (grid-force >=44, spawnBalconyPos->spawnPos,
exitBalconyPos->floorDoorPos). Adds the --fourstory dev-door force and calls
spawnFloorHoleSnipers for FOUR_STORY (spawnFloorNests no-ops at portalCount==0).
Verified by the --fourstory playtest: spawn L3, drop-descend to L0, snipers
plunge-fire, host/client grids identical.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
  ```

I have all anchor text. This is a docs-only phase, so the "failing test" for each task is a documentation-presence assertion (grep) — there is no C++ to compile, and fabricating a doctest would violate the no-placeholder / real-code rule. Each task's implementation step gives the exact `Edit` old→new markdown.

---

### Task 6.1: CLAUDE.md — `FOUR_STORY` "Descent" paragraph + Directory Map bump

**Files:**
- Modify: `/home/aaron/game/CLAUDE.md:149-152` (insert after the VERTICAL_HALL paragraph, before "Crowd control")
- Modify: `/home/aaron/game/CLAUDE.md:191` (Directory Map `src/world/` row)

- [ ] **Step 1: Write the failing test** — a doc-presence assertion (no C++ in a docs phase). Save as `/home/aaron/game/scratchpad/check_6_1.sh`:
```bash
#!/usr/bin/env bash
# Doc-presence "test" for Task 6.1: the FOUR_STORY paragraph + Directory Map bump must exist in CLAUDE.md.
set -u
f=/home/aaron/game/CLAUDE.md
fail=0
grep -q 'Four-story PvE floors (`FOUR_STORY` — "Descent")' "$f" || { echo "MISSING: FOUR_STORY heading"; fail=1; }
grep -q 'MAX_PLATFORMS_PER_CELL' "$f"                       || { echo "MISSING: MAX_PLATFORMS_PER_CELL"; fail=1; }
grep -q '`setPlatform`' "$f"                                || { echo "MISSING: setPlatform"; fail=1; }
grep -q '`addPlatform`' "$f"                                || { echo "MISSING: addPlatform"; fail=1; }
grep -q '6 layout styles' "$f"                              || { echo "MISSING: Directory Map bump to 6 styles"; fail=1; }
[ "$fail" -eq 0 ] && echo "PASS 6.1" || { echo "FAIL 6.1"; exit 1; }
```

- [ ] **Step 2: Run it, verify it fails** — the paragraph does not exist yet:
```bash
bash /home/aaron/game/scratchpad/check_6_1.sh
```
Expected output: `MISSING: FOUR_STORY heading`, `MISSING: MAX_PLATFORMS_PER_CELL`, … then `FAIL 6.1` (exit 1).

- [ ] **Step 3: Minimal implementation** — two `Edit` calls on `/home/aaron/game/CLAUDE.md`.

Edit A — insert the FOUR_STORY paragraph. old_string:
```text
`docs/superpowers/plans/2026-07-20-two-story-vertical-hall-pve.md`.

**Crowd control (CC-Resistance + player stun).**
```
new_string:
```text
`docs/superpowers/plans/2026-07-20-two-story-vertical-hall-pve.md`.

**Four-story PvE floors (`FOUR_STORY` — "Descent").** A dungeon floor stacked **four walkable stories** on
one footprint (`world/level_gen.cpp` `carveFourStory`, weighted-rolled on **floor-6+ non-boss** floors, forced
to a 44-grid; `--fourstory` dev door). You **spawn on the top story (L3 @ 9 m)** and work DOWN to the **exit on
the ground (L0)**; the ONLY inter-level link is **dropping** through holes in the slabs (or off an edge) — no
ramps, no stairs, no way back up. Loot + enemies seed **evenly across all four stories**, so each is its own
fight and diving past one is a genuine tactical **escape** and a **commitment** (no return for skipped loot).
Built on a multi-slab foundation: `GridCell` now carries up to `MAX_PLATFORMS_PER_CELL` (=3) stacked slab tops
(12/24/36 qu = 3/6/9 m) → 4 stories on one footprint (VERTICAL_HALL still uses 1), and the single story
selector `effectiveFloorHeight` ("highest slab top within step tolerance at/below the feet, else the base
floor") drives every consumer (collision snap, both `snapEntityToFloor` twins, raycast, mesher) — so a body
over a **hole** (a cell missing that slab) resolves to the next intact surface below (a clean drop-through).
`carveFourStory` slabs every interior cell then punches **offset drop-holes** in disjoint quadrant sets across
ADJACENT levels (L3→{NW,SE}, L2→{NE,SW}, L1→{NW,SE}) so a dive always lands one story down (never an express
shaft); holes are `>=2×2` (a 1-cell hole lets the floor-snap re-grab you), recorded in
`DungeonResult.dropHoles`. Spawn L3 / exit L0 via `usesBalconyEndpoints` (shared with VERTICAL_HALL);
`spawnFloorHoleSnipers` seats ranged defs at hole edges to plunge-fire down. **Slab authorers:** single-slab
writers use `setPlatform` (REPLACE-to-one — every existing VERTICAL_HALL/arena writer migrated to it, so
shipped geometry stays byte-identical); the FOUR_STORY generator alone uses `addPlatform` (ACCUMULATE,
strictly-ascending sorted-insert) + `removePlatform` (hole puncher). `GridCell` is `calloc`'d per floor and
**never serialized**, and the carve is GenRNG + integer quarter-units only, so this replicates in co-op with
**no `PROTOCOL_VERSION` and no `SAVE_VERSION` bump**. Slab/hole primitives pinned by
`tests/world/test_platform.cpp`, the generator by `tests/world/test_four_story.cpp`. Design:
`docs/superpowers/specs/2026-07-20-four-story-descent-floors-design.md`.

**Crowd control (CC-Resistance + player stun).**
```

Edit B — bump the Directory Map count. old_string:
```text
(5 layout styles: BSP rooms / cavern / gauntlet / hub / vertical-hall two-story, seed-picked per floor)
```
new_string:
```text
(6 layout styles: BSP rooms / cavern / gauntlet / hub / vertical-hall two-story / four-story descent, seed-picked per floor)
```

- [ ] **Step 4: Run the tests, verify they pass** — the doc-presence assertion is now green, and no code file changed:
```bash
bash /home/aaron/game/scratchpad/check_6_1.sh && git -C /home/aaron/game diff --name-only
```
Expected: `PASS 6.1`, and `diff --name-only` lists ONLY `CLAUDE.md` (docs-only → the previously-green build/test suite is unaffected).

- [ ] **Step 5: Commit**
```bash
cd /home/aaron/game
git add CLAUDE.md
git commit -m "docs(claude): FOUR_STORY 'Descent' paragraph + directory-map bump

Add the four-story descent layout beside VERTICAL_HALL: MAX_PLATFORMS_PER_CELL=3
(4 stories), setPlatform vs addPlatform authorers, drop-only, no save/wire change.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6.2: engine-reference — 13-byte `GridCell`, 4 invariants, multi-slab `effectiveFloorHeight`

**Files:**
- Modify: `/home/aaron/game/.claude/skills/engine-reference/SKILL.md:29` (`GridCell` cheat-sheet row)
- Modify: `/home/aaron/game/.claude/skills/engine-reference/SKILL.md:58-66` (the `CELL_PLATFORM` paragraph)

- [ ] **Step 1: Write the failing test** — doc-presence assertion. Save as `/home/aaron/game/scratchpad/check_6_2.sh`:
```bash
#!/usr/bin/env bash
# Doc-presence "test" for Task 6.2: the 13-byte GridCell + 4 invariants + multi-slab rule.
set -u
f=/home/aaron/game/.claude/skills/engine-reference/SKILL.md
fail=0
grep -q 'sizeof(GridCell)==13' "$f"                    || { echo "MISSING: 13-byte static_assert"; fail=1; }
grep -q 'Four invariants' "$f"                          || { echo "MISSING: Four invariants"; fail=1; }
grep -q 'platHeight\[MAX_PLATFORMS_PER_CELL\]' "$f"     || { echo "MISSING: platHeight array"; fail=1; }
grep -q 'highest slab top within' "$f"                  || { echo "MISSING: effectiveFloorHeight rule"; fail=1; }
grep -q 'REPLACE-to-one' "$f"                           || { echo "MISSING: setPlatform authorer"; fail=1; }
grep -q 'stacked slabs' "$f"                            || { echo "MISSING: GridCell row multi-slab note"; fail=1; }
[ "$fail" -eq 0 ] && echo "PASS 6.2" || { echo "FAIL 6.2"; exit 1; }
```

- [ ] **Step 2: Run it, verify it fails**:
```bash
bash /home/aaron/game/scratchpad/check_6_2.sh
```
Expected: `MISSING: 13-byte static_assert`, `MISSING: Four invariants`, … then `FAIL 6.2` (exit 1).

- [ ] **Step 3: Minimal implementation** — two `Edit` calls on `/home/aaron/game/.claude/skills/engine-reference/SKILL.md`.

Edit A — the `GridCell` cheat-sheet row. old_string:
```text
| `LevelGrid` / `GridCell` | `world/level_grid.h` | Cell flags `CELL_SOLID/FLOOR/CEILING` + opt-in verticality `CELL_LEDGE` (jump-gated) / `CELL_JUMPPAD` (launch pad) / `CELL_PLATFORM` (walk-under 2nd story). Heights in quarter-units |
```
new_string:
```text
| `LevelGrid` / `GridCell` | `world/level_grid.h` | Cell flags `CELL_SOLID/FLOOR/CEILING` + opt-in verticality `CELL_LEDGE` (jump-gated) / `CELL_JUMPPAD` (launch pad) / `CELL_PLATFORM` (walk-under slab — now up to `MAX_PLATFORMS_PER_CELL`=3 **stacked slabs** → 4 stories; `GridCell` is 13 all-`u8` bytes, `static_assert`-pinned, `calloc`'d per floor + **never serialized**). Heights in quarter-units |
```

Edit B — rewrite the `CELL_PLATFORM` paragraph opening for N stories. old_string (lines 58-66, ending at "victim's story."):
```text
`CELL_PLATFORM` (flag bit 5) is the real two-story cell: `GridCell.platHeight` (quarter-units)
is the slab TOP, thickness `PLATFORM_THICKNESS_Q` (=2 qu, 0.5 m), underside clamped to the base
floor. The cell's `floorHeight` remains the walkable GROUND story beneath. Story selection is
`LevelGridSystem::effectiveFloorHeight(grid,x,z,feetY)` (slab top iff feet within
`PLATFORM_STEP_TOLERANCE` = `STEP_UP_HEIGHT` = 0.4 m below it — static_assert-pinned in
collision.cpp). Consumers: `Collision::moveAndSlide` (story-aware landing/snap keyed on
PRE-move feet Y, `overlapsPlatformBand` XZ gate, underside head clamp), `Raycast::cast`
(top/underside planes + rim), the mesher (top/underside/owned-rim quads),
`Teleport::resolveDest` (lands at the victim's story).
```
new_string:
```text
`CELL_PLATFORM` (flag bit 5) is the multi-story cell. `GridCell` carries a fixed
`platHeight[MAX_PLATFORMS_PER_CELL]` (=3, quarter-unit slab TOP surfaces, **strictly ascending**) +
`platMaterialId[]` + a `platCount` (0..3) — one footprint, up to **4 walkable stories** (ground + 3 slabs;
`VERTICAL_HALL` uses 1 slab, `FOUR_STORY` uses 3 @ 12/24/36 qu). The struct is **13 all-`u8` bytes**
(`static_assert(sizeof(GridCell)==13)`), `calloc`'d per floor and **never serialized** — a size change
silently breaks the `test_level_gen` determinism `memcmp`. **Four invariants** (header-documented): (a) tops
strictly ascending; (b) `CELL_PLATFORM` set iff `platCount>0`; (c) slots `>= platCount` are **zero**
(canonical byte-form → logically-identical cells compare byte-equal); (d) same-cell tops differ by
`> PLATFORM_STEP_TOLERANCE` (else `effectiveFloorHeight` teleports a body up a slab — debug-asserted in
`addPlatform`). Each slab is `PLATFORM_THICKNESS_Q` (=2 qu, 0.5 m) thick, underside clamped **down to the
next-lower surface** (previous slab top, else base floor). **Three authorers** (`level_grid.{h,cpp}`):
`setPlatform` (REPLACE-to-one, byte-identical to the old scalar write — every single-slab writer uses it),
`addPlatform` (ACCUMULATE, sorted-insert, de-dup, `FOUR_STORY`-only), `removePlatform` (hole puncher — shifts
higher entries down, zeroes the vacated slot, clears the flag at count 0). Query via
`platformCount`/`hasPlatform`/`getPlatformTop[/i]`/`getPlatformUnderside(,,i)`. Story selection is
`LevelGridSystem::effectiveFloorHeight(grid,x,z,feetY)` = the **highest slab top within
`PLATFORM_STEP_TOLERANCE` = `STEP_UP_HEIGHT` = 0.4 m at/below the feet, else the base floor**
(static_assert-pinned in collision.cpp) — so a body over a **hole** (a cell missing that slab) resolves to the
next intact surface below (a clean drop-through). Consumers loop `[0,platformCount)`: `Collision::moveAndSlide`
(story-aware landing/snap keyed on PRE-move feet Y, `overlapsPlatformBand` XZ gate, running-min underside head
clamp), `Raycast::cast` (top-down descend / bottom-up rise slab loops + rim), the mesher (per-slab
top/underside/owned-rim quads), `Teleport::resolveDest` (lands at the victim's story).
```

- [ ] **Step 4: Run the tests, verify they pass**:
```bash
bash /home/aaron/game/scratchpad/check_6_2.sh && git -C /home/aaron/game diff --name-only
```
Expected: `PASS 6.2`, and `diff --name-only` lists ONLY `.claude/skills/engine-reference/SKILL.md` (docs-only → suite stays green).

- [ ] **Step 5: Commit**
```bash
cd /home/aaron/game
git add .claude/skills/engine-reference/SKILL.md
git commit -m "docs(engine-reference): 13-byte GridCell, 4 invariants, multi-slab effectiveFloorHeight

Generalize the CELL_PLATFORM section to N stories: the platHeight[MAX_PLATFORMS_PER_CELL]
array, the four canonical-byte-form invariants, the three authorers, and the
'highest slab top at/below feet' story-selection rule.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6.3: engine-reference — `DungeonResult.dropHoles`, `FOUR_STORY` generator, `--fourstory` dev door

**Files:**
- Modify: `/home/aaron/game/.claude/skills/engine-reference/SKILL.md:73-75` (insert a `FOUR_STORY` paragraph after the `CELL_PLATFORM` block)
- Modify: `/home/aaron/game/.claude/skills/engine-reference/SKILL.md:355` (append `FOUR_STORY` to the seed→`pickLayoutStyle` bullet)

- [ ] **Step 1: Write the failing test** — doc-presence assertion. Save as `/home/aaron/game/scratchpad/check_6_3.sh`:
```bash
#!/usr/bin/env bash
# Doc-presence "test" for Task 6.3: dropHoles + carveFourStory + --fourstory in engine-reference.
set -u
f=/home/aaron/game/.claude/skills/engine-reference/SKILL.md
fail=0
grep -q '`DungeonResult.dropHoles' "$f"        || { echo "MISSING: dropHoles record"; fail=1; }
grep -q 'MAX_DROP_HOLES' "$f"                    || { echo "MISSING: MAX_DROP_HOLES"; fail=1; }
grep -q '`--fourstory`' "$f"                     || { echo "MISSING: --fourstory dev door"; fail=1; }
grep -q 'usesBalconyEndpoints' "$f"              || { echo "MISSING: usesBalconyEndpoints"; fail=1; }
grep -q 'spawnFloorHoleSnipers' "$f"             || { echo "MISSING: spawnFloorHoleSnipers"; fail=1; }
grep -q 'non-boss remap' "$f"                    || { echo "MISSING: non-boss remap"; fail=1; }
[ "$fail" -eq 0 ] && echo "PASS 6.3" || { echo "FAIL 6.3"; exit 1; }
```

- [ ] **Step 2: Run it, verify it fails**:
```bash
bash /home/aaron/game/scratchpad/check_6_3.sh
```
Expected: `MISSING: dropHoles record`, `MISSING: --fourstory dev door`, … then `FAIL 6.3` (exit 1).

- [ ] **Step 3: Minimal implementation** — two `Edit` calls on `/home/aaron/game/.claude/skills/engine-reference/SKILL.md`.

Edit A — insert the `FOUR_STORY` generator paragraph after the `CELL_PLATFORM` block. old_string:
```text
PvP-only. No wire/save change (the grid is seed-built, not serialized).

## Architecture deep-dive: split-screen & shared systems
```
new_string:
```text
PvP-only. No wire/save change (the grid is seed-built, not serialized).

**`FOUR_STORY` "Descent" (the multi-slab consumer).** A `carveFourStory` floor (`level_gen.cpp`, floor-6+
non-boss, forced 44-grid, `--fourstory` dev door) slabs every interior cell at 12/24/36 qu via `addPlatform`,
then punches **offset drop-holes** (`removePlatform`) in disjoint quadrant sets across ADJACENT levels
(L3→{NW,SE}, L2→{NE,SW}, L1→{NW,SE}) so a dive lands exactly one story down; holes are `>=2×2` (a 1-cell hole
lets the floor-snap re-grab a faller). Each hole is recorded in
`DungeonResult.dropHoles[MAX_DROP_HOLES=32]` (`DropHole{Vec3 pos; f32 surfaceY;}` + `dropHoleCount`) for
`spawnFloorHoleSnipers` (ranged defs seated at hole edges, plunge-firing down through their multi-slab LOS).
Spawn L3 (@9 m) / exit L0: `usesBalconyEndpoints(s)` (`s==VERTICAL_HALL || s==FOUR_STORY`) gates the three
`engine_startgame.cpp` sites (grid-force `>=44`, `spawnBalconyPos→spawnPos`, `exitBalconyPos→floorDoorPos`).
Per-level rooms at `floorHeight` 0/3/6/9 spread loot/enemies flatly — the `finalizeDungeon` adjacency test
gained `&& fabsf(ri.floorHeight-rj.floorHeight)<1.5f` so the four stacked same-XZ rooms aren't all mutually
adjacent (which would seed **zero** enemies). `pickLayoutStyle` adds a `FOUR_STORY` weight column + the
**non-boss remap** `if (style==FOUR_STORY && (floor<6 || floor%5==0)) style=BSP_ROOMS;` (mirrors
VERTICAL_HALL — the boss-arena expansion rewrites `floorHeight` + rebuilds the mesh, corrupting the stack).
Seed-built + GenRNG-only → **no wire/save change**; pinned by `tests/world/test_four_story.cpp`.

## Architecture deep-dive: split-screen & shared systems
```

Edit B — append `FOUR_STORY` to the seed→`pickLayoutStyle` bullet. old_string:
```text
via `LevelGen::pickLayoutStyle` — no wire traffic, and generation itself is transcendental-free (LCG + integer math only) so the carved grids are bit-identical across platforms.
```
new_string:
```text
via `LevelGen::pickLayoutStyle` — no wire traffic, and generation itself is transcendental-free (LCG + integer math only) so the carved grids are bit-identical across platforms. A sixth style, `FOUR_STORY` ("Descent" — four dead-stacked walkable stories on one footprint, drop-only via `>=2×2` slab holes, floor-6+ non-boss, `--fourstory`), rolls through the identical seed→`pickLayoutStyle` path.
```

- [ ] **Step 4: Run the tests, verify they pass**:
```bash
bash /home/aaron/game/scratchpad/check_6_3.sh && git -C /home/aaron/game diff --name-only
```
Expected: `PASS 6.3`, and `diff --name-only` lists ONLY `.claude/skills/engine-reference/SKILL.md` (docs-only → suite stays green).

- [ ] **Step 5: Commit**
```bash
cd /home/aaron/game
git add .claude/skills/engine-reference/SKILL.md
git commit -m "docs(engine-reference): FOUR_STORY generator, dropHoles, --fourstory dev door

Document the Descent generator: DungeonResult.dropHoles, offset quadrant-disjoint
holes, usesBalconyEndpoints spawn/exit, spawnFloorHoleSnipers, the finalize adjacency
fix and the non-boss remap; list FOUR_STORY as the sixth layout style.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6.4: engine-how-to — stacked-slab style recipe + multi-slab pitfalls

**Files:**
- Modify: `/home/aaron/game/.claude/skills/engine-how-to/SKILL.md:47` ("New level layout" recipe)
- Modify: `/home/aaron/game/.claude/skills/engine-how-to/SKILL.md:53` ("Two-story cells (`CELL_PLATFORM`)" pitfall)

- [ ] **Step 1: Write the failing test** — doc-presence assertion. Save as `/home/aaron/game/scratchpad/check_6_4.sh`:
```bash
#!/usr/bin/env bash
# Doc-presence "test" for Task 6.4: stacked-slab recipe + the four multi-slab pitfalls.
set -u
f=/home/aaron/game/.claude/skills/engine-how-to/SKILL.md
fail=0
grep -q 'stacked-slab' "$f"                       || { echo "MISSING: stacked-slab recipe"; fail=1; }
grep -q 'Drop-holes must be' "$f"                  || { echo "MISSING: holes >=2x2 pitfall"; fail=1; }
grep -q 'quadrant-disjoint' "$f"                   || { echo "MISSING: offset/quadrant-disjoint pitfall"; fail=1; }
grep -q 'non-boss remap' "$f"                      || { echo "MISSING: non-boss remap pitfall"; fail=1; }
grep -q 'a single-slab writer MUST use `setPlatform`' "$f" || { echo "MISSING: setPlatform pitfall"; fail=1; }
grep -q 'Multi-story cells' "$f"                   || { echo "MISSING: pitfall heading rename"; fail=1; }
[ "$fail" -eq 0 ] && echo "PASS 6.4" || { echo "FAIL 6.4"; exit 1; }
```

- [ ] **Step 2: Run it, verify it fails**:
```bash
bash /home/aaron/game/scratchpad/check_6_4.sh
```
Expected: `MISSING: stacked-slab recipe`, `MISSING: Drop-holes must be`, … then `FAIL 6.4` (exit 1).

- [ ] **Step 3: Minimal implementation** — two `Edit` calls on `/home/aaron/game/.claude/skills/engine-how-to/SKILL.md`.

Edit A — the "New level layout" recipe: bump the style count, add FOUR_STORY to the list, and add the stacked-slab clause to "Adding a style". old_string:
```text
**New level layout**: `LevelGen::generate` (`world/level_gen.cpp`) is the production path — pass a seed, grid dimensions, and a `LayoutStyle`. Five structural styles exist (`BSP_ROOMS` classic, `CAVERN` cellular-automata cave, `GAUNTLET` serpentine arena chain, `HUB` central chamber + spoke vaults + ring, `VERTICAL_HALL` the two-story "Stacked Loop"
```
new_string:
```text
**New level layout**: `LevelGen::generate` (`world/level_gen.cpp`) is the production path — pass a seed, grid dimensions, and a `LayoutStyle`. Six structural styles exist (`BSP_ROOMS` classic, `CAVERN` cellular-automata cave, `GAUNTLET` serpentine arena chain, `HUB` central chamber + spoke vaults + ring, `FOUR_STORY` the drop-only "Descent" (four dead-stacked walkable stories on one footprint via up to `MAX_PLATFORMS_PER_CELL`=3 stacked slabs; spawn top L3, exit ground L0, fall through `>=2×2` slab holes; `carveFourStory`, `--fourstory` dev door), `VERTICAL_HALL` the two-story "Stacked Loop"
```

Then a second `Edit` A2 on the SAME recipe line — extend "Adding a style" with the stacked-slab note. old_string:
```text
and let the shared `finalizeDungeon` handle spawn/exit/bbox-adjacency (pass forcedSpawn/forcedExit only for a style with a mandatory flow, like the gauntlet's start→end).
```
new_string:
```text
and let the shared `finalizeDungeon` handle spawn/exit/bbox-adjacency (pass forcedSpawn/forcedExit only for a style with a mandatory flow, like the gauntlet's start→end). **A stacked-slab (multi-story) style additionally**: slab cells with `LevelGridSystem::addPlatform` (accumulate, sorted-ascending — NEVER `setPlatform`, which replaces to one; NEVER a bare `cell.platHeight=q`, which leaves `platCount==0` so the slab vanishes), punch drops with `removePlatform`, record each hole in `DungeonResult.dropHoles`, set `room.floorHeight` per story so the room-keyed seeders spread content, add the style to `usesBalconyEndpoints` for L-top-spawn/L0-exit, gate `finalizeDungeon` adjacency on `fabsf(floorHeight difference)<1.5f`, and give `pickLayoutStyle` a non-boss remap.
```

Edit B — the "Two-story cells" pitfall: rename to "Multi-story cells" and append the four multi-slab rules. old_string:
```text
**Two-story cells (`CELL_PLATFORM`) — the rules that keep them honest.** (1) A platform cell must keep `CELL_FLOOR`
```
new_string:
```text
**Multi-story cells (`CELL_PLATFORM`) — the rules that keep them honest.** (1) A platform cell must keep `CELL_FLOOR`
```
Then a second `Edit` B2 on the SAME pitfall — append rules (6)-(9) after rule (5). old_string:
```text
(5) World-item ground snapping and enemy AI still read the base floor by design — don't put loot or PvE fights on platforms until those consumers are converted.
```
new_string:
```text
(5) World-item ground snapping and enemy AI still read the base floor by design — don't put loot or PvE fights on platforms until those consumers are converted. (6) **The right authorer** — a single-slab writer MUST use `setPlatform` (replace-to-one); a bare `cell.platHeight=q` now leaves `platCount==0` so `hasPlatform` returns false and the slab silently vanishes, and routing a single-slab junction through `addPlatform` (accumulate) fabricates a **phantom second slab** the determinism `memcmp` won't catch — `addPlatform`/`removePlatform` are `FOUR_STORY`-generator-only. (7) **Drop-holes must be `>=2×2`** — a 1-cell hole lets the floor-snap re-grab a faller onto a neighbour slab so they never fall. (8) **Offset holes per level, quadrant-disjoint across ADJACENT levels** (L3→{NW,SE}, L2→{NE,SW}, L1→{NW,SE}) with a `>=1`-cell quadrant margin, so a column never pierces two adjacent stories (an express shaft) — a dive always lands exactly one story down. (9) A stacked-slab PvE style needs the **non-boss remap** in `pickLayoutStyle` (`floor<6 || floor%5==0 → BSP_ROOMS`), or a boss floor rewrites `floorHeight` + rebuilds the mesh and corrupts the stack; and `finalizeDungeon` adjacency must gate on `fabsf(floorHeight difference)<1.5f` or the four stacked same-XZ rooms all mark mutually adjacent and `spawnFloorEnemies` seeds ZERO enemies.
```

- [ ] **Step 4: Run the tests, verify they pass** — the assertion is green, only the doc changed, and (docs are code-inert) the full suite stays green:
```bash
bash /home/aaron/game/scratchpad/check_6_4.sh && git -C /home/aaron/game diff --name-only
```
Expected: `PASS 6.4`, and `diff --name-only` lists ONLY `.claude/skills/engine-how-to/SKILL.md`. Optional confirmation that the code tree is untouched (all four Phase-6 commits are docs-only, so the previously-green `./build/tests/dungeon_tests` needs no rebuild): `git -C /home/aaron/game log --name-only --oneline -4` shows only `CLAUDE.md` / `.claude/skills/*/SKILL.md` paths — **full suite green** (no `src/` or `tests/` file changed across the phase).

- [ ] **Step 5: Commit**
```bash
cd /home/aaron/game
git add .claude/skills/engine-how-to/SKILL.md
git commit -m "docs(engine-how-to): stacked-slab style recipe + multi-slab pitfalls

Add the recipe for a stacked-slab layout (addPlatform/removePlatform, dropHoles,
per-story room.floorHeight, usesBalconyEndpoints, non-boss remap) and the four
pitfalls: setPlatform for single-slab writers, holes >=2x2, offset/quadrant-disjoint
holes, and the non-boss remap + finalize adjacency guard.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```