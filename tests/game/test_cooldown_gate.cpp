// Tests for the shared lenient skill/potion cooldown gate (GameConst::cooldownReady).
//
// Why this exists: in netplay the client predicts its own skill/potion activations
// locally and the server validates them authoritatively. The pre-existing gate was an
// EXACT integer tick comparison with zero tolerance, so a client running a few ticks
// ahead of the server (clock-sync / RTT skew) — or a MAX(local,snapshot) cooldown
// adoption nudging the timer forward by a tick — could make a perfectly-timed press
// silently drop ("I pressed it and nothing happened"). cooldownReady adds a small grace
// window (ACTIVATION_GRACE_TICKS) shared identically by client prediction and server
// validation, so legitimate boundary presses always land while real abuse (re-firing
// seconds early on a multi-second cooldown) is still rejected.
//
// These tests pin the boundary behaviour and guarantee the grace can't be exploited.

#include "doctest/doctest.h"
#include "game/game_constants.h"

using GameConst::cooldownReady;
using GameConst::ACTIVATION_GRACE_TICKS;

TEST_CASE("Cooldown gate: never-activated sentinel is always ready") {
    // lastActivationTick == 0 is the "never fired" sentinel — ready regardless of now.
    CHECK(cooldownReady(/*now=*/0,    /*lastTick=*/0, /*cooldownTicks=*/120));
    CHECK(cooldownReady(/*now=*/5000, /*lastTick=*/0, /*cooldownTicks=*/120));
}

TEST_CASE("Cooldown gate: grace accepts a client running a few ticks ahead") {
    const u32 cd       = 120;   // 2s skill
    const u32 lastTick = 1000;

    // Exactly cooldownTicks elapsed → ready (baseline, no grace needed).
    CHECK(cooldownReady(lastTick + cd, lastTick, cd));

    // Grace ticks early → still ready. This is the whole point: the client is allowed
    // to be up to ACTIVATION_GRACE_TICKS ahead without its press being dropped.
    CHECK(cooldownReady(lastTick + cd - ACTIVATION_GRACE_TICKS, lastTick, cd));

    // One tick beyond the grace window → NOT ready. Confirms the leniency is bounded.
    CHECK_FALSE(cooldownReady(lastTick + cd - ACTIVATION_GRACE_TICKS - 1, lastTick, cd));
}

TEST_CASE("Cooldown gate: rapid re-fire abuse is still rejected") {
    const u32 cd       = 120;   // 2s — far larger than the grace window
    const u32 lastTick = 1000;

    // Same-tick double press: elapsed 0 → rejected.
    CHECK_FALSE(cooldownReady(lastTick, lastTick, cd));

    // Pressing just past the grace window but nowhere near the real cooldown → rejected.
    // Proves grace (~6 ticks) can't be parlayed into spamming a seconds-long cooldown.
    CHECK_FALSE(cooldownReady(lastTick + ACTIVATION_GRACE_TICKS + 1, lastTick, cd));
}

TEST_CASE("Cooldown gate: degenerate cooldown <= grace is always ready") {
    // computeCooldownTicks floors at 3 ticks; with a 6-tick grace such a tiny cooldown
    // is effectively always ready — assert that doesn't underflow or misbehave.
    CHECK(cooldownReady(1000, 1000, 3));            // elapsed 0, 0+grace >= 3
    CHECK(cooldownReady(1000, 1000, ACTIVATION_GRACE_TICKS));
}

TEST_CASE("Cooldown gate: monotonic ready once past the boundary") {
    // Beyond the boundary the gate stays ready for all larger now values (no wraparound
    // surprises across a wide sweep).
    const u32 cd       = 300;   // 5s potion-scale cooldown
    const u32 lastTick = 1;
    for (u32 elapsed = cd; elapsed < cd + 600; ++elapsed) {
        CHECK(cooldownReady(lastTick + elapsed, lastTick, cd));
    }
    // ...and stays NOT ready for everything strictly inside (cd - grace).
    for (u32 elapsed = 0; elapsed + ACTIVATION_GRACE_TICKS < cd; ++elapsed) {
        CHECK_FALSE(cooldownReady(lastTick + elapsed, lastTick, cd));
    }
}
