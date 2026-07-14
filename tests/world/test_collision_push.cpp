// test_collision_push.cpp — a push may crowd the player, but it may never push them INTO a wall.
//
// The bug: enemy-vs-player crowding and co-op partner separation both wrote straight into
// player.position with no wall test at all, then leaned on a "wall push-out" pass afterwards to
// repair whatever that produced. The header comment on that function even claimed the push was
// "reverted if it would land the player inside solid geometry" — there was no such check anywhere in
// the file.
//
// Repair-after-the-fact cannot work here, and that is the whole point of these tests. The ejection
// pass has to GUESS an exit axis from an overlap it did not create, and while the push keeps being
// re-applied every frame — an enemy leaning on a player who is backed against a wall — the push and
// the ejection fight each other: the player jitters against the wall, and on a one-cell-thick wall
// the ejection can pick the far side and pop them clean through it.
//
// So the rule is now VALIDATION, not repair: a push that would create a wall overlap is not taken.
// These pin that, plus the one exception that keeps it from being a trap (see the last case).

#include <doctest/doctest.h>
#include "world/collision.h"
#include "world/level_grid.h"

namespace {

// A 10x10 open room of 1 m cells, ringed by solid border cells. Cell (gx,gz) spans
// [gx, gx+1) x [gz, gz+1) in world XZ, so cell centres sit at +0.5.
struct TestRoom {
    LevelGrid grid;
    TestRoom() {
        LevelGridSystem::init(grid, 10, 10, 1.0f);
        for (u32 z = 0; z < 10; z++) {
            for (u32 x = 0; x < 10; x++) {
                const bool border = (x == 0 || z == 0 || x == 9 || z == 9);
                grid.cells[z * 10 + x].flags = border ? CELL_SOLID : CELL_FLOOR;
            }
        }
    }
    ~TestRoom() { LevelGridSystem::shutdown(grid); }
};

// The player's XZ footprint. Half-width 0.35 matches PLAYER_HALF_WIDTH; y is unused by the grid test.
constexpr Vec3 PLAYER_HALF = {0.35f, 0.9f, 0.35f};

} // namespace

TEST_CASE("CollisionPush: a push into open floor is taken") {
    TestRoom room;
    Vec3 pos = {5.0f, 0.0f, 5.0f};                       // middle of the room
    CHECK(Collision::tryPushXZ(pos, PLAYER_HALF, room.grid, 0.4f, 0.0f));
    CHECK(pos.x == doctest::Approx(5.4f));
    CHECK(pos.z == doctest::Approx(5.0f));
}

TEST_CASE("CollisionPush: a push that would drive the player INTO a wall is refused") {
    // THE BUG. Player stands against the west wall (solid cells at x < 1). Their left edge is at
    // 1.35 - 0.35 = 1.0, flush with the wall. An enemy leaning on them from the east produces a
    // westward push — straight into the wall. It must not be taken.
    TestRoom room;
    Vec3 pos = {1.35f, 0.0f, 5.0f};
    const Vec3 before = pos;

    CHECK_FALSE(Collision::tryPushXZ(pos, PLAYER_HALF, room.grid, -0.3f, 0.0f));
    CHECK(pos.x == doctest::Approx(before.x));           // not moved at all
    CHECK(pos.z == doctest::Approx(before.z));
}

TEST_CASE("CollisionPush: a refused push leaves the player OUT of geometry") {
    // The property that actually matters, stated directly: whatever the push was, the player is
    // never left inside a wall afterwards.
    TestRoom room;
    Vec3 pos = {1.35f, 0.0f, 5.0f};
    Collision::tryPushXZ(pos, PLAYER_HALF, room.grid, -0.3f, 0.0f);
    CHECK_FALSE(Collision::entityOverlapsGrid(pos, PLAYER_HALF, room.grid));
}

TEST_CASE("CollisionPush: repeated pushes can never accumulate the player through a wall") {
    // The failure mode with teeth. An enemy does not push once — it pushes EVERY FRAME while it is
    // touching you. The old code let each of those land and hoped the ejection pass undid it, which
    // is how a cornered player got squeezed through. 200 frames of shoving must not move them a
    // millimetre into the wall.
    TestRoom room;
    Vec3 pos = {1.35f, 0.0f, 5.0f};
    for (int frame = 0; frame < 200; frame++) {
        Collision::tryPushXZ(pos, PLAYER_HALF, room.grid, -0.3f, 0.0f);
        REQUIRE_FALSE(Collision::entityOverlapsGrid(pos, PLAYER_HALF, room.grid));
    }
    CHECK(pos.x >= 1.35f);                               // never crossed into the wall
}

TEST_CASE("CollisionPush: a push along the wall still works") {
    // Refusing pushes must not weld a cornered player in place — being crowded along a wall is
    // normal, and sliding sideways along it must still be possible.
    TestRoom room;
    Vec3 pos = {1.35f, 0.0f, 5.0f};                      // flush against the west wall
    CHECK(Collision::tryPushXZ(pos, PLAYER_HALF, room.grid, 0.0f, 0.4f));   // parallel to it
    CHECK(pos.z == doctest::Approx(5.4f));
}

TEST_CASE("CollisionPush: diagonal push blocked on one axis is refused as a whole") {
    // tryPushXZ is a single displacement, not two independent axes: if the combined result lands in
    // geometry it is refused outright. (The caller pushes one axis at a time — the minimal-
    // penetration one — so this is the honest contract rather than a half-applied move.)
    TestRoom room;
    Vec3 pos = {1.35f, 0.0f, 5.0f};
    CHECK_FALSE(Collision::tryPushXZ(pos, PLAYER_HALF, room.grid, -0.3f, 0.3f));
    CHECK(pos.x == doctest::Approx(1.35f));
    CHECK(pos.z == doctest::Approx(5.0f));
}

TEST_CASE("CollisionPush: a body ALREADY inside a wall is not trapped there") {
    // The exception that stops the rule becoming a trap. If we refused every push that ends in
    // geometry, a body that spawned or teleported INSIDE a wall could never be moved again — every
    // candidate position overlaps, so every push is refused, and the ejection pass (which works by
    // pushing) is disarmed. So a push made while already overlapping is allowed through.
    TestRoom room;
    Vec3 pos = {0.5f, 0.0f, 5.0f};                       // dead inside the solid west border cell
    REQUIRE(Collision::entityOverlapsGrid(pos, PLAYER_HALF, room.grid));

    CHECK(Collision::tryPushXZ(pos, PLAYER_HALF, room.grid, 1.0f, 0.0f));   // ejecting eastward
    CHECK(pos.x == doctest::Approx(1.5f));
    CHECK_FALSE(Collision::entityOverlapsGrid(pos, PLAYER_HALF, room.grid));   // and it got out
}

TEST_CASE("CollisionPush: out-of-bounds counts as solid") {
    // entityOverlapsGrid treats off-grid as solid, so a push off the edge of the level is refused by
    // the same rule that refuses a push into a wall. Worth pinning: the level border is the one wall
    // there is no cell behind.
    TestRoom room;
    Vec3 pos = {1.35f, 0.0f, 5.0f};
    CHECK_FALSE(Collision::tryPushXZ(pos, PLAYER_HALF, room.grid, -50.0f, 0.0f));
    CHECK(pos.x == doctest::Approx(1.35f));
}
