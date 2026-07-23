// tests/game/test_autoplay_control.cpp — the takeover/resume latch: the bot yields to a human
// instantly on real device activity and resumes after RESUME_SECONDS of inactivity; UI screens
// never count as takeover (so the player can browse the build while the bot keeps fighting).
#include "doctest/doctest.h"
#include "game/autoplay_control.h"

TEST_CASE("starts in bot control") {
    AutoplayControl c;
    CHECK(c.botInControl());
}

TEST_CASE("human activity grabs control instantly, same tick") {
    AutoplayControl c;
    c.tick(/*humanActive=*/true, /*uiOpen=*/false, 1.0f/60.0f);
    CHECK_FALSE(c.botInControl());
}

TEST_CASE("bot resumes only after the full RESUME_SECONDS of inactivity") {
    AutoplayControl c;
    c.tick(true, false, 1.0f/60.0f);              // human takes over
    REQUIRE_FALSE(c.botInControl());
    const f32 dt = 0.1f;
    // Stop the loop a FULL tick short of the threshold: at 0.1 s ticks, 20 iterations would apply
    // exactly RESUME_SECONDS of countdown and the bot would already have resumed on the last tick.
    // -0.15f leaves 19 ticks (timer ~0.1 s), so we are genuinely "just under" with real headroom.
    for (f32 t = 0.0f; t < AutoplayControl::RESUME_SECONDS - 0.15f; t += dt)
        c.tick(false, false, dt);
    CHECK_FALSE(c.botInControl());                // just under the threshold: still manual
    c.tick(false, false, 0.1f);
    CHECK(c.botInControl());                      // crossed RESUME_SECONDS: bot resumes
}

TEST_CASE("activity mid-countdown re-arms the full timer") {
    AutoplayControl c;
    c.tick(true, false, 0.016f);
    for (f32 t = 0; t < 1.0f; t += 0.1f) c.tick(false, false, 0.1f);
    c.tick(true, false, 0.016f);                  // touched again
    for (f32 t = 0; t < AutoplayControl::RESUME_SECONDS - 0.2f; t += 0.1f) c.tick(false, false, 0.1f);
    CHECK_FALSE(c.botInControl());                // timer restarted, not resumed yet
}

TEST_CASE("input while a UI screen is open never grabs control") {
    AutoplayControl c;                            // bot in control
    c.tick(/*humanActive=*/true, /*uiOpen=*/true, 0.016f);   // Tab/nav keys while inventory open
    CHECK(c.botInControl());                      // bot keeps playing underneath the inventory
}

TEST_CASE("closing the UI and then acting DOES grab control") {
    AutoplayControl c;
    c.tick(true, true, 0.016f);                   // activity ignored (UI open)
    CHECK(c.botInControl());
    c.tick(true, false, 0.016f);                  // now acting with no UI open
    CHECK_FALSE(c.botInControl());
}
