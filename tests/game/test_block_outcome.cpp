// test_block_outcome.cpp — perfect-block classification is the trigger for every legendary
// shield effect (Aegis of Blood, Thunderwall, Mirror Aegis), so its boundary is pinned here:
// the perfect window is blockTimer < 0.2s STRICT from the raise, anything later is a normal
// half-damage block, and no block state at all is NONE. classifyBlock is the single source —
// applyDamageToPlayer consumes it, the projectile parry path consumes its return value.

#include <doctest/doctest.h>
#include "game/combat.h"

TEST_CASE("classifyBlock: not blocking is NONE regardless of timer") {
    CHECK(Combat::classifyBlock(false, 0.0f) == Combat::BlockOutcome::NONE);
    CHECK(Combat::classifyBlock(false, 5.0f) == Combat::BlockOutcome::NONE);
}

TEST_CASE("classifyBlock: first 0.2s of a raise is PERFECT, after that BLOCKED") {
    CHECK(Combat::classifyBlock(true, 0.0f)  == Combat::BlockOutcome::PERFECT);
    CHECK(Combat::classifyBlock(true, 0.19f) == Combat::BlockOutcome::PERFECT);
    CHECK(Combat::classifyBlock(true, 0.2f)  == Combat::BlockOutcome::BLOCKED);  // strict <
    CHECK(Combat::classifyBlock(true, 3.0f)  == Combat::BlockOutcome::BLOCKED);
}

// A perfect block is a timing FEAT and must ALWAYS be rewarded — no cooldown gate (the user's
// design principle). classifyBlock is stateless, so a well-timed block classifies PERFECT every
// time, even back-to-back; the PvP throttle is the energy drain, not a lockout.
TEST_CASE("classifyBlock: perfect is always available — no cooldown, repeated perfects classify") {
    CHECK(Combat::classifyBlock(true, 0.05f) == Combat::BlockOutcome::PERFECT);
    CHECK(Combat::classifyBlock(true, 0.05f) == Combat::BlockOutcome::PERFECT);  // again, no lockout
    CHECK(Combat::classifyBlock(true, 0.05f) == Combat::BlockOutcome::PERFECT);
}
