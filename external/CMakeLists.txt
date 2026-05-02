set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)

add_subdirectory(SDL2)

# GLAD - OpenGL loader
add_library(glad STATIC glad/src/glad.c)
target_include_directories(glad PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/glad/include)

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
add_library(enet STATIC ${ENET_SOURCES})
target_include_directories(enet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/enet/include)
if(WIN32)
    target_link_libraries(enet PRIVATE ws2_32 winmm)
endif()
