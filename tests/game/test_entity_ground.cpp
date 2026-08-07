// snapEntityToFloor — DIAGNOSTIC SUITE for "enemies sometimes drive inside the ground when
// attacking and moving towards the player" (Aaron, 2026-08-07).
//
// These are written against the behaviour the GAME needs, not against what the code currently does,
// so a failure here names the mechanism rather than confirming the status quo. The function has
// exactly two early-outs, and the report is consistent with either:
//
//   H1  "airborne" is inferred from `velocity.y != 0`. Any residue in that float — from a vault, a
//       pad, a landing that did not settle exactly on zero — permanently disables floor-snapping.
//   H2  an entity whose CENTRE cell is SOLID is skipped. A crowd shoving each other into a rock
//       clump or a tunnel wall puts centres inside geometry, which is precisely the "attacking and
//       moving towards the player" situation.
//
// Whichever cases fail are the answer. Cases that pass rule their hypothesis OUT, which is worth as
// much: this project has twice chased a theory that the data had already killed.

#include <doctest/doctest.h>
#include "game/entity_ground.h"

#include <initializer_list>

namespace {

// A flat open room at floor height 0, 1 m cells.
struct Room {
    LevelGrid grid;
    explicit Room(u32 w = 16, u32 d = 16) {
        LevelGridSystem::init(grid, w, d, 1.0f);
        for (u32 z = 0; z < d; z++)
            for (u32 x = 0; x < w; x++) {
                GridCell& c = LevelGridSystem::getCell(grid, x, z);
                c.flags = CELL_FLOOR;
                c.floorHeight = 0;
            }
    }
    ~Room() { LevelGridSystem::shutdown(grid); }
    void solid(u32 x, u32 z) { LevelGridSystem::getCell(grid, x, z).flags = CELL_SOLID; }
};

// A ground melee enemy standing on the floor at (x, z). halfExtents.y 0.9 => centre at y 0.9.
Entity walker(f32 x, f32 z) {
    Entity e{};
    e.halfExtents = {0.4f, 0.9f, 0.4f};
    e.position    = {x, 0.9f, z};
    e.velocity    = {0, 0, 0};
    e.flags       = 0;
    return e;
}

constexpr f32 FEET_ON_FLOOR = 0.9f;   // centre height for a body whose feet rest at y = 0

} // namespace

TEST_CASE("baseline: a walker on open floor is held at floor height") {
    Room r;
    Entity e = walker(8.5f, 8.5f);
    e.position.y = 0.2f;                       // sunk, for whatever reason
    snapEntityToFloor(e, r.grid);
    CHECK(e.position.y == doctest::Approx(FEET_ON_FLOOR));
}

// --------------------------------------------------------------------------------------------
// H1 — the velocity.y early-out
// --------------------------------------------------------------------------------------------

TEST_CASE("H1: a walker with RESIDUAL downward Y velocity is not snapped, and sinks") {
    // The failure mode in one case. `velocity.y != 0` is how the code infers "mid-flight", so a
    // walker carrying a scrap of Y velocity is treated as airborne forever and never re-grounded.
    // Nothing about this entity is actually flying: it is standing on the floor.
    Room r;
    Entity e = walker(8.5f, 8.5f);
    e.velocity.y = -0.001f;                    // a single tick of un-cleared gravity
    e.position.y = 0.2f;                       // already dropped below the floor
    snapEntityToFloor(e, r.grid);
    CHECK(e.position.y == doctest::Approx(FEET_ON_FLOOR));
}

TEST_CASE("H1: a jump-pad arc must STILL be left alone — the early-out has a real job") {
    // The counter-case, so any fix is judged against both. An enemy genuinely launched upward must
    // not be yanked back to the floor, or pads become silently useless to enemies.
    Room r;
    Entity e = walker(8.5f, 8.5f);
    e.velocity.y = 12.0f;                      // riding a launch
    e.position.y = 4.0f;
    snapEntityToFloor(e, r.grid);
    CHECK(e.position.y == doctest::Approx(4.0f));   // untouched
}

TEST_CASE("H1: a walker BELOW the floor is never left there, whatever its Y velocity") {
    // The property that actually matters and that neither early-out currently guarantees: a ground
    // entity may be above the floor (a jump, a fall) but must never end up INSIDE it. This is the
    // invariant "enemies drive inside the ground" violates, stated directly.
    Room r;
    for (f32 vy : {-5.0f, -0.001f, 0.0f, 3.0f}) {
        Entity e = walker(8.5f, 8.5f);
        e.velocity.y = vy;
        e.position.y = -2.0f;                  // well under the world
        CAPTURE(vy);
        snapEntityToFloor(e, r.grid);
        CHECK(e.position.y >= doctest::Approx(FEET_ON_FLOOR));
    }
}

// --------------------------------------------------------------------------------------------
// H2 — the solid-cell early-out
// --------------------------------------------------------------------------------------------

TEST_CASE("H2: a walker shoved INTO a rock clump is not snapped, and sinks") {
    // What a crowd does. Enemies push each other while converging on the player, so a centre ends up
    // inside a solid cell — and from that tick the entity is no longer floor-snapped at all.
    Room r;
    r.solid(8, 8);
    Entity e = walker(8.5f, 8.5f);             // centre exactly in the solid cell
    e.position.y = 0.1f;
    snapEntityToFloor(e, r.grid);
    CHECK(e.position.y == doctest::Approx(FEET_ON_FLOOR));
}

TEST_CASE("H2: a walker just OUTSIDE a rock clump is fine — isolating the cell test") {
    // Same geometry, centre one cell over. If this passes and the case above fails, the trigger is
    // precisely "centre cell is solid" and nothing else about the clump.
    Room r;
    r.solid(8, 8);
    Entity e = walker(9.5f, 8.5f);
    e.position.y = 0.1f;
    snapEntityToFloor(e, r.grid);
    CHECK(e.position.y == doctest::Approx(FEET_ON_FLOOR));
}

TEST_CASE("H2: an off-grid walker is not snapped either") {
    // The third way to miss the snap. Less likely in play, but it shares the early-out, so a fix
    // aimed at solid cells alone would leave this one live.
    Room r;
    Entity e = walker(-5.0f, -5.0f);
    e.position.y = -3.0f;
    snapEntityToFloor(e, r.grid);
    // No floor exists off-grid, so the honest requirement is only that it is not driven DEEPER.
    CHECK(e.position.y == doctest::Approx(-3.0f));
}

// --------------------------------------------------------------------------------------------
// The combination the report describes
// --------------------------------------------------------------------------------------------

TEST_CASE("both together: attacking in a crowd against a wall") {
    // "Attacking and moving towards the player" — pressed into geometry AND carrying movement
    // residue. If both early-outs fire, nothing re-grounds this enemy for as long as it stays there.
    Room r;
    r.solid(8, 8);
    Entity e = walker(8.5f, 8.5f);
    e.velocity.y = -0.5f;
    e.position.y = -1.0f;
    snapEntityToFloor(e, r.grid);
    CHECK(e.position.y >= doctest::Approx(FEET_ON_FLOOR));
}

TEST_CASE("flyers are gated OUT by the CALLERS, not by this function") {
    // Written expecting the function to exempt flyers; it does not, and that turned out to be the
    // correct contract rather than a bug. Every call site reads
    //     if (!(e.flags & ENT_FLYING)) snapEntityToFloor(e, grid);
    // so a flyer never reaches here at all, and the ENT_FLYING term inside only qualifies the
    // AIRBORNE test. Pinned as the contract it actually is, so a future caller knows it must do the
    // gating itself — calling this on a bat would put the bat on the floor.
    Room r;
    Entity e = walker(8.5f, 8.5f);
    e.flags |= ENT_FLYING;
    e.position.y = 3.0f;
    e.velocity.y = 0.0f;
    snapEntityToFloor(e, r.grid);
    CHECK(e.position.y == doctest::Approx(FEET_ON_FLOOR));   // grounded — callers must not call it
}
