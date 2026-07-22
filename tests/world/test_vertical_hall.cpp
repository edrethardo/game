// test_vertical_hall.cpp — VERTICAL_HALL ("Sunken Atrium") layout invariants. The style must never
// collapse to the BSP fallback (roomCount >= 5), must carry real two-story content (the CELL_PLATFORM
// balcony ring + the two graduated-slab corner-ramp StoryPortals for the cross-story chase), must keep
// the central PIT genuinely open (far more bare floor than slab, or there's no atrium to look down into),
// and must give a reachable spawn/exit on OPPOSITE stories. The FEEL (the drop into the pit, the sniper
// fire from the balcony, the catwalk jump, the ascend/descend) is a playtest concern; these pin the
// structure so a regression can't silently ship a flat parking-lot or an unplayable floor.

#include <doctest/doctest.h>
#include "world/level_gen.h"
#include "world/level_grid.h"

static u32 countFlag(const LevelGrid& g, u8 flag) {
    u32 n = 0;
    for (u32 i = 0; i < g.width * g.depth; i++)
        if (g.cells[i].flags & flag) n++;
    return n;
}

// startGame forces vhall floors to a 44-grid (the wide gallery + big pit need the room), so test at 44.
TEST_CASE("VERTICAL_HALL generates a valid two-story floor across many seeds") {
    for (u32 seed = 1; seed <= 64; seed++) {
        LevelGrid g;
        LevelGridSystem::init(g, 44, 44, 1.0f);
        DungeonResult r = LevelGen::generate(g, seed, 44, 44, LevelGen::LayoutStyle::VERTICAL_HALL);

        CHECK(r.roomCount >= 5);                     // never falls back to BSP (the Stacked Loop has 9 areas)
        CHECK(countFlag(g, CELL_PLATFORM) > 0);      // the four balconies + catwalks + pinwheel ramps
        CHECK(r.portalCount == 4);                   // the four pinwheel ramps, for the cross-story chase
        CHECK(r.spawnRoomIdx != r.exitRoomIdx);      // spawn (a corner) and exit (a far balcony) differ
        CHECK(lengthSq(r.spawnBalconyPos) > 0.0f);   // both are explicit positions
        CHECK(lengthSq(r.exitBalconyPos) > 0.0f);
        // Entrance is on the opposite STORY from the exit: exactly one sits on the balcony (y≈3m).
        CHECK((r.spawnBalconyPos.y > 1.0f) != (r.exitBalconyPos.y > 1.0f));
        // The central PIT must be genuinely OPEN — far more bare pit floor than balcony slab, or there's
        // no void to look down into / drop through. (Balcony cells carry BOTH CELL_FLOOR and
        // CELL_PLATFORM, so the open pit is the CELL_FLOOR-without-slab remainder.)
        u32 slabCells = countFlag(g, CELL_PLATFORM), floorCells = countFlag(g, CELL_FLOOR);
        CHECK(floorCells > slabCells);               // the pit + the arcade under the balcony dominate

        // ONE jump-pad, spawn-side. Two pads under the catwalk crossing were the floor's biggest
        // shortcut (pad → catwalk → exit balcony in seconds); the single pad is recovery, not a taxi.
        CHECK(countFlag(g, CELL_JUMPPAD) == 1);

        // The DIAGONAL-service rule — the fix for "it's a 5 second walk". The exit-side balcony must
        // be served by the ramp whose FOOT is the far corner: the portal nearest the balcony endpoint
        // (its top) must have its foot on the OPPOSITE side of the floor from the ground endpoint, in
        // BOTH axes. A same-side ramp foot is the one-band shortcut this rule forbids.
        {
            const Vec3 up   = (r.spawnBalconyPos.y > 1.0f) ? r.spawnBalconyPos : r.exitBalconyPos;
            const Vec3 down = (r.spawnBalconyPos.y > 1.0f) ? r.exitBalconyPos  : r.spawnBalconyPos;
            f32 bestD2 = 1e30f; Vec3 foot{};
            for (u8 p = 0; p < r.portalCount; p++) {
                const f32 dx = r.portals[p].highPos.x - up.x, dz = r.portals[p].highPos.z - up.z;
                const f32 d2 = dx * dx + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; foot = r.portals[p].lowPos; }
            }
            const f32 mid = 44.0f * 0.5f;   // grid centre in world units (cellSize 1)
            CHECK(((down.x < mid) != (foot.x < mid)));   // foot across the centre in X…
            CHECK(((down.z < mid) != (foot.z < mid)));   // …and in Z: the diagonal corner
        }

        LevelGridSystem::shutdown(g);
    }
}

TEST_CASE("VERTICAL_HALL falls back to BSP on a grid too small for the wide two-story layout") {
    LevelGrid g;
    LevelGridSystem::init(g, 32, 32, 1.0f);
    DungeonResult r = LevelGen::generate(g, 12345u, 32, 32, LevelGen::LayoutStyle::VERTICAL_HALL);
    CHECK(r.roomCount >= 5);                         // BSP fallback still yields a playable floor
    CHECK(r.portalCount == 0);                       // no story portals — it's a plain BSP floor
    LevelGridSystem::shutdown(g);
}

// Migration guard (Part A foundation, spec 3.7): every legacy single-slab writer routes through
// setPlatform (replace-to-single), so no shipped cell may carry more than one slab. The determinism
// memcmp can't catch an accidental addPlatform (accumulate) — this test is the guard that can.
TEST_CASE("Migration: generated VERTICAL_HALL cells never carry more than one slab") {
    for (u32 seed = 1; seed <= 16; seed++) {
        LevelGrid g;
        LevelGridSystem::init(g, 44, 44, 1.0f);
        LevelGen::generate(g, seed, 44, 44, LevelGen::LayoutStyle::VERTICAL_HALL);
        for (u32 i = 0; i < g.width * g.depth; i++)
            CHECK(g.cells[i].platCount <= 1);
        LevelGridSystem::shutdown(g);
    }
}

TEST_CASE("Migration: setPlatform collapses an overlapping-band double-write to one slab (arena pattern)") {
    // The arena writes a perimeter band at qu12, then a corner ramp/stairwell overwrites the same cell
    // at another height — the junction that would fabricate a phantom slab under addPlatform.
    GridCell c = {};
    c.flags = CELL_FLOOR;
    LevelGridSystem::setPlatform(c, 12, 4);   // perimeter band
    LevelGridSystem::setPlatform(c, 8, 4);    // ramp/stairwell overwrites the junction cell
    CHECK(c.platCount == 1);
    CHECK(c.platHeight[0] == 8);              // last write wins — no accumulation
    CHECK(c.platHeight[1] == 0);
}
