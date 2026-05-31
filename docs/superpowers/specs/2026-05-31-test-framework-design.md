# DungeonEngine Test Framework — Design

A small, vendored, header-only test framework so the netplay rewrite (and any future
state-machine / math-heavy work) can be developed TDD-style.

## Context

**Why.** DungeonEngine has no test infrastructure today. The in-flight netplay rewrite
(14 milestones; M0 just landed) repeatedly hits subsystems that are highly testable —
ClockSync math (M1), input ring buffers (M2), prediction divergence detection (M3),
entity pose ring + lag-comp rewind (M5), delta-compressed snapshot encode/decode (M11),
smooth correction lerp (M8). Without a test framework these have to be verified by
running the game and eyeballing logs, which is slow and unreliable for the kinds of
subtle bugs (off-by-one tick, wrong drift sign, NaN propagation) that netplay generates.

The user's direction is **forward-only TDD**: every netplay rewrite milestone going
forward writes tests first, then code. Existing engine code (combat, AI, item gen) is
NOT backfilled — too much effort, not enough payoff for a solo developer.

**Outcome.** A `tests/` tree, a `dungeon_tests` executable, and a CMake option that
turns the whole thing off cleanly on Switch builds. Test-writing has the smallest
possible per-file boilerplate (one `#include`, `TEST_CASE` macros, no manual `main`).
Running the suite is one command: `cmake --build build && ./build/tests/dungeon_tests`.

## Goals & Non-Goals

**Goals:**

1. **Zero ceremony per test file.** A new test is just a `tests/<subsystem>/test_<unit>.cpp`
   with an `#include` and one or more `TEST_CASE(...)` blocks.
2. **Fast TDD loop.** Incremental rebuild of a single edited test + the units it touches
   in ≤2 seconds on the dev machine. Full suite run in ≤1 second once it has reached
   reasonable size (~100 tests).
3. **Switch unaffected.** Switch builds (via devkitPro Docker) skip the tests/ subtree
   entirely. No new build-time dependencies, no host-only headers leaking in.
4. **Vendored only.** Framework drops into `external/doctest/doctest.h` like every other
   dependency. No network fetch at build time, no submodule.
5. **TDD-required for netplay rewrite.** Each future milestone (M1–M14) adds a test file
   *before* the implementation lands. Tests live alongside the milestone's code commits.
6. **CTest integration.** `ctest --output-on-failure` from the build dir runs every test
   case as a discoverable test. Lets CI hook in later with no rework.

**Non-goals:**

- Backfill tests for existing combat/AI/item-gen/render/level-gen code.
- Property-based / fuzz testing infrastructure.
- Mock/stub frameworks (no GoogleMock, no abstract-interface refactors for testability).
- Sanitizer/coverage build presets — can be added later as a separate concern, not
  blocking v1.
- Integration tests that simulate a fake server/client transport. Those can come if
  needed in a later milestone.
- Tests that need an OpenGL context, an audio device, or a live SDL2 window.

## Framework Choice: doctest

Single header (~7k lines, MIT-licensed) vendored as `external/doctest/doctest.h`.
Chosen over alternatives because:

- **Compile speed.** Fastest of the doctest/Catch2 family — critical for TDD loops.
- **Header-only.** No separate build step, no library to link, matches the project's
  existing "drop into external/, add_subdirectory" pattern.
- **Expression decomposition.** `REQUIRE(a == b)` reports both values on failure, not
  just "expected true, got false". Worth real effort over a custom CHECK macro.
- **`DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`.** One source file pulls in the runner; every
  other test file is just `#include <doctest/doctest.h>` + `TEST_CASE(...)`.
- **Test discovery via macros.** No registration boilerplate — `TEST_CASE` self-registers
  at static-init time.
- **Filter / subset flags.** `-tc=name`, `-ts=suite`, `--no-run` for compile-only checks,
  `-d` for duration reporting. Plenty of TDD-friendly ergonomics.

Rejected alternatives:
- **Catch2 v2 (header-only)**: ~17k lines, slower compile, same features.
- **Catch2 v3 (library)**: faster incremental compile but adds CMake build step.
- **Custom ~50-line framework**: no expression decomposition; TDD ergonomics degrade.

## Architecture

### Vendoring

Drop a single file at `external/doctest/doctest.h`. Source: the latest stable doctest
release from <https://github.com/doctest/doctest> (vendored manually, no submodule).
Bump version when it makes sense by replacing the file.

`external/CMakeLists.txt` adds nothing — doctest is header-only, the test target
just adds `external/doctest` to its include path.

### Directory layout

```
external/
  doctest/
    doctest.h                          # vendored single header

tests/
  CMakeLists.txt                       # builds dungeon_tests, gated on BUILD_TESTS
  test_main.cpp                        # 5-line doctest runner (DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
  net/
    test_clock_sync.cpp                # M1 — ClockSync math + state machine
    test_input_ring.cpp                # M2 — rolling input window + ack ordering
    test_snapshot_delta.cpp            # M11 — delta encode/decode roundtrip
    ...
  core/
    test_quantize.cpp                  # quantization round-trip (when first needed)
  ...
```

`tests/` mirrors `src/` so test → source mapping is unambiguous: `tests/net/test_X.cpp`
tests `src/net/X.{h,cpp}`. Subdirectories grow as needed; no need to pre-create empty
ones.

### CMake integration

Top-level `CMakeLists.txt` gains:

```cmake
option(BUILD_TESTS "Build unit tests" ON)
if(NOT NINTENDO_SWITCH AND BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

`tests/CMakeLists.txt` defines the `dungeon_tests` target. Each test file lists the
production `.cpp` files it needs to link, plus `test_main.cpp` and the doctest include
path. For v1, link the production sources directly — no `dungeon_core` library refactor.
If the test target gets large enough that this becomes painful, the refactor can happen
later as its own milestone.

Sketch:

```cmake
add_executable(dungeon_tests
    test_main.cpp
    net/test_clock_sync.cpp
    ${CMAKE_SOURCE_DIR}/src/net/clock_sync.cpp
    # other production sources added per-test as needed
)
target_include_directories(dungeon_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
)
target_compile_definitions(dungeon_tests PRIVATE
    DOCTEST_CONFIG_SUPER_FAST_ASSERTS
)
add_test(NAME dungeon_tests COMMAND dungeon_tests --no-version)
```

`DOCTEST_CONFIG_SUPER_FAST_ASSERTS` shaves more compile time off non-failing asserts.

### test_main.cpp

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

That's the whole file. Every other test file is:

```cpp
#include <doctest/doctest.h>
#include "net/clock_sync.h"

TEST_CASE("ClockSync seeds from first pong") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    REQUIRE(cs.bootstrapped == false);
    ClockSyncOps::onPongReceived(cs, /*clientSentMs=*/100, /*serverTickAtRecv=*/200, /*pongRecvNowSec=*/0.130);
    CHECK(cs.bootstrapped == true);
    CHECK(cs.pongsReceived == 1);
    CHECK(cs.oneWayTripMs == doctest::Approx(15.0f));  // 30 ms RTT / 2
}
```

## TDD Workflow

For every netplay rewrite milestone (M1+):

1. Write a `TEST_CASE` in `tests/<subsystem>/test_<unit>.cpp` describing the next
   behavior you want. The unit may not exist yet — the test references types/functions
   that haven't been written.
2. Try to build: `cmake --build build/tests`. Expect failure (unknown identifier).
3. Stub out the unit just enough for the test to *compile*. Test will fail because
   it asserts something the stub doesn't deliver.
4. Run: `./build/tests/dungeon_tests -tc="ClockSync seeds*"`. Watch the specific
   failure.
5. Implement the unit until the test passes. Run again. Green.
6. Refactor if needed. Tests stay green.
7. Commit (test + implementation together, or test first then implementation in a
   second commit — either works).
8. Repeat for the next behavior.

**Tactical rules:**

- One `TEST_CASE` per behavior, not per function. Behaviors are what the spec
  describes; functions are an implementation detail.
- Tests assert on outputs and observable state changes, not on internal field values
  the test had to read via a friend declaration. If you have to add a `friend` for a
  test, that's a smell — make the field public or expose a function for it.
- `CHECK` keeps running on failure; `REQUIRE` halts. Use REQUIRE for preconditions
  (assumption-of-a-valid-setup), CHECK for actual assertions. Lets one test failure
  show all related issues at once.
- Floating-point comparisons use `doctest::Approx(x).epsilon(0.001)` — never
  raw `==` on f32/f64.
- Don't write tests for things you can't fail. `CHECK(true)` is a smell.

## Switch-Build Behavior

The `if(NOT NINTENDO_SWITCH AND BUILD_TESTS)` gate at the top level means a Switch
build (via devkitPro Docker) never sees `tests/`. The Docker image doesn't need
doctest or any test-related toolchain bits. `BUILD_TESTS=ON` is the default for
desktop builds (so a stock `cmake -B build` brings tests with it); a `cmake -B
build -DBUILD_TESTS=OFF` opt-out is available for cases where someone wants the
game-only build.

## What Lives in v1 vs Future

**v1 (this design):**
- Vendored doctest header
- `tests/` skeleton with `test_main.cpp` + one example test (`tests/sanity_test.cpp`
  with `TEST_CASE("framework smoke") { CHECK(1 == 1); }` — proves the framework wires
  up before any milestone leans on it).
- CMake wiring + CTest integration
- README note (or section in CLAUDE.md) explaining how to write a test

**Deferred to later milestones (if/when needed):**
- Per-subsystem test binaries (split if `dungeon_tests` link time gets painful)
- `dungeon_core` library refactor (split if many test files need many of the same
  source files)
- Sanitizer build preset (-fsanitize=address,undefined)
- Coverage build preset (gcov / llvm-cov)
- Property/fuzz tests
- Integration tests with fake transport
- CI / GitHub Actions automation

## Open Questions

None blocking implementation. Reasonable defaults picked for everything:
- doctest version: latest stable at time of vendoring
- Test binary name: `dungeon_tests`
- BUILD_TESTS default: ON for non-Switch
- CTest registration: one `add_test` per binary (not per test case) for v1 simplicity

## Verification

- `cmake -B build && cmake --build build` succeeds and produces both
  `build/dungeon_game` (existing) and `build/tests/dungeon_tests` (new).
- `./build/tests/dungeon_tests` exits 0 with the sanity test passing.
- `./build/tests/dungeon_tests -tc="framework smoke"` runs only the sanity case.
- `ctest --test-dir build --output-on-failure` shows `dungeon_tests` as a passing
  test entry.
- `cmake -B build-switch -DNINTENDO_SWITCH=ON` (or however Switch builds are toggled)
  produces no test target and doesn't reference doctest.
- `cmake -B build-notests -DBUILD_TESTS=OFF` produces no test target on desktop too.
