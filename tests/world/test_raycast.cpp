// test_raycast.cpp — floor/ceiling detection in Raycast::cast, especially the ray's STARTING cell.
//
// Regression for "chakrams pass through the floor/ceiling instead of bouncing": the DDA only
// floor/ceiling-tested cells it ADVANCED into, so a short ray that stays inside one cell (a
// projectile's per-tick cast) never produced a {0,±1,0} hit. These cases pin the fixed behavior.

#include "doctest/doctest.h"
#include "world/level_grid.h"
#include "world/raycast.h"

namespace {

// An open w×d room: every cell is floor (non-solid) with a given floor/ceiling height in
// quarter-units (getFloorHeight/getCeilingHeight multiply by 0.25 → metres).
LevelGrid openGrid(u32 w, u32 d, u8 floorQ, u8 ceilQ) {
    LevelGrid g;
    LevelGridSystem::init(g, w, d, 1.0f);
    for (u32 z = 0; z < d; z++)
        for (u32 x = 0; x < w; x++) {
            GridCell& c = LevelGridSystem::getCell(g, x, z);
            c.flags         = CELL_FLOOR;
            c.floorHeight   = floorQ;
            c.ceilingHeight = ceilQ;
        }
    return g;
}

} // namespace

TEST_CASE("Raycast detects floor/ceiling inside the ray's starting cell") {
    // 3×3 open room: floor at y=0, ceiling at y=4 (16 quarter-units). Origin mid-height, center cell.
    LevelGrid g = openGrid(3, 3, 0, 16);
    const Vec3 origin = {1.5f, 2.0f, 1.5f};

    SUBCASE("straight-down ray bounces off the floor (never leaves its cell)") {
        RayHit h = Raycast::cast(g, origin, {0.0f, -1.0f, 0.0f}, 5.0f);
        REQUIRE(h.hit);
        CHECK(h.normal.y == doctest::Approx(1.0f));   // floor normal points up
        CHECK(h.position.y == doctest::Approx(0.0f));
        CHECK(h.distance == doctest::Approx(2.0f));
    }

    SUBCASE("straight-up ray bounces off the ceiling") {
        RayHit h = Raycast::cast(g, origin, {0.0f, 1.0f, 0.0f}, 5.0f);
        REQUIRE(h.hit);
        CHECK(h.normal.y == doctest::Approx(-1.0f));  // ceiling normal points down
        CHECK(h.position.y == doctest::Approx(4.0f));
        CHECK(h.distance == doctest::Approx(2.0f));
    }

    SUBCASE("steep-down ray with a little XZ still hits the floor in-cell") {
        RayHit h = Raycast::cast(g, origin, {0.1f, -1.0f, 0.0f}, 5.0f);
        REQUIRE(h.hit);
        CHECK(h.normal.y == doctest::Approx(1.0f));
    }

    SUBCASE("horizontal eye-height ray gets no floor/ceiling hit in an open room") {
        RayHit h = Raycast::cast(g, origin, {1.0f, 0.0f, 0.0f}, 1.0f);
        CHECK_FALSE(h.hit);
    }
}
