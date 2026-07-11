// test_lag_comp.cpp — the client-reported interp-delay -> server rewind translation.
//
// This is the unit that makes the server's replay of a client input collide against the SAME
// entity poses the client predicted against. Getting the sign, the scale, or the untrusted-input
// clamp wrong here reintroduces exactly the "jitter near moving enemies" it was written to fix,
// so the arithmetic is pinned rather than trusted.

#include <doctest/doctest.h>
#include "net/lag_comp.h"

TEST_CASE("LagComp: delay converts to ticks at the net tick rate") {
    // 60 Hz -> 16.67 ms per tick. The legacy baseline (33 ms) is the 2 ticks the server
    // used to hardcode, so the new path must reproduce the old behavior for a 33 ms client.
    CHECK(LagComp::rewindTicks(33) == doctest::Approx(33.0f * 60.0f / 1000.0f)); // ~1.98
    CHECK(LagComp::rewindTicks(100) == doctest::Approx(6.0f));
    CHECK(LagComp::rewindTicks(150) == doctest::Approx(9.0f));
}

TEST_CASE("LagComp: a jittered client rewinds further than the old hardcoded 2 ticks") {
    // The whole point of the fix: under jitter the client widens its buffer to (say) 100 ms
    // and collides against enemies 6 ticks in the past, while the old server always rewound 2.
    // That 4-tick gap of enemy motion was the divergence.
    const f32 jittered = LagComp::rewindTicks(100);
    const f32 legacy   = LagComp::rewindTicks(LagComp::DEFAULT_INTERP_DELAY_MS);
    CHECK(jittered > legacy);
    CHECK(jittered - legacy > 3.0f);
}

TEST_CASE("LagComp: absent field falls back to the legacy baseline, never to zero") {
    // A zero rewind would collide the replay against LIVE enemy poses — the original bug,
    // and worse than the old guess. 0 on the wire means "old client / not stamped".
    CHECK(LagComp::sanitize(0) == LagComp::DEFAULT_INTERP_DELAY_MS);
    CHECK(LagComp::rewindTicks(0) == doctest::Approx(LagComp::rewindTicks(33)));
    CHECK(LagComp::rewindTicks(0) > 0.0f);
}

TEST_CASE("LagComp: an over-large claimed delay is clamped, not trusted") {
    // Untrusted input: a client claiming 255 ms would otherwise rewind enemies ~15 ticks and
    // could walk through where they used to be.
    CHECK(LagComp::sanitize(255) == LagComp::MAX_INTERP_DELAY_MS);
    CHECK(LagComp::rewindTicks(255) == doctest::Approx(LagComp::rewindTicks(150)));
}

TEST_CASE("LagComp: targetTick rewinds backward from the acked snapshot") {
    // Sign check. Rewinding must move the target INTO THE PAST (a smaller tick).
    CHECK(LagComp::targetTick(1000, 100) == doctest::Approx(994.0f));
    CHECK(LagComp::targetTick(1000, 100) < 1000.0f);
}

TEST_CASE("LagComp: targetTick floors at zero right after a join or floor reset") {
    // The history ring is empty/shallow for the first ticks of a floor; a negative target
    // tick has no meaning and (as an unsigned) would wrap to a huge tick.
    CHECK(LagComp::targetTick(0, 150) == doctest::Approx(0.0f));
    CHECK(LagComp::targetTick(3, 150) == doctest::Approx(0.0f));
}

TEST_CASE("LagComp: toWireMs round-trips the client's adaptive delay range") {
    CHECK(LagComp::toWireMs(0.033f) == 33);
    CHECK(LagComp::toWireMs(0.100f) == 100);
    CHECK(LagComp::toWireMs(0.150f) == 150);
    CHECK(LagComp::toWireMs(0.500f) == LagComp::MAX_INTERP_DELAY_MS); // clamped at the cap
    CHECK(LagComp::toWireMs(-1.0f)  == 0);                            // never negative
}
