# CMake toolchain file for Nintendo Switch (devkitPro / devkitA64 + libnx)
#
# Usage:
#   cmake -B build-switch -DCMAKE_TOOLCHAIN_FILE=cmake/switch.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-switch
#
# Requires: devkitPro with switch-dev, switch-sdl2, switch-mesa packages installed.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(NINTENDO_SWITCH TRUE)

# devkitPro paths
if(NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "DEVKITPRO environment variable not set. Source /opt/devkitpro/switchvars.sh")
endif()

set(DEVKITPRO $ENV{DEVKITPRO})
set(DEVKITA64 ${DEVKITPRO}/devkitA64)

# Cross-compiler
set(CMAKE_C_COMPILER   ${DEVKITA64}/bin/aarch64-none-elf-gcc)
set(CMAKE_CXX_COMPILER ${DEVKITA64}/bin/aarch64-none-elf-g++)
set(CMAKE_AR           ${DEVKITA64}/bin/aarch64-none-elf-ar)
set(CMAKE_RANLIB       ${DEVKITA64}/bin/aarch64-none-elf-ranlib)

# Sysroot and library paths
set(CMAKE_FIND_ROOT_PATH
    ${DEVKITPRO}/libnx
    ${DEVKITPRO}/portlibs/switch
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Compiler flags for Cortex-A57 (Switch CPU)
set(SWITCH_ARCH_FLAGS "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__")
set(CMAKE_C_FLAGS_INIT   "${SWITCH_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${SWITCH_ARCH_FLAGS}")

# Include libnx headers
include_directories(${DEVKITPRO}/libnx/include)

# Linker flags — use _INIT to avoid duplication on re-configure
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=${DEVKITPRO}/libnx/switch.specs -L${DEVKITPRO}/libnx/lib -L${DEVKITPRO}/portlibs/switch/lib")
