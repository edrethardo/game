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
