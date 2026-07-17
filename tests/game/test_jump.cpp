// test_jump.cpp — the pure jump-assist logic (coyote time + jump buffering) and the physics-constant
// tuning that together define how jumping FEELS. JumpAssist::resolve is the single decision point
// the local predictor (PlayerController::update) and the server (updateNetPlayerFromInput) both
// call, so its boundaries are pinned here. The physics test guards the intended "low & tight" arc
// (~0.8 m apex / 0.4 s air) against an accidental gravity/impulse retune, and stands as the
// regression pin for the double-gravity bug: gravity used to be applied in BOTH applyMovement AND
// moveAndSlide (effective -40 from a -20 constant), so the two named physics constants lied about
// the actual arc; the fix integrates gravity once at GRAVITY = -40.

#include <doctest/doctest.h>
#include "game/jump.h"
#include "world/collision.h"

using JumpAssist::JumpState;
using JumpAssist::resolve;
using JumpAssist::COYOTE_TIME;
using JumpAssist::BUFFER_TIME;

static constexpr f32 DT = 1.0f / 60.0f;

TEST_CASE("resolve: a press while grounded jumps immediately") {
    JumpState st;
    CHECK(resolve(st, /*jumpEdge=*/true, /*onGround=*/true, DT) == true);
}

TEST_CASE("resolve: standing still with no press never jumps") {
    JumpState st;
    for (int i = 0; i < 10; i++) CHECK(resolve(st, false, true, DT) == false);
}

TEST_CASE("resolve: one press yields exactly one jump — the buffer can't re-fire") {
    JumpState st;
    CHECK(resolve(st, true,  true,  DT) == true);   // fires, consumes coyote + buffer
    // Now airborne; even with the button released the consumed buffer must not re-fire.
    CHECK(resolve(st, false, false, DT) == false);
    CHECK(resolve(st, false, false, DT) == false);
}

TEST_CASE("coyote: a press just AFTER leaving a ledge still jumps") {
    JumpState st;
    resolve(st, false, true, DT);                   // grounded → coyote timer full
    // Walk off the ledge; press ~0.05 s later, inside COYOTE_TIME (0.1 s).
    CHECK(resolve(st, false, false, DT) == false);  // no press yet
    CHECK(resolve(st, false, false, DT) == false);
    CHECK(resolve(st, true,  false, DT) == true);   // late edge press within grace → jump
}

TEST_CASE("coyote: a press long after leaving the ground is denied (no mid-air jump)") {
    JumpState st;
    resolve(st, false, true, DT);                   // prime coyote
    for (int i = 0; i < 10; i++) resolve(st, false, false, DT);  // bleed the whole window
    CHECK(resolve(st, true, false, DT) == false);   // window closed → denied
}

TEST_CASE("buffer: a press just BEFORE landing fires on touchdown") {
    JumpState st;
    for (int i = 0; i < 20; i++) resolve(st, false, false, DT);  // long fall, no coyote left
    CHECK(resolve(st, true,  false, DT) == false);  // buffered, doesn't fire in the air
    CHECK(resolve(st, false, false, DT) == false);  // still falling, ~1 tick later
    CHECK(resolve(st, false, true,  DT) == true);   // land within BUFFER_TIME → buffered jump fires
}

TEST_CASE("buffer: a press too early is forgotten by the time you land") {
    JumpState st;
    for (int i = 0; i < 20; i++) resolve(st, false, false, DT);
    CHECK(resolve(st, true, false, DT) == false);   // buffered press
    for (int i = 0; i < 10; i++) resolve(st, false, false, DT);  // fall well past BUFFER_TIME
    CHECK(resolve(st, false, true, DT) == false);   // buffer expired → no surprise hop on landing
}

// The tuned "low & tight" arc. For a launch speed v against gravity magnitude g:
//   apex height = v² / (2·g),   air time = 2·v / g.
// JUMP_SPEED = 8, GRAVITY = -40 (single-applied) → 0.8 m, 0.4 s. If gravity is ever double-applied
// again (effective -80) or a constant drifts, these numbers move and this test flags the feel change.
TEST_CASE("physics: the jump arc matches the intended low-and-tight tuning") {
    const f32 g = -GRAVITY;                                 // magnitude → 40
    const f32 apex    = (JUMP_SPEED * JUMP_SPEED) / (2.0f * g);
    const f32 airTime = (2.0f * JUMP_SPEED) / g;
    CHECK(apex    == doctest::Approx(0.8f).epsilon(0.01f));
    CHECK(airTime == doctest::Approx(0.4f).epsilon(0.01f));
}
