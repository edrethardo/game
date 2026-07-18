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

TEST_CASE("Interp delay: sustained huge gaps saturate at the outage clamp, not the MAX cap") {
    // Pre-outage-guard this fed NOMINAL+0.10 s (≈7× nominal) per sample and pinned the delay at
    // MAXD, treating a 100 ms-late feed as legitimate jitter. FIX 1's outage guard reclassifies
    // any sample > 3× nominal as a LOSS regime — handled by the server coast + input-redundancy
    // window, NOT by inflating the render buffer. So the smoothed jitter now SATURATES at the
    // clamped deviation (3×−1×)·nominal = 2× nominal, and the delay settles at base + K·(2×nominal)
    // ≈ 0.116 s — deliberately BELOW the 0.15 cap (an outage must never permanently tax render
    // latency). NOTE: this means the raised long-haul cap is unreachable via the jitter path alone
    // — see the report's concern on the cap becoming jitter-unreachable.
    f32 jitter = 0.0f;
    f32 delay  = BASE;
    for (int i = 0; i < 500; i++) {
        jitter = updateArrivalJitter(jitter, NOMINAL + 0.10f, NOMINAL);
        delay  = computeInterpDelay(delay, jitter, BASE, MAXD);
    }
    CHECK(delay <= MAXD);                                            // still never exceeds the cap
    CHECK(delay < MAXD);                                             // but no longer PINNED at it
    CHECK(jitter == doctest::Approx(2.0f * NOMINAL).epsilon(0.01));  // saturated at the clamp
    CHECK(delay == doctest::Approx(BASE + 2.5f * 2.0f * NOMINAL).epsilon(0.02));
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

TEST_CASE("InterpDelay: adaptive buffer can now widen past the old 150 ms cap") {
    const f32 base = 0.033f;   // 33 ms
    const f32 maxd = 0.250f;   // NEW long-haul cap (was 0.150)
    // Sustained high arrival jitter (~90 ms smoothed) drives the target above the OLD cap.
    f32 delay = base;
    for (int i = 0; i < 400; i++)
        delay = computeInterpDelay(delay, 0.090f, base, maxd);
    CHECK(delay > 0.150f);                    // would have been pinned at 0.150 before
    CHECK(delay <= 0.250f + 1e-4f);           // never exceeds the new cap
    // And it still floors at base when the link is calm.
    f32 calm = 0.100f;
    for (int i = 0; i < 400; i++) calm = computeInterpDelay(calm, 0.0f, base, maxd);
    CHECK(calm == doctest::Approx(base));
}

TEST_CASE("InterpDelay: an outage gap is not jitter — the EMA input is clamped") {
    const f32 nominal = 1.0f / 60.0f;
    // A 380 ms burst outage produces one 0.38 s inter-arrival gap. Unclamped, that single
    // sample would jump the EMA by (0.38-0.0167)/16 ≈ 23 ms — inflating the interp delay for
    // hundreds of ms AFTER the link already recovered. Clamped at 3x nominal, the worst any
    // one gap can inject is (3x-1x)*nominal/16 ≈ 2 ms.
    f32 j = 0.005f;                                       // calm-link smoothed jitter
    f32 after = updateArrivalJitter(j, 0.380f, nominal);
    CHECK(after - j <= (2.0f * nominal) / 16.0f + 1e-5f); // bounded by the clamp
    // Ordinary jitter below the clamp is untouched: a 30 ms-late snapshot still registers fully.
    f32 ordinary = updateArrivalJitter(j, nominal + 0.030f, nominal);
    CHECK(ordinary > j);                                  // still grows
    CHECK(ordinary - j == doctest::Approx((0.030f - j) / 16.0f).epsilon(0.05));
}
