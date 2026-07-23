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
    net/test_net_metrics.cpp
    net/test_snapshot_delta.cpp
    net/test_snapshot_header.cpp
    net/test_snapshot_world_item.cpp
    net/test_snapshot_entity.cpp
    engine/test_lag_comp_ring.cpp
    game/test_cooldown_gate.cpp
    game/test_armor_tier.cpp
    game/test_armor_mitigation.cpp
    game/test_difficulty_scaling.cpp
    game/test_free_play.cpp
    game/test_engine_shield.cpp
    game/test_audio_settings.cpp
    game/test_skill_bar_hit.cpp
    game/test_quickbar.cpp
    game/test_build_score.cpp        # Auto Loot & Equip scorer: family gate, row posture, hysteresis
    game/test_weapon_dps.cpp         # single-source sustained-DPS cycle (scorer + balance lab)
    game/test_champion.cpp
    game/test_floor_event.cpp
    engine/test_asset_manifest.cpp   # mesh table vs. the (gitignored, generated) .obj files
    game/test_interact.cpp           # interact button: item > shrine > exit, tap vs hold
    game/test_source_shard.cpp       # secret superboss key must survive to be picked up
    game/test_chest.cpp              # real chests: never despawn, sentinel-gated, high-half uids
    game/test_shrine_buff.cpp        # VITALITY must never leak permanent max HP
    game/test_max_health.cpp         # max HP = class base x growth + gear + buff (gear was DEAD)
    game/test_opening_strike.cpp     # arriving enemies must never open SLOWER
    game/test_pet_item.cpp           # mini-goblin consumable: unrollable + unequippable
    game/test_lead_assist.cpp        # throwing-knife intercept math — header-only, no .cpp to link
    game/test_shared_loot.cpp        # ground drops are FFA instantly; exclusivity only if re-armed
    game/test_legendary_pool.cpp     # legendary tier = named uniques only; rarityFloor guarantees
    game/test_ai_preference.cpp      # authored combat openers: pure map + enemies.json stat-fit lint
    game/test_dormant_watch.cpp      # weeping-angel view cone: pins the yaw/pitch forward convention
    game/test_teleport_dest.cpp      # teleport landing resolver: never in a wall/body/sealed pocket
    game/test_block_outcome.cpp      # perfect-block window: strict <0.2s, the legendary-shield trigger
    game/test_static_charge.cpp      # Capacitor Mail stack window (header-only)
    game/test_projectile_parry.cpp   # Mirror Aegis reflect (header-only)
    game/test_stash.cpp              # account stash: page math + transfer rules (header-only)
    game/test_arena.cpp              # PvP deathmatch rules: kill credit / win / respawn pad (header-only)
    game/test_crowd_control.cpp      # CC math: tenacity scaling, 60% cap, stun DR ladder (header-only)
    game/test_jump.cpp               # jump assist: coyote time, jump buffering, arc tuning (header-only)
    world/test_ray_targeting.cpp
    world/test_level_gen.cpp         # layout styles: seed-determinism + reachability contract
    world/test_pathfinder.cpp
    world/test_raycast.cpp
    world/test_tile_noise.cpp
    world/test_collision_push.cpp    # a push may crowd the player, never shove them into a wall
    world/test_platform.cpp          # CELL_PLATFORM slab: story selection, walk-under collision, ray planes
    world/test_vertical_hall.cpp     # VERTICAL_HALL: room-count, balconies, ramp portals, spawn/exit
    world/test_story_nav.cpp         # pure cross-story chase helpers (onUpperStory / nearestPortalGoal)
    world/test_four_story.cpp        # FOUR_STORY "Descent": offset holes, quadrant disjointness, L3→L0 descent
    world/test_lava.cpp              # Hellforge lava: walkable, burns on the surface, jumpable vein
    world/test_multislab.cpp         # multi-slab GridCell authorers/getters (FOUR_STORY foundation)
    world/test_mesh_scratch.cpp      # mesher scratch headroom: fully-slabbed same-material section fits
    balance/test_balance_lab.cpp     # balance lab: enemy/boss curves + typical-gear sanity pins
    balance/balance_lab.cpp          # the lab itself (tests-only; NEVER linked into the game)
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
    ${CMAKE_SOURCE_DIR}/src/net/snapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/lag_comp_ring.cpp
    ${CMAKE_SOURCE_DIR}/src/core/log.cpp
    ${CMAKE_SOURCE_DIR}/src/game/item_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/game/item_gen.cpp      # loot roll: pet def must be unrollable
    ${CMAKE_SOURCE_DIR}/src/game/inventory_ui.cpp   # skill-bar + panel hit-test geometry
    ${CMAKE_SOURCE_DIR}/src/game/quickbar.cpp       # quickbar slot bookkeeping
    ${CMAKE_SOURCE_DIR}/src/game/champion.cpp       # champion affix roll rules (pure)
    ${CMAKE_SOURCE_DIR}/src/game/floor_event.cpp   # floor-event weighted pick (pure)
    ${CMAKE_SOURCE_DIR}/src/game/teleport_dest.cpp # teleport landing-spot resolver
    ${CMAKE_SOURCE_DIR}/src/game/world_item.cpp    # world-item pool: spawn + lifetime expiry
    ${CMAKE_SOURCE_DIR}/src/game/inventory.cpp    # stat cache + derived max health
    ${CMAKE_SOURCE_DIR}/src/world/collision.cpp   # (world_item.cpp -> ensureNotInWall)
    stubs_material.cpp
    ${CMAKE_SOURCE_DIR}/src/world/pathfinder.cpp
    ${CMAKE_SOURCE_DIR}/src/world/level_gen.cpp   # structural floor generators
    ${CMAKE_SOURCE_DIR}/src/world/level_grid.cpp
    ${CMAKE_SOURCE_DIR}/src/world/raycast.cpp
    ${CMAKE_SOURCE_DIR}/src/world/combat_query.cpp  # ray/cone targeting geometry
    ${CMAKE_SOURCE_DIR}/src/game/enemy_loader.cpp   # balance lab reads the real roster
    ${CMAKE_SOURCE_DIR}/src/game/boss_loader.cpp    # balance lab reads the real bosses
    ${CMAKE_SOURCE_DIR}/src/game/class_defs.cpp     # balance lab reads the real class table
)

target_include_directories(dungeon_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_CURRENT_SOURCE_DIR}      # so tests-only headers resolve, e.g. "balance/balance_lab.h"
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
