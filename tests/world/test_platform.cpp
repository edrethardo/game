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
