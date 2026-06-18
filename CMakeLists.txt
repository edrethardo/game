cmake_minimum_required(VERSION 3.16)
project(DungeonEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Unit tests are built by default on desktop but skipped on Switch (the devkitPro
# Docker toolchain doesn't see tests/ and shouldn't try to). Opt-out via
# -DBUILD_TESTS=OFF for a game-only desktop build.
option(BUILD_TESTS "Build unit tests" ON)

# Demo variant: configure with -DDEMO_BUILD=ON to produce a demo binary that exposes only
# singleplayer + local couch co-op (no online host/join) and ends after floor 20. Surfaced to
# C++ as GameConst::kDemoBuild (see src/CMakeLists.txt + src/game/game_constants.h); every
# demo-only branch is a constexpr `if` the compiler dead-strips in the normal build.
option(DEMO_BUILD "Build the demo (singleplayer + couch only, ends at floor 20)" OFF)

add_subdirectory(external)
add_subdirectory(src)

# Cross-compile builds (e.g. MinGW-w64 Windows from Linux for the CI release zip) skip
# tests too. The vendored doctest header pulls in <mutex> and <thread>, and the
# packaged mingw-w64 GCC on Ubuntu CI is the win32-threads variant which does NOT
# implement std::mutex / std::thread — building the tests there fails with
# "'mutex' in namespace 'std' does not name a type". Cross-compiled test binaries
# can't run on the host without Wine anyway, so skipping is the right call:
# the release zip never contained dungeon_tests.exe.
if(NOT NINTENDO_SWITCH AND BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_subdirectory(tests)
endif()
