// test_autoplay_vhall.cpp — the VERTICAL_HALL two-story travel field (game/autoplay_vhall.h).
//
// The field replaced a flat 2D flow field plus a pile of ramp heuristics that could not represent
// two stories and routed a bot that had climbed to a balcony straight back off its edge. The
// properties pinned here are the ones that make a (cell, story) field the right answer: it connects
// a GROUND spawn to an UPPER door via the ramp, and it NEVER connects a balcony cell to the ground
// beside it (so it can never steer the bot off an edge).
//
// Built on a synthetic VHALL-shaped grid: a flat ground plane, a graduated-slab RAMP rising from the
// ground to a 3 m balcony, and a balcony slab holding the door. Same construction pattern as the
// other nav tests (LevelGridSystem::init/shutdown, cells z*width+x).

#include <doctest/doctest.h>
#include "game/autoplay_vhall.h"
#include "world/level_grid.h"

namespace {
// A flat all-ground grid at height 0.
LevelGrid makeGroundGrid(u32 w, u32 d) {
    LevelGrid g;
    LevelGridSystem::init(g, w, d, 1.0f);
    for (u32 z = 0; z < d; z++)
        for (u32 x = 0; x < w; x++) {
            GridCell& c = g.cells[z * w + x];
            c.flags = CELL_FLOOR; c.floorHeight = 0; c.ceilingHeight = 48;
        }
    return g;
}
void addSlab(LevelGrid& g, u32 x, u32 z, u8 topQ) { LevelGridSystem::addPlatform(g.cells[z * g.width + x], topQ, 0); }

// Follow the field from `start` (given its feet height) one cell per step; report whether the chain
// arrives at the door. The property that matters: the route exists and terminates at the goal.
bool fieldReachesDoor(const Autoplay::VHallField& f, const LevelGrid& g, Vec3 start, u32 maxSteps) {
    Vec3 p = start;
    for (u32 i = 0; i < maxSteps; i++) {
        if (Autoplay::atVHallGoal(f, g, p)) return true;
        const Vec3 dir = Autoplay::vhallDirection(f, g, p);
        if (lengthSq(dir) < 1e-6f) return false;
        p = p + dir * g.cellSize;
        // Mimic the physics: the feet settle onto whatever surface the destination cell offers within
        // a step (the ramp/balcony slab if present and reachable, else the ground).
        u32 gx, gz;
        if (LevelGridSystem::worldToGrid(g, p, gx, gz))
            p.y = LevelGridSystem::effectiveFloorHeight(g, gx, gz, p.y);
    }
    return false;
}
} // namespace

TEST_CASE("vhall field: routes a ground spawn up a ramp to an upper-story door") {
    // Layout (20x8): ground everywhere. A ramp climbs along +x at row z=4 from the foot (x=5, slab
    // 0.25 m) to the top (x=16, slab 3 m), each cell 0.25 m higher than the last. A 3 m balcony slab
    // fills x=16..18 at z=4 (the ramp top plus the door tile). The door sits on the balcony at x=18.
    LevelGrid g = makeGroundGrid(20, 8);
    for (u32 x = 5; x <= 16; x++) addSlab(g, x, 4, static_cast<u8>((x - 5 + 1)));   // 1..12 q = 0.25..3 m
    for (u32 x = 16; x <= 18; x++) addSlab(g, x, 4, 12);                            // balcony @ 3 m
    // widen the ramp/balcony to 2 rows so the body has room (z=3 and z=4)
    for (u32 x = 5; x <= 16; x++) addSlab(g, x, 3, static_cast<u8>((x - 5 + 1)));
    for (u32 x = 16; x <= 18; x++) addSlab(g, x, 3, 12);

    const Vec3 door{18.5f, 3.0f, 4.5f};
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, g, door, 1));

    // From a far ground corner the field must produce a route that climbs the ramp and reaches the
    // door — a flat field could never (the door cell's node is 3 m up, disconnected from the ground).
    const Vec3 spawn{2.5f, 0.0f, 1.5f};
    CHECK(lengthSq(Autoplay::vhallDirection(f, g, spawn)) > 1e-6f);   // it has a plan on the ground
    CHECK(fieldReachesDoor(f, g, spawn, 400));

    Autoplay::freeVHallField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("vhall field: a balcony cell never routes toward the open ground beside it") {
    // Standing on the balcony top, the field must point ALONG the balcony toward the door, never off
    // its edge onto the 0 m ground — that off-the-edge route is exactly what the old flat field took.
    LevelGrid g = makeGroundGrid(20, 8);
    for (u32 x = 5; x <= 16; x++) { addSlab(g, x, 4, static_cast<u8>((x - 5 + 1))); addSlab(g, x, 3, static_cast<u8>((x - 5 + 1))); }
    for (u32 x = 16; x <= 18; x++) { addSlab(g, x, 4, 12); addSlab(g, x, 3, 12); }
    const Vec3 door{18.5f, 3.0f, 4.5f};
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, g, door, 1));

    // On the balcony at (17,4) @ 3 m: the heading must have a +x component (toward the door at x=18),
    // and must NOT push the bot off the balcony in +z/-z toward the bare ground rows (z=2 / z=5).
    const Vec3 onBalcony{17.5f, 3.0f, 4.5f};
    const Vec3 dir = Autoplay::vhallDirection(f, g, onBalcony);
    REQUIRE(lengthSq(dir) > 1e-6f);
    CHECK(dir.x > 0.3f);                 // toward the door
    // The next cell it steers to must itself be a balcony (has a slab), never bare ground.
    u32 nx, nz;
    REQUIRE(LevelGridSystem::worldToGrid(g, onBalcony + dir * g.cellSize, nx, nz));
    CHECK(LevelGridSystem::hasPlatform(g, nx, nz));

    Autoplay::freeVHallField(f);
    LevelGridSystem::shutdown(g);
}

TEST_CASE("vhall field: a fresh floor stamp invalidates a stale field") {
    LevelGrid g = makeGroundGrid(12, 12);
    for (u32 x = 3; x <= 8; x++) { addSlab(g, x, 6, static_cast<u8>((x - 3 + 1) * 2)); }   // a ramp
    addSlab(g, 8, 6, 12);
    Autoplay::VHallField f;
    REQUIRE(Autoplay::ensureVHallField(f, g, Vec3{8.5f, 3.0f, 6.5f}, /*stamp=*/1));
    const bool wasValid = f.valid;
    // Same grid, new floor stamp: must rebuild (not silently reuse the old routes). We can only
    // observe that it stays valid and does not crash; the stamp field is what forces the rebuild.
    REQUIRE(Autoplay::ensureVHallField(f, g, Vec3{8.5f, 3.0f, 6.5f}, /*stamp=*/2));
    CHECK(wasValid);
    CHECK(f.stamp == 2);
    Autoplay::freeVHallField(f);
    LevelGridSystem::shutdown(g);
}
