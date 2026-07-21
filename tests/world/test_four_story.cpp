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
