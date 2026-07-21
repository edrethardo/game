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
