# Multi-Slab Tile Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Generalize the engine's walk-under `CELL_PLATFORM` slab from one slab per cell to up to three (four walkable stories on one footprint), with **zero gameplay change** and full test coverage.

**Architecture:** `GridCell` grows from scalar `platHeight`/`platMaterialId` to `platCount + platHeight[3] + platMaterialId[3]` (13 all-`u8` bytes; `calloc`'d per floor, never serialized). Every consumer funnels through the generalized `effectiveFloorHeight` (highest slab top at/below the feet, else the base floor), so collision landing/snap, both `snapEntityToFloor` twins, the head-clamp, raycast, and the mesher inherit 4-story footing with near-zero call-site churn. All existing single-slab writers migrate to a `setPlatform` (replace-to-single, byte-identical to the old scalar overwrite); a new `addPlatform` (accumulate) is reserved for the FOUR_STORY generator (built in the sibling plan).

**Tech Stack:** C++17, doctest, CMake. All-`u8` `GridCell` + seed-built geometry ⇒ **no `SAVE_VERSION` / no `PROTOCOL_VERSION` change.**

**Reference spec:** `docs/superpowers/specs/2026-07-20-four-story-descent-floors-design.md` (sections 3.1–3.7, 6, 8). Build the test binary with `cmake --build build --target dungeon_tests`; run `./build/tests/dungeon_tests`.

---

### Task 0.1: GridCell 13-byte multi-slab layout + `setPlatform` + generalized getters + migrate every single-slab writer

This is the atomic core of the foundation: growing `GridCell` from scalar `platHeight`/`platMaterialId` to `platCount`+`platHeight[3]`+`platMaterialId[3]` breaks every call site that touches those fields at compile time, so the struct change, the `setPlatform` authorer, the rewritten getters/`effectiveFloorHeight`, and **all six writer migrations** must land in one commit that restores green with zero behavior change.

**Files:**
- Test: `tests/world/test_multislab.cpp` (new)
- Modify: `tests/CMakeLists.txt:69` (add the new test to the `add_executable(dungeon_tests …)` list)
- Modify: `src/world/level_grid.h:43-54` (add `MAX_PLATFORMS_PER_CELL`, grow struct, add `sizeof` static_assert), `:94` (declare `setPlatform`)
- Modify: `src/world/level_grid.cpp:77-102` (rewrite `hasPlatform`/`getPlatformTop`/`getPlatformUnderside`/`effectiveFloorHeight`; add `setPlatform`)
- Modify (migrate writers): `src/world/level_gen.cpp:839`, `src/engine/engine_arena.cpp:153-157`, `src/world/level_mesh.cpp:353`, `tests/world/test_platform.cpp:38-42` + `:197`, `tests/game/test_teleport_dest.cpp:138-140`, `tests/world/test_story_nav.cpp:44-45`

- [ ] **Step 1: Write the failing test** — create `tests/world/test_multislab.cpp` against the not-yet-existing `setPlatform` API, and register it in CMake.

```cpp
// test_multislab.cpp — the multi-slab GridCell authorers/getters (Part A foundation for FOUR_STORY).
// setPlatform (replace-to-single) is the migration target for every legacy single-slab writer; it must
// leave exactly ONE slab and zero the trailing slots (canonical byte-form the determinism memcmp needs).
#include <doctest/doctest.h>
#include "world/level_grid.h"

TEST_CASE("setPlatform writes exactly one slab and zeroes the trailing slots") {
    GridCell c = {};
    LevelGridSystem::setPlatform(c, 12, 7);
    CHECK(c.platCount == 1);
    CHECK(c.platHeight[0] == 12);
    CHECK(c.platMaterialId[0] == 7);
    CHECK((c.flags & CELL_PLATFORM) != 0);
    CHECK(c.platHeight[1] == 0);        // canonical byte-form: slots >= platCount MUST be zero
    CHECK(c.platHeight[2] == 0);
    CHECK(c.platMaterialId[1] == 0);
    CHECK(c.platMaterialId[2] == 0);
}

TEST_CASE("setPlatform is last-write-wins: a second call replaces, never accumulates") {
    GridCell c = {};
    LevelGridSystem::setPlatform(c, 12, 3);
    LevelGridSystem::setPlatform(c, 24, 5);   // the overlapping-band junction pattern
    CHECK(c.platCount == 1);
    CHECK(c.platHeight[0] == 24);
    CHECK(c.platMaterialId[0] == 5);
    CHECK(c.platHeight[1] == 0);              // no phantom second slab
}

TEST_CASE("GridCell stays 13 all-u8 bytes (calloc'd per floor, never serialized)") {
    CHECK(sizeof(GridCell) == 13);
}
```

Add to `tests/CMakeLists.txt` immediately after line 69 (`world/test_story_nav.cpp`):

```cmake
    world/test_multislab.cpp         # multi-slab GridCell authorers/getters (FOUR_STORY foundation)
```

- [ ] **Step 2: Run it, verify it fails** — the test binary fails to **compile** (the API doesn't exist yet):

```bash
cmake --build build --target dungeon_tests
```

Expected: `error: 'setPlatform' is not a member of 'LevelGridSystem'` (from `tests/world/test_multislab.cpp`).

- [ ] **Step 3: Minimal implementation** — grow the struct + add the authorer + generalize the getters, then migrate every writer.

`src/world/level_grid.h` — after line 43 (`PLATFORM_STEP_TOLERANCE`) and before the struct, add the cap; then **replace** the struct's two scalar slab fields (lines 52-53) and append the static_assert:

```cpp
// Up to 3 walk-under slabs per cell → 4 walkable stories. platHeight[] is STRICTLY ASCENDING by top
// height; slots >= platCount MUST be zero (canonical byte-form: GridCell is calloc'd per floor and
// never serialized, so the test_level_gen determinism memcmp compares raw bytes). CELL_PLATFORM is
// set iff platCount > 0.
static constexpr u8 MAX_PLATFORMS_PER_CELL = 3;

struct GridCell {
    u8 flags;            // CELL_SOLID / CELL_FLOOR / CELL_CEILING / CELL_PLATFORM / ...
    u8 floorHeight;      // quarter-units (multiply by 0.25 for metres)
    u8 ceilingHeight;    // quarter-units
    u8 wallMaterialId;   // material for wall surfaces
    u8 floorMaterialId;  // material for floor surface
    u8 ceilMaterialId;   // material for ceiling surface
    u8 platCount;                              // number of slabs, 0..MAX_PLATFORMS_PER_CELL
    u8 platHeight[MAX_PLATFORMS_PER_CELL];     // slab TOP surfaces, quarter-units, STRICTLY ASCENDING
    u8 platMaterialId[MAX_PLATFORMS_PER_CELL]; // per-slab top + underside material
};
static_assert(sizeof(GridCell) == 13,
    "GridCell must stay 13 all-u8 bytes: calloc'd per floor, never serialized; a size change silently "
    "breaks the test_level_gen determinism memcmp and any future grid memcpy");
```

`src/world/level_grid.h` — in `namespace LevelGridSystem`, after the `getPlatformUnderside` declaration (line 94), declare the authorer:

```cpp
    // Slab AUTHORERS (write paths, operating on a cell reference). setPlatform = REPLACE-to-single:
    // every legacy single-slab writer routes here so its double-write junctions collapse to one slab
    // and shipped geometry stays byte-identical. Keeps the ascending + canonical-byte-form invariants.
    void setPlatform(GridCell& c, u8 topQ, u8 mat);
```

`src/world/level_grid.cpp` — replace `hasPlatform`/`getPlatformTop`/`getPlatformUnderside`/`effectiveFloorHeight` (lines 77-102) and append `setPlatform`:

```cpp
bool LevelGridSystem::hasPlatform(const LevelGrid& grid, u32 x, u32 z) {
    if (!isInBounds(grid, x, z)) return false;
    const GridCell& c = grid.cells[z * grid.width + x];
    // Invariant (b): CELL_PLATFORM set iff platCount>0. A solid cell can't carry a slab.
    return c.platCount > 0 && !(c.flags & CELL_SOLID);
}

f32 LevelGridSystem::getPlatformTop(const LevelGrid& grid, u32 x, u32 z) {
    const GridCell& c = grid.cells[z * grid.width + x];
    // Highest slab top (platHeight ascending). Caller gates on hasPlatform (platCount>0).
    return c.platHeight[c.platCount - 1] * 0.25f;
}

f32 LevelGridSystem::getPlatformUnderside(const LevelGrid& grid, u32 x, u32 z) {
    const GridCell& c = grid.cells[z * grid.width + x];
    // Lowest slab's underside, clamped up to the base floor (thin slabs degrade to riser geometry).
    const s32 underQ = static_cast<s32>(c.platHeight[0]) - PLATFORM_THICKNESS_Q;
    const f32 under  = underQ * 0.25f;
    const f32 fh     = c.floorHeight * 0.25f;
    return under > fh ? under : fh;
}

f32 LevelGridSystem::effectiveFloorHeight(const LevelGrid& grid, u32 x, u32 z, f32 feetY) {
    f32 best = getFloorHeight(grid, x, z);
    if (!hasPlatform(grid, x, z)) return best;
    const GridCell& c = grid.cells[z * grid.width + x];
    // Highest surface among {floor} ∪ {slab tops} at/below feet + step tolerance: a body over a hole
    // (missing that slab) resolves to the next intact surface below; a body on a slab stays on it.
    for (u8 i = 0; i < c.platCount; i++) {
        const f32 top = c.platHeight[i] * 0.25f;
        if (feetY >= top - PLATFORM_STEP_TOLERANCE && top > best) best = top;
    }
    return best;
}

void LevelGridSystem::setPlatform(GridCell& c, u8 topQ, u8 mat) {
    c.platCount         = 1;
    c.platHeight[0]     = topQ;
    c.platMaterialId[0] = mat;
    // Canonical byte-form: zero every slot >= platCount so logically-identical cells compare byte-equal.
    for (u8 i = 1; i < MAX_PLATFORMS_PER_CELL; i++) { c.platHeight[i] = 0; c.platMaterialId[i] = 0; }
    c.flags |= CELL_PLATFORM;   // OR-in: the cell keeps CELL_FLOOR as its ground story
}
```

Migrate the six single-slab writers (all behavior-preserving — `setPlatform` is byte-identical to the old scalar write):

`src/world/level_gen.cpp:839` (inside the `slab` lambda):
```cpp
        LevelGridSystem::setPlatform(c, q, floorMat);   // REPLACE-to-single (was: |=CELL_PLATFORM; platHeight=q; platMaterialId=floorMat)
```

`src/engine/engine_arena.cpp:153-157` (inside the `plat` lambda) — **keep** the sand/stone lines:
```cpp
                c.flags           = static_cast<u8>(CELL_FLOOR);
                LevelGridSystem::setPlatform(c, topQ, plank);   // authorer: platCount=1, sets CELL_PLATFORM
                c.floorMaterialId = sand;    // the arcade ground beneath stays sand
                c.wallMaterialId  = stone;
```

`src/world/level_mesh.cpp:353` — minimal field-access fix (the per-slab mesher loop is a later phase):
```cpp
                    MaterialBucket* pbkt = getBucket(cell.platMaterialId[0]);   // Phase 0: single-slab read
```

`tests/world/test_platform.cpp:38-42` (the `setPlat` method):
```cpp
    void setPlat(u32 x, u32 z, u8 topQ) {
        GridCell& c = grid.cells[z * 12 + x];
        c.flags     = static_cast<u8>(CELL_FLOOR);
        LevelGridSystem::setPlatform(c, topQ, 0);
    }
```

`tests/world/test_platform.cpp:197` (the causeway `plat` lambda):
```cpp
    auto plat  = [&](u32 x){ for (u32 z=1; z<=3; z++){ GridCell& c=g.cells[z*12+x]; c.flags=static_cast<u8>(CELL_FLOOR); LevelGridSystem::setPlatform(c, 12, 0); } };
```

`tests/game/test_teleport_dest.cpp:138-140`:
```cpp
            GridCell& c = g.cells[z * 12 + x];
            c.flags = static_cast<u8>(CELL_FLOOR);
            LevelGridSystem::setPlatform(c, 12, 0);
```

`tests/world/test_story_nav.cpp:44-45`:
```cpp
    c.flags = static_cast<u8>(CELL_FLOOR);
    LevelGridSystem::setPlatform(c, 10, 0);              // slab top 2.5 m
```

- [ ] **Step 4: Run the tests, verify they pass** — build the test binary, run the **full** suite, and build the **full game** (catches `level_mesh.cpp` + `engine_arena.cpp`):

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests && cmake --build build
```

Expected: `[doctest] Status: SUCCESS!` (all cases pass, including the pre-existing `test_platform` / `test_vertical_hall` / `test_story_nav` / `test_teleport_dest` — proving zero behavior change), and the game binary links.

- [ ] **Step 5: Commit** — on a feature branch:

```bash
git checkout -b feature/four-story-descent
git add src/world/level_grid.h src/world/level_grid.cpp src/world/level_gen.cpp \
        src/engine/engine_arena.cpp src/world/level_mesh.cpp \
        tests/world/test_multislab.cpp tests/CMakeLists.txt \
        tests/world/test_platform.cpp tests/game/test_teleport_dest.cpp tests/world/test_story_nav.cpp
git commit -m "$(cat <<'EOF'
feat(world): multi-slab GridCell foundation + setPlatform, migrate single-slab writers

Grow GridCell to platCount + platHeight[3] + platMaterialId[3] (13 all-u8 bytes,
sizeof static_assert), add the setPlatform replace-to-single authorer, and
generalize effectiveFloorHeight/getPlatformTop/getPlatformUnderside/hasPlatform to
the array form. Migrate every existing single-slab writer (VERTICAL_HALL slab lambda,
arena plat lambda, mesher read, 4 test writers) to setPlatform so shipped geometry
stays byte-identical. Zero behavior change; full suite + game build green.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 0.2: `addPlatform` (accumulate) + `platformCount` + indexed getters + 3-slab `effectiveFloorHeight`

**Files:**
- Test: `tests/world/test_multislab.cpp` (extend)
- Modify: `src/world/level_grid.h:94` (declare `addPlatform`, `platformCount`, indexed `getPlatformTop`/`getPlatformUnderside` overloads)
- Modify: `src/world/level_grid.cpp` (add `addPlatform`, `platformCount`, indexed getters; make the no-index getters thin wrappers)

- [ ] **Step 1: Write the failing test** — append to `tests/world/test_multislab.cpp`:

```cpp
TEST_CASE("addPlatform keeps tops strictly ascending even when inserted out of order") {
    GridCell c = {};
    LevelGridSystem::addPlatform(c, 24, 2);
    LevelGridSystem::addPlatform(c, 12, 1);   // lower, added second → must sort in front
    LevelGridSystem::addPlatform(c, 36, 3);
    CHECK(c.platCount == 3);
    CHECK(c.platHeight[0] == 12);
    CHECK(c.platHeight[1] == 24);
    CHECK(c.platHeight[2] == 36);
    CHECK(c.platMaterialId[0] == 1);
    CHECK(c.platMaterialId[1] == 2);
    CHECK(c.platMaterialId[2] == 3);
}

TEST_CASE("addPlatform de-dups a repeated top by overwriting its material, no second entry") {
    GridCell c = {};
    LevelGridSystem::addPlatform(c, 12, 1);
    LevelGridSystem::addPlatform(c, 12, 9);   // same top → overwrite material only
    CHECK(c.platCount == 1);
    CHECK(c.platMaterialId[0] == 9);
}

TEST_CASE("addPlatform is a no-op once the cell holds MAX_PLATFORMS_PER_CELL slabs") {
    GridCell c = {};
    LevelGridSystem::addPlatform(c, 12, 1);
    LevelGridSystem::addPlatform(c, 24, 2);
    LevelGridSystem::addPlatform(c, 36, 3);
    LevelGridSystem::addPlatform(c, 44, 4);   // full → dropped, never writes slot >= platCount
    CHECK(c.platCount == 3);
    CHECK(c.platHeight[2] == 36);
}

TEST_CASE("effectiveFloorHeight over a 3-slab stack picks the highest surface at/below the feet") {
    LevelGrid g;
    LevelGridSystem::init(g, 3, 3, 1.0f);
    for (u32 i = 0; i < 9; i++) { g.cells[i].flags = CELL_FLOOR; g.cells[i].ceilingHeight = 60; }
    GridCell& c = LevelGridSystem::getCell(g, 1, 1);
    c.flags = CELL_FLOOR;
    LevelGridSystem::addPlatform(c, 12, 0);   // top 3 m
    LevelGridSystem::addPlatform(c, 24, 0);   // top 6 m
    LevelGridSystem::addPlatform(c, 36, 0);   // top 9 m
    CHECK(LevelGridSystem::platformCount(g, 1, 1) == 3);
    CHECK(LevelGridSystem::effectiveFloorHeight(g, 1, 1, 9.0f) == doctest::Approx(9.0f));
    CHECK(LevelGridSystem::effectiveFloorHeight(g, 1, 1, 8.9f) == doctest::Approx(9.0f)); // step band
    CHECK(LevelGridSystem::effectiveFloorHeight(g, 1, 1, 8.5f) == doctest::Approx(6.0f));
    CHECK(LevelGridSystem::effectiveFloorHeight(g, 1, 1, 6.0f) == doctest::Approx(6.0f));
    CHECK(LevelGridSystem::effectiveFloorHeight(g, 1, 1, 5.5f) == doctest::Approx(3.0f));
    CHECK(LevelGridSystem::effectiveFloorHeight(g, 1, 1, 3.0f) == doctest::Approx(3.0f));
    CHECK(LevelGridSystem::effectiveFloorHeight(g, 1, 1, 0.0f) == doctest::Approx(0.0f));
    // Indexed getters expose each slab; underside clamps DOWN to the next-lower surface.
    CHECK(LevelGridSystem::getPlatformTop(g, 1, 1, 1) == doctest::Approx(6.0f));
    CHECK(LevelGridSystem::getPlatformUnderside(g, 1, 1, 1) == doctest::Approx(5.5f)); // 24-2=22 qu → 5.5, > prev top 3.0
    CHECK(LevelGridSystem::getPlatformUnderside(g, 1, 1, 0) == doctest::Approx(2.5f)); // 12-2=10 qu → 2.5, > floor 0
    LevelGridSystem::shutdown(g);
}
```

- [ ] **Step 2: Run it, verify it fails**:

```bash
cmake --build build --target dungeon_tests
```

Expected: `error: 'addPlatform' is not a member of 'LevelGridSystem'` (and `platformCount` / the indexed `getPlatformTop(…, u8)` overload).

- [ ] **Step 3: Minimal implementation** — declare in `src/world/level_grid.h` (after the `setPlatform` declaration from Task 0.1):

```cpp
    void addPlatform(GridCell& c, u8 topQ, u8 mat);   // ACCUMULATE (FOUR_STORY generator only)
    u8   platformCount(const LevelGrid& grid, u32 x, u32 z);              // slab count; multi-slab loop bound
    f32  getPlatformTop(const LevelGrid& grid, u32 x, u32 z, u8 i);       // indexed slab top
    f32  getPlatformUnderside(const LevelGrid& grid, u32 x, u32 z, u8 i); // indexed slab underside
```

Add to `src/world/level_grid.cpp` (after `setPlatform`), and simplify the no-index getters to thin wrappers:

```cpp
void LevelGridSystem::addPlatform(GridCell& c, u8 topQ, u8 mat) {
    // De-dup: a slab already at topQ just overwrites its material (no second entry).
    for (u8 i = 0; i < c.platCount; i++)
        if (c.platHeight[i] == topQ) { c.platMaterialId[i] = mat; return; }
    if (c.platCount >= MAX_PLATFORMS_PER_CELL) return;   // full → no-op (never writes slot >= platCount)
    // Sorted insert, keeping platHeight[] strictly ascending.
    u8 ins = c.platCount;
    while (ins > 0 && c.platHeight[ins - 1] > topQ) {
        c.platHeight[ins]     = c.platHeight[ins - 1];
        c.platMaterialId[ins] = c.platMaterialId[ins - 1];
        ins--;
    }
    c.platHeight[ins]     = topQ;
    c.platMaterialId[ins] = mat;
    c.platCount++;
    c.flags |= CELL_PLATFORM;
    // Invariant (d): same-cell slab tops must differ by > PLATFORM_STEP_TOLERANCE, else a body resting
    // on the lower slab teleport-snaps up onto the higher via effectiveFloorHeight (debug-only guard).
    for (u8 i = 1; i < c.platCount; i++)
        ENGINE_ASSERT((c.platHeight[i] - c.platHeight[i - 1]) * 0.25f > PLATFORM_STEP_TOLERANCE,
                      "addPlatform: same-cell slab tops within step tolerance");
}

u8 LevelGridSystem::platformCount(const LevelGrid& grid, u32 x, u32 z) {
    if (!isInBounds(grid, x, z)) return 0;
    return grid.cells[z * grid.width + x].platCount;
}

f32 LevelGridSystem::getPlatformTop(const LevelGrid& grid, u32 x, u32 z, u8 i) {
    return grid.cells[z * grid.width + x].platHeight[i] * 0.25f;
}

f32 LevelGridSystem::getPlatformUnderside(const LevelGrid& grid, u32 x, u32 z, u8 i) {
    const GridCell& c = grid.cells[z * grid.width + x];
    const s32 underQ = static_cast<s32>(c.platHeight[i]) - PLATFORM_THICKNESS_Q;
    // Clamp DOWN to the next-lower surface (previous slab top, else the base floor) so a lower slab's
    // thickness band can't poke into an upper slab's underside.
    const s32 floorQ = (i > 0) ? static_cast<s32>(c.platHeight[i - 1]) : static_cast<s32>(c.floorHeight);
    return (underQ > floorQ ? underQ : floorQ) * 0.25f;
}
```

Then replace the two no-index getters (added in Task 0.1) with thin wrappers per the spec:

```cpp
f32 LevelGridSystem::getPlatformTop(const LevelGrid& grid, u32 x, u32 z) {
    const GridCell& c = grid.cells[z * grid.width + x];
    return getPlatformTop(grid, x, z, static_cast<u8>(c.platCount - 1));   // highest slab
}

f32 LevelGridSystem::getPlatformUnderside(const LevelGrid& grid, u32 x, u32 z) {
    return getPlatformUnderside(grid, x, z, 0);                            // lowest slab
}
```

- [ ] **Step 4: Run the tests, verify they pass**:

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*addPlatform*,*3-slab stack*" && ./build/tests/dungeon_tests
```

Expected: the filtered `addPlatform` / 3-slab cases pass, then the **full suite** reports `[doctest] Status: SUCCESS!` (the no-index wrapper refactor leaves `test_platform`'s 2.5/0.0 underside expectations byte-identical).

- [ ] **Step 5: Commit**:

```bash
git add src/world/level_grid.h src/world/level_grid.cpp tests/world/test_multislab.cpp
git commit -m "$(cat <<'EOF'
feat(world): addPlatform accumulate authorer + platformCount + indexed slab getters

Sorted-insert addPlatform (ascending, de-dup, no-op at MAX_PLATFORMS_PER_CELL, invariant-d
debug guard) reserved for the FOUR_STORY generator, plus platformCount and the indexed
getPlatformTop/getPlatformUnderside overloads (underside clamped to the next-lower surface).
effectiveFloorHeight now resolves a 3-slab stack. No-index getters become thin wrappers.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 0.3: `removePlatform` — the build-time hole puncher

**Files:**
- Test: `tests/world/test_multislab.cpp` (extend)
- Modify: `src/world/level_grid.h` (declare `removePlatform`)
- Modify: `src/world/level_grid.cpp` (add `removePlatform`)

- [ ] **Step 1: Write the failing test** — append to `tests/world/test_multislab.cpp`:

```cpp
TEST_CASE("removePlatform punches the middle slab, shifts higher down, zeroes the vacated slot") {
    GridCell c = {};
    LevelGridSystem::addPlatform(c, 12, 1);
    LevelGridSystem::addPlatform(c, 24, 2);
    LevelGridSystem::addPlatform(c, 36, 3);
    LevelGridSystem::removePlatform(c, 24);        // punch the L2 slab (the drop-hole puncher)
    CHECK(c.platCount == 2);
    CHECK(c.platHeight[0] == 12);
    CHECK(c.platHeight[1] == 36);                  // 36 shifted down into slot 1, order preserved
    CHECK(c.platMaterialId[1] == 3);
    CHECK(c.platHeight[2] == 0);                   // vacated top slot zeroed (canonical byte-form)
    CHECK(c.platMaterialId[2] == 0);
    CHECK((c.flags & CELL_PLATFORM) != 0);         // still has slabs → flag stays set
}

TEST_CASE("removePlatform of the last slab clears the CELL_PLATFORM flag") {
    GridCell c = {};
    LevelGridSystem::setPlatform(c, 12, 1);
    LevelGridSystem::removePlatform(c, 12);
    CHECK(c.platCount == 0);
    CHECK(c.platHeight[0] == 0);
    CHECK((c.flags & CELL_PLATFORM) == 0);         // flag cleared only at count 0
}

TEST_CASE("removePlatform is a no-op when no slab matches the given top") {
    GridCell c = {};
    LevelGridSystem::addPlatform(c, 12, 1);
    LevelGridSystem::addPlatform(c, 24, 2);
    LevelGridSystem::removePlatform(c, 99);        // nothing at 99 qu
    CHECK(c.platCount == 2);
    CHECK(c.platHeight[0] == 12);
    CHECK(c.platHeight[1] == 24);
}
```

- [ ] **Step 2: Run it, verify it fails**:

```bash
cmake --build build --target dungeon_tests
```

Expected: `error: 'removePlatform' is not a member of 'LevelGridSystem'`.

- [ ] **Step 3: Minimal implementation** — declare in `src/world/level_grid.h` (after `addPlatform`):

```cpp
    void removePlatform(GridCell& c, u8 topQ);   // build-time hole puncher (drop-holes)
```

Add to `src/world/level_grid.cpp` (after `addPlatform`):

```cpp
void LevelGridSystem::removePlatform(GridCell& c, u8 topQ) {
    // Find the slab at topQ; no-op if none matches.
    u8 idx = 0;
    while (idx < c.platCount && c.platHeight[idx] != topQ) idx++;
    if (idx >= c.platCount) return;
    // Shift strictly-higher entries down one index (preserves ascending order).
    for (u8 i = idx; i + 1 < c.platCount; i++) {
        c.platHeight[i]     = c.platHeight[i + 1];
        c.platMaterialId[i] = c.platMaterialId[i + 1];
    }
    c.platCount--;
    // Zero the vacated top slot (canonical byte-form: slots >= platCount MUST be zero).
    c.platHeight[c.platCount]     = 0;
    c.platMaterialId[c.platCount] = 0;
    if (c.platCount == 0) c.flags &= ~CELL_PLATFORM;   // clear the flag only when the last slab is gone
}
```

- [ ] **Step 4: Run the tests, verify they pass**:

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*removePlatform*" && ./build/tests/dungeon_tests
```

Expected: the three `removePlatform` cases pass, then the full suite reports `[doctest] Status: SUCCESS!`.

- [ ] **Step 5: Commit**:

```bash
git add src/world/level_grid.h src/world/level_grid.cpp tests/world/test_multislab.cpp
git commit -m "$(cat <<'EOF'
feat(world): removePlatform build-time hole puncher

Finds the slab at a given top, shifts strictly-higher entries down (preserving ascending
order), decrements platCount, zeroes the vacated top slot (canonical byte-form), and clears
CELL_PLATFORM only at count 0. No-op when no slab matches. Completes the Part A authorer set.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 0.4: Migration regression — `platCount <= 1` on generated VERTICAL_HALL + the arena double-write junction

Locks the §3.7 migration: because every legacy writer now routes through `setPlatform` (replace), no shipped cell may ever carry more than one slab. An accidental `addPlatform` (accumulate) at an overlapping-band junction would fabricate a phantom second slab that the determinism `memcmp` **cannot** catch (still deterministic) — this test is the only thing that catches it.

**Files:**
- Test: `tests/world/test_vertical_hall.cpp` (extend — already linked with `level_gen.cpp`/`level_grid.cpp`)

- [ ] **Step 1: Write the failing test** — append to `tests/world/test_vertical_hall.cpp`:

```cpp
// Migration guard (Part A foundation, spec 3.7): every legacy single-slab writer routes through
// setPlatform (replace-to-single), so no shipped cell may carry more than one slab. The determinism
// memcmp can't catch an accidental addPlatform (accumulate) — this test is the guard that can.
TEST_CASE("Migration: generated VERTICAL_HALL cells never carry more than one slab") {
    for (u32 seed = 1; seed <= 16; seed++) {
        LevelGrid g;
        LevelGridSystem::init(g, 44, 44, 1.0f);
        LevelGen::generate(g, seed, 44, 44, LevelGen::LayoutStyle::VERTICAL_HALL);
        for (u32 i = 0; i < g.width * g.depth; i++)
            CHECK(g.cells[i].platCount <= 1);
        LevelGridSystem::shutdown(g);
    }
}

TEST_CASE("Migration: setPlatform collapses an overlapping-band double-write to one slab (arena pattern)") {
    // The arena writes a perimeter band at qu12, then a corner ramp/stairwell overwrites the same cell
    // at another height — the junction that would fabricate a phantom slab under addPlatform.
    GridCell c = {};
    c.flags = CELL_FLOOR;
    LevelGridSystem::setPlatform(c, 12, 4);   // perimeter band
    LevelGridSystem::setPlatform(c, 8, 4);    // ramp/stairwell overwrites the junction cell
    CHECK(c.platCount == 1);
    CHECK(c.platHeight[0] == 8);              // last write wins — no accumulation
    CHECK(c.platHeight[1] == 0);
}
```

- [ ] **Step 2: Run it, verify it fails** — this is a regression **lock**: on the migrated tree it passes immediately (Tasks 0.1-0.3 already routed every writer through `setPlatform`). Prove the guard is not vacuous by temporarily accumulating instead of replacing, watching it fail, then reverting:

```bash
# 1. Confirm it currently passes on the migrated tree:
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Migration*"
# 2. Prove the guard bites — temporarily break the arena-pattern case:
#    in tests/world/test_vertical_hall.cpp change the second call to:  LevelGridSystem::addPlatform(c, 8, 4);
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*arena pattern*"
```

Expected: step 1 passes; step 2 **fails** with `CHECK( c.platCount == 1 )` reporting `2` (accumulate fabricated the phantom slab). **Revert** the `addPlatform` edit back to `setPlatform(c, 8, 4)` before continuing.

- [ ] **Step 3: Minimal implementation** — none required; the migration in Tasks 0.1-0.3 already satisfies the guard. (This task adds only the regression lock.)

- [ ] **Step 4: Run the tests, verify they pass** — with the revert in place, run the guard and the full suite:

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Migration*" && ./build/tests/dungeon_tests
```

Expected: both `Migration` cases pass, the pre-existing `VERTICAL_HALL generates a valid two-story floor` case is unchanged, and the full suite reports `[doctest] Status: SUCCESS!` — Foundation is green with zero behavior change before any 4-story code exists. (The arena grid is built by the Engine-scoped `Engine::buildArenaLevel`, not reachable from the test binary; its `platCount<=1` property is guaranteed by the `setPlatform` migration in Task 0.1 + the arena double-write junction case here, and is spot-checked at runtime in the Phase 5 `--arena` playtest.)

- [ ] **Step 5: Commit**:

```bash
git add tests/world/test_vertical_hall.cpp
git commit -m "$(cat <<'EOF'
test(world): platCount<=1 migration regression on VERTICAL_HALL + arena junction

Locks spec 3.7: every legacy single-slab writer routes through setPlatform (replace), so no
generated VERTICAL_HALL cell carries >1 slab, and an overlapping-band double-write collapses
to a single slab (the arena perimeter-band/corner-ramp junction). Guards against a future
addPlatform misroute the determinism memcmp cannot catch.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

### Task 1.1: `overlapsPlatformBand` — loop every slab (multi-slab band block)

**Files:**
- Test: `/home/aaron/game/tests/world/test_platform.cpp` (add the `StackedRoom` helper in the anon namespace after `walk()` ~line 53; add the TEST_CASE after line 242)
- Modify: `/home/aaron/game/src/world/collision.cpp:105-109` (the `top`/`under`/`if` inside `Collision::overlapsPlatformBand`, lines 96-113)

- [ ] **Step 1: Write the failing test** — add the shared `StackedRoom` helper (used by every Phase‑1 test) plus the band test. `StackedRoom` builds a 12×12 room whose interior cells are full 4‑story stacks via the Phase‑0 authorer `addPlatform`, and punches per‑level holes with `removePlatform`.

```cpp
// --- add to the anonymous namespace, right after walk() (~line 53) --------------------------
// A 12x12 open room (1 m cells, solid border, floor y=0) where every interior cell is a full
// FOUR-STORY Descent stack: slabs at 12/24/36 qu (tops 3/6/9 m) via addPlatform, so a cell carries
// platCount==3 {12,24,36} (undersides 2.5/5.5/8.5). Holes are punched per level with removePlatform.
struct StackedRoom {
    LevelGrid grid;
    StackedRoom() {
        LevelGridSystem::init(grid, 12, 12, 1.0f);
        for (u32 z = 0; z < 12; z++)
            for (u32 x = 0; x < 12; x++) {
                GridCell& c = grid.cells[z * 12 + x];
                const bool border = (x == 0 || z == 0 || x == 11 || z == 11);
                c.flags         = border ? CELL_SOLID : CELL_FLOOR;
                c.floorHeight   = 0;
                c.ceilingHeight = 48;   // FS_CEIL_Q — clears L3 @ 9 m + a 1.8 m body
                if (!border) {
                    LevelGridSystem::addPlatform(c, 12, 1);   // L1 top 3 m
                    LevelGridSystem::addPlatform(c, 24, 1);   // L2 top 6 m
                    LevelGridSystem::addPlatform(c, 36, 1);   // L3 top 9 m
                }
            }
    }
    ~StackedRoom() { LevelGridSystem::shutdown(grid); }

    // Punch a hole at slab-top `topQ` over the inclusive cell rect [x0,x1]x[z0,z1].
    void punch(u32 x0, u32 z0, u32 x1, u32 z1, u8 topQ) {
        for (u32 z = z0; z <= z1; z++)
            for (u32 x = x0; x <= x1; x++)
                LevelGridSystem::removePlatform(grid.cells[z * 12 + x], topQ);
    }
};
```

```cpp
// --- add at the end of the file (after line 242) --------------------------------------------
TEST_CASE("Descent band: overlapsPlatformBand tests every slab, not a phantom full-height band") {
    StackedRoom room;               // interior cells all {12,24,36}: tops 3/6/9, undersides 2.5/5.5/8.5
    const f32 hw = PLAYER_HALF_WIDTH;
    // In the arcade (head 1.8 clears the L1 underside 2.5) → fits fully beneath, no clip
    CHECK_FALSE(Collision::overlapsPlatformBand({6.0f, 0.0f, 6.0f}, hw, room.grid));
    // Feet 1.0 (head 2.8) pokes into the L1 band [2.5,3.0] → blocked
    CHECK(Collision::overlapsPlatformBand({6.0f, 1.0f, 6.0f}, hw, room.grid));
    // Feet 3.0 standing ON L1, head 4.8 clear of the L2 underside 5.5 → must PASS. The old single-slab
    // read (highest top 9.0 + lowest underside 2.5) was a phantom full-height band that wrongly blocked
    // this — the discriminator for the multi-slab loop.
    CHECK_FALSE(Collision::overlapsPlatformBand({6.0f, 3.0f, 6.0f}, hw, room.grid));
    // Feet 4.0 (head 5.8) pokes into the L2 band [5.5,6.0] → blocked
    CHECK(Collision::overlapsPlatformBand({6.0f, 4.0f, 6.0f}, hw, room.grid));
}
```

- [ ] **Step 2: Run it, verify it fails**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Descent band*"
```

Expected: RED at the feet‑3.0 assertion — the old `overlapsPlatformBand` uses the no‑index top (highest = 9.0) with the no‑index underside (lowest = 2.5), a phantom `[2.5, 9.0]` band that blocks a body standing on L1:
```
TEST CASE:  Descent band: overlapsPlatformBand tests every slab, not a phantom full-height band
  CHECK_FALSE( Collision::overlapsPlatformBand({6.0f, 3.0f, 6.0f}, hw, room.grid) ) is NOT correct!
  values: CHECK_FALSE( true )
```

- [ ] **Step 3: Minimal implementation** — replace the single-slab top/under read (collision.cpp lines 105-109) with a per‑slab loop bounded by `platformCount`, using the indexed getters:

```cpp
            if (!LevelGridSystem::hasPlatform(grid, (u32)cx, (u32)cz)) continue;
            // Test EVERY slab this cell carries (up to 3 for a Descent stack): block the XZ step if the
            // body clips ANY band — neither stepping onto that slab's top nor passing under its
            // underside. The old single-slab read used the highest top + lowest underside, a phantom
            // full-height band that wrongly blocked a body standing on a lower slab.
            const u8 n = LevelGridSystem::platformCount(grid, (u32)cx, (u32)cz);
            for (u8 i = 0; i < n; i++) {
                const f32 top   = LevelGridSystem::getPlatformTop(grid, (u32)cx, (u32)cz, i);
                const f32 under = LevelGridSystem::getPlatformUnderside(grid, (u32)cx, (u32)cz, i);
                if (feetPos.y < top - STEP_UP_HEIGHT &&                 // not stepping onto it
                    feetPos.y + PLAYER_HEIGHT > under + 0.001f)          // and poking into the band
                    return true;
            }
```

(This replaces the old `const f32 top = ...getPlatformTop(grid,(u32)cx,(u32)cz);` / `const f32 under = ...getPlatformUnderside(...);` / `if (...) return true;` block; the surrounding `for (cz)`/`for (cx)` loops and the leading `if (!hasPlatform) continue;` stay.)

- [ ] **Step 4: Run the tests, verify they pass**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Descent band*" && ./build/tests/dungeon_tests
```

Expected: `Descent band` GREEN, and the full suite green (the single‑slab `test_platform` cases — `BalconyRoom`, `3 m CELL_LEDGE causeway` — still pass because their cells have `platCount==1`, so the new loop runs one iteration and is byte‑identical to the old scalar read).

- [ ] **Step 5: Commit**

```bash
git add /home/aaron/game/src/world/collision.cpp /home/aaron/game/tests/world/test_platform.cpp
git commit -m "$(cat <<'EOF'
feat(collision): overlapsPlatformBand loops all slabs (multi-story Descent)

Block the XZ step if the body clips ANY slab band in the cell, not the
phantom [lowest-underside, highest-top] band the single-slab read emitted
(which wrongly walled a body standing on a lower slab). Adds the StackedRoom
test fixture (full 12/24/36 stacks via addPlatform) shared by the Phase-1
collision tests.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 1.2: Under-slab head clamp — running MIN over slabs, in BOTH `moveAndSlide` overloads

**Files:**
- Test: `/home/aaron/game/tests/world/test_platform.cpp` (append two TEST_CASEs after Task 1.1's)
- Modify: `/home/aaron/game/src/world/collision.cpp:223-233` (grid‑only overload head clamp) **and** `/home/aaron/game/src/world/collision.cpp:358-368` (entity‑obstacle overload head clamp) — the two blocks are byte‑identical, so one `replace_all` edit covers both and they can't diverge

- [ ] **Step 1: Write the failing test** — one TEST_CASE per overload. A body standing on L1 (feet 3.0) launched upward must bonk the L2 underside (5.5) and never pop through onto L3 (8.5). The arcade subcase (feet 0 → L1 underside 2.5) is the design‑stated behavior and a regression guard.

```cpp
// --- append after the band test -------------------------------------------------------------
TEST_CASE("Descent head-clamp: running-min under a 3-slab stack (grid overload)") {
    StackedRoom room;   // interior cells all {12,24,36}: undersides 2.5 / 5.5 / 8.5

    SUBCASE("from the arcade → bonks the L1 underside (2.5), never pops onto L2") {
        Player p;
        p.position = {6.0f, 0.0f, 6.0f};   // feet on the ground floor, under L1
        p.velocity = {0.0f, 30.0f, 0.0f};  // over-strong launch: proves running-min, not last-wins
        p.onGround = false;
        f32 maxHead = 0.0f;
        for (int i = 0; i < 120; i++) {
            Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
            maxHead = std::max(maxHead, p.position.y + PLAYER_HEIGHT);
        }
        CHECK(maxHead <= 2.5f + 0.001f);                // clamped at the LOWEST underside (L1)
        CHECK(p.position.y == doctest::Approx(0.0f));   // fell back to the ground story
        CHECK(p.onGround);
    }
    SUBCASE("standing on L1 → bonks the L2 underside (5.5), never pops onto L3") {
        Player p;
        p.position = {6.0f, 3.0f, 6.0f};   // feet on L1 (top 3.0), under L2 (underside 5.5)
        p.velocity = {0.0f, 30.0f, 0.0f};
        p.onGround = false;
        f32 maxHead = 0.0f;
        for (int i = 0; i < 120; i++) {
            Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
            maxHead = std::max(maxHead, p.position.y + PLAYER_HEIGHT);
        }
        CHECK(maxHead <= 5.5f + 0.001f);                // bonked L2, never popped onto L3 (8.5)
        CHECK(p.position.y == doctest::Approx(3.0f));   // fell back onto L1
        CHECK(p.onGround);
    }
}

TEST_CASE("Descent head-clamp: running-min under a 3-slab stack (entity-obstacle overload)") {
    StackedRoom room;
    Player p;
    p.position = {6.0f, 3.0f, 6.0f};   // feet on L1, under L2 (underside 5.5)
    p.velocity = {0.0f, 30.0f, 0.0f};
    p.onGround = false;
    f32 maxHead = 0.0f;
    for (int i = 0; i < 120; i++) {
        Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f, nullptr, 0);  // the 5-arg entity overload
        maxHead = std::max(maxHead, p.position.y + PLAYER_HEIGHT);
    }
    CHECK(maxHead <= 5.5f + 0.001f);                // same clamp holds in the entity-obstacle overload
    CHECK(p.position.y == doctest::Approx(3.0f));
    CHECK(p.onGround);
}
```

- [ ] **Step 2: Run it, verify it fails**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Descent head-clamp*"
```

Expected: RED. The old clamp reads only the no‑index underside (i=0 = 2.5); the "started fully below" gate is `feet+1.8 <= 2.5`, which is false for a body on L1 (feet 3.0), so it applies NO clamp and the body flies to its ballistic apex (feet 3.0 + 30²/80 ≈ 14.25, head ≈ 16.0):
```
TEST CASE:  Descent head-clamp: running-min under a 3-slab stack (grid overload)
  SUBCASE: standing on L1 → bonks the L2 underside (5.5), never pops onto L3
  CHECK( maxHead <= 5.5f + 0.001f ) is NOT correct!
  values: CHECK( 16.05 <= 5.501 )
```
(The "from the arcade" subcase passes; the entity‑obstacle TEST_CASE fails identically.)

- [ ] **Step 3: Minimal implementation** — replace the single‑underside clamp with a per‑slab loop that clamps to the **minimum** qualifying `underside[i] - PLAYER_HEIGHT` among the slabs the body started fully below. The old block appears verbatim in both overloads, so use `replace_all: true` — one edit, both overloads, no divergence.

Old block (both sites):
```cpp
                    if (!LevelGridSystem::hasPlatform(grid, (u32)cx, (u32)cz)) continue;
                    const f32 under = LevelGridSystem::getPlatformUnderside(grid, (u32)cx, (u32)cz);
                    if (preFeetY + PLAYER_HEIGHT <= under + 0.001f &&   // started below the slab
                        tryPos.y + PLAYER_HEIGHT > under) {             // would poke into it
                        tryPos.y          = under - PLAYER_HEIGHT;
                        player.velocity.y = 0.0f;
                    }
```

New block (replaces both):
```cpp
                    if (!LevelGridSystem::hasPlatform(grid, (u32)cx, (u32)cz)) continue;
                    // Multi-slab (up to a 4-story Descent stack): clamp to the LOWEST qualifying
                    // underside among the slabs the body STARTED fully below — a running MIN, not
                    // last-wins — so a body under L1 bonks L1's floor and never pops up into an L2/L3
                    // band above it.
                    const u8 n = LevelGridSystem::platformCount(grid, (u32)cx, (u32)cz);
                    for (u8 i = 0; i < n; i++) {
                        const f32 under = LevelGridSystem::getPlatformUnderside(grid, (u32)cx, (u32)cz, i);
                        if (preFeetY + PLAYER_HEIGHT <= under + 0.001f &&   // started below this slab
                            tryPos.y + PLAYER_HEIGHT > under) {             // would poke into it
                            const f32 clampY = under - PLAYER_HEIGHT;
                            if (clampY < tryPos.y) tryPos.y = clampY;       // running MIN across slabs
                            player.velocity.y = 0.0f;
                        }
                    }
```

Leave the floor‑snap **UP** loop (`effectiveFloorHeight(..., preFeetY)`) in both overloads untouched — it is the velocity‑agnostic landing path and Task 1.3 pins it.

- [ ] **Step 4: Run the tests, verify they pass**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Descent head-clamp*" && ./build/tests/dungeon_tests
```

Expected: both head‑clamp TEST_CASEs GREEN, and the full suite green — including `test_platform`'s single‑slab "rising under the balcony bonks the underside" case (its cell has `platCount==1`, so the loop runs once and clamps at 2.5 exactly as before).

- [ ] **Step 5: Commit**

```bash
git add /home/aaron/game/src/world/collision.cpp /home/aaron/game/tests/world/test_platform.cpp
git commit -m "$(cat <<'EOF'
feat(collision): running-min under-slab head clamp for multi-story stacks

Clamp a rising body to the LOWEST qualifying slab underside among the slabs
it started fully below (running MIN, not last-wins), so a body under L1 bonks
L1 and never pops through onto L2/L3. Applied identically to both moveAndSlide
overloads (single replace_all — they can't diverge).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 1.3: Velocity-agnostic landing — free-fall / stacked-drop / sticky-lip / high-velocity regression guards

**Files:**
- Test: `/home/aaron/game/tests/world/test_platform.cpp` (append four TEST_CASEs; uses the `StackedRoom` helper and `walk()` already present)

No production code changes — this task pins that the floor‑snap **UP** loop stays keyed on `preFeetY` and velocity‑agnostic (the landing mechanism, unchanged by Tasks 1.1/1.2), so a fall through a hole reliably lands exactly one intact story down.

- [ ] **Step 1: Write the failing test** — append the four landing cases:

```cpp
// --- append after the head-clamp tests ------------------------------------------------------
TEST_CASE("Descent fall: free-fall through a 2x2 L3 hole lands one story down on L2") {
    StackedRoom room;
    room.punch(5, 5, 6, 6, 36);        // 2x2 L3 hole → cells (5..6,5..6) now carry {12,24}
    Player p;
    p.position = {6.0f, 9.5f, 6.0f};   // centred on the hole (AABB stays over holed cells), above L3
    p.velocity = {0, 0, 0};
    p.onGround = false;
    f32 minY = p.position.y;
    for (int i = 0; i < 120; i++) {
        Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
        minY = std::min(minY, p.position.y);
    }
    CHECK(p.onGround);
    CHECK(p.position.y == doctest::Approx(6.0f));   // caught on the next intact slab, L2
    CHECK(minY >= 6.0f - 0.001f);                    // the per-tick snap never let it dip below L2
}

TEST_CASE("Descent fall: a column holed at L3 and L2 drops through to L1") {
    StackedRoom room;
    room.punch(5, 5, 6, 6, 36);        // L3 hole
    room.punch(5, 5, 6, 6, 24);        // + L2 hole → those cells carry only {12}
    Player p;
    p.position = {6.0f, 9.5f, 6.0f};
    p.velocity = {0, 0, 0};
    p.onGround = false;
    for (int i = 0; i < 120 && !p.onGround; i++) Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
    CHECK(p.onGround);
    CHECK(p.position.y == doctest::Approx(3.0f));   // fell past the L2 hole onto L1
}

TEST_CASE("Descent fall: walking into a 2x2 hole completes the fall (no sticky lip)") {
    StackedRoom room;
    room.punch(5, 5, 6, 6, 36);        // 2x2 L3 hole (cells x5..6, z5..6)
    Player p;
    p.position = {6.0f, 9.0f, 7.6f};   // on L3 (feet 9.0), just north of the hole, x on the cell-5/6 seam
    p.velocity = {0, 0, 0};
    p.onGround = true;
    // Modest speed so horizontal drift during the fall stays over the 2x2 hole. Once the 0.6 m AABB
    // fully clears the intact lip it must fall — a 1-cell hole could not clear it, hence >=2 wide.
    walk(p, room.grid, 0.0f, -1.5f, 120);
    CHECK(p.position.y == doctest::Approx(6.0f));   // the lip did NOT re-grab it onto L3 — it fell to L2
    CHECK(p.onGround);
}

TEST_CASE("Descent fall: a high-velocity single tick still lands on the top slab") {
    StackedRoom room;                  // full stacks, no hole
    Player p;
    p.position = {6.0f, 9.3f, 6.0f};   // just above the L3 top (9.0), within step tolerance
    p.velocity = {0.0f, -30.0f, 0.0f}; // 0.5 m in one 1/60 s tick — a velocity-gated snap would tunnel
    p.onGround = false;
    Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);   // exactly one tick
    CHECK(p.onGround);
    CHECK(p.position.y == doctest::Approx(9.0f));   // caught on L3 despite the 30 m/s descent
}
```

- [ ] **Step 2: Run it, verify status** — these are regression guards for the deliberately‑unchanged, velocity‑agnostic snap, so with Tasks 1.1/1.2 in they pass immediately:

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Descent fall*"
```

Expected: all four GREEN. To confirm they actually bite (proof of guard, then revert): temporarily gate the floor‑snap on low speed in `collision.cpp` (e.g. wrap the `if (player.position.y < fh)` body in `if (fabsf(player.velocity.y) < 5.0f)`) and re‑run — the high‑velocity and free‑fall cases go RED (`p.position.y` tunnels below the slab), showing the guard defends the "keep the snap velocity‑agnostic" requirement. Revert the sabotage before Step 4.

- [ ] **Step 3: Minimal implementation** — none. Phase 1 must NOT touch the floor‑snap UP loop; these tests lock that. Confirm the two floor‑snap loops (`effectiveFloorHeight(grid, (u32)cx, (u32)cz, preFeetY)` at collision.cpp lines ~257 and ~390) remain unmodified.

- [ ] **Step 4: Run the tests, verify they pass**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Descent fall*" && ./build/tests/dungeon_tests
```

Expected: all four `Descent fall` cases GREEN and the full suite green — Phase 1 collision (multi‑slab band + running‑min head clamp) is complete with zero regression to the single‑slab `test_platform` / `test_vertical_hall` cases.

- [ ] **Step 5: Commit**

```bash
git add /home/aaron/game/tests/world/test_platform.cpp
git commit -m "$(cat <<'EOF'
test(collision): pin velocity-agnostic Descent landing through drop-holes

Free-fall through a 2x2 hole lands exactly one intact story down (L2), a
L3+L2-holed column drops to L1, walking into a 2x2 hole completes the fall
(no sticky-lip re-grab), and a -30 m/s single tick still snaps onto the top
slab. Guards the floor-snap UP loop against ever being velocity-gated.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

### Task 2.1: Raycast platform rim — per-slab side bands with band-subtraction

**Files:**
- Test: `tests/world/test_raycast.cpp` (append one `TEST_CASE`)
- Modify: `src/world/raycast.cpp:174-191` (the "Platform RIM" block, the second slab-testing site)

*(No `tests/CMakeLists.txt` change: `world/test_raycast.cpp`, `src/world/level_grid.cpp`, and `src/world/raycast.cpp` are already in the `dungeon_tests` source list. This phase assumes Phase 0's multi-slab API — `platformCount`, indexed `getPlatformTop(grid,x,z,i)` / `getPlatformUnderside(grid,x,z,i)`, and the `addPlatform`/`removePlatform` authorers — has landed. Do this task FIRST: Task 2.2's "highest-slab-exits-cell" case enters a slab cell at a mid-stack Y that the OLD single-band rim would falsely snag, so the rim must be multi-slab before the descending loop is testable.)*

- [ ] **Step 1: Write the failing test** — append to `tests/world/test_raycast.cpp` (after the existing `TEST_CASE`, before the file end; the `openGrid` helper at the top of the file is reused):

```cpp
TEST_CASE("Raycast platform rim is per-slab with band-subtraction") {
    // 5-wide open room, floor 0, ceiling 12 m (qu 48). Cell (2,1) carries a two-slab stack:
    // L1 top 3 m (qu12) and L2 top 6 m (qu24). Undersides (PLATFORM_THICKNESS_Q = 2) clamp UP to
    // the next-lower surface: L1 underside = max(12-2,0)=10 qu = 2.5 m; L2 underside =
    // max(24-2,12)=22 qu = 5.5 m. Side bands are L1 (2.5,3) and L2 (5.5,6); the story between them
    // (3..5.5 m) is OPEN and must emit NO rim.
    LevelGrid g = openGrid(5, 3, 0, 48);
    GridCell& c = LevelGridSystem::getCell(g, 2, 1);
    LevelGridSystem::addPlatform(c, 12, 0);   // L1
    LevelGridSystem::addPlatform(c, 24, 0);   // L2

    SUBCASE("horizontal ray at an upper slab's band height hits that slab's rim") {
        // y = 5.75 lands inside the L2 band (5.5,6): the balcony's visible edge.
        RayHit h = Raycast::cast(g, {0.5f, 5.75f, 1.5f}, {1.0f, 0.0f, 0.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.cellX == 2);                          // stopped at the slab cell, not the far wall
        CHECK(h.position.x == doctest::Approx(2.0f));
        CHECK(h.normal.x == doctest::Approx(-1.0f));  // entered from -x → face normal points -x
    }

    SUBCASE("horizontal ray in the open story between slabs emits no rim and reaches the far wall") {
        // y = 4.5 is between L1 top (3) and L2 underside (5.5) — clear air. A single-band rim
        // (2.5..6) FALSELY snags here; per-slab band-subtraction lets the ray pass through to the
        // out-of-bounds wall at the grid's +x edge (x = 5).
        RayHit h = Raycast::cast(g, {0.5f, 4.5f, 1.5f}, {1.0f, 0.0f, 0.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.position.x == doctest::Approx(5.0f));
    }
}
```

- [ ] **Step 2: Run it, verify it fails** —

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="Raycast platform rim*"
```

Expected: the second subcase FAILS — the old single-band rim (`undH`=2.5, `topH`=6.0) treats y=4.5 as inside the band and returns a rim hit at the slab cell, so `CHECK( h.position.x == doctest::Approx(5.0f) )` reports `2.0 == Approx(5.0)`. (The first subcase passes.)

- [ ] **Step 3: Minimal implementation** — in `src/world/raycast.cpp`, replace the current "Platform RIM" block (match by content; originally lines 174-191):

Replace:
```cpp
        // Platform RIM: entering a slab cell with the crossing Y inside the slab band is a hit on
        // the slab's side face (the balcony's visible edge). Strict epsilons let surface-grazing
        // shots — a sniper firing flat across their own slab top — pass instead of snagging.
        if (LevelGridSystem::hasPlatform(grid, (u32)cx, (u32)cz)) {
            const f32 yAt  = origin.y + dir.y * t;
            const f32 topH = LevelGridSystem::getPlatformTop(grid, (u32)cx, (u32)cz);
            const f32 undH = LevelGridSystem::getPlatformUnderside(grid, (u32)cx, (u32)cz);
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
```

with:
```cpp
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
```

- [ ] **Step 4: Run the tests, verify they pass** —

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="Raycast platform rim*"
```

Expected: both subcases pass (`Raycast platform rim is per-slab with band-subtraction` green). No regression in the existing `Raycast detects floor/ceiling*` case.

- [ ] **Step 5: Commit** —

```bash
git add src/world/raycast.cpp tests/world/test_raycast.cpp
git commit -m "$(cat <<'EOF'
feat(raycast): per-slab rim band-subtraction for multi-story slabs

The rim side-face test now loops every slab in the cell and hits only if
the crossing Y lies inside that slab's (underside,top) band. Because
getPlatformUnderside(i) is clamped up to the next-lower surface, the open
story between two stacked slabs no longer emits a phantom rim, so a shot
can pass through the gap instead of snagging on a single fat band.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2.2: Raycast descending — explicit top-down slab-top loop

**Files:**
- Test: `tests/world/test_raycast.cpp` (append one `TEST_CASE`)
- Modify: `src/world/raycast.cpp:56-81` (the `dir.y < 0.0f` branch inside the `tryFloorCeil` lambda, the first slab-testing site)

*(Depends on Task 2.1: the "highest-slab-exits-cell" and sniper subcases traverse a slab cell mid-stack, which the pre-2.1 single-band rim would falsely stop. The `Edit` matches by content, so the line drift from Task 2.1's edit — which is BELOW this branch — does not matter.)*

- [ ] **Step 1: Write the failing test** — append to `tests/world/test_raycast.cpp`:

```cpp
TEST_CASE("Raycast descends through a multi-slab stack (top-down)") {
    SUBCASE("a ray starting between two slab tops lands on the nearest slab below it") {
        // Cell (1,1) stacks L1 top 3 m (qu12) and L2 top 6 m (qu24). Eye at y=5 is BELOW L2's top
        // and ABOVE L1's — the loop must skip L2 (origin under it) and land on L1, never fall past
        // it to the base floor.
        LevelGrid g = openGrid(3, 3, 0, 48);
        GridCell& c = LevelGridSystem::getCell(g, 1, 1);
        LevelGridSystem::addPlatform(c, 12, 0);
        LevelGridSystem::addPlatform(c, 24, 0);
        RayHit h = Raycast::cast(g, {1.5f, 5.0f, 1.5f}, {0.0f, -1.0f, 0.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.normal.y == doctest::Approx(1.0f));
        CHECK(h.position.y == doctest::Approx(3.0f));   // L1 top, not base floor 0
    }

    SUBCASE("a ray drops through a punched hole to the intact slab below") {
        // Full L1/L2/L3 stack, then removePlatform punches the L3 (qu36) slab out — a drop-hole.
        // A straight-down ray from above L3 threads the hole and lands on intact L2 (6 m).
        LevelGrid g = openGrid(3, 3, 0, 48);
        GridCell& c = LevelGridSystem::getCell(g, 1, 1);
        LevelGridSystem::addPlatform(c, 12, 0);
        LevelGridSystem::addPlatform(c, 24, 0);
        LevelGridSystem::addPlatform(c, 36, 0);
        LevelGridSystem::removePlatform(c, 36);          // punch the L3 hole
        RayHit h = Raycast::cast(g, {1.5f, 10.0f, 1.5f}, {0.0f, -1.0f, 0.0f}, 20.0f);
        REQUIRE(h.hit);
        CHECK(h.normal.y == doctest::Approx(1.0f));
        CHECK(h.position.y == doctest::Approx(6.0f));   // L2 top (L3 gone)
    }

    SUBCASE("the highest slab's crossing exits the cell, so the lower in-cell slab is hit") {
        // Cell (1,1) has L1 (qu12) + L2 (qu24); cell (0,1) is plain floor. The ray enters cell
        // (1,1) angling steeply down: its L2 (y=6) plane-crossing falls back in cell 0 (out of this
        // cell), so a naive "highest-then-base-floor" would skip to the ground and report y=0. The
        // top-down loop instead continues to L1 (y=3), which IS in-cell.
        LevelGrid g = openGrid(4, 3, 0, 48);
        GridCell& c = LevelGridSystem::getCell(g, 1, 1);
        LevelGridSystem::addPlatform(c, 12, 0);
        LevelGridSystem::addPlatform(c, 24, 0);
        RayHit h = Raycast::cast(g, {0.9f, 9.0f, 1.5f}, {1.0f, -40.0f, 0.0f}, 20.0f);
        REQUIRE(h.hit);
        CHECK(h.cellX == 1);
        CHECK(h.normal.y == doctest::Approx(1.0f));
        CHECK(h.position.y == doctest::Approx(3.0f));   // L1 top in cell (1,1)
    }

    SUBCASE("a hole-edge sniper threads the gap and reaches the surface one level down") {
        // Cells x=0,1 carry the full L1/L2/L3 stack (the sniper's balcony at 9 m); cells x=2..4 are
        // missing L3 (a hole). An eye at the +x edge of its L3 slab fires down-and-forward: its own
        // L3 crossing lands in the next cell (clears its own floor), and the shot lands on intact
        // L2 (6 m) two cells over — the plunge-fire-through-a-hole contract.
        LevelGrid g = openGrid(6, 3, 0, 48);
        for (u32 x = 0; x <= 1; ++x) {                   // full stack under the balcony
            GridCell& s = LevelGridSystem::getCell(g, x, 1);
            LevelGridSystem::addPlatform(s, 12, 0);
            LevelGridSystem::addPlatform(s, 24, 0);
            LevelGridSystem::addPlatform(s, 36, 0);
        }
        for (u32 x = 2; x <= 4; ++x) {                   // L3 hole (only L1+L2 remain)
            GridCell& s = LevelGridSystem::getCell(g, x, 1);
            LevelGridSystem::addPlatform(s, 12, 0);
            LevelGridSystem::addPlatform(s, 24, 0);
        }
        RayHit h = Raycast::cast(g, {1.9f, 9.5f, 1.5f}, {1.6f, -3.5f, 0.0f}, 20.0f);
        REQUIRE(h.hit);
        CHECK(h.cellX == 3);
        CHECK(h.normal.y == doctest::Approx(1.0f));
        CHECK(h.position.y == doctest::Approx(6.0f));   // landed on L2 across the hole
    }
}
```

- [ ] **Step 2: Run it, verify it fails** —

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="Raycast descends*"
```

Expected: FAILS. Subcase 1 — old code tests only the highest top (6.0); the origin is below it (`tP<=0`), so it falls to the base floor → `h.position.y` is `0.0`, not `3.0`. Subcase 3 — old code tests the highest top, whose crossing is out-of-cell, then falls to the base floor → `h.position.y` reports `0.0` instead of `3.0`. (Subcases 2 and 4 already pass, because the highest present slab happens to be the target; they are contract pins.)

- [ ] **Step 3: Minimal implementation** — in `src/world/raycast.cpp`, replace the `dir.y < 0.0f` branch inside `tryFloorCeil` (match by content; originally lines 56-81):

Replace:
```cpp
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
        }
```

with:
```cpp
        if (dir.y < 0.0f) {
            // Platform slab TOPS, tested HIGHEST-first (platHeight[] is strictly ascending). A
            // descending ray from above the stack crosses higher tops at a smaller t, so top-down
            // order returns the nearest surface. If a higher top's crossing lands OUTSIDE this cell
            // — the ray is threading a hole or angling off an edge — CONTINUE to the next-lower slab
            // instead of jumping to the base floor; a naive "highest else floor" would thread a ray
            // straight past a slab it should still hit one story down. tP <= 0 (origin at/below a
            // top) also falls through. Base floor is tested only after ALL slabs miss.
            if (LevelGridSystem::hasPlatform(grid, (u32)fcx, (u32)fcz)) {
                const u8 pc = LevelGridSystem::platformCount(grid, (u32)fcx, (u32)fcz);
                for (s32 i = (s32)pc - 1; i >= 0; --i) {
                    const f32 topH = LevelGridSystem::getPlatformTop(grid, (u32)fcx, (u32)fcz, (u32)i);
                    const f32 tP = (topH - origin.y) / dir.y;
                    if (tP <= 0.0f || tP > tExit) continue;
                    Vec3 hp = origin + dir * tP;
                    if (static_cast<s32>(std::floor(hp.x / cs)) != fcx ||
                        static_cast<s32>(std::floor(hp.z / cs)) != fcz) continue;
                    RayHit hit; hit.hit = true; hit.position = hp; hit.normal = {0.0f, 1.0f, 0.0f};
                    hit.cellX = (u32)fcx; hit.cellZ = (u32)fcz; hit.distance = tP; return hit;
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
        }
```

- [ ] **Step 4: Run the tests, verify they pass** —

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="Raycast descends*"
```

Expected: all four subcases pass (`Raycast descends through a multi-slab stack (top-down)` green).

- [ ] **Step 5: Commit** —

```bash
git add src/world/raycast.cpp tests/world/test_raycast.cpp
git commit -m "$(cat <<'EOF'
feat(raycast): top-down multi-slab descending floor test

A descending ray now walks slab tops highest-first, continuing to the
next-lower slab when a higher top's crossing lands out-of-cell, and only
falls to the base floor after every slab misses. This lets a shot drop
through a punched hole onto the intact slab one story down and lets a
hole-edge sniper plunge-fire through the gap, instead of threading a ray
past a slab it should hit or snapping straight to the ground.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2.3: Raycast rising — bottom-up slab-underside loop

**Files:**
- Test: `tests/world/test_raycast.cpp` (append one `TEST_CASE`)
- Modify: `src/world/raycast.cpp:82-105` (the `dir.y > 0` else-branch inside `tryFloorCeil`, same first slab-testing site)

*(The `Edit` matches by content, so the line drift from Task 2.2's edit — which is immediately above this branch — does not matter.)*

- [ ] **Step 1: Write the failing test** — append to `tests/world/test_raycast.cpp`:

```cpp
TEST_CASE("Raycast rising ray hits the nearest slab underside (bottom-up)") {
    // Cell (1,1) stacks L1 top 3 m (qu12) and L2 top 6 m (qu24). Undersides (PLATFORM_THICKNESS_Q
    // = 2): L1 = max(12-2,0)=10 qu = 2.5 m; L2 = max(24-2,12)=22 qu = 5.5 m. Ceiling is 12 m.
    LevelGrid g = openGrid(3, 3, 0, 48);
    GridCell& c = LevelGridSystem::getCell(g, 1, 1);
    LevelGridSystem::addPlatform(c, 12, 0);
    LevelGridSystem::addPlatform(c, 24, 0);

    SUBCASE("from the ground story, a rising ray bonks the lowest underside") {
        RayHit h = Raycast::cast(g, {1.5f, 0.5f, 1.5f}, {0.0f, 1.0f, 0.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.normal.y == doctest::Approx(-1.0f));
        CHECK(h.position.y == doctest::Approx(2.5f));   // L1 underside
    }

    SUBCASE("from between the slabs, a rising ray hits the next underside up, not the ceiling") {
        // Eye at y=4 is above L1's top (3) and below L2's underside (5.5). A single-underside
        // (i=0 only) test would miss L1 (already above it) and shoot to the 12 m ceiling; the
        // bottom-up loop finds L2's underside.
        RayHit h = Raycast::cast(g, {1.5f, 4.0f, 1.5f}, {0.0f, 1.0f, 0.0f}, 20.0f);
        REQUIRE(h.hit);
        CHECK(h.normal.y == doctest::Approx(-1.0f));
        CHECK(h.position.y == doctest::Approx(5.5f));   // L2 underside, not the ceiling
    }
}
```

- [ ] **Step 2: Run it, verify it fails** —

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="Raycast rising*"
```

Expected: the second subcase FAILS — old code tests only the (i=0) underside 2.5, which is below the origin (`tU<=0`), so it falls to the ceiling → `h.position.y` reports `12.0` instead of `5.5`. (The first subcase passes.)

- [ ] **Step 3: Minimal implementation** — in `src/world/raycast.cpp`, replace the `else // dir.y > 0` branch inside `tryFloorCeil` (match by content; originally lines 82-105):

Replace:
```cpp
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
```

with:
```cpp
        } else { // dir.y > 0 — ceiling / slab undersides
            // Platform slab UNDERSIDES, tested LOWEST-first. A rising ray from below the stack
            // crosses the lowest underside at a smaller t, so bottom-up order returns the nearest
            // overhead surface — a body under L1 bonks L1's underside, never L2's two stories up. A
            // crossing outside the cell (or origin at/above that underside → tU <= 0) CONTINUES to
            // the next underside up; the ceiling is tested only after all undersides miss.
            if (LevelGridSystem::hasPlatform(grid, (u32)fcx, (u32)fcz)) {
                const u8 pc = LevelGridSystem::platformCount(grid, (u32)fcx, (u32)fcz);
                for (u32 i = 0; i < pc; ++i) {
                    const f32 undH = LevelGridSystem::getPlatformUnderside(grid, (u32)fcx, (u32)fcz, i);
                    const f32 tU = (undH - origin.y) / dir.y;
                    if (tU <= 0.0f || tU > tExit) continue;
                    Vec3 hp = origin + dir * tU;
                    if (static_cast<s32>(std::floor(hp.x / cs)) != fcx ||
                        static_cast<s32>(std::floor(hp.z / cs)) != fcz) continue;
                    RayHit hit; hit.hit = true; hit.position = hp; hit.normal = {0.0f, -1.0f, 0.0f};
                    hit.cellX = (u32)fcx; hit.cellZ = (u32)fcz; hit.distance = tU; return hit;
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
```

- [ ] **Step 4: Run the tests, verify they pass** — the filtered case, then the full suite:

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="Raycast rising*"
./build/tests/dungeon_tests
```

Expected: `Raycast rising ray hits the nearest slab underside (bottom-up)` green, and the full suite reports `[doctest] Status: SUCCESS!` (all Phase 2 raycast cases plus every pre-existing case pass — full suite green).

- [ ] **Step 5: Commit** —

```bash
git add src/world/raycast.cpp tests/world/test_raycast.cpp
git commit -m "$(cat <<'EOF'
feat(raycast): bottom-up multi-slab rising underside test

A rising ray now walks slab undersides lowest-first, continuing upward
when a nearer underside is out-of-cell or already below the origin, and
only reaches the ceiling after every underside misses. A body between two
stories now bonks the underside directly above it instead of shooting
through to the ceiling. Completes the Phase 2 multi-slab raycast loops
(descending tops, rising undersides, per-slab rim).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

### Task 3.1: Mesher scratch-headroom tripwire test

**Files:**
- Create/Test: `tests/world/test_mesh_scratch.cpp`
- Modify: `tests/CMakeLists.txt:61-70` (add the test file to the `add_executable(dungeon_tests …)` source list — no new production `.cpp` is linked; the mesher itself needs a GL context so it stays out)

- [ ] **Step 1: Write the failing test** — create `tests/world/test_mesh_scratch.cpp`. This is the pure-arithmetic "headroom check" the design calls for (§6 "Mesher headroom"): `level_mesh.cpp` calls `MeshSystem::create` (GL) so it can't be linked headless; instead we replicate the worst-case quad count and prove it fits the 12288/16384 scratch, so the new `pushQuad` overflow warn (Task 3.2) is a real-bug backstop, not a routine budget shortfall.

```cpp
// tests/world/test_mesh_scratch.cpp
// Headroom tripwire for the level mesher's per-material scratch buckets (src/world/level_mesh.cpp).
//
// Phase 3 of the FOUR_STORY work makes the mesher emit a top+underside+rim per SLAB (up to
// MAX_PLATFORMS_PER_CELL of them) instead of one slab per cell, so a fully-slabbed same-material
// section is the densest bucket the builder can produce. This proves that worst case still fits the
// 12288/16384 vertex/index scratch, so the pushQuad overflow path (a one-shot LOG_WARN) is a genuine
// bug backstop — and we do NOT bump the scratch (doubling adds ~7 MB transient during buildAll,
// Switch static-init pressure). level_mesh.cpp needs a GL context (MeshSystem::create) so it is not
// linked here; this is the pure-arithmetic headroom check.

#include "doctest/doctest.h"
#include "core/types.h"
#include "world/level_grid.h"   // MAX_PLATFORMS_PER_CELL (level_grid.cpp already linked into the suite)

namespace {
// Mirror the mesher's private scratch caps + section size. Keep in sync with:
//   src/world/level_mesh.cpp: SCRATCH_VERTS / SCRATCH_INDICES
//   src/world/level_mesh.h:   SECTION_SIZE
constexpr u32 kScratchVerts   = 12288;
constexpr u32 kScratchIndices = 16384;
constexpr u32 kSectionSize    = 16;
}

TEST_CASE("Mesher: worst-case fully-slabbed same-material section fits scratch") {
    // Densest same-material bucket: every cell in the 16x16 section is an open FOUR_STORY interior
    // cell whose floor, all MAX_PLATFORMS_PER_CELL slab tops/undersides and rims share ONE material.
    const u32 cells = kSectionSize * kSectionSize;   // 256

    // Quads such a cell contributes to that single bucket:
    //   1        floor quad
    //   0        ceiling quad  — SKIPPED on a full MAX-slab stack (the CELL_CEILING skip, Task 3.4)
    //   2 * MAX  slab top + underside (one pair per slab, Task 3.3)
    //   0        rim   — interior cell: every neighbour shares the same bands -> both strips empty
    //   0        riser — flat L0, no lower neighbour
    const u32 quadsPerCell = 1u + 2u * MAX_PLATFORMS_PER_CELL;   // = 7 at MAX=3
    CHECK(quadsPerCell == 7u);

    const u32 worstVerts   = cells * quadsPerCell * 4u;   // 4 verts   / quad -> 7168
    const u32 worstIndices = cells * quadsPerCell * 6u;   // 6 indices / quad -> 10752

    CHECK(worstVerts   <= kScratchVerts);     // 7168  <= 12288
    CHECK(worstIndices <= kScratchIndices);   // 10752 <= 16384

    // Even WITHOUT the ceiling skip (8 quads/cell) the section still fits (8192 verts / 12288
    // indices) — the skip is a fill/overdraw win, not a budget necessity.
    const u32 withCeilingVerts   = cells * (quadsPerCell + 1u) * 4u;
    const u32 withCeilingIndices = cells * (quadsPerCell + 1u) * 6u;
    CHECK(withCeilingVerts   <= kScratchVerts);
    CHECK(withCeilingIndices <= kScratchIndices);
}
```

- [ ] **Step 2: Run it, verify it fails** — the file isn't in the `add_executable` list yet, so the case isn't compiled into the binary and the filter matches nothing (red = not wired):

```bash
cmake --build build --target dungeon_tests \
  && ./build/tests/dungeon_tests -tc="*fully-slabbed*"
```
Expected: `Unknown test cases!` / `test cases: 0 | 0 passed | 0 failed` — the case is absent.

- [ ] **Step 3: Minimal implementation** — register the test in `tests/CMakeLists.txt`, in the `world/` block of the `add_executable(dungeon_tests …)` list (right after the `test_story_nav.cpp` line at :69):

```cmake
    world/test_story_nav.cpp         # pure cross-story chase helpers (onUpperStory / nearestPortalGoal)
    world/test_mesh_scratch.cpp      # mesher scratch headroom: fully-slabbed same-material section fits
```

- [ ] **Step 4: Run the tests, verify they pass**

```bash
cmake --build build --target dungeon_tests \
  && ./build/tests/dungeon_tests -tc="*fully-slabbed*"
```
Expected: `test cases: 1 | 1 passed | 0 failed`, all `CHECK`s green (7168≤12288, 10752≤16384).

- [ ] **Step 5: Commit**

```bash
git add tests/world/test_mesh_scratch.cpp tests/CMakeLists.txt
git commit -m "test(mesh): headroom tripwire for fully-slabbed same-material section

Proves the worst-case FOUR_STORY bucket (~7168/10752) fits the 12288/16384
scratch, so the pushQuad overflow warn is a bug backstop, not a budget issue.
Pure arithmetic - level_mesh.cpp needs a GL context so it is not linked.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3.2: pushQuad one-shot scratch-overflow warn

**Files:**
- Modify: `src/world/level_mesh.cpp:29` (add the latch beside `s_buckets`)
- Modify: `src/world/level_mesh.cpp:61` (replace the silent overflow `return`)
- Modify: `src/world/level_mesh.cpp:462` (reset the latch per `buildAll`)

- [ ] **Step 1: Write the failing test** — no new unit test (the mesher is GL-bound and unlinkable headless). The guarding test is Task 3.1's headroom tripwire, which asserts the fully-slabbed section stays *under* the scratch so this warn must NOT fire in normal builds. Re-confirm it is green as the pre-condition for adding the backstop:

```bash
./build/tests/dungeon_tests -tc="*fully-slabbed*"
```
Expected: `1 passed` — the budget contract holds, so a runtime overflow would signal a real geometry bug.

- [ ] **Step 2: Run it, verify it fails** — confirm the current code path is the *silent* return we are replacing (a dropped quad = invisible geometry the player falls through):

```bash
sed -n '58,62p' src/world/level_mesh.cpp
```
Expected: line 61 is `if (bkt.vertCount + 4 > SCRATCH_VERTS || bkt.indexCount + 6 > SCRATCH_INDICES) return;` — a bare `return`, no diagnostic (the defect this task fixes).

- [ ] **Step 3: Minimal implementation** — add the latch after the `s_buckets` declaration (line 29):

```cpp
// Heap-allocated scratch buckets — only used during buildAll(), freed after.
// Avoids 3.6MB of BSS which crashes the Switch at static init.
static MaterialBucket* s_buckets = nullptr;

// One-shot warning latch for pushQuad scratch overflow. A dropped quad is INVISIBLE geometry —
// e.g. a slab TOP the player then falls through — so the old silent return was dangerous. We surface
// it once per buildAll (reset there) and total the drops, WITHOUT bumping SCRATCH_VERTS/INDICES: the
// worst fully-slabbed same-material section needs only ~7168/10752 of the 12288/16384 scratch (see
// tests/world/test_mesh_scratch.cpp), so an overflow is a real geometry bug, not a tight budget.
static u32 s_quadOverflowDrops = 0;
```

Replace the silent return in `pushQuad` (line 61):

```cpp
    if (bkt.vertCount + 4 > SCRATCH_VERTS || bkt.indexCount + 6 > SCRATCH_INDICES) {
        if (s_quadOverflowDrops++ == 0)
            LOG_WARN("LevelMesh: material-bucket scratch overflow (cap %u verts / %u indices) — "
                     "dropping quads; INVISIBLE geometry. A fully-slabbed section should fit "
                     "(~7168/10752); an overflow is a geometry bug, not a budget shortfall.",
                     SCRATCH_VERTS, SCRATCH_INDICES);
        return;
    }
```

Reset the latch once per build, right after the heap alloc in `buildAll` (line 462):

```cpp
    // Allocate scratch buckets on heap (3.6MB — too large for BSS on Switch)
    if (!s_buckets) s_buckets = new MaterialBucket[MAX_SUBMESHES_PER_SECTION];

    s_quadOverflowDrops = 0;   // fresh one-shot overflow warn per rebuild
```

- [ ] **Step 4: Run the tests, verify they pass** — full game build compiles, the headroom tripwire and full suite stay green (render-only: the grid is untouched, so the determinism `memcmp` is unaffected):

```bash
cmake --build build \
  && cmake --build build --target dungeon_tests \
  && ./build/tests/dungeon_tests
grep -n "s_quadOverflowDrops" src/world/level_mesh.cpp
```
Expected: game + test binaries build clean; `all tests passed`; grep shows the latch declared, incremented in `pushQuad`, and reset in `buildAll` (3 hits).

- [ ] **Step 5: Commit**

```bash
git add src/world/level_mesh.cpp
git commit -m "feat(mesh): one-shot LOG_WARN on pushQuad scratch overflow

A dropped quad is invisible geometry (a slab the player falls through), so
the silent return is dangerous. Warn once per buildAll and total the drops;
do NOT bump SCRATCH_VERTS/INDICES (Switch static-init pressure) — a fully-
slabbed section fits ~7168/10752 of 12288/16384, so overflow means a bug.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3.3: Per-slab top/underside/rim loop

**Files:**
- Modify: `src/world/level_mesh.cpp:350-426` (wrap the `CELL_PLATFORM` block in `for (u8 s = 0; s < slabCount; ++s)`, indexed getters + `cell.platMaterialId[s]`, per-slab band-subtraction rim)

Depends on the Phase 0 API: `LevelGridSystem::platformCount(grid,x,z)` and the indexed `getPlatformTop(grid,x,z,i)` / `getPlatformUnderside(grid,x,z,i)` overloads, and `GridCell::platMaterialId[]`.

- [ ] **Step 1: Write the failing test** — no new headless unit test (GL-bound mesher). The guards are (a) the existing determinism suite, which proves the *grid* is unchanged by a render-only edit, and (b) a manual single-slab visual (VERTICAL_HALL/arena cells are `platCount==1`, so `slabCount==1` must emit byte-identically to the old single-slab code). Establish the pre-change baseline:

```bash
cmake --build build --target dungeon_tests \
  && ./build/tests/dungeon_tests -tc="*deterministic*"
```
Expected: `LevelGen: every style is deterministic from the seed` passes — the memcmp baseline this edit must not disturb.

- [ ] **Step 2: Run it, verify it fails** — confirm the current block is single-slab (scalar getters, `cell.platMaterialId`, no loop) — the limitation this task removes:

```bash
sed -n '350,354p' src/world/level_mesh.cpp
```
Expected: lines read `if (cell.flags & CELL_PLATFORM) {`, `const f32 topH = LevelGridSystem::getPlatformTop(grid, x, z);`, `const f32 undH = LevelGridSystem::getPlatformUnderside(grid, x, z);`, `MaterialBucket* pbkt = getBucket(cell.platMaterialId);` — one slab only, no `for`.

- [ ] **Step 3: Minimal implementation** — replace the whole `if (cell.flags & CELL_PLATFORM) { … }` block (lines 350-426) with the per-slab loop:

```cpp
                // Platform slabs (CELL_PLATFORM): the walk-under stories. Emit EVERY slab this cell
                // carries (up to MAX_PLATFORMS_PER_CELL) — top like a floor quad at the slab top,
                // underside like a ceiling quad, rim faces toward neighbours that don't cover our
                // band. A 2-story VERTICAL_HALL/arena cell has slabCount==1 → byte-identical to the
                // old single-slab emission; a FOUR_STORY interior cell has 3. Rim ownership mirrors
                // the riser faces: each cell draws only the strip of its own rim a neighbour leaves
                // exposed, so shared faces emit exactly once.
                if (cell.flags & CELL_PLATFORM) {
                    const u8 slabCount = LevelGridSystem::platformCount(grid, x, z);
                    for (u8 s = 0; s < slabCount; ++s) {
                        const f32 topH = LevelGridSystem::getPlatformTop(grid, x, z, s);
                        const f32 undH = LevelGridSystem::getPlatformUnderside(grid, x, z, s);
                        MaterialBucket* pbkt = getBucket(cell.platMaterialId[s]);
                        {   // top (+Y) — same layout/winding as the floor quad
                            Vec3 n{0.0f, 1.0f, 0.0f};
                            Vec3 p0{wx,      topH, wz + cs};
                            Vec3 p1{wx + cs, topH, wz + cs};
                            Vec3 p2{wx + cs, topH, wz};
                            Vec3 p3{wx,      topH, wz};
                            Vertex v0{p0, n, {0.0f, cs  }};
                            Vertex v1{p1, n, {cs,   cs  }};
                            Vertex v2{p2, n, {cs,   0.0f}};
                            Vertex v3{p3, n, {0.0f, 0.0f}};
                            v0.color = v1.color = v2.color = v3.color = tileTint;
                            pushQuad(*pbkt, v0, v1, v2, v3);
                            expand(wx, topH, wz); expand(wx + cs, topH, wz + cs);
                        }
                        if (undH > floorH + 0.001f) {   // underside (−Y) — same winding as the ceiling quad
                            Vec3 n{0.0f, -1.0f, 0.0f};
                            Vec3 p0{wx,      undH, wz};
                            Vec3 p1{wx + cs, undH, wz};
                            Vec3 p2{wx + cs, undH, wz + cs};
                            Vec3 p3{wx,      undH, wz + cs};
                            Vertex v0{p0, n, {0.0f, 0.0f}};
                            Vertex v1{p1, n, {cs,   0.0f}};
                            Vertex v2{p2, n, {cs,   cs  }};
                            Vertex v3{p3, n, {0.0f, cs  }};
                            v0.color = v1.color = v2.color = v3.color = tileTint;
                            pushQuad(*pbkt, v0, v1, v2, v3);
                            expand(wx, undH, wz); expand(wx + cs, undH, wz + cs);
                        }
                        {   // rim faces for THIS slab. A rim spans the part of our band [undH, topH]
                            // a neighbour leaves exposed. Slabs are spaced > their thickness apart, so
                            // at most ONE neighbour slab overlaps our band; subtracting it can leave a
                            // LOWER and/or UPPER strip (a stepped slab staircase). If NO neighbour slab
                            // overlaps (a punched drop-hole, or a non-slab neighbour) the whole band is
                            // the exposed hole/edge rim we want. Render-only: a miss is a see-through
                            // edge, never a desync. Winding mirrors the riser faces above.
                            MaterialBucket* rbkt = getBucket(cell.wallMaterialId);
                            static const s32 kpdx[4] = {1, -1, 0, 0};
                            static const s32 kpdz[4] = {0, 0, 1, -1};
                            // Emit one vertical rim quad spanning [B,T] on edge ei; self-guards degenerate spans.
                            auto emitRim = [&](int ei, f32 B, f32 T) {
                                if (T <= B + 0.001f) return;
                                const f32 vSpan = T - B;
                                Vec3 rn, rp0, rp1, rp2, rp3;
                                if (kpdz[ei] == -1)      { rn = {0,0,-1}; rp0={wx+cs,B,wz};    rp1={wx,B,wz};       rp2={wx,T,wz};       rp3={wx+cs,T,wz}; }
                                else if (kpdz[ei] == 1)  { rn = {0,0, 1}; rp0={wx,B,wz+cs};    rp1={wx+cs,B,wz+cs}; rp2={wx+cs,T,wz+cs}; rp3={wx,T,wz+cs}; }
                                else if (kpdx[ei] == -1) { rn = {-1,0,0}; rp0={wx,B,wz};       rp1={wx,B,wz+cs};    rp2={wx,T,wz+cs};    rp3={wx,T,wz}; }
                                else                     { rn = { 1,0,0}; rp0={wx+cs,B,wz+cs}; rp1={wx+cs,B,wz};    rp2={wx+cs,T,wz};    rp3={wx+cs,T,wz+cs}; }
                                Vertex rv0{rp0, rn, {0.0f, 0.0f }};
                                Vertex rv1{rp1, rn, {cs,   0.0f }};
                                Vertex rv2{rp2, rn, {cs,   vSpan}};
                                Vertex rv3{rp3, rn, {0.0f, vSpan}};
                                rv0.color = rv1.color = rv2.color = rv3.color = tileTint;
                                pushQuad(*rbkt, rv0, rv1, rv2, rv3);
                                expand(rp0.x, B, rp0.z); expand(rp2.x, T, rp2.z);
                            };
                            for (int ei = 0; ei < 4; ei++) {
                                s32 nx = (s32)x + kpdx[ei], nz = (s32)z + kpdz[ei];
                                if (!LevelGridSystem::isInBounds(grid, (u32)nx, (u32)nz)) continue;
                                if (LevelGridSystem::isSolid(grid, (u32)nx, (u32)nz)) continue; // wall face covers it
                                // Find the one neighbour slab whose band overlaps ours [undH, topH].
                                f32 nbTop = 0.0f, nbUnd = 0.0f;
                                bool covered = false;
                                const u8 nbCount = LevelGridSystem::platformCount(grid, (u32)nx, (u32)nz);
                                for (u8 j = 0; j < nbCount; ++j) {
                                    const f32 t = LevelGridSystem::getPlatformTop(grid, (u32)nx, (u32)nz, j);
                                    const f32 u = LevelGridSystem::getPlatformUnderside(grid, (u32)nx, (u32)nz, j);
                                    if (t > undH + 0.001f && u < topH - 0.001f) { nbTop = t; nbUnd = u; covered = true; break; }
                                }
                                if (covered) {
                                    // exposed = our band minus the neighbour's band → up to two strips (each self-guards).
                                    emitRim(ei, undH, nbUnd < topH ? nbUnd : topH);   // lower strip
                                    emitRim(ei, nbTop > undH ? nbTop : undH, topH);   // upper strip
                                } else {
                                    emitRim(ei, undH, topH);                          // full exposed band (hole/edge)
                                }
                            }
                        }
                    }
                }
```

- [ ] **Step 4: Run the tests, verify they pass** — full game builds; the determinism suite and full suite stay green (grid untouched); a single-slab visual pass confirms `slabCount==1` still renders balconies identically (`--fourstory` doesn't exist until Phase 5, so exercise the single-slab path here):

```bash
cmake --build build \
  && cmake --build build --target dungeon_tests \
  && ./build/tests/dungeon_tests
./build/dungeon_game --vhall     # VERTICAL_HALL: slabCount==1 → balcony/ramp geometry unchanged
./build/dungeon_game --arena     # arena: single-slab balconies/tower unchanged
```
Expected: clean build; `all tests passed`; `--vhall` and `--arena` render slabs, undersides and rims exactly as before this change (no missing/see-through balcony edges).

- [ ] **Step 5: Commit**

```bash
git add src/world/level_mesh.cpp
git commit -m "feat(mesh): per-slab top/underside/rim loop for multi-story cells

Wrap the CELL_PLATFORM block in for(s<platformCount) using the indexed
getters + platMaterialId[s], with the two-strip rim band-subtraction nested
per slab against the one overlapping neighbour slab (a missing neighbour slab
= exposed hole rim). slabCount==1 is byte-identical to the old single-slab
path; render-only, grid untouched so the determinism memcmp stays green.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3.4: CELL_CEILING skip for FOUR_STORY interior cells

**Files:**
- Modify: `src/world/level_mesh.cpp:327-328` (gate the ceiling quad on `platformCount < MAX_PLATFORMS_PER_CELL`)

- [ ] **Step 1: Write the failing test** — no new headless unit test (GL-bound). Guard is the determinism suite (grid untouched) plus the headroom tripwire, which already encodes the skip: `quadsPerCell == 7` counts the ceiling as skipped on a full stack. Confirm both are green before the edit:

```bash
./build/tests/dungeon_tests -tc="*fully-slabbed*" -tc="*deterministic*"
```
Expected: both pass — the headroom math assumes the ceiling is skipped on a `MAX`-slab cell, which this task must make true in the mesher.

- [ ] **Step 2: Run it, verify it fails** — confirm the ceiling quad is currently emitted unconditionally (the full-overdraw layer down a FOUR_STORY shaft this task removes):

```bash
sed -n '327,329p' src/world/level_mesh.cpp
```
Expected: lines read `// Ceiling quad` then `if (cell.flags & CELL_CEILING) {` — no slab-count gate.

- [ ] **Step 3: Minimal implementation** — replace the ceiling-quad gate (lines 327-328):

```cpp
                // Ceiling quad. Skipped for a FOUR_STORY interior cell (a full MAX_PLATFORMS_PER_CELL
                // stack): three stacked slabs occlude the ceiling from every walkable story, so drawing
                // it is pure overdraw down the shaft. platformCount==MAX is the render-only tell —
                // VERTICAL_HALL/arena cells are single-slab, and a FOUR_STORY *hole* cell (L3 punched →
                // count<MAX) KEEPS its ceiling so you don't see a black void up through the hole.
                if ((cell.flags & CELL_CEILING) &&
                    LevelGridSystem::platformCount(grid, x, z) < MAX_PLATFORMS_PER_CELL) {
```

- [ ] **Step 4: Run the tests, verify they pass** — full game builds; full suite green; single-slab floors are unaffected (their `platformCount` is ≤1 < MAX, so the ceiling still draws — verify visually):

```bash
cmake --build build \
  && cmake --build build --target dungeon_tests \
  && ./build/tests/dungeon_tests
./build/dungeon_game --vhall     # single-slab: ceilings still render (count 0/1 < MAX)
```
Expected: clean build; `all tests passed` (headroom + determinism green); `--vhall` ceilings unchanged. (Full-stack skip is visually confirmed on a `--fourstory` floor in Phase 5, where a GPU capture verifies the dropped overdraw layer.)

- [ ] **Step 5: Commit**

```bash
git add src/world/level_mesh.cpp
git commit -m "perf(mesh): skip ceiling quad on full-stack FOUR_STORY cells

A MAX_PLATFORMS_PER_CELL stack occludes the ceiling from every walkable
story, so drawing it is pure overdraw down the shaft. Gate on
platformCount<MAX: single-slab (VH/arena) and hole cells (L3 punched) keep
their ceiling, so a drop-hole never shows a black void. Render-only.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```