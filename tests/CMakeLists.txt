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
    net/test_clock_sync.cpp
    net/test_interp_delay.cpp
    net/test_input_wire.cpp
    net/test_input_drain.cpp
    net/test_prediction_ring.cpp
    net/test_render_offset.cpp
    net/test_lobby_code.cpp     # header-only codec — no production .cpp to link
    net/test_lag_comp.cpp       # header-only rewind math — no production .cpp to link
    net/test_pending_hit_ring.cpp
    net/test_pending_damage_ring.cpp
    net/test_pending_pickup_ring.cpp
    net/test_pending_skill_ring.cpp
    net/test_snapshot_baseline.cpp
    net/test_net_metrics.cpp
    net/test_snapshot_delta.cpp
    net/test_snapshot_header.cpp
    net/test_snapshot_world_item.cpp
    net/test_snapshot_entity.cpp
    engine/test_lag_comp_ring.cpp
    game/test_cooldown_gate.cpp
    game/test_armor_tier.cpp
    game/test_difficulty_scaling.cpp
    game/test_free_play.cpp
    game/test_engine_shield.cpp
    game/test_audio_settings.cpp
    game/test_skill_bar_hit.cpp
    game/test_quickbar.cpp
    game/test_champion.cpp
    game/test_floor_event.cpp
    engine/test_asset_manifest.cpp   # mesh table vs. the (gitignored, generated) .obj files
    game/test_interact.cpp           # interact button: item > shrine > exit, tap vs hold
    world/test_ray_targeting.cpp
    world/test_pathfinder.cpp
    world/test_raycast.cpp
    world/test_tile_noise.cpp
    renderer/test_hud_cooldown.cpp
    ${CMAKE_SOURCE_DIR}/src/net/clock_sync.cpp
    ${CMAKE_SOURCE_DIR}/src/net/client_time.cpp
    ${CMAKE_SOURCE_DIR}/src/net/input_wire.cpp
    ${CMAKE_SOURCE_DIR}/src/net/prediction_ring.cpp
    ${CMAKE_SOURCE_DIR}/src/net/render_offset.cpp
    ${CMAKE_SOURCE_DIR}/src/net/pending_hit_ring.cpp
    ${CMAKE_SOURCE_DIR}/src/net/pending_damage_ring.cpp
    ${CMAKE_SOURCE_DIR}/src/net/pending_pickup_ring.cpp
    ${CMAKE_SOURCE_DIR}/src/net/pending_skill_ring.cpp
    ${CMAKE_SOURCE_DIR}/src/net/snapshot_baseline.cpp
    ${CMAKE_SOURCE_DIR}/src/net/snapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/lag_comp_ring.cpp
    ${CMAKE_SOURCE_DIR}/src/core/log.cpp
    ${CMAKE_SOURCE_DIR}/src/game/item_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/game/inventory_ui.cpp   # skill-bar + panel hit-test geometry
    ${CMAKE_SOURCE_DIR}/src/game/quickbar.cpp       # quickbar slot bookkeeping
    ${CMAKE_SOURCE_DIR}/src/game/champion.cpp       # champion affix roll rules (pure)
    ${CMAKE_SOURCE_DIR}/src/game/floor_event.cpp   # floor-event weighted pick (pure)
    stubs_material.cpp
    ${CMAKE_SOURCE_DIR}/src/world/pathfinder.cpp
    ${CMAKE_SOURCE_DIR}/src/world/level_grid.cpp
    ${CMAKE_SOURCE_DIR}/src/world/raycast.cpp
    ${CMAKE_SOURCE_DIR}/src/world/combat_query.cpp  # ray/cone targeting geometry
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
    # test_asset_manifest.cpp stats the generated .obj files, which live at paths relative to the
    # repo root — the test binary runs from the build dir, so it cannot use a relative path.
    DUNGEON_REPO_ROOT="${CMAKE_SOURCE_DIR}"
)

# CTest registration. One add_test() per binary — doctest enumerates the
# individual TEST_CASEs inside. `--no-version` keeps the output clean.
add_test(NAME dungeon_tests COMMAND dungeon_tests --no-version)
