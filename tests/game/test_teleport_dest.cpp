// test_teleport_dest.cpp — the shared teleport landing-spot resolver.
//
// Every player blink/dash-warp funnels through Teleport::resolveDest. Before it existed each
// skill validated its own way (thin center ray, one cell-center check, or nothing) and every
// variant could wedge the player permanently: inside a wall the axis-separated moveAndSlide
// blocks all movement forever, and inside a wide enemy the push-out has no direction. These
// cases pin the resolver's contract: never in a wall, never in a body, never through a thin
// wall into a sealed pocket, floor-snapped, and start (a guaranteed-valid no-op) when the
// whole line is blocked.

#include <doctest/doctest.h>
#include "game/teleport_dest.h"
#include "game/entity.h"
#include "world/level_grid.h"

namespace {

LevelGrid openGrid(u32 w, u32 d) {
    LevelGrid g;
    LevelGridSystem::init(g, w, d, 1.0f);
    for (u32 z = 0; z < d; z++)
        for (u32 x = 0; x < w; x++) {
            GridCell& c = LevelGridSystem::getCell(g, x, z);
            c.flags         = CELL_FLOOR;
            c.floorHeight   = 0;
            c.ceilingHeight = 16;
        }
    return g;
}

void solidColumn(LevelGrid& g, u32 x) {
    for (u32 z = 0; z < g.depth; z++)
        LevelGridSystem::getCell(g, x, z).flags = CELL_SOLID;
}

void addBody(EntityPool& pool, Vec3 pos, f32 halfW, u8 extraFlags = 0) {
    u32 idx = pool.activeCount;
    pool.entities[idx] = Entity{};
    Entity& e = pool.entities[idx];
    e.flags       = static_cast<u8>(ENT_ACTIVE | extraFlags);
    e.position    = pos;
    e.halfExtents = {halfW, 0.9f, halfW};
    pool.activeList[pool.activeCount++] = static_cast<u16>(idx);
}

f32 xzDist(Vec3 a, Vec3 b) {
    Vec3 d = a - b; d.y = 0;
    return length(d);
}

} // namespace

TEST_CASE("resolveDest: clear line lands at the desired point, floor-snapped") {
    LevelGrid g = openGrid(10, 3);
    EntityPool pool{};
    Vec3 dest = Teleport::resolveDest(g, pool, {1.5f, 0, 1.5f}, {7.5f, 3.0f, 1.5f});
    CHECK(dest.x == doctest::Approx(7.5f));
    CHECK(dest.z == doctest::Approx(1.5f));
    CHECK(dest.y == doctest::Approx(0.0f));   // snapped to the landing cell's floor
}

TEST_CASE("resolveDest: a desired point inside a wall backs off to a spot the footprint fits") {
    LevelGrid g = openGrid(10, 3);
    solidColumn(g, 6);
    EntityPool pool{};
    Vec3 dest = Teleport::resolveDest(g, pool, {1.5f, 0, 1.5f}, {6.5f, 0, 1.5f});
    // Whole footprint (half-width 0.3) must clear cell x=6 — not just the point.
    CHECK(dest.x < 6.0f - 0.29f);
    CHECK(dest.x > 1.5f);                     // still moved, not a refusal
}

TEST_CASE("resolveDest: never lands inside a body (the dash-to-enemy-center bug)") {
    LevelGrid g = openGrid(10, 3);
    EntityPool pool{};
    const Vec3 bodyPos = {5.0f, 0, 1.5f};
    addBody(pool, bodyPos, 0.8f);             // Butcher-sized
    // Holy Smite's exact failure: desired = the stopped-on enemy's CENTER.
    Vec3 dest = Teleport::resolveDest(g, pool, {1.5f, 0, 1.5f}, bodyPos);
    CHECK(xzDist(dest, bodyPos) >= 0.8f + 0.3f);   // outside body + player half-width
    CHECK(dest.x > 1.5f);                          // landed close, didn't refuse
}

TEST_CASE("resolveDest: UNTARGETABLE entities (drones, cosmetic pets) never block") {
    LevelGrid g = openGrid(10, 3);
    EntityPool pool{};
    addBody(pool, {5.0f, 0, 1.5f}, 0.8f, ENT_UNTARGETABLE);
    Vec3 dest = Teleport::resolveDest(g, pool, {1.5f, 0, 1.5f}, {5.0f, 0, 1.5f});
    CHECK(dest.x == doctest::Approx(5.0f));
}

TEST_CASE("resolveDest: no blinking through a thin wall into a sealed pocket") {
    LevelGrid g = openGrid(12, 3);
    solidColumn(g, 6);                        // one-cell wall; open floor on the far side
    EntityPool pool{};
    Vec3 dest = Teleport::resolveDest(g, pool, {1.5f, 0, 1.5f}, {10.5f, 0, 1.5f});
    // Far-side samples have a clear footprint but no line of sight — must stay near side.
    CHECK(dest.x < 6.0f);
}

TEST_CASE("resolveDest: fully blocked line refuses the movement (returns start)") {
    LevelGrid g = openGrid(3, 3);
    solidColumn(g, 0);
    solidColumn(g, 2);
    for (u32 x = 0; x < 3; x++) {             // only the center cell is open
        LevelGridSystem::getCell(g, x, 0).flags = CELL_SOLID;
        LevelGridSystem::getCell(g, x, 2).flags = CELL_SOLID;
    }
    EntityPool pool{};
    const Vec3 start = {1.5f, 0, 1.5f};
    Vec3 dest = Teleport::resolveDest(g, pool, start, {2.5f, 0, 1.5f});
    CHECK(dest.x == doctest::Approx(start.x));
    CHECK(dest.z == doctest::Approx(start.z));
}

TEST_CASE("resolveDest: lands on the destination cell's floor height") {
    LevelGrid g = openGrid(10, 3);
    for (u32 z = 0; z < 3; z++)
        LevelGridSystem::getCell(g, 7, z).floorHeight = 4;   // 4 quarter-units = 1 m ledge
    EntityPool pool{};
    Vec3 dest = Teleport::resolveDest(g, pool, {1.5f, 0, 1.5f}, {7.5f, 0, 1.5f});
    CHECK(dest.y == doctest::Approx(1.0f));
}

TEST_CASE("TeleportDest: a blink to a balcony target lands ON the balcony story") {
    // 12x12 room with a 3.0 m platform band along the north wall (z=1..2) — caster and victim
    // both stand on it. The landing Y must be the SLAB top, not the ground floor beneath.
    LevelGrid g;
    LevelGridSystem::init(g, 12, 12, 1.0f);
    for (u32 z = 0; z < 12; z++)
        for (u32 x = 0; x < 12; x++) {
            GridCell& c = g.cells[z * 12 + x];
            const bool border = (x == 0 || z == 0 || x == 11 || z == 11);
            c.flags = border ? CELL_SOLID : CELL_FLOOR;
            c.floorHeight = 0; c.ceilingHeight = 20;
        }
    for (u32 z = 1; z <= 2; z++)
        for (u32 x = 1; x <= 10; x++) {
            GridCell& c = g.cells[z * 12 + x];
            c.flags = static_cast<u8>(CELL_FLOOR);
            LevelGridSystem::setPlatform(c, 12, 0);
        }
    EntityPool pool{};                               // zero entities — nobody blocks the landing
    Vec3 dest = Teleport::resolveDest(g, pool, {3.5f, 3.0f, 1.5f}, {8.5f, 3.0f, 1.5f});
    CHECK(dest.y == doctest::Approx(3.0f));
    LevelGridSystem::shutdown(g);
}

// A dash must never land the caster INSIDE the low end of a VERTICAL_HALL ramp. Reported live:
// "the paladin can get stuck in the stairs with holy smite". A dash direction is flattened to XZ, so
// `desired.y` is the caster's own feet height; the story selector then resolves the ramp cell to the
// GROUND story beneath the slab, and footprintClear — which only asks whether cells are SOLID — was
// happy to put the body there. Pinned under a slab, the axis-separated moveAndSlide has nowhere to
// push it out to, which is the same permanent wedge this resolver exists to prevent.
TEST_CASE("Teleport::resolveDest refuses a landing pinned under a low ramp slab") {
    LevelGrid g = openGrid(12, 5);
    EntityPool pool{};

    // A ramp's low end across x = 5..7: slab TOP 1.25 m => underside 0.75 m. Too high to step onto
    // (PLATFORM_STEP_TOLERANCE 0.4 m), too low to stand under (BODY_CLEARANCE 0.8 m).
    for (u32 x = 5; x <= 7; x++)
        for (u32 z = 0; z < g.depth; z++) {
            GridCell& c = LevelGridSystem::getCell(g, x, z);
            c.flags |= CELL_PLATFORM;
            c.platCount = 1;
            c.platHeight[0] = 5;
        }

    const Vec3 start{2.5f, 0.0f, 2.5f};
    const Vec3 desired{7.5f, 0.0f, 2.5f};            // straight into the pinch
    const Vec3 got = Teleport::resolveDest(g, pool, start, desired);

    // It must stop SHORT of the ramp rather than land beneath it...
    CHECK(got.x < 5.0f);
    // ...and wherever it lands, the body must not be pinned.
    u32 gx, gz;
    REQUIRE(LevelGridSystem::worldToGrid(g, got, gx, gz));
    CHECK_FALSE(LevelGridSystem::bodyPinnedUnderSlab(g, gx, gz, got.y));

    // Sanity: without the pinch the same dash reaches its full distance, so the guard is what moved
    // the landing and not some unrelated rejection.
    LevelGrid open = openGrid(12, 5);
    const Vec3 far = Teleport::resolveDest(open, pool, start, desired);
    CHECK(far.x == doctest::Approx(7.5f));
    LevelGridSystem::shutdown(open);

    LevelGridSystem::shutdown(g);
}
