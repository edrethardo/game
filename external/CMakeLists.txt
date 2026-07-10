# On Switch, use devkitPro's pre-built SDL2 — don't build from source
if(NOT NINTENDO_SWITCH)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(SDL2)

    # SDL_mixer — audio mixing library (WAV + OGG support)
    set(SDL2MIXER_VENDORED ON CACHE BOOL "" FORCE)
    set(SDL2MIXER_SAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_FLAC OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_MOD OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_MP3 OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_MIDI OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_OPUS OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_WAVPACK OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_OGG OFF CACHE BOOL "" FORCE)
    # Force SDL_mixer to build as static library
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(SDL_mixer)
    # Fix INTERFACE_SDL2_SHARED to match SDL2-static (FALSE)
    set_property(TARGET SDL2_mixer PROPERTY INTERFACE_SDL2_SHARED FALSE)
endif()

# GLAD - OpenGL loader
if(NINTENDO_SWITCH)
    # Use devkitPro's pre-built glad which has proper Switch GL bindings
    add_library(glad STATIC IMPORTED GLOBAL)
    set_target_properties(glad PROPERTIES
        IMPORTED_LOCATION $ENV{DEVKITPRO}/portlibs/switch/lib/libglad.a)
    # Use devkitPro's glad headers, but fall back to ours for compatibility
    target_include_directories(glad INTERFACE
        $ENV{DEVKITPRO}/portlibs/switch/include
        ${CMAKE_CURRENT_SOURCE_DIR}/glad/include)
else()
    add_library(glad STATIC glad/src/glad.c)
    target_include_directories(glad PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/glad/include)
endif()

# ENet - reliable UDP networking
set(ENET_SOURCES
    enet/callbacks.c
    enet/compress.c
    enet/host.c
    enet/list.c
    enet/packet.c
    enet/peer.c
    enet/protocol.c
    enet/unix.c
    enet/win32.c
)
if(NINTENDO_SWITCH)
    # Use devkitPro's pre-built enet (has proper Switch socket support)
    add_library(enet STATIC IMPORTED GLOBAL)
    set_target_properties(enet PROPERTIES
        IMPORTED_LOCATION $ENV{DEVKITPRO}/portlibs/switch/lib/libenet.a)
    target_include_directories(enet INTERFACE $ENV{DEVKITPRO}/portlibs/switch/include)
else()
    add_library(enet STATIC ${ENET_SOURCES})
    target_include_directories(enet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/enet/include)
    if(WIN32)
        target_link_libraries(enet PRIVATE ws2_32 winmm)
    endif()
endif()

# miniupnpc - UPnP IGD client used by Net::hostServer to auto-open the game's
# UDP port on the user's router so friends can join without manual port-forwarding.
# Vendored as a git submodule at external/miniupnp (pinned tag miniupnpc_2_2_8);
# CI fetches it via `submodules: recursive` in .github/workflows/build.yml.
# Skipped on Switch — sandboxed nn::socket can't do the SSDP multicast that UPnP
# discovery requires, and the engine's UPnP call site is itself stubbed out on
# Switch via the absence of the UPNP_AVAILABLE compile definition (see src/CMakeLists.txt).
if(NOT NINTENDO_SWITCH)
    # Force static-only; skip upstream's test/sample executables and install rules.
    set(UPNPC_BUILD_STATIC ON  CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    set(UPNPC_NO_INSTALL   ON  CACHE BOOL "" FORCE)
    add_subdirectory(miniupnp/miniupnpc)
    # Upstream provides `miniupnpc::miniupnpc` as an alias when only the static
    # target is built — link that instead of the bare libminiupnpc-static name
    # so we're tracking the namespaced API rather than upstream's internal target.
endif()

# --- Steamworks SDK (optional, proprietary, user-downloaded → external/steamworks/, gitignored) ---
# Provides Steam relay networking (ISteamNetworkingMessages) + matchmaking (ISteamMatchmaking).
# Steam is OPT-IN so ONE source tree builds both stores from a single flag:
#   Steam release:   cmake -B build-steam -DUSE_STEAM=ON   (requires external/steamworks/)
#   itch.io / other: cmake -B build                        (DEFAULT — no Steamworks dependency)
# When OFF, src/platform/steam.cpp compiles as no-op stubs so the game is byte-for-byte Steam-free.
# Never available on Switch.
option(USE_STEAM "Build with Steamworks (relay networking + matchmaking) — Steam release only" OFF)
if(USE_STEAM AND NINTENDO_SWITCH)
    message(FATAL_ERROR "USE_STEAM is not supported on Switch.")
endif()

if(USE_STEAM)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/steamworks/public/steam/steam_api.h")
        message(FATAL_ERROR "USE_STEAM=ON but the Steamworks SDK is missing. Unzip it to "
                "external/steamworks/ so external/steamworks/public/steam/steam_api.h exists "
                "(proprietary — gitignored, not committed).")
    endif()
    set(STEAM_SDK    "${CMAKE_CURRENT_SOURCE_DIR}/steamworks")
    set(STEAM_REDIST "${STEAM_SDK}/redistributable_bin")
    add_library(steam_api SHARED IMPORTED GLOBAL)
    target_include_directories(steam_api INTERFACE "${STEAM_SDK}/public")

    if(WIN32)
        # Steam ships an MSVC steam_api64.lib that mingw's linker can't consume. Derive a
        # mingw-compatible import lib from the DLL at configure time (gendef + dlltool).
        set(_STEAM_DLL    "${STEAM_REDIST}/win64/steam_api64.dll")
        set(_STEAM_IMPLIB "${CMAKE_CURRENT_BINARY_DIR}/libsteam_api64.a")
        if(NOT EXISTS "${_STEAM_IMPLIB}")
            if(NOT DLLTOOL_GENDEF OR NOT DLLTOOL_EXE)
                message(FATAL_ERROR "USE_STEAM on Windows needs DLLTOOL_GENDEF/DLLTOOL_EXE "
                        "(cmake/windows.cmake) + mingw-w64-tools (gendef).")
            endif()
            message(STATUS "Steam: generating mingw import lib from steam_api64.dll")
            execute_process(COMMAND ${DLLTOOL_GENDEF} "${_STEAM_DLL}"
                            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" RESULT_VARIABLE _rc)
            if(_rc)
                message(FATAL_ERROR "gendef failed for steam_api64.dll (rc=${_rc})")
            endif()
            execute_process(COMMAND ${DLLTOOL_EXE} --dllname steam_api64.dll
                            --def "${CMAKE_CURRENT_BINARY_DIR}/steam_api64.def"
                            --output-lib "${_STEAM_IMPLIB}" RESULT_VARIABLE _rc2)
            if(_rc2)
                message(FATAL_ERROR "dlltool failed for steam_api64.dll (rc=${_rc2})")
            endif()
        endif()
        set_target_properties(steam_api PROPERTIES
            IMPORTED_LOCATION "${_STEAM_DLL}"
            IMPORTED_IMPLIB   "${_STEAM_IMPLIB}")
    elseif(APPLE)
        set_target_properties(steam_api PROPERTIES
            IMPORTED_LOCATION "${STEAM_REDIST}/osx/libsteam_api.dylib")
    else() # Linux
        set_target_properties(steam_api PROPERTIES
            IMPORTED_LOCATION "${STEAM_REDIST}/linux64/libsteam_api.so")
    endif()
    message(STATUS "Steam: USE_STEAM=ON (SDK at ${STEAM_SDK})")
endif()
