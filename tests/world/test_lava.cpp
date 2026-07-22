// test_lava.cpp — Hellforge lava (CELL_LAVA). The tier-4 "melted walls" hazard: every interior wall
// becomes a WALKABLE molten surface, so the floor is islands of stone in a lava sea with no sightline
// blockers. It burns the player only; monsters wade through and flank. These pin the grid-side rule
// the burn tick depends on — lava is walkable, feet at/below the surface are burning, and being
// AIRBORNE over it is safe, which is the whole reason a 1-cell vein is jumpable rather than lethal.

#include <doctest/doctest.h>
#include "world/level_grid.h"
#include "world/level_gen.h"
#include "world/collision.h"
#include "game/player.h"

#include <initializer_list>

namespace {
// A 10x10 room whose interior is stone floor, with a 1-cell lava vein running down x=5 — the
// jumpable case the Hellforge theme produces from a 1-cell room wall.
struct LavaRoom {
    LevelGrid grid;
    LavaRoom() {
        LevelGridSystem::init(grid, 10, 10, 1.0f);
        for (u32 z = 0; z < 10; z++)
            for (u32 x = 0; x < 10; x++) {
                GridCell& c = grid.cells[z * 10 + x];
                const bool border = (x == 0 || z == 0 || x == 9 || z == 9);
                c.flags         = border ? CELL_SOLID : (CELL_FLOOR | CELL_CEILING);
                c.floorHeight   = 0;
                c.ceilingHeight = 20;
            }
        for (u32 z = 1; z <= 8; z++) {           // the vein
            GridCell& c = grid.cells[z * 10 + 5];
            c.flags = CELL_FLOOR | CELL_LAVA;
        }
    }
    ~LavaRoom() { LevelGridSystem::shutdown(grid); }
};
} // namespace

TEST_CASE("Lava cells are WALKABLE, not solid") {
    // The whole design rests on this: lava replaces walls without replacing them with obstacles, so
    // a monster can wade through and the player can choose to eat the damage. If lava ever reads as
    // solid, the tier silently becomes a normal floor with orange walls.
    LavaRoom room;
    CHECK(LevelGridSystem::isLava(room.grid, 5, 4));
    CHECK_FALSE(LevelGridSystem::isSolid(room.grid, 5, 4));
    CHECK_FALSE(LevelGridSystem::isLava(room.grid, 4, 4));   // ordinary floor beside it

    Player p;
    p.position = {3.5f, 0.0f, 4.5f};
    p.onGround = true;
    for (int i = 0; i < 60; i++) {                            // walk straight across the vein
        p.velocity.x = 6.0f; p.velocity.z = 0.0f;
        Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
    }
    CHECK(p.position.x > 6.0f);                               // crossed it — nothing blocked us
}

TEST_CASE("feetInLava burns you on the surface and spares you in the air") {
    LavaRoom room;
    // Standing in it: burning.
    CHECK(LevelGridSystem::feetInLava(room.grid, {5.5f, 0.0f, 4.5f}));
    // Standing beside it: not.
    CHECK_FALSE(LevelGridSystem::feetInLava(room.grid, {4.5f, 0.0f, 4.5f}));
    // Airborne OVER it: not — this is what makes a jump the answer to a 1-cell vein.
    CHECK_FALSE(LevelGridSystem::feetInLava(room.grid, {5.5f, 0.8f, 4.5f}));
    // A grazing height still counts, so you cannot cheese it by hopping 1 cm.
    CHECK(LevelGridSystem::feetInLava(room.grid, {5.5f, 0.05f, 4.5f}));
}

TEST_CASE("A jump clears a 1-cell lava vein without ever touching the surface") {
    // The physics claim behind "you can jump over it": at 6 m/s the 0.4 s airtime covers 2.4 m, while
    // a 1 m vein plus a 0.6 m body needs 1.6 m. The 0.8 m of slack IS the takeoff window — jump too
    // early and you land in it (from x=3.5 you touch down at 5.9, mid-vein), which is the timing the
    // player is actually being asked to judge. Take off near the lip, as anyone would.
    LavaRoom room;
    Player p;
    p.position = {4.4f, 0.0f, 4.5f};   // body edge at 4.7, just shy of the vein at x=5
    p.onGround = true;
    p.velocity.y = JUMP_SPEED;
    p.onGround = false;
    bool burned = false;
    for (int i = 0; i < 60; i++) {
        p.velocity.x = 6.0f; p.velocity.z = 0.0f;
        Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
        if (LevelGridSystem::feetInLava(room.grid, p.position)) burned = true;
    }
    CHECK(p.position.x > 6.0f);      // landed on the far side
    CHECK_FALSE(burned);             // and never dipped into the molten surface
}

TEST_CASE("isLavaFloor: only a FEW Hellforge floors melt, and never one outside the tier") {
    // Ten straight lava floors stops being an event and becomes the norm, so the rule picks a
    // minority of the tier. It also drives GEOMETRY, so it must be a pure function of seed+floor —
    // if host and client ever disagree, one of them melts a floor the other did not.
    for (u32 seed : {1u, 42u, 9999u, 0xBEEFu}) {
        for (u32 f = 1; f <= 60; f++) {
            const bool lava = LevelGen::isLavaFloor(seed, f);
            CAPTURE(seed); CAPTURE(f);
            if (lava) { CHECK(f >= 31); CHECK(f <= 40); }          // never outside Hellforge
            CHECK(lava == LevelGen::isLavaFloor(seed, f));          // deterministic
        }
    }

    // Across the tier it must be a MINORITY but not zero — averaged over many seeds so a single
    // unlucky run can't make this flap.
    u32 lavaCount = 0, total = 0;
    for (u32 s = 0; s < 400; s++)
        for (u32 f = 31; f <= 40; f++) {
            if (LevelGen::isLavaFloor(s * 2654435761u, f)) lavaCount++;
            total++;
        }
    CHECK(lavaCount > total / 10);       // > 10% — the feature actually appears
    CHECK(lavaCount < total / 2);        // < 50% — still the exception, not the tier
}
