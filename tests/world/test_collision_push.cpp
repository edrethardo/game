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
#include "game/player.h"   // Player, for the end-to-end jump-pad launch tests

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

// ensureNotInWall is the primitive the enemy wall-ejection safety net (EnemyAI::update's final pass)
// relies on: after every position write this tick, any entity left embedded in geometry is ejected,
// so "nothing ever ends a tick inside or between walls" — the enemy counterpart to the player's
// per-tick wall push-out. These pin the two properties that make that net correct.

TEST_CASE("ensureNotInWall: an embedded body is ejected to open floor") {
    // A summoner revives a corpse that died flush against a wall, or a boss leash-clamp overshoots
    // into one. Whatever put the body there, the net must leave it OUT of geometry.
    TestRoom room;
    Vec3 pos = {0.5f, 0.9f, 5.0f};                       // dead inside the solid west border cell
    REQUIRE(Collision::entityOverlapsGrid(pos, PLAYER_HALF, room.grid));
    Collision::ensureNotInWall(pos, PLAYER_HALF, room.grid);
    CHECK_FALSE(Collision::entityOverlapsGrid(pos, PLAYER_HALF, room.grid));   // the invariant
}

TEST_CASE("ensureNotInWall: a body already in the open is left exactly where it is") {
    // The property that keeps the net from jittering ordinary movement: an enemy that isn't embedded
    // is never nudged, so wall-hugging pathfinding (which already validates its own footprint) is
    // untouched. The net only ever moves a body that is genuinely stuck inside geometry.
    TestRoom room;
    Vec3 pos = {5.0f, 0.9f, 5.0f};                       // middle of the room, nowhere near a wall
    const Vec3 before = pos;
    Collision::ensureNotInWall(pos, PLAYER_HALF, room.grid);
    CHECK(pos.x == doctest::Approx(before.x));
    CHECK(pos.z == doctest::Approx(before.z));
}

// overlapsLedgeAbove is the Quake-style jump-gate: a CELL_LEDGE cell whose floor is more than
// STEP_UP_HEIGHT above the body's feet reads as a wall, so you must JUMP onto it. These pin the
// three behaviours moveAndSlide relies on: grounded body blocked, mid-jump body allowed, and a
// plain (non-LEDGE) raised floor never gated — which is what keeps every existing level and every
// walkable tier on the unlimited walk-up path (zero regression).

TEST_CASE("overlapsLedgeAbove: a jump-ledge above the step-up gate blocks a grounded body") {
    TestRoom room;
    GridCell& c = LevelGridSystem::getCell(room.grid, 5, 5);
    c.flags = CELL_FLOOR | CELL_LEDGE;
    c.floorHeight = 3;                                    // 0.75 m (quarter-units)
    // Feet on the ground: 0.75 > 0 + STEP_UP_HEIGHT (0.4) → gated.
    CHECK(Collision::overlapsLedgeAbove({5.5f, 0.0f, 5.5f}, PLAYER_HALF.x, room.grid));
}

TEST_CASE("overlapsLedgeAbove: jumping high enough clears the gate") {
    TestRoom room;
    GridCell& c = LevelGridSystem::getCell(room.grid, 5, 5);
    c.flags = CELL_FLOOR | CELL_LEDGE;
    c.floorHeight = 3;                                    // 0.75 m
    // Mid-jump feet at 0.5 m: 0.75 > 0.5 + 0.4 = 0.9 is false → the step onto the ledge is allowed.
    CHECK_FALSE(Collision::overlapsLedgeAbove({5.5f, 0.5f, 5.5f}, PLAYER_HALF.x, room.grid));
}

TEST_CASE("overlapsLedgeAbove: a plain raised floor (no LEDGE flag) never gates") {
    TestRoom room;
    GridCell& c = LevelGridSystem::getCell(room.grid, 5, 5);
    c.flags = CELL_FLOOR;                                 // raised, but NOT a jump-ledge
    c.floorHeight = 8;                                    // 2 m, far above the step-up
    CHECK_FALSE(Collision::overlapsLedgeAbove({5.5f, 0.0f, 5.5f}, PLAYER_HALF.x, room.grid));
}

// onJumpPad is the Quake/Combat-Hall launch trigger: a body RESTING on a CELL_JUMPPAD cell. These pin
// that it detects the pad you're on, ignores a plain floor, and — the important one — ignores a pad
// that sits well below your feet (so standing on a taller platform whose AABB happens to span a low
// pad cell never relaunches you).

TEST_CASE("jumpPadSpeed: standing on a pad cell is detected") {
    TestRoom room;
    GridCell& c = LevelGridSystem::getCell(room.grid, 5, 5);
    c.flags = CELL_FLOOR | CELL_JUMPPAD;
    c.floorHeight = 0;                                    // ground-level pad
    CHECK(Collision::jumpPadSpeed({5.5f, 0.0f, 5.5f}, PLAYER_HALF.x, room.grid) == doctest::Approx(JUMPPAD_LAUNCH));
}

TEST_CASE("jumpPadSpeed: a plain floor cell is not a pad") {
    TestRoom room;                                        // interior cells are plain CELL_FLOOR
    CHECK(Collision::jumpPadSpeed({5.5f, 0.0f, 5.5f}, PLAYER_HALF.x, room.grid) == 0.0f);
}

TEST_CASE("jumpPadSpeed: a pad far below the feet does not trigger") {
    TestRoom room;
    GridCell& c = LevelGridSystem::getCell(room.grid, 5, 5);
    c.flags = CELL_FLOOR | CELL_JUMPPAD;
    c.floorHeight = 0;                                    // pad at 0 m
    // Feet at 2 m (standing on a taller platform above the pad): 0 m is more than STEP_UP_HEIGHT
    // below, so it's not the pad we're resting on → no launch.
    CHECK(Collision::jumpPadSpeed({5.5f, 2.0f, 5.5f}, PLAYER_HALF.x, room.grid) == 0.0f);
}

// The end-to-end behaviour: one moveAndSlide tick over a pad replaces velocity.y with JUMPPAD_LAUNCH
// and lifts the body off the ground. This is the whole mechanic — and because every movement path
// (local predict, server drain, reconcile replay) funnels through moveAndSlide, this one impulse is
// what replicates the launch in co-op with no wire change.

TEST_CASE("moveAndSlide: resting on a jump pad launches the body upward") {
    TestRoom room;
    GridCell& c = LevelGridSystem::getCell(room.grid, 5, 5);
    c.flags = CELL_FLOOR | CELL_JUMPPAD;
    c.floorHeight = 0;

    Player p;
    p.position = {5.5f, 0.0f, 5.5f};                      // over the pad, at floor height
    p.velocity = {0.0f, 0.0f, 0.0f};
    p.onGround = false;                                   // gravity dips us onto the pad, floor-snap grounds us

    Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);

    CHECK(p.velocity.y == doctest::Approx(JUMPPAD_LAUNCH)); // flung up
    CHECK(p.onGround == false);                             // and airborne
}

TEST_CASE("moveAndSlide: resting on plain floor does NOT launch") {
    TestRoom room;                                        // cell (5,5) is plain CELL_FLOOR
    Player p;
    p.position = {5.5f, 0.0f, 5.5f};
    p.velocity = {0.0f, 0.0f, 0.0f};
    p.onGround = false;

    Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);

    CHECK(p.velocity.y == doctest::Approx(0.0f));         // grounded, not launched
    CHECK(p.onGround == true);
}
