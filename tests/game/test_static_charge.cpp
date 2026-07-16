// test_static_charge.cpp — Capacitor Mail (STATIC_CHARGE armor passive) stack logic. One pure
// helper feeds all the call sites (host + remote, tickArmorRingPassives + serverNetPost), so
// this is the whole behavior surface: hits build stacks, the 5th discharges and resets, and
// stacks decay when the 10s window runs dry.

#include <doctest/doctest.h>
#include "game/static_charge.h"

TEST_CASE("StaticCharge: hits build stacks, the 5th discharges and resets") {
    u8 s = 0; f32 t = 0.0f;
    for (int i = 0; i < 4; i++) CHECK_FALSE(StaticCharge::accumulate(s, t, true, 0.016f));
    CHECK(s == 4);
    CHECK(t == doctest::Approx(StaticCharge::WINDOW_SEC));
    CHECK(StaticCharge::accumulate(s, t, true, 0.016f));   // 5th hit -> discharge
    CHECK(s == 0);                                          // reset after discharge
    CHECK(t == 0.0f);
}

TEST_CASE("StaticCharge: stacks decay when the window runs out") {
    u8 s = 0; f32 t = 0.0f;
    StaticCharge::accumulate(s, t, true, 0.016f);
    CHECK(s == 1);
    StaticCharge::accumulate(s, t, false, StaticCharge::WINDOW_SEC + 1.0f);
    CHECK(s == 0);
    CHECK(t == 0.0f);
}

TEST_CASE("StaticCharge: quiet ticks only run the window down") {
    u8 s = 2; f32 t = 5.0f;
    CHECK_FALSE(StaticCharge::accumulate(s, t, false, 0.016f));
    CHECK(s == 2);
    CHECK(t == doctest::Approx(5.0f - 0.016f));
}
