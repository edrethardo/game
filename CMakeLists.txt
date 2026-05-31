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
