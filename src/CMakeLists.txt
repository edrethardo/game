add_executable(DungeonEngine
    main.cpp
    core/log.cpp
    core/frame_allocator.cpp
    core/allocation_tracker.cpp
    platform/window.cpp
    platform/clock.cpp
    platform/input.cpp
    engine/engine.cpp
    renderer/gl_context.cpp
    renderer/shader.cpp
    renderer/mesh.cpp
    renderer/texture.cpp
    renderer/camera.cpp
    renderer/renderer.cpp
    renderer/debug_draw.cpp
    renderer/font.cpp
    renderer/item_icons.cpp
    renderer/hud.cpp
    renderer/minimap.cpp
    world/level_grid.cpp
    world/level_mesh.cpp
    world/level_gen.cpp
    world/collision.cpp
    world/raycast.cpp
    world/combat_query.cpp
    game/player.cpp
    game/entity.cpp
    game/combat.cpp
    game/projectile.cpp
    game/enemy_ai.cpp
    game/item.cpp
    game/inventory_ui.cpp
    game/limb_system.cpp
    game/skill.cpp
    net/net.cpp
    net/snapshot.cpp
    net/server.cpp
    net/client.cpp
    renderer/material.cpp
    renderer/obj_loader.cpp
    world/level_loader.cpp
)

target_include_directories(DungeonEngine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/external
)
target_link_libraries(DungeonEngine PRIVATE SDL2-static glad enet)

if(UNIX AND NOT APPLE)
    target_link_libraries(DungeonEngine PRIVATE dl)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "")
    target_compile_definitions(DungeonEngine PRIVATE ENGINE_DEBUG=1)
endif()

# Standalone weapon model generator tool
add_subdirectory(tools)
