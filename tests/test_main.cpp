// test_main.cpp — doctest runner.
//
// One file in the test target defines DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN, which makes
// doctest emit its `int main(int, char**)`. All other test files just
// `#include <doctest/doctest.h>` and register TEST_CASEs — the runner picks them up
// via static-init self-registration. Adding a new test file does NOT require any
// edit here.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
