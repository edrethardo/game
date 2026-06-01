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
