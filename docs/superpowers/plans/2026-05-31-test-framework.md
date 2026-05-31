# Test Framework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Vendor doctest as a single-header dependency, add a `tests/` tree with a sanity test that proves the wiring works, hook into the top-level CMake build (BUILD_TESTS option + NINTENDO_SWITCH gate), wire CTest integration, and document the TDD workflow in CLAUDE.md — so the netplay rewrite (M1+) can be developed test-first.

**Architecture:** Vendored `external/doctest/doctest.h` (single ~225 KB header, MIT). New `tests/` directory with `test_main.cpp` (5-line doctest runner) + `sanity_test.cpp` (permanent canary). New `tests/CMakeLists.txt` builds `dungeon_tests` executable. Top-level `CMakeLists.txt` gains `option(BUILD_TESTS "Build unit tests" ON)` + `if(NOT NINTENDO_SWITCH AND BUILD_TESTS)` gate around `enable_testing()` and `add_subdirectory(tests)`. CTest registers `dungeon_tests` as one test; doctest itself enumerates the individual `TEST_CASE`s. Switch builds skip the entire tests/ subtree.

**Tech Stack:** doctest 2.4.11 (single-header), CMake 3.16+, C++17, CTest.

**Reference spec:** [/home/aaron/game/docs/superpowers/specs/2026-05-31-test-framework-design.md](../specs/2026-05-31-test-framework-design.md). Read its "Architecture", "Directory layout", "CMake integration", and "Switch-Build Behavior" sections before executing.

---

## Pre-flight Notes

**This is a meta-plan: the test framework's own verification IS its test.** The "TDD" pattern doesn't apply recursively to building a test runner — you can't write a doctest test to verify doctest is wired up before doctest is wired up. The sanity test created in Task 2 is the canary that proves correctness from Task 3 onward.

**Tree state when starting:** Master is clean (per M0). `external/` already has the vendoring pattern in place (enet/, glad/, SDL2/, SDL_mixer/, json/, stb/). The top-level CMakeLists.txt is 10 lines; the per-directory CMakeLists are `external/CMakeLists.txt` (deps) and `src/CMakeLists.txt` (game binary).

**Reproducible vendoring:** Pin to doctest v2.4.11 (a known-stable tag at time of writing). If the tag has moved or no longer exists when the plan is executed, fall back to v2.4.12 / latest stable — the API is forward-compatible within 2.x.

**Switch build:** This plan does not attempt to build for Switch. The Switch build runs in a separate devkitPro Docker environment; the gate `if(NOT NINTENDO_SWITCH AND BUILD_TESTS)` keeps the test target invisible to that toolchain. Verification of the gate is by inspection of the CMake config output, not by running a Switch build.

---

## Task 1: Vendor doctest Header

**Files:**
- Create: `external/doctest/doctest.h` (downloaded, single header ~225 KB)

- [ ] **Step 1: Create the doctest directory**

```bash
mkdir -p external/doctest
```

Expected: directory exists, no output.

- [ ] **Step 2: Download doctest v2.4.11 single header**

```bash
curl -fsSL -o external/doctest/doctest.h \
    https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h
```

Expected: silent success. If you see "Could not resolve host" or 404, network or tag is wrong — try `v2.4.12`, then `v2.4.10`, then `master` (last resort, not pinned).

- [ ] **Step 3: Verify the download**

```bash
ls -la external/doctest/doctest.h && head -5 external/doctest/doctest.h
```

Expected:
- File size: roughly 220–240 KB (within 10% — exact byte count depends on tag).
- First 5 lines include `// = LICENSING INFORMATION` or similar doctest header banner mentioning MIT license. Specifically the first non-blank line should reference "doctest.h" or "the lightest feature-rich C++ single-header testing framework". If you instead see a 404 HTML body, the download failed — re-run Step 2 with a different tag.

- [ ] **Step 4: Record the SHA256 for reproducibility**

```bash
sha256sum external/doctest/doctest.h
```

Copy the hex output — you'll paste it into the commit message in Step 5 so a future re-vendor can verify they got the same file.

- [ ] **Step 5: Stage and commit**

```bash
git add external/doctest/doctest.h
git diff --cached --stat
```

Expected: 1 file added, ~7000 lines (doctest source).

Then (substitute the SHA you recorded in Step 4 into the message):

```bash
git commit -m "$(cat <<'EOF'
deps: vendor doctest v2.4.11 single header

Drops doctest.h into external/doctest/ matching the existing vendoring
pattern (enet, glad, json, stb, SDL2, SDL_mixer). Header-only, MIT-licensed,
no build step. Pinned to v2.4.11 for reproducibility — bump the URL and
the SHA256 below when intentionally updating.

SHA256: <paste hex from sha256sum here>

Source: https://github.com/doctest/doctest/releases/tag/v2.4.11

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

If you can't run interactive editing, build the message in a temp file with the actual SHA substituted before invoking `git commit -F <file>`.

---

## Task 2: Create tests/ Skeleton

**Files:**
- Create: `tests/test_main.cpp`
- Create: `tests/sanity_test.cpp`
- Create: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `tests/` directory and the runner file**

```bash
mkdir -p tests
```

Write `tests/test_main.cpp` with exactly this content:

```cpp
// test_main.cpp — doctest runner.
//
// One file in the test target defines DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN, which makes
// doctest emit its `int main(int, char**)`. All other test files just
// `#include <doctest/doctest.h>` and register TEST_CASEs — the runner picks them up
// via static-init self-registration. Adding a new test file does NOT require any
// edit here.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

- [ ] **Step 2: Create the sanity test**

Write `tests/sanity_test.cpp` with exactly this content:

```cpp
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
```

- [ ] **Step 3: Create the test CMakeLists**

Write `tests/CMakeLists.txt` with exactly this content:

```cmake
# tests/CMakeLists.txt — dungeon_tests executable.
#
# Gated by the top-level BUILD_TESTS option (default ON for desktop, OFF for Switch).
# v1 ships with sanity-only — no production sources linked in yet. Future test files
# add (a) themselves and (b) any production .cpp they need by extending the
# add_executable() source list. When the list grows past comfortable, refactor src/
# into a dungeon_core library that both DungeonEngine and dungeon_tests link.

add_executable(dungeon_tests
    test_main.cpp
    sanity_test.cpp
)

target_include_directories(dungeon_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
)

target_compile_features(dungeon_tests PRIVATE cxx_std_17)

# Faster compile of passing asserts — TDD loop friendliness. Failing asserts still
# produce full decomposition output; this only affects the hot path.
target_compile_definitions(dungeon_tests PRIVATE
    DOCTEST_CONFIG_SUPER_FAST_ASSERTS
)

# CTest registration. One add_test() per binary — doctest enumerates the
# individual TEST_CASEs inside. `--no-version` keeps the output clean.
add_test(NAME dungeon_tests COMMAND dungeon_tests --no-version)
```

- [ ] **Step 4: Verify the files exist**

```bash
ls -la tests/
```

Expected output:
```
test_main.cpp
sanity_test.cpp
CMakeLists.txt
```

Do NOT try to build yet — the top-level CMakeLists hasn't been wired (Task 3). At this point the new files exist on disk but are not part of any target.

- [ ] **Step 5: Stage and commit**

```bash
git add tests/test_main.cpp tests/sanity_test.cpp tests/CMakeLists.txt
git diff --cached --stat
git commit -m "$(cat <<'EOF'
tests: add tests/ skeleton with sanity canary (test framework foundation)

Three files:
  test_main.cpp        doctest runner (one DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
  sanity_test.cpp      permanent canary — trivial arithmetic + Approx + decomp
  CMakeLists.txt       dungeon_tests target (sanity-only sources for now)

DOCTEST_CONFIG_SUPER_FAST_ASSERTS shaved off the passing-assert hot path for
TDD-loop responsiveness. CTest registers dungeon_tests as a single add_test;
doctest itself enumerates the per-TEST_CASE results.

Top-level CMake wiring lands in the next commit — these files are inert until
add_subdirectory(tests) is called.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Hook tests/ Into Top-Level CMake + Verify End-to-End

**Files:**
- Modify: `CMakeLists.txt` (top-level)

- [ ] **Step 1: Read the current top-level CMakeLists**

```bash
cat CMakeLists.txt
```

Expected current content (verbatim):
```
cmake_minimum_required(VERSION 3.16)
project(DungeonEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(external)
add_subdirectory(src)
```

(10 lines. If it differs, base your edit on the actual file.)

- [ ] **Step 2: Edit top-level CMakeLists**

Use the Edit tool. Replace the full file with this version:

old_string:
```
cmake_minimum_required(VERSION 3.16)
project(DungeonEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(external)
add_subdirectory(src)
```

new_string:
```
cmake_minimum_required(VERSION 3.16)
project(DungeonEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Unit tests are built by default on desktop but skipped on Switch (the devkitPro
# Docker toolchain doesn't see tests/ and shouldn't try to). Opt-out via
# -DBUILD_TESTS=OFF for a game-only desktop build.
option(BUILD_TESTS "Build unit tests" ON)

add_subdirectory(external)
add_subdirectory(src)

if(NOT NINTENDO_SWITCH AND BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

- [ ] **Step 3: Re-configure CMake (picks up the new option + subdir)**

```bash
cmake -B build 2>&1 | tail -20
```

Expected: configuration succeeds, no errors. Watch for any line like `-- Configuring done` near the bottom. If you see a CMake error about a missing target or include, the file edit went wrong.

- [ ] **Step 4: Build the test target**

```bash
cmake --build build --target dungeon_tests 2>&1 | tail -15
```

Expected: compiles cleanly. The doctest header is ~225 KB but single-translation-unit compilation should take <10 seconds even cold. Warnings about unused `-W` flags from the host compiler are OK; hard errors are not.

If you get `fatal error: doctest/doctest.h: No such file or directory`, the include path didn't pick up — re-check that `target_include_directories` in `tests/CMakeLists.txt` references `${CMAKE_SOURCE_DIR}/external` and that the header was vendored to `external/doctest/doctest.h`.

- [ ] **Step 5: Run the test binary directly**

```bash
./build/tests/dungeon_tests
```

Expected output (approximate — formatting depends on doctest version):
```
[doctest] doctest version is "2.4.11"
[doctest] run with "--help" for options
===============================================================================
[doctest] test cases:  3 |  3 passed | 0 failed | 0 skipped
[doctest] assertions:  5 |  5 passed | 0 failed |
[doctest] Status: SUCCESS!
```

3 test cases, 5 assertions, all passing. If anything fails, the sanity test or the framework wiring is wrong — read the failure output, it tells you the file:line and decomposed expression.

- [ ] **Step 6: Verify CTest integration**

```bash
ctest --test-dir build --output-on-failure
```

Expected:
```
Test project /home/aaron/game/build
    Start 1: dungeon_tests
1/1 Test #1: dungeon_tests ...................   Passed    <time>

100% tests passed, 0 tests failed out of 1
```

- [ ] **Step 7: Verify BUILD_TESTS=OFF gate**

```bash
cmake -B build-notests -DBUILD_TESTS=OFF 2>&1 | grep -i 'tests\|dungeon_tests\|doctest' | head -5
```

Expected: no `tests/` references in the configure output. Confirm the build directory has no test target:

```bash
cmake --build build-notests --target dungeon_tests 2>&1 | tail -3
```

Expected: failure with a message like `make: *** No rule to make target 'dungeon_tests'`. That proves the gate works.

Cleanup:
```bash
rm -rf build-notests
```

- [ ] **Step 8: Verify Switch gate by inspection**

We don't actually run a Switch build here (no devkitPro toolchain). Instead, verify by reading the CMakeLists edit:

```bash
grep -A1 'NINTENDO_SWITCH' CMakeLists.txt
```

Expected: shows the `if(NOT NINTENDO_SWITCH AND BUILD_TESTS)` line. The Switch build path (in `src/CMakeLists.txt`, gated on `if(NINTENDO_SWITCH)`) won't trigger because that variable would have to be set externally — meaning our new condition `NOT NINTENDO_SWITCH` is false and tests are skipped.

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt
git diff --cached --stat
git commit -m "$(cat <<'EOF'
build: wire tests/ into top-level CMakeLists + CTest

Adds BUILD_TESTS option (default ON) gated by NOT NINTENDO_SWITCH so
desktop developers get the test target for free while Switch builds via
devkitPro Docker never see tests/. enable_testing() + add_subdirectory(tests)
brings dungeon_tests under CTest as a single test entry; doctest's runner
enumerates the per-TEST_CASE results.

Verified end-to-end:
  - cmake -B build && cmake --build build --target dungeon_tests succeeds
  - ./build/tests/dungeon_tests reports 3 cases, 5 assertions, SUCCESS
  - ctest --test-dir build --output-on-failure reports 1/1 passed
  - cmake -B build-notests -DBUILD_TESTS=OFF skips the test target

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Document Testing Workflow in CLAUDE.md

**Files:**
- Modify: `CLAUDE.md` (add Testing section)

- [ ] **Step 1: Find the Build section in CLAUDE.md**

```bash
grep -n '^## Build\|^## Architecture\|^## Directory Map' CLAUDE.md
```

Expected: line numbers for the `## Build`, `## Architecture`, `## Directory Map`, `## Conventions` headers. The new Testing section goes between Build and Architecture.

- [ ] **Step 2: Read the Build section to find an anchor**

```bash
sed -n '/^## Build/,/^## Architecture/p' CLAUDE.md
```

Note the last line of the Build section — that's where the Edit will anchor.

- [ ] **Step 3: Insert the Testing section**

Use the Edit tool. Add a new `## Testing` section immediately before the existing `## Architecture` heading. Use this content:

old_string (the section break — actual text from grep above):
```
## Architecture
```

new_string:
```
## Testing

doctest-based unit tests live in `tests/` (mirrors `src/` structure). The framework is vendored as a single header at `external/doctest/doctest.h`.

```bash
cmake --build build --target dungeon_tests   # build only the test binary
./build/tests/dungeon_tests                   # run the full suite
./build/tests/dungeon_tests -tc="*ClockSync*" # filter to matching cases
ctest --test-dir build --output-on-failure   # CTest wrapper
```

`BUILD_TESTS=ON` is the default for desktop; Switch builds skip tests entirely. Opt out with `cmake -B build -DBUILD_TESTS=OFF` for a game-only desktop build.

**TDD workflow.** The netplay rewrite (M1+) is test-first: write a `TEST_CASE` in `tests/<subsystem>/test_<unit>.cpp` describing the next behavior, watch it fail with a specific assertion, implement until it passes, refactor, commit. doctest's expression decomposition (`REQUIRE(a == b)` shows both values on failure) and `doctest::Approx` (for floating-point) are the primary tools.

**Adding a test that touches production code.** Extend the `add_executable(dungeon_tests ...)` source list in `tests/CMakeLists.txt` with both the new test file AND any production `.cpp` it links against. When the list gets unwieldy, refactor `src/` into a `dungeon_core` library that both `DungeonEngine` and `dungeon_tests` link.

**Scope.** Forward-only on new code — no backfill tests for existing combat/AI/item-gen/render. See [docs/superpowers/specs/2026-05-31-test-framework-design.md](docs/superpowers/specs/2026-05-31-test-framework-design.md) for the rationale.

## Architecture
```

The triple-backtick fence inside the new_string is part of the markdown — be careful with HEREDOC escaping if the Edit tool needs it; the Edit tool itself treats both arguments as literal strings, so the fences pass through unchanged.

- [ ] **Step 4: Verify the section landed correctly**

```bash
grep -n '^## Testing\|^## Architecture' CLAUDE.md
```

Expected: line numbers for both, with Testing appearing before Architecture.

```bash
sed -n '/^## Testing/,/^## Architecture/p' CLAUDE.md | head -40
```

Expected: the Testing section content as you inserted it, ending with the `## Architecture` line.

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md
git diff --cached --stat
git commit -m "$(cat <<'EOF'
docs: document Testing workflow in CLAUDE.md

New ## Testing section between Build and Architecture covering:
  - Where tests live (tests/ mirrors src/)
  - How to build and run (cmake target, doctest filters, ctest)
  - BUILD_TESTS gate (default ON desktop, OFF Switch)
  - TDD workflow for netplay rewrite milestones
  - How to extend the test binary's source list
  - Forward-only scope with pointer to the design spec

Closes the test-framework v1 — M1's ClockSync work can now lead with
tests.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Final End-to-End Verification

**Files:** none (verification only)

- [ ] **Step 1: Confirm clean tree**

```bash
git status --short
```

Expected: empty.

- [ ] **Step 2: Confirm 4 new commits land in the test-framework series**

```bash
git log 4173e19..HEAD --oneline
```

(4173e19 is the test-framework SPEC commit, made at the end of the brainstorming session.) Expected output:

```
<hash> docs: document Testing workflow in CLAUDE.md
<hash> build: wire tests/ into top-level CMakeLists + CTest
<hash> tests: add tests/ skeleton with sanity canary (test framework foundation)
<hash> deps: vendor doctest v2.4.11 single header
```

(4 commits, hashes will differ.)

- [ ] **Step 3: Final clean-build sanity from a fresh configure**

```bash
rm -rf build && cmake -B build 2>&1 | tail -5 && cmake --build build 2>&1 | tail -5
```

Expected: full configure + full game build + test build all succeed. The last line should be `[100%] Built target ...`.

- [ ] **Step 4: Re-run the full test suite**

```bash
./build/tests/dungeon_tests
```

Expected: 3 cases, 5 assertions, SUCCESS.

- [ ] **Step 5: Re-run CTest**

```bash
ctest --test-dir build --output-on-failure
```

Expected: `1/1 Test #1: dungeon_tests ...   Passed`.

- [ ] **Step 6: Document any deviations**

If any task hit a snag (e.g., doctest tag v2.4.11 was unavailable and you fell back to v2.4.12, or the CLAUDE.md insertion ended up before a slightly different anchor), record it as a quick reply to the user before declaring done — so the next agent has the actual final state.

---

## What This Plan Does Not Do

- **Does not write any real tests.** The sanity test is a canary; real tests come with M1 (ClockSync), M2 (input ring), M3+ (prediction), etc.
- **Does not refactor `src/` into a library.** Test target links production sources directly per-test for v1. If many tests start sharing many sources, that refactor becomes worthwhile — its own milestone.
- **Does not add sanitizers / coverage / CI.** All deferred to later concerns.
- **Does not retrofit the M1 plan to TDD.** That's a separate follow-up that uses this framework.

## Definition of Done

- [ ] `git status --short` is empty
- [ ] `git log 4173e19..HEAD --oneline | wc -l` returns 4
- [ ] `external/doctest/doctest.h` exists and is ~225 KB
- [ ] `tests/test_main.cpp`, `tests/sanity_test.cpp`, `tests/CMakeLists.txt` all exist
- [ ] `cmake -B build && cmake --build build` succeeds clean (full configure + full build)
- [ ] `./build/tests/dungeon_tests` returns 0 with "SUCCESS!" output and 3 cases / 5 assertions
- [ ] `ctest --test-dir build --output-on-failure` reports `1/1 Test ... Passed`
- [ ] `cmake -B build-notests -DBUILD_TESTS=OFF` does NOT produce a `dungeon_tests` target (verified by `make dungeon_tests` failing with "No rule"). Then `rm -rf build-notests`.
- [ ] `grep -c '^## Testing' CLAUDE.md` returns 1
