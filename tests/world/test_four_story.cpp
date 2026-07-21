// test_four_story.cpp — FOUR_STORY "Descent" generator invariants. A plain dungeon floor stacked FOUR
// walkable stories deep on one footprint (L0 ground + three CELL_PLATFORM slabs @ 3/6/9 m), traversed by a
// one-way, drop-only descent through OFFSET holes: spawn L3, fall to the L0 exit, never climb. These pin
// the structural contract — per-level holes, adjacent-level quadrant disjointness (so a dive lands exactly
// one story down), an L3→L0 descent path, a real L3 spawn slab, and the sub-40 BSP fallback — so a
// regression can't ship an express shaft, an unreachable exit, or a host/client grid mismatch.

#include "doctest/doctest.h"
#include "world/level_gen.h"
#include "world/level_grid.h"
#include "world/collision.h"   // JUMPPAD_LAUNCH, the default the maze pads must beat

#include <cmath>
#include <cstring>
#include <vector>

TEST_CASE("FOUR_STORY: type + styleName wiring") {
    CHECK(LevelGen::LayoutStyle::FOUR_STORY < LevelGen::LayoutStyle::COUNT);
    CHECK(std::strcmp(LevelGen::styleName(LevelGen::LayoutStyle::FOUR_STORY), "descent") == 0);
    DungeonResult r{};
    CHECK(r.dropHoleCount == 0);
    CHECK(DungeonResult::MAX_DROP_HOLES == 64);
}

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
            if (LevelGridSystem::isSolid(g, (u32)nx, (u32)nz)) continue;   // the maze's walls
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

TEST_CASE("FOUR_STORY: every upper level records a way down, and every recorded hole is unjumpable") {
    for (u32 seed : {1u, 99u, 4242u}) {
        LevelGrid g;
        LevelGridSystem::init(g, 48, 48, 1.0f);
        DungeonResult r = LevelGen::generate(g, seed, 48, 48, LevelGen::LayoutStyle::FOUR_STORY);
        CAPTURE(seed);
        u32 per[3] = {0, 0, 0};                       // recorded holes at L1 / L2 / L3
        for (u8 i = 0; i < r.dropHoleCount; i++) {
            const f32 y = r.dropHoles[i].surfaceY;
            const u32 lv = (y > 8.0f) ? 2u : (y > 5.0f) ? 1u : 0u;
            per[lv]++;
            // A recorded hole is a COMMITTED descent: >= 2 cells across in BOTH axes. At the 6 m/s
            // base speed a jump reaches 2.4 m and a 0.6 m body needs gap + 0.6, so a 2 m gap cannot
            // be cleared. (1-cell JUMP gaps are deliberately not recorded — they are hazards, not
            // ways down.) Probe outward from the hole centre for missing slab on both axes.
            u32 hx, hz;
            REQUIRE(LevelGridSystem::worldToGrid(g, r.dropHoles[i].pos, hx, hz));
            const f32 top = r.dropHoles[i].surfaceY;
            u32 spanX = 0, spanZ = 0;
            for (u32 x = 1; x < g.width - 1; x++)  if (!hasSlabAt(g, x, hz, top) && !LevelGridSystem::isSolid(g, x, hz)) { if (x >= hx - 2 && x <= hx + 2) spanX++; }
            for (u32 z = 1; z < g.depth - 1; z++)  if (!hasSlabAt(g, hx, z, top) && !LevelGridSystem::isSolid(g, hx, z)) { if (z >= hz - 2 && z <= hz + 2) spanZ++; }
            CHECK(spanX >= 2);
            CHECK(spanZ >= 2);
        }
        CHECK(per[0] > 0);   // L1 must record a way down to L0, or the last descent is unseatable
        CHECK(per[1] > 0);
        CHECK(per[2] > 0);
        LevelGridSystem::shutdown(g);
    }
}

TEST_CASE("FOUR_STORY: the floor is a MAZE, not an open plain") {
    // The guard against the failure this style shipped with first: a technically-valid four-story
    // floor whose every level was one open 46x46 field with ~1% punched out. Structure is content —
    // assert real wall bulk and real corridor bulk, so neither an empty box nor a solid block passes.
    for (u32 seed : {5u, 606u, 90210u}) {
        LevelGrid g;
        LevelGridSystem::init(g, 48, 48, 1.0f);
        LevelGen::generate(g, seed, 48, 48, LevelGen::LayoutStyle::FOUR_STORY);
        CAPTURE(seed);
        u32 interior = 0, solid = 0, pads = 0;
        for (u32 z = 1; z < g.depth - 1; z++)
            for (u32 x = 1; x < g.width - 1; x++) {
                interior++;
                if (LevelGridSystem::isSolid(g, x, z)) solid++;
                else if (LevelGridSystem::getCell(g, x, z).flags & CELL_JUMPPAD) pads++;
            }
        CHECK(solid > interior / 5);           // >20% wall: a real labyrinth, not a field
        CHECK(solid < (interior * 3) / 5);     // <60% wall: still a floor you can walk
        CHECK(pads > 0);                       // dead-end jump pads exist
        LevelGridSystem::shutdown(g);
    }
}

TEST_CASE("FOUR_STORY: jump pads are authored stronger than the global default") {
    LevelGrid g;
    LevelGridSystem::init(g, 48, 48, 1.0f);
    LevelGen::generate(g, 0x51EEDu, 48, 48, LevelGen::LayoutStyle::FOUR_STORY);
    u32 checked = 0;
    for (u32 z = 1; z < g.depth - 1 && checked == 0; z++)
        for (u32 x = 1; x < g.width - 1; x++) {
            const GridCell& c = LevelGridSystem::getCell(g, x, z);
            if (!(c.flags & CELL_JUMPPAD)) continue;
            // Per-cell strength, so the Arena/VERTICAL_HALL pads keep their tuned 3.6 m apex while
            // the Descent's climb ~two stories. Apex = v^2 / (2*|GRAVITY|) with GRAVITY -40.
            const f32 v = c.jumpPadQ * 0.25f;
            CHECK(v > JUMPPAD_LAUNCH);
            CHECK((v * v) / 80.0f > 6.0f);     // clears two 3 m stories
            checked++;
            break;
        }
    CHECK(checked == 1);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("FOUR_STORY: no column is open through two stories (max one story per dive)") {
    for (u32 seed : {2u, 808u, 31337u}) {
        LevelGrid g;
        LevelGridSystem::init(g, 48, 48, 1.0f);
        LevelGen::generate(g, seed, 48, 48, LevelGen::LayoutStyle::FOUR_STORY);
        const u32 W = g.width, D = g.depth;
        for (u32 z = 1; z < D - 1; z++)
            for (u32 x = 1; x < W - 1; x++) {
                if (LevelGridSystem::isSolid(g, x, z)) continue;   // a maze wall carries no slabs
                bool m3 = !hasSlabAt(g, x, z, 9.0f), m2 = !hasSlabAt(g, x, z, 6.0f),
                     m1 = !hasSlabAt(g, x, z, 3.0f);
                CAPTURE(x); CAPTURE(z);
                // doctest cannot decompose &&/||, so collapse to a single bool first.
                const bool shaft32 = m3 && m2, shaft21 = m2 && m1;
                CHECK_FALSE(shaft32);   // no column pierces L3 AND L2 (would be a 2-level express shaft)
                CHECK_FALSE(shaft21);   // nor L2 AND L1
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
