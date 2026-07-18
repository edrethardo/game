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
