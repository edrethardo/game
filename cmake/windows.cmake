# CMake toolchain file for cross-compiling to Windows from Linux using MinGW-w64
#
# Usage:
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/windows.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win
#
# Requires: mingw-w64 (apt install mingw-w64)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# MinGW-w64 compilers
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

# Search paths — only look in MinGW sysroot, not host Linux paths
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Static linking so the exe doesn't need mingw DLLs at runtime
set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")

# Steamworks (external/CMakeLists.txt) derives a mingw import lib from steam_api64.dll because the
# shipped steam_api64.lib is MSVC-format. Point at the toolchain-prefixed binutils. `gendef` comes
# from the mingw-w64-tools package (apt install mingw-w64-tools).
set(DLLTOOL_EXE    x86_64-w64-mingw32-dlltool)
set(DLLTOOL_GENDEF x86_64-w64-mingw32-gendef)
