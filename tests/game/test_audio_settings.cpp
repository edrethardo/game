// test_audio_settings.cpp — unit tests for the pure audio-settings helpers (audio/audio_settings.h).
// SDL-free: the header pulls in only core/types.h, so no SDL_mixer enters the test binary.
#include "doctest/doctest.h"
#include "audio/audio_settings.h"

using namespace AudioSettings;

TEST_CASE("clampVol keeps volume in [0,1]") {
    CHECK(clampVol(-0.5f) == doctest::Approx(0.0f));
    CHECK(clampVol(1.5f)  == doctest::Approx(1.0f));
    CHECK(clampVol(0.7f)  == doctest::Approx(0.7f));
    CHECK(clampVol(0.0f)  == doctest::Approx(0.0f));
    CHECK(clampVol(1.0f)  == doctest::Approx(1.0f));
}

TEST_CASE("stepVol applies a 0.05 increment and clamps at the edges") {
    CHECK(stepVol(0.50f, +1.0f) == doctest::Approx(0.55f));
    CHECK(stepVol(0.50f, -1.0f) == doctest::Approx(0.45f));
    CHECK(stepVol(1.00f, +1.0f) == doctest::Approx(1.00f));  // clamp high
    CHECK(stepVol(0.00f, -1.0f) == doctest::Approx(0.00f));  // clamp low
    CHECK(stepVol(0.98f, +1.0f) == doctest::Approx(1.00f));  // clamp near the top edge
}

TEST_CASE("default constants match the shipped levels") {
    CHECK(DEFAULT_MASTER == doctest::Approx(1.0f));
    CHECK(DEFAULT_SFX    == doctest::Approx(1.0f));
    CHECK(DEFAULT_MUSIC  == doctest::Approx(0.3f));  // music sits below SFX (engine_init default)
}
