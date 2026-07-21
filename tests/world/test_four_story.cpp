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
                    const bool wideX = !hasSlabAt(g, x - 1, z, lvM[L]) || !hasSlabAt(g, x + 1, z, lvM[L]);
                    const bool wideZ = !hasSlabAt(g, x, z - 1, lvM[L]) || !hasSlabAt(g, x, z + 1, lvM[L]);
                    CHECK(wideX);
                    CHECK(wideZ);
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
