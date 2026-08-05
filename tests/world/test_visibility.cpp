// Occlusion visibility — the tests that keep the cull CONSERVATIVE.
//
// The renderer culls by frustum only, which on a maze or a stacked hall rejects almost nothing:
// measured on FOUR_STORY, 114 entities produced 165 draw calls a frame and the level submitted a
// constant 55 submeshes no matter where the camera looked. Adding an occlusion test halved the draw
// calls (VHALL median 332 -> 160, FOUR_STORY 416 -> 183) and brought the peak back inside the 500
// budget — but the failure mode flips: instead of drawing too much, a wrong answer here makes an
// enemy or a slab of world VANISH.
//
// So these tests are almost entirely about the SAFE direction. The interesting cases are the ones
// where the honest answer is "I am not sure" — those must come back visible.

#include <doctest/doctest.h>
#include "world/visibility.h"

#include <vector>

namespace {

// An open room with a solid pillar/wall wherever `walls` says so. Cells are 1 m.
struct TestGrid {
    LevelGrid grid;
    explicit TestGrid(u32 w, u32 d) {
        LevelGridSystem::init(grid, w, d, 1.0f);
        for (u32 z = 0; z < d; z++)
            for (u32 x = 0; x < w; x++) {
                GridCell& c = LevelGridSystem::getCell(grid, x, z);
                c.flags = CELL_FLOOR;
                c.floorHeight = 0;
            }
    }
    ~TestGrid() { LevelGridSystem::shutdown(grid); }
    void solid(u32 x, u32 z) {
        GridCell& c = LevelGridSystem::getCell(grid, x, z);
        c.flags = CELL_SOLID;
    }
};

constexpr f32 FAR = Visibility::MIN_CULL_DISTANCE + 4.0f;   // past the never-cull radius

} // namespace

TEST_CASE("an unobstructed line is clear") {
    TestGrid t(24, 8);
    const Vec3 eye{1.5f, 1.5f, 4.5f};
    CHECK(Visibility::segmentClear(t.grid, eye, Vec3{1.5f + FAR, 1.5f, 4.5f}));
}

TEST_CASE("a wall between eye and target blocks the line") {
    TestGrid t(24, 8);
    for (u32 z = 0; z < 8; z++) t.solid(8, z);          // full-height wall across the room
    const Vec3 eye{1.5f, 1.5f, 4.5f};
    CHECK_FALSE(Visibility::segmentClear(t.grid, eye, Vec3{16.5f, 1.5f, 4.5f}));
}

TEST_CASE("a target on the far side of a wall is culled, but only past the safety radius") {
    TestGrid t(24, 8);
    for (u32 z = 0; z < 8; z++) t.solid(8, z);
    const Vec3 eye{1.5f, 1.5f, 4.5f};
    const Vec3 half{0.4f, 0.9f, 0.4f};

    // Far behind the wall: every sample is blocked, so it may be culled.
    CHECK_FALSE(Visibility::entityVisible(t.grid, eye, Vec3{16.5f, 0.0f, 4.5f}, half));

    // The SAME occlusion close up is deliberately NOT culled. A body pressed against the far side
    // of geometry can show a sliver that a handful of rays miss, and the saving is negligible — so
    // inside MIN_CULL_DISTANCE the answer is always "draw it".
    TestGrid t2(24, 8);
    t2.solid(3, 4);
    CHECK(Visibility::entityVisible(t2.grid, Vec3{1.5f, 1.5f, 4.5f},
                                    Vec3{4.5f, 0.0f, 4.5f}, half));
}

TEST_CASE("a shoulder past a doorway keeps the whole body visible") {
    // A one-cell gap in the wall. The body CENTRE is behind solid rock, but part of it is exposed
    // through the opening — the lateral samples exist exactly for this, and getting it wrong would
    // pop enemies in and out as the player strafes across a door.
    TestGrid t(24, 12);
    for (u32 z = 0; z < 12; z++) if (z != 6) t.solid(8, z);

    const Vec3 eye{1.5f, 1.5f, 6.5f};
    const Vec3 half{0.4f, 0.9f, 0.4f};
    CHECK(Visibility::entityVisible(t.grid, eye, Vec3{16.5f, 0.0f, 6.5f}, half));
}

TEST_CASE("a box is visible when ANY corner is, not only its centre") {
    // A section-sized box whose centre is hidden behind a pillar but whose corners are wide open.
    // Testing the centre alone would blink a 16x16 m slab of the world out of existence.
    TestGrid t(40, 40);
    for (u32 z = 18; z < 22; z++)
        for (u32 x = 18; x < 22; x++) t.solid(x, z);

    const Vec3 eye{2.5f, 1.5f, 20.5f};
    CHECK(Visibility::boxVisible(t.grid, eye, Vec3{16.0f, 0.0f, 12.0f}, Vec3{32.0f, 4.0f, 28.0f}));
}

TEST_CASE("a fully walled-off box is culled") {
    TestGrid t(40, 40);
    for (u32 z = 0; z < 40; z++) t.solid(20, z);        // wall spanning the whole grid
    const Vec3 eye{2.5f, 1.5f, 20.5f};
    CHECK_FALSE(Visibility::boxVisible(t.grid, eye, Vec3{30.0f, 0.0f, 18.0f},
                                                    Vec3{34.0f, 4.0f, 22.0f}));
}

TEST_CASE("degenerate inputs resolve to visible") {
    // Zero-length segment, and a target at the eye. Anything ambiguous must fail SAFE — the cost of
    // a spurious draw call is nothing next to geometry disappearing.
    TestGrid t(8, 8);
    const Vec3 eye{4.5f, 1.5f, 4.5f};
    CHECK(Visibility::segmentClear(t.grid, eye, eye));
    CHECK(Visibility::entityVisible(t.grid, eye, eye, Vec3{0.4f, 0.9f, 0.4f}));
}
