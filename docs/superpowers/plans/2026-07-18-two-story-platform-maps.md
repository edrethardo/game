# Two-Story Platform Maps + Combat Hall Arena Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Real walk-under two-story geometry (`CELL_PLATFORM` slab) through the engine's three
grid chokes, then rebuild the arena as a 44×44 Combat Hall with a perimeter sniper balcony.

**Architecture:** One optional platform slab per grid cell (top surface + fixed 0.5 m thickness),
consumed by exactly three chokes — `Collision::moveAndSlide` (story selection, underside head
clamp), `Raycast::cast` (top/underside/rim planes; all 24 LOS/hitscan/projectile call sites
inherit it), and the level mesher (top/underside/rim quads). Deterministic seed-built geometry on
every peer ⇒ **no wire change, no save change** (the jump-pad replication story). Spec:
`docs/superpowers/specs/2026-07-17-two-story-platform-maps-design.md`.

**Tech Stack:** C++17, doctest (`tests/`), CMake. No new dependencies.

**Repo rules that bind this plan:** the repo has a **no-unprompted-commits** rule — the commit
steps below run only under the user's standing per-task authorization for this execution (confirm
it exists before the first commit; otherwise leave changes uncommitted and say so). Assets are
untouched (no meshes/textures needed — slabs are level geometry using existing materials).
`GridCell` is NOT serialized (rebuilt from seed) so growing it is save-safe and wire-safe.

---

## File map

| File | Change |
|---|---|
| `src/world/level_grid.h` | `CELL_PLATFORM` flag, 2 new `GridCell` fields, 2 constants, 4 helper decls |
| `src/world/level_grid.cpp` | helper impls |
| `src/world/collision.h` | `overlapsPlatformBand` decl |
| `src/world/collision.cpp` | band block in X/Z, story-aware landing/snap, underside head clamp — **both overloads** |
| `src/world/raycast.cpp` | slab top/underside planes in `tryFloorCeil`, rim hit in the DDA loop |
| `src/world/level_mesh.cpp` | slab top/underside/rim quads in the open-cell branch |
| `src/game/teleport_dest.cpp` | destination Y resolves at the victim's story |
| `src/engine/engine_arena.cpp` | 44×44 Combat Hall rebuild + new `kArenaPads` |
| `tests/world/test_platform.cpp` | **new** — grid/collision/raycast platform tests |
| `tests/game/test_teleport_dest.cpp` | one new balcony case |
| `tests/CMakeLists.txt` | add `world/test_platform.cpp` (all needed production .cpp already linked) |
| `CLAUDE.md`, `.claude/skills/engine-reference/SKILL.md`, `.claude/skills/engine-how-to/SKILL.md` | doc sync |

---

### Task 1: Grid data model + story-selection helpers

**Files:**
- Modify: `src/world/level_grid.h` (flags block ~line 22, `GridCell` ~line 24, namespace ~line 55)
- Modify: `src/world/level_grid.cpp` (after `getCeilingHeight`, ~line 75)
- Create: `tests/world/test_platform.cpp`
- Modify: `tests/CMakeLists.txt` (test list, after `world/test_collision_push.cpp` line 66)

- [ ] **Step 1: Register the test file**

In `tests/CMakeLists.txt` after the `world/test_collision_push.cpp` line add:

```cmake
    world/test_platform.cpp          # CELL_PLATFORM slab: story selection, walk-under collision, ray planes
```

(`level_grid.cpp`, `collision.cpp`, `raycast.cpp` are already in the link list — no other change.)

- [ ] **Step 2: Write the failing tests**

Create `tests/world/test_platform.cpp`:

```cpp
// test_platform.cpp — CELL_PLATFORM: real two-story cells (a walk-under slab over a normal floor).
//
// The balcony contract, pinned from three sides: the GRID picks which story a body interacts with
// from its feet height; COLLISION lands bodies on the slab top, lets them walk beneath, and bonks
// a rising head on the underside (a 17 m/s jump-pad launch under a balcony must never tunnel up
// through the walkway); RAYCAST treats the slab as solid from every side (top, underside, rim)
// while letting shots pass cleanly under and over it — the sniper-balcony sightlines.

#include <doctest/doctest.h>
#include "world/collision.h"
#include "world/level_grid.h"
#include "world/raycast.h"
#include "game/player.h"
#include <algorithm>

namespace {

// A 12x12 open room (1 m cells, solid border, floor y=0, walls 5 m) with a BALCONY: a 2-cell-deep
// platform band along the north wall (z=1..2, x=1..10), slab top 3.0 m (12 qu, underside 2.5 m) —
// the arena's exact shape in miniature. Cell (gx,gz) spans [gx,gx+1)x[gz,gz+1); centres at +0.5.
struct BalconyRoom {
    LevelGrid grid;
    BalconyRoom() {
        LevelGridSystem::init(grid, 12, 12, 1.0f);
        for (u32 z = 0; z < 12; z++)
            for (u32 x = 0; x < 12; x++) {
                GridCell& c = grid.cells[z * 12 + x];
                const bool border = (x == 0 || z == 0 || x == 11 || z == 11);
                c.flags         = border ? CELL_SOLID : CELL_FLOOR;
                c.floorHeight   = 0;
                c.ceilingHeight = 20;
            }
        for (u32 z = 1; z <= 2; z++)
            for (u32 x = 1; x <= 10; x++) setPlat(x, z, 12);
    }
    ~BalconyRoom() { LevelGridSystem::shutdown(grid); }

    void setPlat(u32 x, u32 z, u8 topQ) {
        GridCell& c  = grid.cells[z * 12 + x];
        c.flags      = static_cast<u8>(CELL_FLOOR | CELL_PLATFORM);
        c.platHeight = topQ;
    }
};

// Step a body through moveAndSlide with a held horizontal velocity, the way the movement code
// drives it (velocity.x/z re-asserted every tick; Y left to gravity/impulses).
void walk(Player& p, const LevelGrid& g, f32 vx, f32 vz, int ticks) {
    for (int i = 0; i < ticks; i++) {
        p.velocity.x = vx;
        p.velocity.z = vz;
        Collision::moveAndSlide(p, g, 1.0f / 60.0f);
    }
}

} // namespace

TEST_CASE("Platform grid: helpers expose top/underside and pick the story by feet height") {
    BalconyRoom room;
    CHECK(LevelGridSystem::hasPlatform(room.grid, 5, 1));
    CHECK_FALSE(LevelGridSystem::hasPlatform(room.grid, 5, 5));
    CHECK(LevelGridSystem::getPlatformTop(room.grid, 5, 1) == doctest::Approx(3.0f));
    CHECK(LevelGridSystem::getPlatformUnderside(room.grid, 5, 1) == doctest::Approx(2.5f));

    // Story selection: feet at/near the top walk the slab; feet below keep the ground floor.
    CHECK(LevelGridSystem::effectiveFloorHeight(room.grid, 5, 1, 3.0f) == doctest::Approx(3.0f));
    CHECK(LevelGridSystem::effectiveFloorHeight(room.grid, 5, 1, 2.7f) == doctest::Approx(3.0f)); // within step tolerance
    CHECK(LevelGridSystem::effectiveFloorHeight(room.grid, 5, 1, 2.5f) == doctest::Approx(0.0f)); // below tolerance
    CHECK(LevelGridSystem::effectiveFloorHeight(room.grid, 5, 1, 0.0f) == doctest::Approx(0.0f));
    CHECK(LevelGridSystem::effectiveFloorHeight(room.grid, 5, 5, 9.0f) == doctest::Approx(0.0f)); // no platform

    // A slab too thin for under-space clamps its underside to the base floor (low stair steps).
    room.setPlat(8, 5, 1);
    CHECK(LevelGridSystem::getPlatformUnderside(room.grid, 8, 5) == doctest::Approx(0.0f));
}
```

- [ ] **Step 3: Run to verify failure**

```bash
cmake --build build --target dungeon_tests
```
Expected: **compile FAILURE** — `CELL_PLATFORM`, `platHeight`, `hasPlatform` undeclared.

- [ ] **Step 4: Implement the data model**

In `src/world/level_grid.h`, after the `CELL_JUMPPAD` block (line 22) add:

```cpp
// A PLATFORM SLAB: a second walkable story floating above this cell's normal floor. The cell keeps
// its base floor (CELL_FLOOR + floorHeight) as the GROUND story — bodies below walk UNDER the slab
// (its underside is their local ceiling), bodies above walk ON it. Real two-story, unlike
// CELL_LEDGE risers, which are solid underneath. Consumed by exactly three chokes:
// Collision::moveAndSlide (story selection + underside head clamp), Raycast::cast (top/underside/
// rim planes — nothing shoots through a balcony floor), and the level mesher (top/underside/rim
// quads). The grid is rebuilt from the seed on every peer, so slabs replicate in co-op with NO
// wire change — the jump-pad story. Enemies never jump, so AI keeps navigating the ground story
// and simply walks under platforms (PvP-only above, the pads/ledges policy).
static constexpr u8 CELL_PLATFORM = 1 << 5;

// Slab thickness in quarter-units (0.5 m). Underside = platHeight - this, clamped to floorHeight,
// so a low stair step degrades gracefully into riser-like geometry with no dead under-space.
static constexpr u8 PLATFORM_THICKNESS_Q = 2;

// Feet within this many metres below a slab top interact with the SLAB story (walk on / step up);
// further below and they belong to the ground story. Must equal Collision::STEP_UP_HEIGHT (the
// ledge step gate) — collision.cpp static_asserts the pair; it lives here because collision.h
// includes this header, not the other way round.
static constexpr f32 PLATFORM_STEP_TOLERANCE = 0.4f;
```

In `struct GridCell`, after `ceilMaterialId`:

```cpp
    u8 platHeight;       // CELL_PLATFORM only: slab TOP surface, quarter-units (×0.25 = metres)
    u8 platMaterialId;   // CELL_PLATFORM only: slab top + underside material
```

In `namespace LevelGridSystem`, after `getCeilingHeight`:

```cpp
    // CELL_PLATFORM (two-story) queries. hasPlatform is the gate; the getters assume it passed.
    bool hasPlatform(const LevelGrid& grid, u32 x, u32 z);
    f32  getPlatformTop(const LevelGrid& grid, u32 x, u32 z);
    f32  getPlatformUnderside(const LevelGrid& grid, u32 x, u32 z);
    // THE story selector: the walkable floor a body with feet at feetY interacts with in this
    // cell. Slab cells return the slab top for feet within PLATFORM_STEP_TOLERANCE below it
    // (walking the balcony, stepping up a slab stair), else the base floor (walking the arcade
    // beneath). Every consumer that means "the floor under THIS body" must use this, never raw
    // getFloorHeight, or bodies under a balcony get teleport-snapped up through it.
    f32  effectiveFloorHeight(const LevelGrid& grid, u32 x, u32 z, f32 feetY);
```

In `src/world/level_grid.cpp`, after `getCeilingHeight` (line 75):

```cpp
bool LevelGridSystem::hasPlatform(const LevelGrid& grid, u32 x, u32 z) {
    if (!isInBounds(grid, x, z)) return false;
    const GridCell& c = grid.cells[z * grid.width + x];
    // A solid cell can't carry a slab — the wall already fills the column.
    return (c.flags & CELL_PLATFORM) != 0 && !(c.flags & CELL_SOLID);
}

f32 LevelGridSystem::getPlatformTop(const LevelGrid& grid, u32 x, u32 z) {
    return grid.cells[z * grid.width + x].platHeight * 0.25f;
}

f32 LevelGridSystem::getPlatformUnderside(const LevelGrid& grid, u32 x, u32 z) {
    const GridCell& c = grid.cells[z * grid.width + x];
    // Signed math: platHeight < thickness must clamp, not wrap the u8.
    const s32 underQ = static_cast<s32>(c.platHeight) - PLATFORM_THICKNESS_Q;
    const f32 under  = underQ * 0.25f;
    const f32 fh     = c.floorHeight * 0.25f;
    return under > fh ? under : fh;
}

f32 LevelGridSystem::effectiveFloorHeight(const LevelGrid& grid, u32 x, u32 z, f32 feetY) {
    const f32 fh = getFloorHeight(grid, x, z);
    if (!hasPlatform(grid, x, z)) return fh;
    const f32 top = getPlatformTop(grid, x, z);
    return (feetY >= top - PLATFORM_STEP_TOLERANCE) ? top : fh;
}
```

- [ ] **Step 5: Run to verify pass**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Platform grid*"
```
Expected: PASS (1 test case). Also run the FULL suite — `GridCell` grew from 6 to 8 bytes and
`calloc` zero-fills, so nothing else may notice: `./build/tests/dungeon_tests` → all green.

- [ ] **Step 6: Commit**

```bash
git add src/world/level_grid.h src/world/level_grid.cpp tests/world/test_platform.cpp tests/CMakeLists.txt
git commit -m "feat(world): CELL_PLATFORM data model — walk-under slab cells + story selector"
```

---

### Task 2: Collision — walk under, land on, head-bonk, step gate

**Files:**
- Modify: `src/world/collision.h` (namespace, after `onJumpPad` decl ~line 67)
- Modify: `src/world/collision.cpp` (**both** `moveAndSlide` overloads + new helper)
- Test: `tests/world/test_platform.cpp` (append)

- [ ] **Step 1: Write the failing tests** — append to `tests/world/test_platform.cpp`:

```cpp
TEST_CASE("Platform collision: a ground body walks UNDER the balcony unobstructed") {
    BalconyRoom room;
    Player p;
    p.position = {5.5f, 0.0f, 4.5f};
    p.velocity = {0, 0, 0};
    p.onGround = true;
    walk(p, room.grid, 0.0f, -3.0f, 90);            // stroll north into the band
    CHECK(p.position.z < 2.0f);                     // deep in the arcade under the slab
    CHECK(p.position.y == doctest::Approx(0.0f));   // never lifted onto the slab
    CHECK(p.onGround);
}

TEST_CASE("Platform collision: a falling body lands ON the slab top") {
    BalconyRoom room;
    Player p;
    p.position = {5.5f, 3.6f, 1.5f};                // over the band, above the top
    p.velocity = {0, 0, 0};
    p.onGround = false;
    for (int i = 0; i < 60 && !p.onGround; i++) Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
    CHECK(p.onGround);
    CHECK(p.position.y == doctest::Approx(3.0f));
}

TEST_CASE("Platform collision: rising under the balcony bonks the underside, never tunnels up") {
    BalconyRoom room;
    Player p;
    p.position = {5.5f, 0.0f, 1.5f};                // in the arcade
    p.velocity = {0, 17.0f, 0};                     // jump-pad-scale launch (the worst case)
    p.onGround = false;
    f32 maxHead = 0.0f;
    for (int i = 0; i < 120; i++) {
        Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
        maxHead = std::max(maxHead, p.position.y + PLAYER_HEIGHT);
    }
    CHECK(maxHead <= 2.5f + 0.001f);                // head stopped at the underside
    CHECK(p.position.y == doctest::Approx(0.0f));   // and came back down to the ground story
    CHECK(p.onGround);
}

TEST_CASE("Platform collision: a slab band the body would clip blocks XZ like a wall") {
    BalconyRoom room;
    room.setPlat(5, 6, 6);                          // lone 1.5 m slab (underside 1.0) in the open
    Player p;
    p.position = {5.5f, 0.0f, 8.0f};
    p.velocity = {0, 0, 0};
    p.onGround = true;
    walk(p, room.grid, 0.0f, -3.0f, 60);            // walk north into it: head would clip the band
    CHECK(p.position.z >= 7.0f + PLAYER_HALF_WIDTH - 0.01f);   // stopped at the cell edge
    CHECK(p.position.y == doctest::Approx(0.0f));   // and was NOT hoisted onto it
}

TEST_CASE("Platform collision: graduated slab stairs climb like stairs") {
    BalconyRoom room;
    // 6 steps against the west wall: x1 top 1.5 m ... x6 top 0.25 m (0.25 m per step).
    for (u32 i = 0; i < 6; i++) room.setPlat(1 + i, 5, static_cast<u8>(6 - i));
    Player p;
    p.position = {8.5f, 0.0f, 5.5f};
    p.velocity = {0, 0, 0};
    p.onGround = true;
    walk(p, room.grid, -3.0f, 0.0f, 180);           // west, up the steps, into the wall
    CHECK(p.position.y == doctest::Approx(1.5f));   // standing on the top step
    CHECK(p.onGround);
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Platform collision*"
```
Expected: FAIL — "walks UNDER" fails (floor-snap hoists the body to 3.0), "bonks" fails
(maxHead ≈ 0 + apex 3.6 + 1.8 > 2.5), "blocks XZ" fails, "lands ON" may pass by accident.

- [ ] **Step 3: Implement**

`src/world/collision.h` — after the `onJumpPad` declaration add:

```cpp
    // True if an AABB (feet at feetPos, halfWidth in XZ, PLAYER_HEIGHT tall) would CLIP a
    // CELL_PLATFORM slab band [underside, top]: the body is neither stepping ONTO the slab (feet
    // within STEP_UP_HEIGHT of the top — slab stairs, jump landings) nor passing fully BENEATH it
    // (head clear of the underside). moveAndSlide treats that as a wall on the X/Z axes, so a
    // too-high slab can't be walked up onto and a body can't wedge into a slab edge mid-jump.
    bool overlapsPlatformBand(Vec3 feetPos, f32 halfWidth, const LevelGrid& grid);
```

`src/world/collision.cpp`:

**(a)** Near the top (after the includes) pin the tolerance pair:

```cpp
// The grid's story selector and the collision step gate must agree on one number, or a slab you
// can step onto could still read as the ground story (or vice versa) for the body stepping on it.
static_assert(PLATFORM_STEP_TOLERANCE == STEP_UP_HEIGHT,
              "PLATFORM_STEP_TOLERANCE must track Collision::STEP_UP_HEIGHT");
```

**(b)** After `onJumpPad` add the band test:

```cpp
bool Collision::overlapsPlatformBand(Vec3 feetPos, f32 halfWidth, const LevelGrid& grid) {
    const f32 minX = feetPos.x - halfWidth, maxX = feetPos.x + halfWidth;
    const f32 minZ = feetPos.z - halfWidth, maxZ = feetPos.z + halfWidth;
    s32 cx0, cx1, cz0, cz1;
    cellRange(minX, maxX, grid.cellSize, cx0, cx1);
    cellRange(minZ, maxZ, grid.cellSize, cz0, cz1);
    for (s32 cz = cz0; cz <= cz1; cz++) {
        for (s32 cx = cx0; cx <= cx1; cx++) {
            if (!LevelGridSystem::hasPlatform(grid, (u32)cx, (u32)cz)) continue;
            const f32 top   = LevelGridSystem::getPlatformTop(grid, (u32)cx, (u32)cz);
            const f32 under = LevelGridSystem::getPlatformUnderside(grid, (u32)cx, (u32)cz);
            if (feetPos.y < top - STEP_UP_HEIGHT &&                 // not stepping onto it
                feetPos.y + PLAYER_HEIGHT > under + 0.001f)          // and poking into the band
                return true;
        }
    }
    return false;
}
```

**(c)** In BOTH `moveAndSlide` overloads (the two deliberately mirror each other — keep them in
lockstep), extend the X and Z axis checks. Grid-only overload:

```cpp
    if (overlapsGrid(tryPos, grid) || overlapsLedgeAbove(tryPos, PLAYER_HALF_WIDTH, grid) ||
        overlapsPlatformBand(tryPos, PLAYER_HALF_WIDTH, grid)) {
```

Obstacle overload (both axes):

```cpp
    if (overlapsWorld(tryPos, grid, obstacles, obstacleCount) ||
        overlapsLedgeAbove(tryPos, PLAYER_HALF_WIDTH, grid) ||
        overlapsPlatformBand(tryPos, PLAYER_HALF_WIDTH, grid)) {
```

**(d)** In BOTH overloads, make the Y phase story-aware. Right after `player.onGround = false;`
capture the story key:

```cpp
    // Story selection key: the PRE-move feet height. The post-move Y must never pick the story —
    // a fast fall can cross the slab top within one tick and would then read as "under",
    // tunneling the body down through the walkway.
    const f32 preFeetY = player.position.y;
```

In the falling/landing branch, replace the floor read:

```cpp
                        f32 fh = LevelGridSystem::effectiveFloorHeight(grid, (u32)cx, (u32)cz, preFeetY);
```

In the `else` (no solid overlap) branch, BEFORE `player.position.y = tryPos.y;`, add the underside
clamp:

```cpp
        if (delta.y > 0.0f) {
            // Platform underside head clamp. Open cells have never collided with their real
            // ceilings (a 0.8 m jump can't reach one) and that legacy stays frozen — but a slab
            // underside MUST stop a rising body (a 17 m/s pad launch under a 3 m balcony), or it
            // tunnels up through the walkway. Only slabs the body STARTED fully below count, so a
            // body already standing on a slab is never yanked beneath it.
            f32 hMinX, hMaxX, hMinY, hMaxY, hMinZ, hMaxZ;
            playerAABB(tryPos, hMinX, hMaxX, hMinY, hMaxY, hMinZ, hMaxZ);
            s32 hx0, hx1, hz0, hz1;
            cellRange(hMinX, hMaxX, grid.cellSize, hx0, hx1);
            cellRange(hMinZ, hMaxZ, grid.cellSize, hz0, hz1);
            for (s32 cz = hz0; cz <= hz1; cz++) {
                for (s32 cx = hx0; cx <= hx1; cx++) {
                    if (!LevelGridSystem::hasPlatform(grid, (u32)cx, (u32)cz)) continue;
                    const f32 under = LevelGridSystem::getPlatformUnderside(grid, (u32)cx, (u32)cz);
                    if (preFeetY + PLAYER_HEIGHT <= under + 0.001f &&   // started below the slab
                        tryPos.y + PLAYER_HEIGHT > under) {             // would poke into it
                        tryPos.y          = under - PLAYER_HEIGHT;
                        player.velocity.y = 0.0f;
                    }
                }
            }
        }
```

In the floor-height snap block, replace the floor read the same way:

```cpp
                f32 fh = LevelGridSystem::effectiveFloorHeight(grid, (u32)cx, (u32)cz, preFeetY);
```

The jump-pad launch at the very end is untouched (`onJumpPad` reads base-floor pads only —
authoring rule: pads never sit ON a slab or UNDER one; a violated under-slab pad would just
bounce the body gently against the underside forever, harmless but silly).

- [ ] **Step 4: Run to verify pass**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Platform collision*"
```
Expected: 5 PASS. Then the full suite — the jump-pad and push tests in
`test_collision_push.cpp` must stay green: `./build/tests/dungeon_tests`.

- [ ] **Step 5: Commit**

```bash
git add src/world/collision.h src/world/collision.cpp tests/world/test_platform.cpp
git commit -m "feat(world): two-story collision — land on / walk under / head-bonk CELL_PLATFORM slabs"
```

---

### Task 3: Raycast — the slab is solid from every side

**Files:**
- Modify: `src/world/raycast.cpp` (`tryFloorCeil` lambda ~line 53, DDA loop ~line 124)
- Test: `tests/world/test_platform.cpp` (append)

- [ ] **Step 1: Write the failing tests** — append:

```cpp
TEST_CASE("Platform raycast: the slab is solid from every side, transparent past it") {
    BalconyRoom room;

    SUBCASE("downward ray hits the slab TOP, not the ground floor beneath it") {
        RayHit h = Raycast::cast(room.grid, {5.5f, 4.5f, 1.5f}, {0.0f, -1.0f, 0.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.position.y == doctest::Approx(3.0f));
        CHECK(h.normal.y == doctest::Approx(1.0f));
    }
    SUBCASE("upward ray from the arcade hits the UNDERSIDE") {
        RayHit h = Raycast::cast(room.grid, {5.5f, 0.5f, 1.5f}, {0.0f, 1.0f, 0.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.position.y == doctest::Approx(2.5f));
        CHECK(h.normal.y == doctest::Approx(-1.0f));
    }
    SUBCASE("horizontal ray at slab height hits the RIM") {
        RayHit h = Raycast::cast(room.grid, {5.5f, 2.75f, 5.5f}, {0.0f, 0.0f, -1.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.position.z == doctest::Approx(3.0f));    // front edge of the band (cells z=1..2)
        CHECK(h.normal.z == doctest::Approx(1.0f));
    }
    SUBCASE("horizontal ray UNDER the slab passes beneath, hits the far wall") {
        RayHit h = Raycast::cast(room.grid, {5.5f, 1.5f, 5.5f}, {0.0f, 0.0f, -1.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.position.z == doctest::Approx(1.0f));    // the north border wall, 2 cells past the band edge
    }
    SUBCASE("horizontal ray ABOVE the slab passes over, hits the far wall") {
        RayHit h = Raycast::cast(room.grid, {5.5f, 3.5f, 5.5f}, {0.0f, 0.0f, -1.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.position.z == doctest::Approx(1.0f));
    }
    SUBCASE("balcony sniper: a down-angled shot over the edge reaches the pit floor") {
        // Eye above the band firing south-down into the room — must clear its own slab edge.
        Vec3 d = normalize(Vec3{0.0f, -0.8f, 1.0f});
        RayHit h = Raycast::cast(room.grid, {5.5f, 4.6f, 2.5f}, d, 20.0f);
        REQUIRE(h.hit);
        CHECK(h.normal.y == doctest::Approx(1.0f));
        CHECK(h.position.y == doctest::Approx(0.0f));    // ground story, out in the pit
        CHECK(h.position.z > 3.0f);
    }
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Platform raycast*"
```
Expected: FAIL — "downward" hits y 0.0, "upward" misses, "rim" hits the far wall instead.

- [ ] **Step 3: Implement** in `src/world/raycast.cpp`:

**(a)** Inside the `tryFloorCeil` lambda, the slab planes go FIRST in each direction branch —
they are the nearer plane whenever both are crossable (origin above the top ⇒ top before floor;
origin below the underside ⇒ underside before ceiling). After `if (dir.y < 0.0f) {`:

```cpp
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
```

And after `} else { // dir.y > 0 — ceiling`:

```cpp
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
```

**(b)** In the DDA loop, right after the `isSolid` wall-hit block (before the "Empty cell —
floor/ceiling crossing" step), add the rim:

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

(No starting-cell rim case is needed: an origin already inside the band is inside solid matter,
which collision prevents; a ray from inside escapes through the base-floor/ceiling tests.)

- [ ] **Step 4: Run to verify pass**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*Platform raycast*"
```
Expected: 6 subcases PASS. Full suite green (`test_raycast.cpp`, `test_ray_targeting.cpp`,
`test_teleport_dest.cpp` all exercise `Raycast::cast` on platform-free grids — they must not
notice). **Note for the reviewer:** this task alone fixes meteor targeting on balconies —
`fireMeteorStrike` (`skill_legendary.cpp:196`) takes its impact point from this raycast, so a
meteor aimed at a balcony player now detonates ON the balcony. No meteor-side change needed.

- [ ] **Step 5: Commit**

```bash
git add src/world/raycast.cpp tests/world/test_platform.cpp
git commit -m "feat(world): raycast knows CELL_PLATFORM slabs — top/underside/rim planes for all LOS+hitscan+projectiles"
```

---

### Task 4: Mesher — draw the slab

**Files:**
- Modify: `src/world/level_mesh.cpp` (open-cell branch of `buildSection`, after the ceiling-quad
  block ~line 342)

No unit test — `MeshSystem::create` needs a GL context; verification is the Task 6 visual pass.

- [ ] **Step 1: Implement.** Inside the `else` (open cell) branch, AFTER the `if (cell.flags &
CELL_CEILING)` block, add:

```cpp
                // Platform slab (CELL_PLATFORM): the second story. Top drawn like a floor quad at
                // the slab top, underside like a ceiling quad at the underside (skipped when the
                // clamp left no under-space — low stair steps), rim faces toward every neighbour
                // that doesn't cover our band. Ownership mirrors the riser faces: each cell draws
                // only the part of its own rim a neighbour leaves exposed, so shared faces emit
                // exactly once and stair runs show just their 0.25 m step slivers.
                if (cell.flags & CELL_PLATFORM) {
                    const f32 topH = LevelGridSystem::getPlatformTop(grid, x, z);
                    const f32 undH = LevelGridSystem::getPlatformUnderside(grid, x, z);
                    MaterialBucket* pbkt = getBucket(cell.platMaterialId);
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
                    {   // rim faces — same quad construction as the riser faces above
                        MaterialBucket* rbkt = getBucket(cell.wallMaterialId);
                        static const s32 kpdx[4] = {1, -1, 0, 0};
                        static const s32 kpdz[4] = {0, 0, 1, -1};
                        for (int ei = 0; ei < 4; ei++) {
                            s32 nx = (s32)x + kpdx[ei], nz = (s32)z + kpdz[ei];
                            if (!LevelGridSystem::isInBounds(grid, (u32)nx, (u32)nz)) continue;
                            if (LevelGridSystem::isSolid(grid, (u32)nx, (u32)nz)) continue; // wall face covers it
                            f32 B = undH, T = topH;
                            if (LevelGridSystem::hasPlatform(grid, (u32)nx, (u32)nz)) {
                                const f32 nbTop = LevelGridSystem::getPlatformTop(grid, (u32)nx, (u32)nz);
                                if (nbTop >= topH - 0.001f) continue;   // neighbour covers our whole band
                                if (nbTop > B) B = nbTop;               // stair sliver: only the exposed part
                            }
                            if (T <= B + 0.001f) continue;
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
                        }
                    }
                }
```

- [ ] **Step 2: Build both targets clean**

```bash
cmake --build build 2>&1 | tail -3 && ./build/tests/dungeon_tests | tail -2
```
Expected: builds with zero new warnings; full suite green (mesher isn't linked into tests).

- [ ] **Step 3: Commit**

```bash
git add src/world/level_mesh.cpp
git commit -m "feat(world): mesher draws CELL_PLATFORM slabs — top, underside, and owned rim faces"
```

---

### Task 5: Teleport/dash destinations resolve at the victim's story

**Files:**
- Modify: `src/game/teleport_dest.cpp:73-77`
- Test: `tests/game/test_teleport_dest.cpp` (append)

- [ ] **Step 1: Write the failing test** — append to `tests/game/test_teleport_dest.cpp` (reuse
that file's existing includes/fixtures; if it has no platform grid, build one inline):

```cpp
TEST_CASE("TeleportDest: a blink to a balcony target lands ON the balcony story") {
    // 12x12 room with a 3.0 m platform band along the north wall (z=1..2) — caster and victim
    // both stand on it. The landing Y must be the SLAB top, not the ground floor beneath.
    LevelGrid g;
    LevelGridSystem::init(g, 12, 12, 1.0f);
    for (u32 z = 0; z < 12; z++)
        for (u32 x = 0; x < 12; x++) {
            GridCell& c = g.cells[z * 12 + x];
            const bool border = (x == 0 || z == 0 || x == 11 || z == 11);
            c.flags = border ? CELL_SOLID : CELL_FLOOR;
            c.floorHeight = 0; c.ceilingHeight = 20;
        }
    for (u32 z = 1; z <= 2; z++)
        for (u32 x = 1; x <= 10; x++) {
            GridCell& c = g.cells[z * 12 + x];
            c.flags = static_cast<u8>(CELL_FLOOR | CELL_PLATFORM);
            c.platHeight = 12;
        }
    static EntityPool pool;                          // zero entities — nobody blocks the landing
    Vec3 dest = Teleport::resolveDest(g, pool, {3.5f, 3.0f, 1.5f}, {8.5f, 3.0f, 1.5f});
    CHECK(dest.y == doctest::Approx(3.0f));
    LevelGridSystem::shutdown(g);
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*TeleportDest*"
```
Expected: the new case FAILS with `dest.y == 0.0` (raw `getFloorHeight`); existing cases pass.

- [ ] **Step 3: Implement** — in `src/game/teleport_dest.cpp` replace the landing-height lines:

```cpp
        // Land on the destination cell's floor — STORY-AWARE: resolved against the height the
        // TARGET occupies (desired.y is the victim's feet for closing skills like Shadow Dance),
        // so a blink to a balcony player lands ON the balcony beside them, never on the ground
        // story underneath. Platform-free cells behave exactly as before.
        u32 gx, gz;
        if (LevelGridSystem::worldToGrid(grid, p, gx, gz))
            p.y = LevelGridSystem::effectiveFloorHeight(grid, gx, gz, desired.y);
```

- [ ] **Step 4: Run to verify pass** — same command; all `TeleportDest` cases green.

- [ ] **Step 5: Commit**

```bash
git add src/game/teleport_dest.cpp tests/game/test_teleport_dest.cpp
git commit -m "fix(game): teleport/dash destinations resolve at the victim's story on two-story maps"
```

---

### Task 6: The Combat Hall arena (44×44 rebuild)

**Files:**
- Modify: `src/engine/engine_arena.cpp` (header comment lines 1-19, constants block lines 38-57,
  `buildArenaLevel` lines 61-177)

Everything below `buildArenaLevel` (deathmatch loop, respawn, events) is untouched —
`kArenaPads` keeps its name/shape and `padYawToCenter` derives from `ARENA_W/D`.

- [ ] **Step 1: Update constants + spawn pads** — replace the anonymous-namespace block:

```cpp
// Arena layout constants — one place, shared by build + spawn placement so they can't drift.
namespace {
    constexpr u32 ARENA_W = 44, ARENA_D = 44;
    constexpr f32 ARENA_CS = 1.0f;

    // Spawn pads live in the ARCADE — the covered ground story under the perimeter balcony — one
    // per wall, rotationally symmetric ((x,z) -> (43-z, x)), each tucked beside a support column
    // and near a corner stairwell: you respawn in cover, out of every balcony sightline, with the
    // stairs and the pit both a few steps away.
    constexpr Vec3 kArenaPads[MAX_PLAYERS] = {
        { 1.5f, 0.0f, 10.5f},   // west arcade,  beside the column at (2,10)
        {33.5f, 0.0f,  1.5f},   // north arcade, beside the column at (33,2)
        {42.5f, 0.0f, 33.5f},   // east arcade,  beside the column at (41,33)
        {10.5f, 0.0f, 42.5f},   // south arcade, beside the column at (10,41)
    };

    // Yaw that faces the arena center from a pad. Forward is {-sin(yaw), 0, -cos(yaw)}
    // (the engine-wide convention), so yaw = atan2(-dx, -dz).
    f32 padYawToCenter(const Vec3& pad) {
        f32 dx = (ARENA_W * 0.5f) * ARENA_CS - pad.x;
        f32 dz = (ARENA_D * 0.5f) * ARENA_CS - pad.z;
        return std::atan2(-dx, -dz);
    }
}
```

- [ ] **Step 2: Rebuild `buildArenaLevel`.** Keep the init call, material lookups, base-floor
loop, and the existing `solid`/`raise`/`pad`/`ramp` lambdas exactly as they are, with ONE change
in the base loop — the taller walls:

```cpp
                c.ceilingHeight   = 20;   // 5 m walls: a balcony jump (3.0 + 0.8 m) cannot clear them
```

Then REPLACE everything between the lambdas and the mesh build (the old tier/pad/crate section)
with:

```cpp
    // Mark a block as PLATFORM SLABS: a second story floating topQ*0.25 m over the cell's normal
    // ground floor, which stays walkable beneath (the arcade). Plank walkway, stone rim faces.
    auto plat = [&](u32 sx, u32 sz, u32 w, u32 d, u8 topQ) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags           = static_cast<u8>(CELL_FLOOR | CELL_PLATFORM);
                c.platHeight      = topQ;
                c.platMaterialId  = plank;
                c.floorMaterialId = sand;    // the arcade ground beneath stays sand
                c.wallMaterialId  = stone;
            }
    };
    // Rotate a cell k*90° about the arena centre (one turn: (x,z) -> (W-1-z, x)). Building all
    // four corners/walls from ONE template through this guarantees perfect 4-fold symmetry — no
    // spawn corner ever faces different geometry.
    auto rotCell = [&](u32 x, u32 z, u32 k, u32& ox, u32& oz) {
        ox = x; oz = z;
        for (u32 i = 0; i < k; i++) { u32 t = ox; ox = ARENA_W - 1 - oz; oz = t; }
    };

    // --- SECOND STORY: the perimeter SNIPER BALCONY @ 3.0 m (the Combat-Hall signature) --------
    // A 2-cell walkway hugging every wall; open inner edge (drop off / fire into the pit
    // anywhere), covered arcade beneath (underside 2.5 m — 0.7 m of headroom over a body).
    plat(1,  1, 42,  2, 12);   // north band (z 1..2)
    plat(1, 41, 42,  2, 12);   // south band
    plat(1,  3,  2, 38, 12);   // west band  (x 1..2)
    plat(41, 3,  2, 38, 12);   // east band

    // Corner STAIRWELLS: an L-switchback of graduated slabs (0.25 m steps — walkable under
    // STEP_UP_HEIGHT), arcade -> balcony, overwriting band cells. The quiet route up; the pads
    // below are the fast, loud one. Both lanes of each leg step together.
    for (u32 k = 0; k < 4; k++) {
        u32 ox, oz;
        for (u32 i = 0; i < 6; i++)                       // leg A: h 0.25..1.5 m
            for (u32 lane = 1; lane <= 2; lane++) {
                rotCell(8 - i, lane, k, ox, oz);
                plat(ox, oz, 1, 1, static_cast<u8>(1 + i));
            }
        for (u32 cx2 = 1; cx2 <= 2; cx2++)                // corner landing @ 1.5 m
            for (u32 cz2 = 1; cz2 <= 2; cz2++) {
                rotCell(cx2, cz2, k, ox, oz);
                plat(ox, oz, 1, 1, 6);
            }
        for (u32 i = 0; i < 6; i++)                       // leg B: h 1.75..3.0 m, meets the band
            for (u32 lane = 1; lane <= 2; lane++) {
                rotCell(lane, 3 + i, k, ox, oz);
                plat(ox, oz, 1, 1, static_cast<u8>(7 + i));
            }
    }

    // Support COLUMNS: full-height solid pillars on the balcony's inner-edge row — arcade cover
    // below, pillars to strafe around above (the walkway narrows to one cell at each), and the
    // structure that visually carries the slab. Mirror-symmetric pairs (10,33) and (16,27).
    {
        static constexpr u32 kColX[4] = {10, 16, 27, 33};
        for (u32 k = 0; k < 4; k++)
            for (u32 ci = 0; ci < 4; ci++) {
                u32 ox, oz;
                rotCell(kColX[ci], 2, k, ox, oz);
                solid(ox, oz, 1, 1, stone);
            }
    }

    // Wall-midpoint JUMP PADS: pit -> balcony (launch apex 3.6 m; air-steer onto the 3.0 m band
    // edge). Never ON or UNDER a slab — a pad launch must own its full arc.
    pad(21,  4, 2, 2, 0); pad(38, 21, 2, 2, 0);
    pad(21, 38, 2, 2, 0); pad( 4, 21, 2, 2, 0);

    // --- CENTER: tower + crown (solid-riser tiers, as before, shifted to the 44x44 centre).
    // The crown now sits at BALCONY height so the two commanding vantages duel across the map.
    raise(19, 19, 6, 6, 6);            // tower @ 1.5 m, reached by the four ramps
    pad(20, 20, 4, 4, 6);              // crown launch-ring (12 pad cells after the crown overwrite)
    raise(21, 21, 2, 2, 12);           // crown @ 3.0 m — level with the sniper balcony
    ramp(25, 21,  1,  0, 0,  1, 6, 6); // east  (x 25..30)
    ramp(18, 21, -1,  0, 0,  1, 6, 6); // west  (x 18..13)
    ramp(21, 18,  0, -1, 1,  0, 6, 6); // north (z 18..13)
    ramp(21, 25,  0,  1, 1,  0, 6, 6); // south (z 25..30)

    // --- Pit cover: one crate cluster per diagonal quadrant (mirror pairs 10 <-> 32) -----------
    solid(10, 10, 2, 2, plank); solid(32, 10, 2, 2, plank);
    solid(10, 32, 2, 2, plank); solid(32, 32, 2, 2, plank);
```

- [ ] **Step 3: Rewrite the file-top layout comment** (lines 8-19) to match:

```cpp
// The layout is a two-story Quake / Metroid-Prime-Hunters COMBAT HALL (44x44, 4-fold rotational
// symmetry — every spawn corner faces identical geometry):
//   - TIER 0 (ground): the open pit with crate cover, wall-midpoint jump pads, and the covered
//     perimeter ARCADE under the balcony (spawn bays live here, in cover, out of sniper LOS).
//   - TIER 1 (1.5 m): the central tower, reached by four cardinal LEDGE ramps.
//   - TIER 2 (3.0 m): TWO dueling vantages — the perimeter SNIPER BALCONY (CELL_PLATFORM slabs:
//     stand on it, walk under it; open inner edge to drop/fire from; corner slab stairwells and
//     the midpoint pads to get up) and the tower's crown at the same height across the map.
// Verticality rides three opt-in cell flags: CELL_LEDGE (jump-gated risers), CELL_JUMPPAD
// (launch pads), CELL_PLATFORM (walk-under slabs). All are deterministic level geometry built
// from the seed on every peer, so co-op needs NO wire change (posY + onGround are snapshotted).
```

- [ ] **Step 4: Build + SP visual verification**

```bash
cmake --build build && ./build/dungeon_game --arena
```

Checklist (F2 noclip to inspect, F10 hides HUD):
1. Balcony ring renders: plank top, underside visible from the arcade, stone rims, columns meet
   the slab. No missing faces, no z-fighting, no gaps at stair steps.
2. Climb a corner stairwell to the top — smooth 0.25 m steps, no snap-up from the arcade.
3. Walk the full balcony ring; squeeze past each column; drop off the inner edge anywhere.
4. Walk the arcade beneath — no head clipping, no hoisting onto the slab.
5. Stand on a midpoint pad — launch arcs you onto the balcony edge with air-steer.
6. From the balcony, fire down into the pit — impact FX land on the sand past the open edge, and
   a shot aimed at the walkway itself impacts ON the slab. From the arcade, fire straight up —
   impact on the underside, never on the sky.
7. Jump under the balcony — head bonks at 2.5 m, no tunneling.
8. Spawn view: you start under the balcony beside a column, facing the center.
9. Pause → Leave Arena → menu works as before.
10. Perf: the 1 Hz stats log stays within the 300–500 draw-call budget (slab quads join existing
    section submeshes — expect more triangles, not more draw calls) and 60 FPS holds.

- [ ] **Step 5: Co-op verification** (the real test — reconciliation over the slab):

```bash
./build/dungeon_game --arena &
./build/dungeon_game --join 127.0.0.1 --net-loss 15 --net-latency 100
```
- Client climbs the stairs and walks the balcony: **no rubber-banding** (F9 net-graph, 1 Hz
  `[NET-GRAPH]` log: 0 hard snaps).
- Client pad-launches onto the balcony while the host watches: the arc replicates.
- Host on the balcony, client in the arcade below: neither can shoot the other through the slab;
  over the open edge both can.
- PvP kill over the balcony edge credits and respawns normally.

- [ ] **Step 6: Commit**

```bash
git add src/engine/engine_arena.cpp
git commit -m "feat(arena): 44x44 two-story Combat Hall — perimeter sniper balcony, arcade, corner stairwells"
```

---

### Task 7: Doc sync + full verification

**Files:**
- Modify: `CLAUDE.md` (Arena mode paragraph)
- Modify: `.claude/skills/engine-reference/SKILL.md` (cell-flags/constants area)
- Modify: `.claude/skills/engine-how-to/SKILL.md` (vertical-variety pitfall section)

- [ ] **Step 1: CLAUDE.md** — in the **Arena mode (PvP)** paragraph, replace the layout sentences
(the "Quake / Metroid-Prime-Hunters Combat-Hall-style 3-tier map" description through the
`CELL_LEDGE`/`CELL_JUMPPAD` sentence) with:

> The layout is a **two-story Quake / MPH Combat Hall** (44×44, 4-fold symmetric): a ground pit
> (crates, wall-midpoint jump pads, central 1.5 m tower via four ramps) under TWO dueling 3.0 m
> vantages — a **perimeter sniper balcony** you stand on AND walk under (covered arcade beneath
> holds the spawn bays; corner slab **stairwells** + pads go up; open inner edge to drop/fire),
> and the tower's crown at the same height. Verticality rides three opt-in cell flags —
> `CELL_LEDGE` (jump-gated risers), `CELL_JUMPPAD` (launch pads), and **`CELL_PLATFORM`**
> (real walk-under second-story slabs: story-selecting collision via
> `LevelGridSystem::effectiveFloorHeight`, slab-aware `Raycast::cast` so nothing shoots through a
> balcony floor, mesher top/underside/rim quads). All three are deterministic seed-built geometry,
> so they replicate in co-op with **no wire change / no PROTOCOL bump**.

- [ ] **Step 2: engine-reference** — in the cell-flags/`LevelGrid` row of the type cheat sheet
add `CELL_PLATFORM` beside the existing flags, and in the vertical-level-cells paragraph (near
`JUMP_SPEED`/`CELL_JUMPPAD`) append:

> `CELL_PLATFORM` (flag bit 5) is the real two-story cell: `GridCell.platHeight` (quarter-units)
> is the slab TOP, thickness `PLATFORM_THICKNESS_Q` (=2 qu, 0.5 m), underside clamped to the base
> floor. The cell's `floorHeight` remains the walkable GROUND story beneath. Story selection is
> `LevelGridSystem::effectiveFloorHeight(grid,x,z,feetY)` (slab top iff feet within
> `PLATFORM_STEP_TOLERANCE` = `STEP_UP_HEIGHT` = 0.4 m below it — static_assert-pinned in
> collision.cpp). Consumers: `Collision::moveAndSlide` (story-aware landing/snap keyed on
> PRE-move feet Y, `overlapsPlatformBand` XZ gate, underside head clamp), `Raycast::cast`
> (top/underside planes + rim), the mesher (top/underside/owned-rim quads),
> `Teleport::resolveDest` (lands at the victim's story). AI/pathfinding read only the base floor
> — enemies walk under platforms; the second story is PvP-only, like pads/ledges.

- [ ] **Step 3: engine-how-to** — extend the "Adding vertical variety" pitfall with:

> **Two-story cells (`CELL_PLATFORM`) — the rules that keep them honest.** (1) A platform cell
> must keep `CELL_FLOOR` — its ground story is real and walkable; setting `CELL_LEDGE` on it
> would wall off the arcade beneath. (2) Adjacent slab tops may differ by at most 1 qu (0.25 m)
> where players walk between them — a bigger step reads as a wall (`overlapsPlatformBand`).
> Slab STAIRS are just graduated `platHeight` runs. (3) **Never put a `CELL_JUMPPAD` on or under
> a slab** — on top it launches over the arena walls; underneath it bounces the body against the
> underside forever. (4) Any code that means "the floor under THIS body" must call
> `effectiveFloorHeight(x,z,feetY)`, never `getFloorHeight` — the raw getter is the ground story
> and will teleport-snap a balcony walker down (or an arcade walker up). (5) World-item ground
> snapping and enemy AI still read the base floor by design — don't put loot or PvE fights on
> platforms until those consumers are converted.

- [ ] **Step 4: Full verification**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel 2>&1 | tail -3
```
Expected: full suite green, Release builds clean with zero new warnings.

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md .claude/skills/engine-reference/SKILL.md .claude/skills/engine-how-to/SKILL.md \
        docs/superpowers/specs/2026-07-17-two-story-platform-maps-design.md \
        docs/superpowers/plans/2026-07-18-two-story-platform-maps.md
git commit -m "docs: two-story CELL_PLATFORM cells + Combat Hall arena — spec, plan, skills, CLAUDE.md"
```

---

## Post-plan notes (parked, from the spec)

- Balcony-camping balance knobs: column spacing (`kColX`), stairwell count, crown duel line —
  playtest, then tune constants in `engine_arena.cpp` only.
- `upper-solid` flag (independent upper-story walls) and dungeon-gen adoption (bridges,
  galleries): additive follow-ups; nothing here blocks them.
- World-item + enemy-AI platform awareness: required before any PvE/loot use of platforms.
