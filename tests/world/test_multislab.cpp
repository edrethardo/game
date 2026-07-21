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
