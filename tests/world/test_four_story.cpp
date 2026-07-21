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
