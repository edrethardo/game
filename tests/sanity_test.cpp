// sanity_test.cpp — permanent canary. Proves the framework is wired up. If this
// ever fails, doctest itself or the build wiring is broken, not the unit under test.
// Do NOT remove these cases when adding real tests — leave the canary in tree.

#include <doctest/doctest.h>

TEST_CASE("framework smoke: trivial arithmetic") {
    CHECK(1 + 1 == 2);
    CHECK(2 * 3 == 6);
}

TEST_CASE("framework smoke: expression decomposition works") {
    // doctest decomposes CHECK(a == b) and reports both values on failure.
    // This case stays passing, but verifies the macro is real (not a `#define CHECK(x)
    // ((void)0)` from a broken include path).
    const int a = 5;
    const int b = 5;
    CHECK(a == b);
}

TEST_CASE("framework smoke: floating point with Approx") {
    // doctest::Approx is the canonical fp comparison helper — netplay tests will lean
    // on it heavily for tick / time / quantized-position math.
    const double x = 0.1 + 0.2;
    CHECK(x == doctest::Approx(0.3));
}
