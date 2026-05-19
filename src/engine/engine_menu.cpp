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
// Menu
// ---------------------------------------------------------------------------
void Engine::updateMenu(f32 dt) {
    if (m_menu.msgTimer > 0.0f) m_menu.msgTimer -= dt;

    // Sub-menu for single player: New Game / Continue
    if (m_menu.subState == 1) {
        if (Input::isActionPressed(GameAction::MENU_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
            if (m_menu.subSelection > 0) { m_menu.subSelection--; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isActionPressed(GameAction::MENU_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
            if (m_menu.subSelection < 1) { m_menu.subSelection++; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isActionPressed(GameAction::MENU_BACK)) {
            AudioSystem::play(SfxId::UI_BACK);
            m_menu.subState = 0;
            return;
        }
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            // Keep m_netRole if already set to SERVER (hosting), otherwise NONE (singleplayer)
            if (m_netRole != NetRole::SERVER) m_netRole = NetRole::NONE;
            m_localPlayerIndex = 0;
            if (m_menu.subSelection == 0) {
                // New Game — show slot selection so the player picks where to save
                scanSaveSlots();
                m_level.currentFloor = 1;
                m_menu.subState = 6; // slot selection screen
                m_menu.subSelection = 0;
                m_menu.msgTimer = 0.0f;
                // Store intent (0 = new game, 1 = continue) in the message pointer slot.
                // We use a sentinel string — checked in the slot-selection handler below.
                m_menu.msg = "new";
            } else {
                // Continue — show slot selection to pick which save to load
                scanSaveSlots();
                m_menu.subState = 6;
                m_menu.subSelection = 0;
                m_menu.msg = "continue";
            }
        }
        return;
    }

    // Save-slot selection screen (subState 6)
    // m_menu.msg is "new" or "continue" to remember which path brought us here.
    if (m_menu.subState == 6) {
        // Navigate the list of 20 slots
        if (Input::isActionPressed(GameAction::MENU_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
            if (m_menu.subSelection > 0) { m_menu.subSelection--; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isActionPressed(GameAction::MENU_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
            if (m_menu.subSelection < MAX_SAVE_SLOTS - 1) { m_menu.subSelection++; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isActionPressed(GameAction::MENU_BACK)) {
            AudioSystem::play(SfxId::UI_BACK);
            m_menu.subState = 1;
            m_menu.subSelection = (m_menu.msg && m_menu.msg[0] == 'c') ? 1 : 0;
            m_menu.msg = nullptr;
            return;
        }
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            u8 selectedSlot = static_cast<u8>(m_menu.subSelection + 1); // 1-based
            bool isContinue = (m_menu.msg && m_menu.msg[0] == 'c');

            if (isContinue) {
                // Load the selected slot; it must exist
                if (!m_saveSlots[m_menu.subSelection].exists) {
                    // Nothing saved here — ignore or redirect to new game
                    m_menu.msg = "new";
                    m_menu.subState = 2; // fall through to class selection as new game
                    m_menu.subSelection = 0;
                    m_menu.msgTimer = 0.0f;
                    m_level.currentFloor = 1;
                    return;
                }
                if (loadGame(selectedSlot)) {
                    m_level.currentFloor = m_level.savedFloor;
                    if (m_netRole == NetRole::SERVER) {
                        if (!Net::hostServer()) { m_netRole = NetRole::NONE; return; }
                        LOG_INFO("Hosting game (continue slot %u)...", selectedSlot);
                    }
                    m_menu.subState = 0;
                    m_menu.msg = nullptr;
                    startGame();
                    // Position P2 next to P1 at the new dungeon spawn
                    if (m_splitPlayerCount > 1) {
                        m_localPlayers[1].position = m_localPlayer.position + Vec3{1.0f, 0.0f, 0.0f};
                        m_localPlayers[1].velocity = {0, 0, 0};
                        m_localPlayers[1].yaw = m_localPlayer.yaw;
                        m_localPlayers[1].eyeHeight = m_localPlayer.eyeHeight;
                        m_players[1].spawnPosition = m_localPlayers[1].position;
                        m_localPlayers[0] = m_localPlayer;
                        m_cameras[0] = m_camera;
                    }
                } else {
                    // File corrupt or version mismatch — fall back to new game
                    m_level.currentFloor = 1;
                    m_menu.subState = 2;
                    m_menu.subSelection = 0;
                    m_menu.msg = "Save file incompatible — starting new game";
                    m_menu.msgTimer = 5.0f;
                }
            } else {
                // New game — record chosen slot and proceed to class selection
                m_activeSaveSlot = selectedSlot;
                m_level.currentFloor = 1;
                m_menu.subState = 2; // class selection
                m_menu.subSelection = 0;
                m_menu.msg = nullptr;
            }
        }
        return;
    }

    // Class selection screen (subState 2)
    if (m_menu.subState == 2) {
        u8 classCount = static_cast<u8>(PlayerClass::CLASS_COUNT);
        if (Input::isActionPressed(GameAction::MENU_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
            if (m_menu.subSelection > 0) { m_menu.subSelection--; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isActionPressed(GameAction::MENU_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
            if (m_menu.subSelection < classCount - 1) { m_menu.subSelection++; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isActionPressed(GameAction::MENU_BACK)) {
            AudioSystem::play(SfxId::UI_BACK);
            m_menu.subState = 1;
            m_menu.subSelection = 0;
            return;
        }
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            m_playerClass = static_cast<PlayerClass>(m_menu.subSelection);
            m_activeClassSkill = 0;

            // Apply class stats to player
            const ClassDef& cls = kClassDefs[m_menu.subSelection];
            m_localPlayer.maxHealth = cls.baseHealth;
            m_localPlayer.health = cls.baseHealth;
            m_localPlayer.moveSpeed = cls.baseMoveSpeed;
            m_skillStates[m_localPlayerIndex].maxEnergy = cls.baseEnergy;
            m_skillStates[m_localPlayerIndex].energy = cls.baseEnergy;
            // Warrior passive: 30% damage reduction
            m_localPlayer.damageReduction = (m_playerClass == PlayerClass::WARRIOR) ? 0.3f : 0.0f;

            // Init class skill cooldown states
            for (u32 s = 0; s < 4; s++) {
                m_classSkillStates[s] = SkillState{};
                m_classSkillStates[s].activeSkill = cls.skills[s];
                m_classSkillStates[s].maxEnergy = cls.baseEnergy;
                m_classSkillStates[s].energy = cls.baseEnergy;
            }

            // Store P1 state into split-screen arrays
            m_localPlayers[0] = m_localPlayer;
            m_playerClasses[0] = m_playerClass;
            std::memcpy(m_classSkillStatesPerPlayer[0], m_classSkillStates, sizeof(m_classSkillStates));

            // Go to difficulty selection (subState 7) before co-op/network start
            if (m_netRole == NetRole::SERVER) {
                // Network hosting — skip couch co-op, start server directly
                if (!Net::hostServer()) {
                    m_netRole = NetRole::NONE;
                    LOG_WARN("Failed to start server");
                    return;
                }
                LOG_INFO("Hosting game...");
                m_menu.subState = 0;
                m_splitPlayerCount = 1;
                startGame();
                // Auto-equip starting weapon for P1
                const ClassDef& cls2 = kClassDefs[static_cast<u32>(m_playerClasses[0])];
                for (u32 wi = 0; wi < m_itemDefCount; wi++) {
                    if (std::strcmp(m_itemDefs[wi].name, cls2.startingWeaponName) == 0) {
                        ItemInstance wpn; wpn.defId = static_cast<u16>(wi);
                        wpn.rarity = Rarity::COMMON; wpn.itemLevel = 1;
                        wpn.damage = m_itemDefs[wi].baseDamage; wpn.uid = m_worldItems.nextUid++;
                        m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)] = wpn;
                        Inventory::recalculateStats(m_inventories[m_localPlayerIndex]);
                        Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                        break;
                    }
                }
            } else {
                // Skip difficulty selection — difficulty is automatic per save (Diablo-style)
                m_difficulty = 0;  // new games always start Normal
                m_menu.subState = 4;
                m_menu.subSelection = 0;
            }
        }
        return;
    }

    // (Difficulty selection removed — difficulty is automatic per save, Diablo-style)

    // Couch co-op join screen — P1 selected class, waiting for P2 (subState 4)
    if (m_menu.subState == 4) {
        // P2 presses A on their controller to join
        if (Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_A)) {
            m_splitPlayerCount = 2;
            Input::setSplitScreen(true);
            m_menu.subState = 5; // P2 class selection
            m_menu.subSelection = 0;
        }
        // +/Start or Enter/Space = start solo (skip P2)
        if (Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_START) ||
            Input::isKeyPressed(SDL_SCANCODE_RETURN) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            m_splitPlayerCount = 1;
            Input::setSplitScreen(false);
            m_menu.subState = 0;
            startGame();
            // Equip P1 starting weapon
            const ClassDef& cls2 = kClassDefs[static_cast<u32>(m_playerClasses[0])];
            for (u32 wi = 0; wi < m_itemDefCount; wi++) {
                if (std::strcmp(m_itemDefs[wi].name, cls2.startingWeaponName) == 0) {
                    ItemInstance wpn; wpn.defId = static_cast<u16>(wi);
                    wpn.rarity = Rarity::COMMON; wpn.itemLevel = 1;
                    wpn.damage = m_itemDefs[wi].baseDamage; wpn.uid = m_worldItems.nextUid++;
                    m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)] = wpn;
                    Inventory::recalculateStats(m_inventories[m_localPlayerIndex]);
                    Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                    break;
                }
            }
        }
        // ESC/B goes back to P1 class selection
        if (Input::isActionPressed(GameAction::MENU_BACK)) {
            m_menu.subState = 2;
            m_menu.subSelection = 0;
        }
        return;
    }

    // P2 class selection (subState 5) — P2 navigates with their own controller
    if (m_menu.subState == 5) {
        u8 classCount = static_cast<u8>(PlayerClass::CLASS_COUNT);
        // Read from both controllers so either player can navigate for P2
        if (Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_DPAD_UP) ||
            Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_DPAD_UP) ||
            Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_UP)) {
            if (m_menu.subSelection > 0) { m_menu.subSelection--; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
            Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
            Input::isKeyPressed(SDL_SCANCODE_S) || Input::isKeyPressed(SDL_SCANCODE_DOWN)) {
            if (m_menu.subSelection < classCount - 1) { m_menu.subSelection++; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_A) ||
            Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_A) ||
            Input::isKeyPressed(SDL_SCANCODE_RETURN) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            // P2 selected their class
            m_playerClasses[1] = static_cast<PlayerClass>(m_menu.subSelection);
            const ClassDef& cls2 = kClassDefs[m_menu.subSelection];
            m_localPlayers[1].maxHealth = cls2.baseHealth;
            m_localPlayers[1].health = cls2.baseHealth;
            m_localPlayers[1].moveSpeed = cls2.baseMoveSpeed;
            m_localPlayers[1].damageReduction = (m_playerClasses[1] == PlayerClass::WARRIOR) ? 0.3f : 0.0f;
            m_skillStates[1].maxEnergy = cls2.baseEnergy;
            m_skillStates[1].energy = cls2.baseEnergy;
            for (u32 s = 0; s < 4; s++) {
                m_classSkillStatesPerPlayer[1][s] = SkillState{};
                m_classSkillStatesPerPlayer[1][s].activeSkill = cls2.skills[s];
                m_classSkillStatesPerPlayer[1][s].maxEnergy = cls2.baseEnergy;
                m_classSkillStatesPerPlayer[1][s].energy = cls2.baseEnergy;
            }

            // Both players ready — start the game
            m_menu.subState = 0;
            startGame();

            // Set P2 spawn at same location as P1 (slightly offset)
            m_localPlayers[1].position = m_localPlayer.position + Vec3{1.0f, 0.0f, 0.0f};
            m_localPlayers[1].yaw = m_localPlayer.yaw;
            m_localPlayers[1].eyeHeight = m_localPlayer.eyeHeight;
            m_players[1].spawnPosition = m_localPlayers[1].position; // for respawn
            // Copy P1 state into arrays too
            m_localPlayers[0] = m_localPlayer;
            m_cameras[0] = m_camera;

            // Equip starting weapons for both players
            for (u8 pi = 0; pi < 2; pi++) {
                const ClassDef& pc = kClassDefs[static_cast<u32>(m_playerClasses[pi])];
                for (u32 wi = 0; wi < m_itemDefCount; wi++) {
                    if (std::strcmp(m_itemDefs[wi].name, pc.startingWeaponName) == 0) {
                        ItemInstance wpn; wpn.defId = static_cast<u16>(wi);
                        wpn.rarity = Rarity::COMMON; wpn.itemLevel = 1;
                        wpn.damage = m_itemDefs[wi].baseDamage; wpn.uid = m_worldItems.nextUid++;
                        m_inventories[pi].equipped[static_cast<u32>(ItemSlot::WEAPON)] = wpn;
                        Inventory::recalculateStats(m_inventories[pi]);
                        Quickbar::syncWeaponSlot(m_quickbars[pi], m_inventories[pi]);
                        break;
                    }
                }
            }
        }
        return;
    }

    // Options / controls rebinding screen (subState 3)
    if (m_menu.subState == 3) {
        // Number of rebindable actions (skip menu navigation actions)
        static constexpr u32 REBIND_COUNT = static_cast<u32>(GameAction::INVENTORY) + 1;
        // Extra options after rebind actions: stick sens, gyro sens, stick invertY, gyro invertY, reset
        static constexpr u32 OPT_STICK_SENS   = REBIND_COUNT;
        static constexpr u32 OPT_GYRO_SENS    = REBIND_COUNT + 1;
        static constexpr u32 OPT_STICK_INVERT = REBIND_COUNT + 2;
        static constexpr u32 OPT_GYRO_INVERT  = REBIND_COUNT + 3;
        static constexpr u32 OPT_SPLIT_MODE   = REBIND_COUNT + 4;
        static constexpr u32 OPT_RESET        = REBIND_COUNT + 5;
        static constexpr u32 TOTAL_OPTIONS     = REBIND_COUNT + 6;

        if (m_menu.bindCapture) {
            // Waiting for player to press a key or button to rebind
            if (m_menu.bindKeyboard) {
                // Scan all keys for a press
                for (s32 sc = 0; sc < 512; sc++) {
                    if (Input::isKeyPressed(sc)) {
                        GameAction act = static_cast<GameAction>(m_menu.subSelection);
                        Input::setKeyBinding(act, sc);
                        m_menu.bindCapture = false;
                        break;
                    }
                }
            } else {
                // Scan gamepad buttons for a press
                for (s32 b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
                    if (Input::isButtonPressed(0, b)) {
                        GameAction act = static_cast<GameAction>(m_menu.subSelection);
                        // If L is held, set as modified binding
                        if (Input::isModifierHeld() && b != SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                            Input::setButtonBinding(act, b, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
                        } else {
                            Input::setButtonBinding(act, b);
                        }
                        m_menu.bindCapture = false;
                        break;
                    }
                }
            }
            // ESC cancels capture
            if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) {
                m_menu.bindCapture = false;
            }
        } else {
            // Normal navigation
            if (Input::isActionPressed(GameAction::MENU_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
                if (m_menu.subSelection > 0) { m_menu.subSelection--; AudioSystem::play(SfxId::MENU_HOVER); }
            }
            if (Input::isActionPressed(GameAction::MENU_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
                if (m_menu.subSelection < TOTAL_OPTIONS - 1) { m_menu.subSelection++; AudioSystem::play(SfxId::MENU_HOVER); }
            }
            if (Input::isActionPressed(GameAction::MENU_BACK)) {
                // Save and return to main menu
#ifndef __SWITCH__
                Input::saveBindings("assets/config/controls.json");
#endif
                m_menu.subState = 0;
                m_menu.subSelection = 0;
                return;
            }
            if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
                if (m_menu.subSelection < REBIND_COUNT) {
                    m_menu.bindCapture = true;
                    m_menu.bindKeyboard = true;
                } else if (m_menu.subSelection == OPT_STICK_INVERT) {
                    Input::setStickInvertY(!Input::getStickInvertY());
                } else if (m_menu.subSelection == OPT_GYRO_INVERT) {
                    Input::setGyroInvertY(!Input::getGyroInvertY());
                } else if (m_menu.subSelection == OPT_SPLIT_MODE) {
                    m_splitMode = m_splitMode == 0 ? 1 : 0;
                } else if (m_menu.subSelection == OPT_RESET) {
                    Input::resetBindingsToDefaults();
                    Input::setStickSensitivity(1.5f);
                    Input::setGyroSensitivity(5.0f);
                    Input::setStickInvertY(false);
                    Input::setGyroInvertY(true);
                }
            }
            // Left/Right adjusts sensitivity sliders or toggles column
            if (Input::isKeyPressed(SDL_SCANCODE_LEFT) || Input::isKeyPressed(SDL_SCANCODE_A) ||
                Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
                if (m_menu.subSelection == OPT_STICK_SENS) {
                    Input::setStickSensitivity(Input::getStickSensitivity() - 0.25f);
                    if (Input::getStickSensitivity() < 0.25f) Input::setStickSensitivity(0.25f);
                } else if (m_menu.subSelection == OPT_GYRO_SENS) {
                    Input::setGyroSensitivity(Input::getGyroSensitivity() - 1.0f);
                    if (Input::getGyroSensitivity() < 1.0f) Input::setGyroSensitivity(1.0f);
                } else {
                    m_menu.bindKeyboard = true;
                }
            }
            if (Input::isKeyPressed(SDL_SCANCODE_RIGHT) || Input::isKeyPressed(SDL_SCANCODE_D) ||
                Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
                if (m_menu.subSelection == OPT_STICK_SENS) {
                    Input::setStickSensitivity(Input::getStickSensitivity() + 0.25f);
                    if (Input::getStickSensitivity() > 5.0f) Input::setStickSensitivity(5.0f);
                } else if (m_menu.subSelection == OPT_GYRO_SENS) {
                    Input::setGyroSensitivity(Input::getGyroSensitivity() + 1.0f);
                    if (Input::getGyroSensitivity() > 20.0f) Input::setGyroSensitivity(20.0f);
                } else {
                    m_menu.bindKeyboard = false;
                }
            }
        }
        return;
    }

    // Credits screen — auto-scroll + back button
    if (m_menu.subState == 7) {
        m_menu.creditsScroll += dt * 40.0f; // auto-scroll upward
        if (Input::isActionDown(GameAction::MENU_DOWN) || Input::isKeyDown(SDL_SCANCODE_S))
            m_menu.creditsScroll += dt * 120.0f; // fast scroll down
        if (Input::isActionDown(GameAction::MENU_UP) || Input::isKeyDown(SDL_SCANCODE_W))
            m_menu.creditsScroll -= dt * 120.0f; // scroll back up
        if (m_menu.creditsScroll < 0.0f) m_menu.creditsScroll = 0.0f;
        if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE) ||
            Input::isActionPressed(GameAction::MENU_BACK)) {
            m_menu.subState = 0;
            AudioSystem::play(SfxId::UI_CONFIRM);
        }
        return;
    }

    if (Input::isActionPressed(GameAction::MENU_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
        if (m_menu.selection > 0) m_menu.selection--;
    }
    if (Input::isActionPressed(GameAction::MENU_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
        if (m_menu.selection < 5) m_menu.selection++;
    }
    if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
        AudioSystem::play(SfxId::UI_CONFIRM);
        switch (m_menu.selection) {
        case 0: // Singleplayer — show sub-menu
            scanSaveSlots(); // scan early so "Continue" is available if saves exist
            m_menu.subState = 1;
            m_menu.subSelection = 0;
            break;
        case 1: // Host — same flow as singleplayer (new/continue → class selection)
            m_netRole = NetRole::SERVER;
            m_localPlayerIndex = 0;
            scanSaveSlots();
            m_menu.subState = 1;
            m_menu.subSelection = 0;
            break;
        case 2: // Join
            m_netRole = NetRole::CLIENT;
            if (Net::connectToServer(m_menu.connectAddress)) {
                m_gameState = GameState::CONNECTING;
                LOG_INFO("Connecting to %s...", m_menu.connectAddress);
            }
            break;
        case 3: // Options — controls rebinding
            m_menu.subState = 3;
            m_menu.subSelection = 0;
            m_menu.bindCapture = false;
            break;
        case 4: // Credits
            m_menu.subState = 7;
            m_menu.creditsScroll = 0.0f;
            break;
        case 5: // Exit
            m_running = false;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Lobby / Connecting
// ---------------------------------------------------------------------------
void Engine::updateLobby(f32 dt) {
    (void)dt;
    if (m_gameState == GameState::CONNECTING) {
        // Wait for join accept — check if we got assigned a player index
        u8 idx = Net::getLocalPlayerIndex();
        if (idx != 0 || Net::getConnectedCount() > 0) {
            // We're connected and got a slot
            m_localPlayerIndex = idx;
            startGame();
        }
    }
}

