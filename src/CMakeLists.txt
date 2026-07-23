add_executable(DungeonEngine
    main.cpp
    core/log.cpp
    core/frame_allocator.cpp
    core/allocation_tracker.cpp
    platform/window.cpp
    platform/clock.cpp
    platform/input.cpp
    platform/user_paths.cpp
    platform/steam.cpp
    engine/engine.cpp
    engine/engine_init.cpp
    engine/engine_init_assets.cpp
    engine/engine_init_callbacks.cpp
    engine/engine_death.cpp
    engine/engine_town.cpp
    engine/engine_arena.cpp
    engine/engine_autoloot.cpp
    engine/engine_persist.cpp
    engine/engine_startgame.cpp
    engine/engine_launch.cpp
    engine/launch_options.cpp
    engine/engine_spawn.cpp
    engine/engine_update.cpp
    engine/engine_update_player.cpp
    engine/engine_update_skills.cpp
    engine/engine_menu.cpp
    engine/engine_inventory.cpp
    engine/lag_comp_ring.cpp
    engine/engine_combat.cpp
    engine/engine_net.cpp
    engine/engine_render.cpp
    engine/engine_render_viewmodel.cpp
    engine/engine_render_entities.cpp
    engine/engine_render_effects.cpp
    engine/engine_render_world.cpp
    engine/engine_render_character.cpp
    engine/engine_hud.cpp
    engine/engine_render_menus.cpp
    renderer/gl_context.cpp
    renderer/shader.cpp
    renderer/mesh.cpp
    renderer/texture.cpp
    renderer/screenshot.cpp
    renderer/camera.cpp
    renderer/renderer.cpp
    renderer/debug_draw.cpp
    renderer/font.cpp
    renderer/item_icons.cpp
    renderer/hud.cpp
    renderer/hud_input_glyphs.cpp
    renderer/hud_status.cpp
    renderer/hud_skill_bar.cpp
    renderer/hud_inventory.cpp
    renderer/hud_portraits.cpp
    renderer/minimap.cpp
    renderer/particles.cpp
    renderer/projectile_renderer.cpp
    world/level_grid.cpp
    world/level_mesh.cpp
    world/level_gen.cpp
    world/collision.cpp
    world/raycast.cpp
    world/combat_query.cpp
    world/pathfinder.cpp
    game/class_defs.cpp
    game/player.cpp
    game/entity.cpp
    game/combat.cpp
    game/projectile.cpp
    game/enemy_ai.cpp
    game/enemy_ai_boss.cpp
    game/enemy_ai_roles.cpp
    game/enemy_ai_friendly.cpp
    game/enemy_ai_states.cpp
    game/boss_ai.cpp
    game/boss_loader.cpp
    game/enemy_loader.cpp
    game/item_loader.cpp
    game/item_gen.cpp
    game/inventory.cpp
    game/world_item.cpp
    game/quickbar.cpp
    game/champion.cpp
    game/floor_event.cpp
    game/floor_event_loader.cpp
    game/inventory_ui.cpp
    game/limb_system.cpp
    game/skill_system.cpp
    game/teleport_dest.cpp
    game/skill_legendary.cpp
    game/skill_warrior.cpp
    game/skill_ranger.cpp
    game/skill_sorcerer.cpp
    game/skill_rogue.cpp
    game/skill_paladin.cpp
    game/skill_engineer.cpp
    game/skill_marksman.cpp
    game/skill_tinkerer.cpp
    game/squad.cpp
    net/net.cpp
    net/clock_sync.cpp
    net/snapshot.cpp
    net/server.cpp
    net/client.cpp
    net/client_time.cpp
    net/input_wire.cpp
    net/prediction_ring.cpp
    net/render_offset.cpp
    net/pending_hit_ring.cpp
    net/pending_damage_ring.cpp
    net/pending_pickup_ring.cpp
    net/pending_skill_ring.cpp
    net/upnp.cpp
    renderer/material.cpp
    renderer/obj_loader.cpp
    world/level_loader.cpp
    audio/audio.cpp
)

target_include_directories(DungeonEngine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/external
)

if(NINTENDO_SWITCH)
    # Switch build: use devkitPro portlibs for SDL2, mesa GL, and ENet
    target_include_directories(DungeonEngine PRIVATE
        $ENV{DEVKITPRO}/portlibs/switch/include
        $ENV{DEVKITPRO}/portlibs/switch/include/SDL2
    )
    # Link order matters: enet needs nx sockets, SDL2 needs EGL/GL/drm, nx must be last
    target_link_libraries(DungeonEngine PRIVATE
        SDL2_mixer vorbisidec opusfile opus FLAC modplug mpg123 ogg
        SDL2 glad enet EGL glapi drm_nouveau nx
    )
    # Generate NACP metadata (required for romfs embedding)
    add_custom_command(TARGET DungeonEngine POST_BUILD
        COMMAND $ENV{DEVKITPRO}/tools/bin/nacptool --create
            "Curse of the Dungeon Engine" "ed_rethardo" "1.6.2"
            ${CMAKE_BINARY_DIR}/DungeonEngine.nacp
        COMMENT "Creating NACP..."
    )
    # Generate NRO from ELF, bundling assets into romfs
    add_custom_command(TARGET DungeonEngine POST_BUILD
        COMMAND $ENV{DEVKITPRO}/tools/bin/elf2nro
            $<TARGET_FILE:DungeonEngine>
            ${CMAKE_BINARY_DIR}/DungeonEngine.nro
            --nacp=${CMAKE_BINARY_DIR}/DungeonEngine.nacp
            --romfsdir=${CMAKE_SOURCE_DIR}/assets
        COMMENT "Building NRO with romfs..."
    )
else()
    target_link_libraries(DungeonEngine PRIVATE SDL2-static SDL2_mixer glad enet)
    if(WIN32)
        target_link_libraries(DungeonEngine PRIVATE winmm)  # timeBeginPeriod — 1ms timer resolution
        target_link_options(DungeonEngine PRIVATE -mwindows)  # suppress console window (SUBSYSTEM:WINDOWS)
    endif()
    if(UNIX AND NOT APPLE)
        target_link_libraries(DungeonEngine PRIVATE dl)
    endif()
    # miniupnpc — auto port-forward on Host Game so friends can join without router
    # config. Desktop-only (Switch's sandboxed network stack can't do SSDP); the
    # UPNP_AVAILABLE compile define is the gate src/net/upnp.cpp uses to switch
    # between the real implementation and the no-op stub.
    target_link_libraries(DungeonEngine PRIVATE miniupnpc::miniupnpc)
    target_compile_definitions(DungeonEngine PRIVATE UPNP_AVAILABLE=1)

    # Steamworks — relay networking + matchmaking. USE_STEAM is defined in external/CMakeLists.txt
    # (auto-ON when external/steamworks/ exists). When absent, src/platform/steam.cpp compiles as
    # no-op stubs and nothing here runs, so non-Steam / CI builds are unaffected.
    if(USE_STEAM)
        target_link_libraries(DungeonEngine PRIVATE steam_api)
        target_compile_definitions(DungeonEngine PRIVATE USE_STEAM=1)
        # Find the co-located Steam runtime lib at runtime (dev + packaged) without LD_LIBRARY_PATH.
        if(APPLE)
            set_target_properties(DungeonEngine PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
        elseif(UNIX)
            set_target_properties(DungeonEngine PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
        endif()
        # Ship the Steam runtime lib + a dev steam_appid.txt next to the binary.
        add_custom_command(TARGET DungeonEngine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_PROPERTY:steam_api,IMPORTED_LOCATION> $<TARGET_FILE_DIR:DungeonEngine>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/tools/steam_appid.txt $<TARGET_FILE_DIR:DungeonEngine>/steam_appid.txt
            COMMENT "Copying Steam runtime lib + steam_appid.txt next to the binary")
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "")
    target_compile_definitions(DungeonEngine PRIVATE ENGINE_DEBUG=1)
endif()

# Demo variant gate (see top-level option(DEMO_BUILD ...)). Read only by
# src/game/game_constants.h, which exposes GameConst::kDemoBuild to the rest of the engine.
if(DEMO_BUILD)
    target_compile_definitions(DungeonEngine PRIVATE DEMO_BUILD=1)
endif()

# Auto-fetch CC0 audio assets on first build (requires internet + ffmpeg).
# On Switch, audio must be pre-fetched on the host before the Docker build
# since the Docker container may lack internet/ffmpeg. The romfs bundles
# assets/audio/ so the WAVs must exist before elf2nro runs.
set(AUDIO_STAMP "${CMAKE_SOURCE_DIR}/assets/audio/.fetched")
if(NOT NINTENDO_SWITCH)
    add_custom_command(
        OUTPUT ${AUDIO_STAMP}
        COMMAND ${CMAKE_COMMAND} -E echo "Fetching CC0 audio assets..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/fetch_audio.py
        COMMAND ${CMAKE_COMMAND} -E touch ${AUDIO_STAMP}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Downloading and pitch-processing CC0 sound effects"
    )
    add_custom_target(fetch_audio DEPENDS ${AUDIO_STAMP})
    add_dependencies(DungeonEngine fetch_audio)
else()
    # Switch: verify audio was pre-fetched on the host
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/assets/audio/sfx_weapon_sword.wav")
        message(WARNING "assets/audio/ is empty — run 'python3 tools/fetch_audio.py' on the host before the Switch build")
    endif()
endif()

# Standalone weapon model generator tool
add_subdirectory(tools)
