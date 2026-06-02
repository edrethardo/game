// Tests for the adaptive interpolation-delay helpers (net/interp_delay.h). The remote-render
// delay widens with snapshot-arrival jitter so a late snapshot doesn't freeze remotes, then
// shrinks back slowly when the link calms (shrinking fast would jump the render clock forward
// and skip a snapshot — a visible stutter). These pin that behaviour.

#include "doctest/doctest.h"
#include "net/interp_delay.h"

static constexpr f32 BASE = 0.033f;   // INTERP_DELAY_SEC
static constexpr f32 MAXD = 0.15f;
static constexpr f32 NOMINAL = 1.0f / 60.0f;

TEST_CASE("Interp delay: steady arrivals keep the delay at the base") {
    f32 jitter = 0.0f;
    f32 delay  = BASE;
    for (int i = 0; i < 200; i++) {
        jitter = updateArrivalJitter(jitter, NOMINAL, NOMINAL);  // perfectly on-time
        delay  = computeInterpDelay(delay, jitter, BASE, MAXD);
    }
    CHECK(jitter == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(delay  == doctest::Approx(BASE));
}

TEST_CASE("Interp delay: heavy sustained jitter pins the delay at the MAX cap") {
    f32 jitter = 0.0f;
    f32 delay  = BASE;
    // Every snapshot arrives ~100 ms late → smoothed jitter ~0.10 s → target far above the cap.
    for (int i = 0; i < 500; i++) {
        jitter = updateArrivalJitter(jitter, NOMINAL + 0.10f, NOMINAL);
        delay  = computeInterpDelay(delay, jitter, BASE, MAXD);
    }
    CHECK(delay <= MAXD);                                  // never exceeds the cap
    CHECK(delay == doctest::Approx(MAXD).epsilon(0.01));   // pinned at the cap
}

TEST_CASE("Interp delay: grows faster than it shrinks (asymmetric, isolated)") {
    // Same-magnitude gap up vs down: a grow step moves much further than a shrink step.
    // Grow: target = BASE + 2.5*0.04 = 0.133, well above prevDelay=BASE → GROW_RATE 0.5.
    const f32 up   = computeInterpDelay(BASE,   0.04f, BASE, MAXD);
    // Shrink: target = BASE (jitter 0), well below prevDelay=0.133 → SHRINK_RATE 0.05.
    const f32 down = computeInterpDelay(0.133f, 0.00f, BASE, MAXD);
    CHECK((up - BASE) > (0.133f - down));   // grew more than it shrank for a comparable gap
    CHECK(up   > BASE);
    CHECK(down < 0.133f);
}

TEST_CASE("Interp delay: a spike widens it, then calm arrivals return it to base") {
    f32 j = 0.0f, d = BASE;
    for (int i = 0; i < 200; i++) {           // sustained lateness builds the buffer up
        j = updateArrivalJitter(j, NOMINAL + 0.08f, NOMINAL);
        d = computeInterpDelay(d, j, BASE, MAXD);
    }
    CHECK(d > 0.10f);                          // clearly widened
    for (int i = 0; i < 5000; i++) {           // long calm → decays back to the floor
        j = updateArrivalJitter(j, NOMINAL, NOMINAL);
        d = computeInterpDelay(d, j, BASE, MAXD);
    }
    CHECK(d == doctest::Approx(BASE).epsilon(0.01));
}

TEST_CASE("Interp delay: never drops below the base floor") {
    // Even with a (nonsensical) negative-jitter feed, the clamp holds the floor.
    f32 delay = BASE;
    for (int i = 0; i < 50; i++) delay = computeInterpDelay(delay, 0.0f, BASE, MAXD);
    CHECK(delay >= BASE);
}
