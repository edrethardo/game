// test_grounded.cpp — Player.onGround must stay TRUE while a body rests on the floor.
//
// Found while tracing an Autoplay stall: a bot standing perfectly still on flat ground (position.y
// pinned to the centimetre, velocity.y never positive) reported onGround on only HALF its ticks —
// measured 150 of every 300, exactly alternating. The cause is a two-tick cycle in
// Collision::moveAndSlide: gravity is applied only when !onGround, so a grounded body has
// velocity.y == 0, hence delta.y == 0, hence the swept position equals the current one and does not
// overlap the grid — so the landing branch that sets onGround never runs, and the unconditional
// `onGround = false` at the top of the Y axis stands. The next tick then has !onGround, applies
// gravity, overlaps, lands, sets onGround, and the cycle repeats.
//
// It matters because onGround GATES real behaviour: the jump (a jump request is dropped unless
// grounded), the Autoplay VHALL fall veto (off on the odd ticks, so a bot could step off a balcony
// it was supposed to be protected from), and the grounded-only navigation rolls. It is also
// snapshotted for co-op. A resting body reading "airborne" half the time is wrong for all of them.

#include <doctest/doctest.h>
#include "world/level_grid.h"
#include "world/collision.h"
#include "game/player.h"

namespace {
// A plain 8x8 room, solid border, flat floor at height 0.
struct FlatRoom {
    LevelGrid grid;
    FlatRoom() {
        LevelGridSystem::init(grid, 8, 8, 1.0f);
        for (u32 z = 0; z < 8; z++)
            for (u32 x = 0; x < 8; x++) {
                GridCell& c = grid.cells[z * 8 + x];
                const bool border = (x == 0 || z == 0 || x == 7 || z == 7);
                c.flags         = border ? CELL_SOLID : (CELL_FLOOR | CELL_CEILING);
                c.floorHeight   = 0;
                c.ceilingHeight = 20;
            }
    }
    ~FlatRoom() { LevelGridSystem::shutdown(grid); }
};
} // namespace

TEST_CASE("A body resting on flat ground stays grounded every tick") {
    FlatRoom room;
    Player p{};
    p.position = Vec3{4.5f, 0.0f, 4.5f};   // standing on the floor, dead centre
    p.velocity = Vec3{0.0f, 0.0f, 0.0f};
    constexpr f32 dt = 1.0f / 60.0f;

    // Settle first, so we are measuring the RESTING state and not the first landing.
    for (int i = 0; i < 10; i++) Collision::moveAndSlide(p, room.grid, dt);
    REQUIRE(p.position.y == doctest::Approx(0.0f));

    // Now every subsequent tick must report grounded. Before the fix this alternated, giving 30/60.
    u32 grounded = 0;
    for (int i = 0; i < 60; i++) {
        Collision::moveAndSlide(p, room.grid, dt);
        if (p.onGround) grounded++;
    }
    CHECK(grounded == 60u);
    CHECK(p.position.y == doctest::Approx(0.0f));   // and it never sinks or creeps
}

TEST_CASE("Resting grounded does not break jumping or falling") {
    FlatRoom room;
    Player p{};
    p.position = Vec3{4.5f, 0.0f, 4.5f};
    constexpr f32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 10; i++) Collision::moveAndSlide(p, room.grid, dt);
    REQUIRE(p.onGround);

    // A jump still leaves the ground and comes back down to the floor.
    p.velocity.y = JUMP_SPEED;
    p.onGround   = false;
    Collision::moveAndSlide(p, room.grid, dt);
    CHECK(p.position.y > 0.0f);
    CHECK_FALSE(p.onGround);

    f32 apex = p.position.y;
    for (int i = 0; i < 240 && !p.onGround; i++) {
        Collision::moveAndSlide(p, room.grid, dt);
        if (p.position.y > apex) apex = p.position.y;
    }
    CHECK(apex > 0.5f);                              // a real arc, not a hop cancelled by gravity
    CHECK(p.onGround);                               // and it lands
    CHECK(p.position.y == doctest::Approx(0.0f));

    // A body spawned in the air falls and lands.
    p.position.y = 5.0f;
    p.velocity   = Vec3{0.0f, 0.0f, 0.0f};
    p.onGround   = false;
    for (int i = 0; i < 240 && !p.onGround; i++) Collision::moveAndSlide(p, room.grid, dt);
    CHECK(p.onGround);
    CHECK(p.position.y == doctest::Approx(0.0f));
}
