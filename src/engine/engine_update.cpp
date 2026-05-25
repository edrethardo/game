// Engine module — see engine.h for class definition.
// Split from engine.cpp for manageability. All methods are Engine:: members.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
#include "platform/window.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "renderer/gl_context.h"
#include "renderer/renderer.h"
#include "renderer/debug_draw.h"
#include "renderer/hud.h"
#include "renderer/minimap.h"
#include "renderer/font.h"
#include "renderer/item_icons.h"
#include "renderer/material.h"
#include "renderer/obj_loader.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "world/level_loader.h"
#include "world/collision.h"
#include "world/combat_query.h"
#include "game/player.h"
#include "game/combat.h"
#include "game/enemy_ai.h"
#include "game/squad.h"
#include "game/limb_system.h"
#include "game/projectile.h"
#include "game/item.h"
#include "game/skill.h"
#include "game/inventory_ui.h"
#include "game/game_constants.h"
#include "net/net.h"
#include "net/server.h"
#include "net/client.h"
#include "net/snapshot.h"
#include "net/packet.h"
#include "core/log.h"
#include "core/math.h"
#include "core/frame_allocator.h"
#include "core/allocation_tracker.h"
#include "core/profiler.h"
#include "audio/audio.h"

#include <glad/glad.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// Shared statics defined in engine.cpp
// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;

// ---------------------------------------------------------------------------
// Update (60 Hz fixed timestep) — dispatches based on role
// ---------------------------------------------------------------------------
void Engine::update(f32 dt) {
    // Death screen input — handle before the generic ESC check so ESC goes to menu
    if (m_gameState == GameState::GAME_OVER) {
        if (m_menu.confirmQuit) {
            // "Are you sure?" confirmation before quitting to menu
            if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_Y)) {
                m_menu.confirmQuit = false;
                m_gameState = GameState::MENU;
                AudioSystem::stopMusic();
                Input::setRelativeMouseMode(false);
            }
            if (Input::isActionPressed(GameAction::MENU_BACK) || Input::isKeyPressed(SDL_SCANCODE_N)) {
                m_menu.confirmQuit = false;
            }
        } else {
            // A / Space = revive at entrance
            if (Input::isActionPressed(GameAction::JUMP) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
                m_localPlayer.health = m_localPlayer.maxHealth;
                m_localPlayer.position = m_players[m_localPlayerIndex].spawnPosition;
                m_localPlayer.velocity = {0, 0, 0};
                m_localPlayer.invulnTimer = 1.5f;
                m_inventoryOpen = false;
                // Sync to per-player array so swapInPlayer doesn't overwrite
                m_localPlayers[m_localPlayerIndex] = m_localPlayer;
                snapCameraToPlayer(); // no camera-interp smear on revive at entrance
                // In networked mode, also update the NetPlayer so server state matches
                if (m_netRole == NetRole::SERVER) {
                    // Host: directly update authoritative NetPlayer
                    NetPlayer& np = m_players[m_localPlayerIndex];
                    np.health = np.maxHealth;
                    np.position = np.spawnPosition;
                    np.velocity = {0, 0, 0};
                    np.invulnTimer = 1.5f;
                    np.isDead = false;
                } else if (m_netRole == NetRole::CLIENT) {
                    // Client: send respawn input to server so it clears isDead
                    // Layout: header(4) + tick(4) + moveFlags(1) + weaponId(1) +
                    //         mouseDX(2) + mouseDY(2) + extFlags(1) + skillSlot(1)
                    u8 buf[sizeof(PacketHeader) + 12];
                    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
                    hdr->type = NetPacketType::CL_INPUT;
                    hdr->flags = 0;
                    hdr->seq = 0;
                    std::memset(buf + sizeof(PacketHeader), 0, 12);
                    std::memcpy(buf + sizeof(PacketHeader), &m_serverTick, 4);
                    buf[sizeof(PacketHeader) + 10] = INPUT_EX_RESPAWN; // extFlags at offset 10
                    Net::sendToServer(buf, sizeof(buf), true);
                }
                m_gameState = GameState::IN_GAME;
            }
            // Enter/X = reload last save (singleplayer only)
            if (m_netRole == NetRole::NONE &&
                (Input::isActionPressed(GameAction::PICKUP) || Input::isKeyPressed(SDL_SCANCODE_RETURN))) {
                if (loadGame(m_activeSaveSlot)) {
                    m_level.currentFloor = m_level.savedFloor;
                } else {
                    m_level.currentFloor = 1;
                }
                m_localPlayer.health = m_localPlayer.maxHealth;
                m_localPlayer.invulnTimer = 2.5f;
                m_inventoryOpen = false;
                startGame(GameStart::CONTINUE); // loadGame already restored gear/HP
                m_gameState = GameState::IN_GAME;
            }
            // ESC/B = ask to quit
            if (Input::isActionPressed(GameAction::PAUSE)) {
                m_menu.confirmQuit = true;
            }
        }
        // Keep enemies and projectiles ticking while dead so they walk home
        EnemyAI::update(m_entities, m_level.grid, m_localPlayer, m_projectiles, dt, &m_level.squads, nullptr, 0, &m_level.dungeon);
        EntitySystem::tickTimers(m_entities, dt);
        SpatialGridSystem::rebuild(m_spatialGrid, m_entities);
        ProjectileSystem::update(m_projectiles, m_level.grid, m_entities, m_localPlayer, dt, &m_spatialGrid);
        return;
    }

    // Escape closes inventory on PC — checked before confirmQuit/pause handlers
    // so neither can swallow the keypress. On Switch, B (MENU_BACK at line 1273)
    // closes inventory; minus is drop-all only.
    if (m_gameState == GameState::IN_GAME && m_inventoryOpen &&
        Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) {
        m_inventoryOpen = false;
        m_inventoryOpenArr[m_localPlayerIndex] = false; // sync to per-player array
        Input::setRelativeMouseMode(true);
        return;
    }

    // Pause/quit selection menu
    if (m_menu.confirmQuit) {
        if (Input::isActionPressed(GameAction::MENU_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
            if (m_menu.subSelection > 0) m_menu.subSelection--;
        }
        if (Input::isActionPressed(GameAction::MENU_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
            if (m_menu.subSelection < 1) m_menu.subSelection++;
        }
        if (Input::isActionPressed(GameAction::MENU_BACK)) {
            m_menu.confirmQuit = false;          // ESC/B = resume
            Input::setRelativeMouseMode(true);   // recapture the cursor for gameplay
        }
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            if (m_menu.subSelection == 0) {
                // Continue Playing
                m_menu.confirmQuit = false;
                Input::setRelativeMouseMode(true); // recapture the cursor for gameplay
            } else {
                // Save and Quit
                m_menu.confirmQuit = false;
                saveGame(m_activeSaveSlot);
                if (m_netRole != NetRole::NONE) {
                    Net::disconnect();
                    m_netRole = NetRole::NONE;
                }
                m_gameState = GameState::MENU;
                AudioSystem::stopMusic();
                Input::setRelativeMouseMode(false);
            }
        }
        return;
    }

    // Check pause from any player — in split-screen, active player may be P2 from last frame
    bool anyPause = Input::isKeyPressed(SDL_SCANCODE_ESCAPE);
    if (!anyPause) {
        for (u8 pp = 0; pp < m_splitPlayerCount; pp++) {
            Input::setActivePlayer(pp);
            if (Input::isActionPressed(GameAction::PAUSE)) { anyPause = true; break; }
        }
        Input::setActivePlayer(0); // restore default
    }
    if (anyPause) {
        // Inventory open: gamepad minus (-) skips the pause handler entirely
        // so it falls through to drop-all in updateInventoryInteraction.
        // (PC Escape already handled above, before confirmQuit.)
        if (m_gameState == GameState::IN_GAME && m_inventoryOpen) {
            // Skip pause menu — minus reaches updateInventoryInteraction below
        } else if (m_gameState == GameState::MENU) {
            if (m_menu.subState == 0) {
                m_running = false;
                return;
            }
            // Sub-menus: fall through so updateMenu() sees MENU_BACK
        } else if (m_gameState == GameState::IN_GAME) {
            m_menu.confirmQuit = true;
            m_menu.subSelection = 0;
            // Free the cursor while paused: visible and able to leave the window so
            // the player can click other apps (discreet "play at work" pause).
            Input::setRelativeMouseMode(false);
            return;
        } else if (m_gameState != GameState::GAME_OVER &&
                   m_gameState != GameState::VICTORY) {
            // Lobby/connecting states — ESC disconnects and returns to menu
            Net::disconnect();
            m_netRole = NetRole::NONE;
            m_gameState = GameState::MENU;
            Input::setRelativeMouseMode(false);
            return;
        }
    }

    switch (m_gameState) {
    case GameState::MENU:
        updateMenu(dt);
        break;
    case GameState::LOBBY_HOST:
    case GameState::LOBBY_JOIN:
    case GameState::CONNECTING:
        updateLobby(dt);
        break;
    case GameState::FLOOR_TRANSITION:
        m_transition.timer -= dt;
        if (m_transition.timer <= 0.0f) {
            startGame(GameStart::DESCEND); // keep inventory & HP into the next floor
            // In split-screen, reposition both players at the new spawn
            if (m_splitPlayerCount > 1) {
                m_localPlayers[1].maxHealth *= 1.015f;
                m_localPlayers[1].health = m_localPlayers[1].maxHealth;
                m_skillStates[1].maxEnergy *= 1.015f;
                m_skillStates[1].energy = m_skillStates[1].maxEnergy;

                m_localPlayers[0] = m_localPlayer;
                m_localPlayers[1].position = m_localPlayer.position + Vec3{1.0f, 0.0f, 0.0f};
                m_localPlayers[1].velocity = {0, 0, 0};
                m_localPlayers[1].invulnTimer = 2.5f;
                m_players[0].spawnPosition = m_localPlayer.position;
                m_players[1].spawnPosition = m_localPlayers[1].position;
                m_cameras[0] = m_camera;
            }
            m_gameState = GameState::IN_GAME;
            // startGame() can take 100ms+ (BSP gen, mesh build, spawning).
            // Reset accumulator and clock so the next frame doesn't see
            // the loading spike and try to catch up with multiple ticks.
            m_accumulator = 0.0;
            Clock::update();
        }
        break;
    case GameState::IN_GAME:
        // Unified game loop: networking pre → gameplay → networking post
        if (m_netRole == NetRole::SERVER) serverNetPre(dt);
        if (m_netRole == NetRole::CLIENT) clientNetPre(dt);

        // Tick the spawn-calm window once per frame (here, not in gameUpdate, so
        // couch co-op — which calls gameUpdate per player — doesn't double-decrement).
        if (m_spawnCalmTimer > 0.0f) m_spawnCalmTimer -= dt;

        // Split-screen: update each local player in turn
        for (u8 sp = 0; sp < m_splitPlayerCount; sp++) {
            m_activePlayerIndex = sp;
            swapInPlayer(sp);
            Input::setActivePlayer(sp);

            if (m_playerDead[sp]) {
                // Dead player: check for respawn input (A / Space), skip gameplay
                if (Input::isActionPressed(GameAction::JUMP) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
                    // If ALL players were dead, reset enemies to their rooms
                    bool allDead = true;
                    for (u32 p = 0; p < m_splitPlayerCount; p++) {
                        if (!m_playerDead[p]) { allDead = false; break; }
                    }
                    m_localPlayer.health = m_localPlayer.maxHealth;
                    m_localPlayer.position = m_players[sp].spawnPosition;
                    m_localPlayer.velocity = {0, 0, 0};
                    m_localPlayer.invulnTimer = 2.5f;
                    m_localPlayer.health = m_localPlayer.maxHealth;
                    m_localPlayer.hurtVignette = 0.0f; // no red lingering on co-op respawn
                    snapCameraToPlayer();              // no camera-interp smear on co-op respawn
                    m_playerDead[sp] = false;

                    // Network sync: update NetPlayer for server, send packet for client
                    if (m_netRole == NetRole::SERVER) {
                        NetPlayer& np = m_players[m_localPlayerIndex];
                        np.health = np.maxHealth;
                        np.position = np.spawnPosition;
                        np.velocity = {0, 0, 0};
                        np.invulnTimer = 2.5f;
                        np.isDead = false;
                    } else if (m_netRole == NetRole::CLIENT) {
                        u8 buf[sizeof(PacketHeader) + 12];
                        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
                        hdr->type = NetPacketType::CL_INPUT;
                        hdr->flags = 0;
                        hdr->seq = 0;
                        std::memset(buf + sizeof(PacketHeader), 0, 12);
                        std::memcpy(buf + sizeof(PacketHeader), &m_serverTick, 4);
                        buf[sizeof(PacketHeader) + 10] = INPUT_EX_RESPAWN;
                        Net::sendToServer(buf, sizeof(buf), true);
                    }
                }
                swapOutPlayer(sp);

                // When P1 (sp=0) is dead, shared systems (AI, projectiles, entities)
                // still need to tick — they're normally gated on activePlayerIndex==0
                if (sp == 0) {
                    // Shared systems must always tick so enemies return to spawns
                    // when both players are dead, and projectiles expire normally.
                    if (m_splitPlayerCount > 1 && !m_playerDead[1]) {
                        EnemyAI::update(m_entities, m_level.grid, m_localPlayers[1], m_projectiles, dt, &m_level.squads, nullptr, 0, &m_level.dungeon);
                        SquadSystem::update(m_level.squads, m_level.dungeon, m_entities, m_localPlayers[1].position, dt);
                        SpatialGridSystem::rebuild(m_spatialGrid, m_entities);
                        ProjectileSystem::update(m_projectiles, m_level.grid, m_entities, m_localPlayers[1], dt, &m_spatialGrid);
                    } else {
                        // P0 alive or both dead — use P0 as reference (enemies idle/return if dead)
                        EnemyAI::update(m_entities, m_level.grid, m_localPlayers[0], m_projectiles, dt, &m_level.squads, nullptr, 0, &m_level.dungeon);
                        SquadSystem::update(m_level.squads, m_level.dungeon, m_entities, m_localPlayers[0].position, dt);
                        SpatialGridSystem::rebuild(m_spatialGrid, m_entities);
                        ProjectileSystem::update(m_projectiles, m_level.grid, m_entities, m_localPlayers[0], dt, &m_spatialGrid);
                    }
                    EntitySystem::tickTimers(m_entities, dt);
                    WorldItemSystem::update(m_worldItems, dt);
                }
                continue;
            }

            gameUpdate(dt);
            swapOutPlayer(sp);
        }

        if (m_netRole == NetRole::SERVER) serverNetPost(dt);
        if (m_netRole == NetRole::CLIENT) clientNetPost(dt);
        break;
    case GameState::GAME_OVER:
        break; // handled above
    case GameState::VICTORY:
        // Final victory (Hell floor 50 cleared) — "You conquered the Dungeon Engine."
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) ||
            Input::isActionPressed(GameAction::JUMP) ||
            Input::isKeyPressed(SDL_SCANCODE_SPACE) ||
            Input::isKeyPressed(SDL_SCANCODE_RETURN)) {
            m_gameState = GameState::MENU;
            AudioSystem::stopMusic();
            Input::setRelativeMouseMode(false);
        }
        break;
    }
}


// ---------------------------------------------------------------------------
// Singleplayer update (unchanged from Phase 3)
// ---------------------------------------------------------------------------
void Engine::gameUpdate(f32 dt) {
    m_transition.floorTime += dt;
    m_transition.totalPlayTime += dt;

    // Player footstep sound — heavy steps every ~4.5m to match the weighty FOV bob
    {
        static f32 stepAccum = 0.0f;
        f32 hSpeed = sqrtf(m_localPlayer.velocity.x * m_localPlayer.velocity.x +
                           m_localPlayer.velocity.z * m_localPlayer.velocity.z);
        if (hSpeed > 0.5f) {
            stepAccum += hSpeed * dt;
            if (stepAccum > 4.5f) {
                AudioSystem::play(SfxId::FOOTSTEP_STONE, 0.6f);
                stepAccum = 0.0f;
            }
        }
    }

    // In multiplayer, sync NetPlayer → m_localPlayer so gameplay sees current state.
    // In singleplayer, m_localPlayer is the authority — no sync needed.
    if (m_netRole != NetRole::NONE) {
        syncNetPlayerToLocalPlayer();
    }

    // Tick invulnerability timer
    if (m_localPlayer.invulnTimer > 0.0f) {
        m_localPlayer.invulnTimer -= dt;
        if (m_localPlayer.invulnTimer < 0.0f) m_localPlayer.invulnTimer = 0.0f;
    }

    tickWandererTimers(dt);
    tickPlayerStatusEffects(dt);

    // Tick floating damage numbers — drift upward and expire after 1s
    for (u32 i = 0; i < MAX_DAMAGE_NUMBERS; i++) {
        if (!m_fx.damageNumbers[i].active) continue;
        m_fx.damageNumbers[i].timer -= dt;
        m_fx.damageNumbers[i].position.y += dt * 1.5f;
        if (m_fx.damageNumbers[i].timer <= 0.0f)
            m_fx.damageNumbers[i].active = false;
    }

    // Tick directional damage indicators
    for (u32 i = 0; i < Player::MAX_HIT_INDICATORS; i++) {
        if (m_localPlayer.hitIndicators[i].timer > 0.0f)
            m_localPlayer.hitIndicators[i].timer -= dt;
    }

    // Check for player death
    if (m_localPlayer.health <= 0.0f) {
        // Clear the damage vignette the instant the player dies so no red lingers
        // into the game-over / respawn screen. (The low-HP vignette term is already
        // 0 at 0 HP, and 0 again once respawn restores full HP, so this closes the
        // whole death->respawn window.)
        m_localPlayer.hurtVignette = 0.0f;
        // Reset Wanderer transient state on death so timers/marks don't persist to respawn
        if (m_playerClass == PlayerClass::WANDERER) {
            m_localPlayer.dodgeState      = {};
            m_localPlayer.deflectTimer    = 0.0f;
            m_localPlayer.markedEntityIdx = 0xFFFF;
            m_localPlayer.markTimer       = 0.0f;
            m_localPlayer.deathsDanceTimer = 0.0f;
        }
        if (m_splitPlayerCount > 1 || m_netRole != NetRole::NONE) {
            // Multiplayer or co-op: this player dies, game keeps running
            m_playerDead[m_activePlayerIndex] = true;
            // If ALL players are now dead, send enemies walking home
            bool allDead = true;
            for (u32 p = 0; p < m_splitPlayerCount; p++) {
                if (!m_playerDead[p]) { allDead = false; break; }
            }
            if (allDead) resetEnemiesToRooms();
            return;
        }
        // True singleplayer: full game over screen — send enemies home immediately
        resetEnemiesToRooms();
        m_gameState = GameState::GAME_OVER;
        AudioSystem::play(SfxId::PLAYER_DEATH);
        return;
    }

    PROFILE_SCOPE(0, "Update");

    handleDebugKeys();

    // Quickbar slot switching (mouse wheel only — keys 1-4 are for class skills)
    WeaponState& ws = m_players[0].weaponState;
    s32 wheel = Input::getMouseWheelDelta();
    {
        s32 slot = static_cast<s32>(m_quickbars[m_localPlayerIndex].activeSlot);
        if (wheel != 0) {
            slot -= wheel; // scroll up = previous slot, down = next
            if (slot < 0) slot = QUICKBAR_SLOTS - 1;
            if (slot >= static_cast<s32>(QUICKBAR_SLOTS)) slot = 0;
        }
        // Controller quickbar switching
        if (Input::isActionPressed(GameAction::QUICKBAR_PREV)) { slot--; if (slot < 0) slot = QUICKBAR_SLOTS - 1; }
        if (Input::isActionPressed(GameAction::QUICKBAR_NEXT)) { slot++; if (slot >= static_cast<s32>(QUICKBAR_SLOTS)) slot = 0; }
        m_quickbars[m_localPlayerIndex].activeSlot = static_cast<u8>(slot);
    }

    // Healing potion (Q key) — restores 60% HP + 30% energy
    if (m_potionCooldown > 0.0f) m_potionCooldown -= dt;
    if (Input::isActionPressed(GameAction::POTION) && m_potionCooldown <= 0.0f) {
        f32 healAmount = m_localPlayer.maxHealth * GameConst::POTION_HEAL_PCT;
        m_localPlayer.health += healAmount;
        if (m_localPlayer.health > m_localPlayer.maxHealth)
            m_localPlayer.health = m_localPlayer.maxHealth;
        SkillState& ss = m_skillStates[m_localPlayerIndex];
        f32 energyAmt = ss.maxEnergy * GameConst::POTION_ENERGY_PCT;
        ss.energy += energyAmt;
        if (ss.energy > ss.maxEnergy) ss.energy = ss.maxEnergy;
        // Potion cooldown benefits from 10% of item CDR
        f32 cdr = m_inventories[m_localPlayerIndex].bonusCooldownReduction * 0.1f;
        m_potionCooldown = GameConst::POTION_COOLDOWN * (1.0f - cdr);
        AudioSystem::play(SfxId::POTION_USE);
        LOG_INFO("Used potion: +%.0f HP, +%.0f EN", healAmount, energyAmt);
    }

    // Player movement/aiming — disabled while inventory is open
    if (!m_inventoryOpen) {
        // Speed modifiers: blocking slows, buffs speed up
        f32 savedSpeed = m_localPlayer.moveSpeed;
        if (m_localPlayer.blocking) m_localPlayer.moveSpeed *= 0.4f;
        if (m_localPlayer.shadowDanceTimer > 0.0f) m_localPlayer.moveSpeed *= 1.2f;
        if (m_localPlayer.shrineBuff == 2) m_localPlayer.moveSpeed *= (1.0f + m_localPlayer.shrineBuffValue);
        if (m_localPlayer.overdriveTimer > 0.0f) m_localPlayer.moveSpeed *= 1.3f;
        PlayerController::update(m_localPlayer, dt);
        if (m_localPlayer.dodgeState.rolling) m_dodgeRolledOnce = true;
        m_localPlayer.moveSpeed = savedSpeed;
        if (!m_localPlayer.noclip) {
            // Build obstacle list using frame allocator (avoids stack array)
            auto* obstacles = static_cast<CollisionObstacle*>(
                s_frameAllocator.alloc(MAX_ENTITIES * sizeof(CollisionObstacle)));
            u32 obsCount = 0;
            for (u32 a = 0; a < m_entities.activeCount; a++) {
                const Entity& e = m_entities.entities[m_entities.activeList[a]];
                if (e.flags & ENT_DEAD) continue;
                if (e.flags & ENT_FRIENDLY) continue;
                if (e.enemyType == EnemyType::PROP) continue;
                obstacles[obsCount++] = {e.position, e.halfExtents};
            }
            Collision::moveAndSlide(m_localPlayer, m_level.grid, dt, obstacles, obsCount);
        }
    }

    // Sync to NetPlayer for consistent rendering
    syncLocalPlayerToNetPlayer();

    // Target lock and weapon fire — disabled while inventory is open
    if (!m_inventoryOpen) {
        updateTargetLock(dt);
        handleWeaponFire(dt);
    }

    // Update viewmodel animation timers — faithful Doom weapon bob algorithm
    // Doom: bob = (momx² + momy²) >> 2, clamped to MAXBOB.
    // Weapon swings at FINEANGLES/70 per tic (period = 2s at 35Hz).
    // View bobs at FINEANGLES/20 per tic (period = 0.571s at 35Hz).
    {
        // Bob amplitude from speed squared (Doom: momentum² / 4, capped)
        // Freeze bob while inventory is open so the view stays still
        f32 vx = m_inventoryOpen ? 0.0f : m_localPlayer.velocity.x;
        f32 vz = m_inventoryOpen ? 0.0f : m_localPlayer.velocity.z;
        f32 speedSq = vx * vx + vz * vz;
        // Normalize: at max run speed (~6 m/s), speedSq=36. Scale so max=1.0
        f32 bob = speedSq * 0.028f; // ~1.0 at full sprint
        if (bob > 1.0f) bob = 1.0f;

        // Bob timer advances at constant rate while moving (Doom uses leveltime)
        // Doom weapon period = 2.0s → angular freq = π rad/s (3.14)
        // Doom view period = 0.571s → angular freq = 11.0 rad/s
        if (speedSq > 0.25f) {
            m_viewmodelState.bobTimer += dt;
        }

        // Sway from mouse/gyro look — weapon lags behind camera turns
        static f32 s_prevYaw[2] = {};
        static f32 s_prevPitch[2] = {};
        u8 pi = m_localPlayerIndex;
        f32 yawDelta = m_localPlayer.yaw - s_prevYaw[pi];
        f32 pitchDelta = m_localPlayer.pitch - s_prevPitch[pi];
        if (yawDelta >  3.14159f) yawDelta -= 6.28318f;
        if (yawDelta < -3.14159f) yawDelta += 6.28318f;
        s_prevYaw[pi] = m_localPlayer.yaw;
        s_prevPitch[pi] = m_localPlayer.pitch;
        f32 targetSwayYaw = -yawDelta * 0.6f;
        f32 targetSwayPitch = -pitchDelta * 0.4f;
        m_viewmodelState.swayYaw += (targetSwayYaw - m_viewmodelState.swayYaw) * 8.0f * dt;
        m_viewmodelState.swayPitch += (targetSwayPitch - m_viewmodelState.swayPitch) * 8.0f * dt;
        if (m_viewmodelState.swayYaw >  0.04f) m_viewmodelState.swayYaw =  0.04f;
        if (m_viewmodelState.swayYaw < -0.04f) m_viewmodelState.swayYaw = -0.04f;
        if (m_viewmodelState.swayPitch >  0.03f) m_viewmodelState.swayPitch =  0.03f;
        if (m_viewmodelState.swayPitch < -0.03f) m_viewmodelState.swayPitch = -0.03f;

        // Exponential recoil decay each tick
        m_viewmodelState.recoilKick *= 0.88f;
        if (m_viewmodelState.recoilKick < 0.001f) m_viewmodelState.recoilKick = 0.0f;
        // Count down melee swing animation
        if (m_viewmodelState.attackAnimT > 0.0f) m_viewmodelState.attackAnimT -= dt;
        // Count down ranged fire shake
        if (m_viewmodelState.fireShakeTimer > 0.0f) m_viewmodelState.fireShakeTimer -= dt;

        // --- View bob: figure-8 (lemniscate) pattern for heavy footfalls ---
        // Horizontal sway at base frequency, vertical at 2× (traces a lying 8).
        // Slower cadence than Doom for a more deliberate, powerful stride.
        f32 viewBobFreq = 5.5f; // rad/s — slower than Doom's 11 for heavier steps
        f32 viewBobAngle = m_viewmodelState.bobTimer * viewBobFreq;
        // Vertical: 2× frequency = two bounces per full lateral swing (figure-8)
        f32 viewBobY = bob * 0.09f * sinf(viewBobAngle * 2.0f);
        m_localPlayer.eyeHeight = 1.7f + viewBobY;
    }

    // Enemy AI — run ONCE per frame, enemies pick the nearest player to target
    if (m_activePlayerIndex == 0) {
        PROFILE_SCOPE(1, "AI");
        bool spawnCalm = m_spawnCalmTimer > 0.0f; // floor-start calm window
        if (m_splitPlayerCount > 1 && !m_playerDead[1]) {
            // Co-op: pass P2 as extra target so enemies chase the nearest player
            Player* extras[] = { &m_localPlayers[1] };
            EnemyAI::update(m_entities, m_level.grid, m_localPlayer, m_projectiles, dt, &m_level.squads, extras, 1, &m_level.dungeon, spawnCalm);
        } else {
            EnemyAI::update(m_entities, m_level.grid, m_localPlayer, m_projectiles, dt, &m_level.squads, nullptr, 0, &m_level.dungeon, spawnCalm);
        }
        // Propagate squad alerts and reassign roles for the active tick
        SquadSystem::update(m_level.squads, m_level.dungeon, m_entities, m_localPlayer.position, dt);
    }

    // Decay speech timers + log new speech to chat
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        u32 idx = m_entities.activeList[a];
        Entity& e = m_entities.entities[idx];
        if (e.speechTimer > 0.0f) {
            // Log new speech to the chat log. We detect "first frame" by
            // checking a negative-flag trick: aiCheckIdx bit 15 is set once logged,
            // cleared when speechText changes. Simpler: just compare last-logged
            // pointer. Use the animTimer trick: if speechTimer > 2.0 it's fresh spawn
            // speech, otherwise it's combat/hurt speech that may repeat.
            // Simplest: always log, but cap speechTimer to prevent re-entry.
            if (e.speechText && e.speechTimer > 1.9f) {
                const char* name = "???";
                if (e.flags & ENT_FRIENDLY) {
                    switch (e.npcClass) {
                        case NpcClass::CLERIC:  name = "Cleric";  break;
                        case NpcClass::ARCHER:  name = "Archer";  break;
                        case NpcClass::MAGE:    name = "Mage";    break;
                        case NpcClass::ROGUE:   name = "Rogue";   break;
                        case NpcClass::PALADIN: name = "Paladin"; break;
                        default:                name = "Ally";     break;
                    }
                } else if (e.enemyType == EnemyType::BOSS) {
                    name = e.nameTag ? e.nameTag : "Boss";
                }
                Vec3 chatCol = (e.flags & ENT_FRIENDLY)
                    ? Vec3{0.4f, 1.0f, 0.5f}
                    : Vec3{1.0f, 0.3f, 0.3f};
                addChatMessage(name, e.speechText, chatCol);
                e.speechTimer = 1.8f; // prevent re-logging on next tick
            }
            e.speechTimer -= dt;
            if (e.speechTimer <= 0.0f) {
                e.speechText  = nullptr;
                e.speechTimer = 0.0f;
            }
        }
    }

    // Decay chat line timers
    for (u32 i = 0; i < MAX_CHAT_LINES; i++) {
        if (m_chatLog[i].timer > 0.0f) m_chatLog[i].timer -= dt;
    }

    // Shared systems — only run once per frame (first player pass in split-screen)
    if (m_activePlayerIndex == 0) {
        { PROFILE_SCOPE(2, "Projectiles");
        SpatialGridSystem::rebuild(m_spatialGrid, m_entities);
        ProjectileSystem::update(m_projectiles, m_level.grid, m_entities, m_localPlayer, dt, &m_spatialGrid);
        // In co-op, also check enemy projectile collision with P2
        if (m_splitPlayerCount > 1 && !m_playerDead[1]) {
            AABB p2Box = {
                m_localPlayers[1].position + Vec3{-PLAYER_HALF_WIDTH, 0, -PLAYER_HALF_WIDTH},
                m_localPlayers[1].position + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
            };
            u32 pSeen = 0;
            for (u32 pi = 0; pi < MAX_PROJECTILES && pSeen < m_projectiles.activeCount; pi++) {
                Projectile& p = m_projectiles.projectiles[pi];
                if (!p.active) continue;
                pSeen++;
                if (p.fromPlayer) continue; // only enemy projectiles
                AABB projBox = {p.position - Vec3{p.radius,p.radius,p.radius},
                                p.position + Vec3{p.radius,p.radius,p.radius}};
                if (CombatQuery::aabbOverlap(projBox, p2Box)) {
                    Combat::applyDamageToPlayer(m_localPlayers[1], p.damage, &p.position);
                    p.active = false;
                }
            }
        }
        }
        EntitySystem::tickTimers(m_entities, dt);
        WorldItemSystem::update(m_worldItems, dt);
    }

    tickFXDecay(dt);
    tickSkillCooldowns(dt);

    // Update orb projectiles (spawn ice shards for Frozen Orb)
    SkillSystem::updateOrbProjectiles(m_projectiles, m_skillDefs, m_skillDefCount, dt);

    // Update pending meteors (+ holy bombardment ticking + pillar healing)
    SkillSystem::updateMeteors(m_entities, m_localPlayer, dt);

    // Tick the particle pool (motion, gravity, lifetime decay)
    ParticleSystem::update(m_particles, dt);

    tickPassiveEquipment();

    // eyePos is computed here (after view-bob has updated eyeHeight) and threaded
    // into both skill-activation helpers that need it.
    Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
    handleClassSkillActivation(dt, eyePos);
    handleEquipmentSkillActivation(dt, eyePos);

    // --- Shield blocking (Ctrl / Left Trigger) ---
    {
        bool wantsBlock = Input::isActionDown(GameAction::BLOCK) && !m_inventoryOpen;
        if (wantsBlock && !m_localPlayer.blocking) {
            m_localPlayer.blocking = true;
            m_localPlayer.blockTimer = 0.0f; // start perfect block window
            m_shieldBlockedOnce = true;
        } else if (!wantsBlock) {
            m_localPlayer.blocking = false;
        }
        if (m_localPlayer.blocking) {
            m_localPlayer.blockTimer += dt;
        }
    }

    tickArmorRingPassives(dt);

    updatePlayerPickup();

    // updateFloorDoor returns true when the player descends — skip remainder of tick
    if (updateFloorDoor()) return;

    // Toggle inventory (Tab key)
    if (Input::isActionPressed(GameAction::INVENTORY)) {
        m_inventoryOpen = !m_inventoryOpen;
        Input::setRelativeMouseMode(!m_inventoryOpen);
        AudioSystem::play(SfxId::UI_CLICK);
        if (m_inventoryOpen) {
            m_inventoryOpenedOnce = true; // dismiss "Open Inventory" tooltip
            // Show equip tutorial on first inventory open after first pickup
            if (m_firstPickupTooltipShown && !m_equipTooltipShown)
                m_equipTooltipShown = true;
        }
        // Reset drag/click state when toggling inventory
        m_dragState = {};
        m_dblClickState = {};
    }
    if (Input::isActionPressed(GameAction::MENU_BACK) && m_inventoryOpen) {
        m_inventoryOpen = false;
        Input::setRelativeMouseMode(true);
    }

    updateInventoryInteraction(dt);

    // Debug: F7 gives random item
    if (Input::isKeyPressed(SDL_SCANCODE_F7)) {
        ItemInstance item = ItemGen::rollItem(1, m_itemDefs, m_itemDefCount,
                                              m_affixDefs, m_affixDefCount);
        if (!isItemEmpty(item)) {
            if (Inventory::addToBackpack(m_inventories[m_localPlayerIndex], item)) {
                LOG_INFO("Debug: gave %s (rarity %u, damage %.1f)",
                         m_itemDefs[item.defId].name, (u32)item.rarity, item.damage);
            }
        }
    }

    tickVisualFeedback(dt);
    tickMiscTimers(dt);

    pushPlayerFromEntities();

    // Update fog-of-war
    Minimap::updateVisited(m_level.grid, m_localPlayer.position, m_entities);

    syncLocalPlayerToNetPlayer();
}

// ---------------------------------------------------------------------------
// gameUpdate sub-functions
// ---------------------------------------------------------------------------

// Auto-pickup health/energy globes (walk-over) and E-key item pickup.
// Globes are consumed immediately; regular items go to the backpack.
void Engine::updatePlayerPickup() {
    // Auto-pickup health/energy globes (no key press needed, walk-over activation)
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = m_worldItems.items[i];
        if (!wi.active) continue;
        if (!isGlobe(wi.item)) continue;

        Vec3 delta = m_localPlayer.position - wi.position;
        // Use horizontal distance only — globes float above ground
        f32 dist = sqrtf(delta.x * delta.x + delta.z * delta.z);
        if (dist < 3.0f) {
            if (isGlobe(wi.item)) {
                // Globe restores 30% max HP and 30% max energy
                f32 healAmt = m_localPlayer.maxHealth * GameConst::GLOBE_HEAL_PCT;
                m_localPlayer.health += healAmt;
                if (m_localPlayer.health > m_localPlayer.maxHealth)
                    m_localPlayer.health = m_localPlayer.maxHealth;
                SkillState& ss = m_skillStates[m_localPlayerIndex];
                f32 energyAmt = ss.maxEnergy * GameConst::GLOBE_ENERGY_PCT;
                ss.energy += energyAmt;
                if (ss.energy > ss.maxEnergy)
                    ss.energy = ss.maxEnergy;
            }
            wi.active = false;
            if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
        }
    }

    // Item pickup (E key / action) — pick up the nearest item the player is roughly facing
    if (!m_inventoryOpen && Input::isActionPressed(GameAction::PICKUP)) {
        // Find the best item: prefer aimed (high dot), fall back to nearest in range.
        // Use XZ-only alignment so items on the floor are reachable.
        Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
        Vec3 fwd = m_localPlayer.forward;
        f32 bestScore = -1.0f;
        s32 bestIdx = -1;
        for (u32 wi = 0; wi < MAX_WORLD_ITEMS; wi++) {
            WorldItem& w = m_worldItems.items[wi];
            if (!w.active) continue;
            if (isGlobe(w.item)) continue;
            if (w.item.defId >= m_itemDefCount) continue;
            Vec3 toItem = w.position - m_localPlayer.position;
            f32 hDist = sqrtf(toItem.x * toItem.x + toItem.z * toItem.z);
            if (hDist > 3.5f) continue;
            // Horizontal-only dot product (ignore Y so floor items work)
            f32 hLen = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
            f32 dot = 0.0f;
            if (hDist > 0.1f && hLen > 0.01f) {
                dot = (fwd.x * toItem.x + fwd.z * toItem.z) / (hDist * hLen);
            } else {
                dot = 1.0f; // very close = always pickable
            }
            if (dot < 0.3f) continue; // must be in front half (~70 degrees each side)
            // Score: prefer high dot, penalize distance
            f32 score = dot - hDist * 0.1f;
            // Legendary items get pickup priority
            if (w.item.rarity == Rarity::LEGENDARY) score += 0.5f;
            if (score > bestScore) {
                bestScore = score; bestIdx = static_cast<s32>(wi);
            }
        }
        ItemInstance picked = {};
        if (bestIdx >= 0) {
            picked = m_worldItems.items[bestIdx].item;
            m_worldItems.items[bestIdx].active = false;
            if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
        }
        if (!isItemEmpty(picked)) {
            if (Inventory::addToBackpack(m_inventories[m_localPlayerIndex], picked)) {
                AudioSystem::play(SfxId::ITEM_PICKUP);
                if (!m_firstPickupTooltipShown) {
                    m_firstPickupTooltipShown = true;
                }
                if (picked.defId < m_itemDefCount &&
                    m_itemDefs[picked.defId].slot == ItemSlot::WEAPON) {
                    u8 bpIdx = 0xFF;
                    for (u8 bi = 0; bi < MAX_INVENTORY_ITEMS; bi++) {
                        if (m_inventories[m_localPlayerIndex].backpack[bi].uid == picked.uid) {
                            bpIdx = bi; break;
                        }
                    }
                    if (bpIdx != 0xFF) {
                        const ItemInstance& eqWpn = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
                        if (isItemEmpty(eqWpn)) {
                            Inventory::equip(m_inventories[m_localPlayerIndex], bpIdx, m_itemDefs);
                            Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                        } else {
                            Quickbar::assignItem(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex], bpIdx);
                        }
                    }
                }
            } else {
                // Backpack full: drop item back at its position
                WorldItemSystem::spawn(m_worldItems, picked,
                    m_localPlayer.position + Vec3{0, 0.5f, 0});
                m_fullBackpackNotifyTimer = 2.0f;
            }
        }
    }
}

// Floor door interaction — descend to next floor when near and E is pressed.
// Returns true if the player descended (caller must return immediately to skip
// the rest of the tick with the now-regenerated level state).
bool Engine::updateFloorDoor() {
    if (m_level.floorDoorActive) {
        Vec3 toDoor = m_level.floorDoorPos - m_localPlayer.position;
        if (lengthSq(toDoor) < 4.0f) {
            if (Input::isActionPressed(GameAction::PICKUP)) {
                m_level.currentFloor++;
                // All players grow 1.5% stronger each floor (multiplicative)
                m_localPlayer.maxHealth *= 1.015f;
                m_localPlayer.health = m_localPlayer.maxHealth;
                m_skillStates[m_localPlayerIndex].maxEnergy *= 1.015f;
                m_skillStates[m_localPlayerIndex].energy = m_skillStates[m_localPlayerIndex].maxEnergy;
                // Scale all networked players too
                for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
                    if (!m_players[pi].active) continue;
                    m_players[pi].maxHealth *= 1.015f;
                    m_players[pi].health = m_players[pi].maxHealth;
                    m_players[pi].invulnTimer = 2.5f;
                    m_players[pi].isDead = false;
                }
                // Upgrade equipment for NPCs that survived this floor
                upgradeNpcEquipment(static_cast<u8>(m_level.currentFloor));
                // Save progress before descending so death respawn returns here
                m_level.savedFloor = m_level.currentFloor;
                m_level.savedSeed = static_cast<u32>(std::rand());
                saveGame(m_activeSaveSlot);
                LOG_INFO("Descending to floor %u", m_level.currentFloor);

                // Refresh all cooldowns on floor descend
                for (u32 p = 0; p < MAX_PLAYERS; p++) {
                    m_skillStates[p].cooldownTimer = 0.0f;
                    for (u32 s = 0; s < 4; s++) m_classSkillStates[s].cooldownTimer = 0.0f;
                }
                m_bootSkillStates[0].cooldownTimer = 0.0f;
                m_helmetSkillStates[0].cooldownTimer = 0.0f;

                // Snapshot floor stats for transition screen
                m_transition.snapshotKills = m_transition.floorKillCount;
                m_transition.snapshotTime = m_transition.floorTime;

                // Floor 50 is the final floor of each difficulty
                if (m_level.currentFloor > 50) {
                    if (m_difficulty < 2) {
                        // Advance to next difficulty — reset to floor 1, keep gear
                        m_difficulty++;
                        m_highestUnlocked = m_difficulty;
                        FILE* uf = std::fopen("difficulty_unlock.dat", "wb");
                        if (uf) { std::fwrite(&m_highestUnlocked, 1, 1, uf); std::fclose(uf); }
                        m_level.currentFloor = 1;
                        m_level.savedFloor = 1;
                        saveGame(m_activeSaveSlot);
                        // Show transition with difficulty name
                        m_transition.timer = 3.0f;
                        m_gameState = GameState::FLOOR_TRANSITION;
                        AudioSystem::play(SfxId::LEVEL_UP);
                        LOG_INFO("Advancing to %s difficulty",
                                 m_difficulty == 1 ? "Nightmare" : "Hell");
                    } else {
                        // Hell complete — final victory
                        m_gameState = GameState::VICTORY;
                        AudioSystem::stopMusic();
                        Input::setRelativeMouseMode(false);
                    }
                } else {
                    m_transition.timer = 2.0f;
                    m_gameState = GameState::FLOOR_TRANSITION;
                    AudioSystem::play(SfxId::LEVEL_UP);
                }
                return true;
            }
        }
    }
    return false;
}

void Engine::spawnDynamicLight(Vec3 pos, Vec3 color, f32 duration) {
    // Find an expired slot (or overwrite oldest)
    u32 best = 0;
    f32 bestTimer = m_dynamicLights[0].timer;
    for (u32 i = 0; i < MAX_DYNAMIC_LIGHTS; i++) {
        if (m_dynamicLights[i].timer <= 0.0f) { best = i; break; }
        if (m_dynamicLights[i].timer < bestTimer) { best = i; bestTimer = m_dynamicLights[i].timer; }
    }
    m_dynamicLights[best] = {pos, color, duration};
}

// Inventory drag-and-drop state machine — handles click, double-click, and drag
// across backpack, equipment, and quickbar panels.

void Engine::resetEnemiesToRooms() {
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = m_entities.entities[i];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        // Set a single-waypoint path back to spawn and use RETREAT to walk there
        e.pathWaypoints[0] = e.homePosition;
        e.pathLen = 1;
        e.pathIdx = 0;
        e.aiState = AIState::RETREAT;
        e.tacticalTimer = 30.0f; // long timer so they walk all the way home
        e.hasRetreated = false;
        e.velocity = {0, 0, 0};
        e.stunTimer = 0.0f;
    }
    // Clear squad alerts so enemies don't immediately re-aggro
    for (u32 s = 0; s < m_level.squads.squadCount; s++) {
        m_level.squads.squads[s].alerted = false;
        m_level.squads.squads[s].alertTimer = 0.0f;
    }
}

// Spawns a floating damage number at a world position.
// Finds the first inactive slot in the damage number pool.
void Engine::spawnDamageNumber(Vec3 pos, f32 amount, bool isHeal, bool isCrit) {
    for (u32 i = 0; i < MAX_DAMAGE_NUMBERS; i++) {
        if (!m_fx.damageNumbers[i].active) {
            m_fx.damageNumbers[i] = {pos, amount, 1.0f, true, isHeal, isCrit};
            return;
        }
    }
}

// Pushes the local player out of all active hostile entity AABBs.
// Uses the minimal-penetration axis to avoid tunneling on corners.
// Reverts push if it would land the player inside solid geometry.
void Engine::pushPlayerFromEntities() {
    AABB playerBox = {
        m_localPlayer.position + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
        m_localPlayer.position + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
    };
    // Use activeList instead of scanning all 128 entity slots
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        u32 idx = m_entities.activeList[a];
        Entity& e = m_entities.entities[idx];
        if (e.flags & ENT_DEAD) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;
        AABB entBox = entityAABB(e);
        if (CombatQuery::aabbOverlap(playerBox, entBox)) {
            Vec3 toPlayer = m_localPlayer.position - e.position;
            f32 pushX = (e.halfExtents.x + PLAYER_HALF_WIDTH) - fabsf(toPlayer.x);
            f32 pushZ = (e.halfExtents.z + PLAYER_HALF_WIDTH) - fabsf(toPlayer.z);
            if (pushX > 0.0f && pushZ > 0.0f) {
                // Safety net — entities are solid, push the player out fully
                f32 playerPush = 1.0f;
                f32 enemyPush  = 0.0f;
                if (pushX < pushZ) {
                    f32 dir = (toPlayer.x > 0) ? 1.0f : -1.0f;
                    m_localPlayer.position.x += dir * pushX * playerPush;
                    e.position.x -= dir * pushX * enemyPush;
                } else {
                    f32 dir = (toPlayer.z > 0) ? 1.0f : -1.0f;
                    m_localPlayer.position.z += dir * pushZ * playerPush;
                    e.position.z -= dir * pushZ * enemyPush;
                }
            }
        }
    }

    // Player-to-player push in split-screen — gentle separation so they don't overlap
    if (m_splitPlayerCount > 1) {
        u8 otherP = (m_activePlayerIndex == 0) ? 1 : 0;
        if (!m_playerDead[otherP]) {
            Vec3 toMe = m_localPlayer.position - m_localPlayers[otherP].position;
            f32 dist = sqrtf(toMe.x * toMe.x + toMe.z * toMe.z);
            f32 minSep = 0.7f; // minimum separation (2 × player half-width)
            if (dist > 0.01f && dist < minSep) {
                f32 push = (minSep - dist) * 0.5f; // each player moves half
                Vec3 dir = {toMe.x / dist, 0, toMe.z / dist};
                m_localPlayer.position.x += dir.x * push;
                m_localPlayer.position.z += dir.z * push;
            }
        }
    }

    // Wall push-out: check all cells the player AABB touches and resolve ALL overlaps
    f32 cs = m_level.grid.cellSize;
    f32 pw = PLAYER_HALF_WIDTH;
    // Find grid range that the player AABB covers
    f32 pMinX = m_localPlayer.position.x - pw;
    f32 pMaxX = m_localPlayer.position.x + pw;
    f32 pMinZ = m_localPlayer.position.z - pw;
    f32 pMaxZ = m_localPlayer.position.z + pw;

    s32 gxMin = static_cast<s32>(pMinX / cs);
    s32 gxMax = static_cast<s32>(pMaxX / cs);
    s32 gzMin = static_cast<s32>(pMinZ / cs);
    s32 gzMax = static_cast<s32>(pMaxZ / cs);

    for (s32 gx = gxMin; gx <= gxMax; gx++) {
        for (s32 gz = gzMin; gz <= gzMax; gz++) {
            if (gx < 0 || gz < 0 || gx >= static_cast<s32>(m_level.grid.width) ||
                gz >= static_cast<s32>(m_level.grid.depth)) continue;
            if (!LevelGridSystem::isSolid(m_level.grid, static_cast<u32>(gx), static_cast<u32>(gz))) continue;

            f32 wallMinX = static_cast<f32>(gx) * cs;
            f32 wallMaxX = wallMinX + cs;
            f32 wallMinZ = static_cast<f32>(gz) * cs;
            f32 wallMaxZ = wallMinZ + cs;

            // Recompute player AABB (position may have shifted from previous push)
            pMinX = m_localPlayer.position.x - pw;
            pMaxX = m_localPlayer.position.x + pw;
            pMinZ = m_localPlayer.position.z - pw;
            pMaxZ = m_localPlayer.position.z + pw;

            if (pMaxX > wallMinX && pMinX < wallMaxX &&
                pMaxZ > wallMinZ && pMinZ < wallMaxZ) {
                f32 penRight = pMaxX - wallMinX;
                f32 penLeft  = wallMaxX - pMinX;
                f32 penFwd   = pMaxZ - wallMinZ;
                f32 penBack  = wallMaxZ - pMinZ;

                f32 minPenX = (penRight < penLeft) ? penRight : penLeft;
                f32 minPenZ = (penFwd < penBack) ? penFwd : penBack;

                if (minPenX < minPenZ) {
                    m_localPlayer.position.x += (penRight < penLeft) ? -penRight : penLeft;
                } else {
                    m_localPlayer.position.z += (penFwd < penBack) ? -penFwd : penBack;
                }
            }
        }
    }
}

