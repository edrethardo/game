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
// Centered menu hit-test: returns item index under mouse or -1.
// baseY/spacing/topDown match the render layout in engine_hud.cpp.
// ---------------------------------------------------------------------------
static s32 menuMouseHit(u32 sw, u32 sh,
                        f32 baseY, f32 spacing, u32 itemCount,
                        f32 itemW, f32 itemH, bool topDown)
{
    s32 mx, my;
    Input::getMousePosition(mx, my);
    f32 hudX = static_cast<f32>(mx);
    f32 hudY = static_cast<f32>(sh) - static_cast<f32>(my);

    f32 cx = static_cast<f32>(sw) * 0.5f;
    f32 halfW = itemW * 0.5f;
    if (hudX < cx - halfW || hudX > cx + halfW) return -1;

    for (u32 i = 0; i < itemCount; i++) {
        f32 y = topDown ? (baseY - i * spacing) : (baseY + (itemCount - 1 - i) * spacing);
        if (hudY >= y && hudY <= y + itemH) return static_cast<s32>(i);
    }
    return -1;
}

// Compute which menu item the mouse hovers for the current subState.
// Layouts must match renderMenu() in engine_hud.cpp exactly.
static s32 menuMouseForState(u32 sw, u32 sh, f32 uiScale, u8 subState, u8 itemCount) {
    switch (subState) {
    case 0: // Main menu: 6 items, Y = sh*0.2 + (5-i)*50*uiScale
        return menuMouseHit(sw, sh, sh * 0.2f, 50.0f * uiScale, 6,
                            250.0f * uiScale, 35.0f * uiScale, false);
    case 1: // Singleplayer: 2 items, Y = sh*0.38 + (1-i)*50*uiScale
    case 10: // Host-mode chooser (LAN/Online): same 2-option layout as subState 1
        return menuMouseHit(sw, sh, sh * 0.38f, 50.0f * uiScale, 2,
                            250.0f * uiScale, 35.0f * uiScale, false);
    case 2: // Class selection: N items top-down
    case 5: // P2 class selection: same layout
        return menuMouseHit(sw, sh, sh * 0.54f, 38.0f * uiScale, itemCount,
                            400.0f * uiScale, 32.0f * uiScale, true);
    case 8: // Overwrite confirm: 2 items (Yes/No), same layout as singleplayer sub-menu
        return menuMouseHit(sw, sh, sh * 0.38f, 50.0f * uiScale, 2,
                            250.0f * uiScale, 35.0f * uiScale, false);
    case 6: { // Save slot selection: scrollable list, top-down from sh*0.82
        // Must account for scroll offset — compute it the same way as render
        static constexpr u32 VISIBLE = 14;
        u32 scrollOff = 0;
        // itemCount doubles as subSelection here for scroll calc
        if (itemCount >= VISIBLE) scrollOff = itemCount - VISIBLE + 1;
        f32 lineH = 28.0f * (static_cast<f32>(sh) / 720.0f);
        // Hit-test visible slots only
        s32 hit = menuMouseHit(sw, sh, sh * 0.82f, lineH, VISIBLE,
                               sw * 0.7f, lineH - 4.0f * (static_cast<f32>(sh) / 720.0f), true);
        if (hit >= 0) return static_cast<s32>(hit + scrollOff);
        return -1;
    }
    default:
        return -1;
    }
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
void Engine::updateMenu(f32 dt) {
    if (m_menu.msgTimer > 0.0f) m_menu.msgTimer -= dt;

    // --- Unified mouse handling for all menu screens ---
    // Release mouse capture so cursor is visible, compute hover + click once.
    Input::setRelativeMouseMode(false);
    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    bool mouseClicked = Input::isMouseButtonPressed(MOUSE_LEFT);

    // Item count depends on subState
    u8 mouseItemCount = 0;
    if (m_menu.subState == 2 || m_menu.subState == 5)
        mouseItemCount = static_cast<u8>(PlayerClass::CLASS_COUNT);
    else if (m_menu.subState == 6)
        mouseItemCount = m_menu.subSelection; // pass current selection for scroll offset calc

    s32 mouseHit = menuMouseForState(sw, sh, uiScale, m_menu.subState, mouseItemCount);

    // Apply hover: update selection when mouse moves over an item
    u8* selPtr = (m_menu.subState == 0) ? &m_menu.selection : &m_menu.subSelection;
    if (mouseHit >= 0 && static_cast<u8>(mouseHit) != *selPtr) {
        *selPtr = static_cast<u8>(mouseHit);
        AudioSystem::play(SfxId::MENU_HOVER);
    }

    // mouseConfirm is true when the user clicks a valid menu item
    bool mouseConfirm = (mouseHit >= 0 && mouseClicked);

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
            // Drop the network-role hint set by main menu (Host or Join) when backing out
            // of the chooser — otherwise the role would leak into a different next action.
            m_netRole = NetRole::NONE;
            m_clientLoadedFromSave = false;
            m_menu.subState = 0;
            return;
        }
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)
            || mouseConfirm) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            // Preserve m_netRole if it was set by the main menu (SERVER for Host, CLIENT
            // for Join — both route through subState 1's New/Continue chooser). Reset to
            // NONE only when we arrived from the singleplayer path.
            if (m_netRole != NetRole::SERVER && m_netRole != NetRole::CLIENT) m_netRole = NetRole::NONE;
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

    // Host-mode chooser (subState 10) — LAN-only vs Online (UPnP). Reached from
    // the main menu's "Host Game" pick before the shared New/Continue chooser.
    // The two options differ ONLY in whether Net::hostServer asks the router for
    // a UPnP IGD mapping; on the wire everything is identical, so a LAN-mode
    // host can still accept connections from any peer that can reach the LAN IP
    // (port-forwarded manually, Tailscale, etc.). The default highlight is LAN
    // (subSelection=0) because that's the strictly-safer / no-side-effects mode.
    if (m_menu.subState == 10) {
        if (Input::isActionPressed(GameAction::MENU_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
            if (m_menu.subSelection > 0) { m_menu.subSelection--; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isActionPressed(GameAction::MENU_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
            if (m_menu.subSelection < 1) { m_menu.subSelection++; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isActionPressed(GameAction::MENU_BACK)) {
            AudioSystem::play(SfxId::UI_BACK);
            m_netRole = NetRole::NONE;     // drop the SERVER hint set by main menu
            m_menu.subState = 0;
            return;
        }
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)
            || mouseConfirm) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            m_menu.hostOnline = (m_menu.subSelection == 1);  // 0 = LAN, 1 = Online
            m_menu.subState = 1;            // fall into the shared New/Continue chooser
            m_menu.subSelection = 0;
        }
        return;
    }

    // Save-slot selection screen (subState 6)
    // m_menu.msg is "new" or "continue" to remember which path brought us here.
    if (m_menu.subState == 6) {
        bool isContinue = (m_menu.msg && m_menu.msg[0] == 'c');

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
            m_menu.subSelection = isContinue ? 1 : 0;
            m_menu.msg = nullptr;
            return;
        }
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)
            || mouseConfirm) {
            bool slotExists = m_saveSlots[m_menu.subSelection].exists;

            // Continue mode: block empty slots
            if (isContinue && !slotExists) {
                AudioSystem::play(SfxId::UI_BACK);
                return;
            }

            // New game mode: warn before overwriting existing save
            if (!isContinue && slotExists) {
                AudioSystem::play(SfxId::UI_CONFIRM);
                m_menu.overwriteSlot = static_cast<u8>(m_menu.subSelection);
                m_menu.overwriteLane = 0; // Player 1's slot
                m_menu.subState = 8;
                m_menu.subSelection = 0; // default to "Yes"
                return;
            }

            AudioSystem::play(SfxId::UI_CONFIRM);
            u8 selectedSlot = static_cast<u8>(m_menu.subSelection + 1); // 1-based

            if (isContinue) {
                if (loadGame(selectedSlot)) {
                    m_level.currentFloor = m_level.savedFloor;
                    if (m_netRole == NetRole::CLIENT) {
                        // Continue-Join: skip class select (class came from the save) and
                        // connect directly. updateLobby sends CL_INVENTORY_SYNC right after
                        // SV_JOIN_ACCEPT to push our loaded inventory to the host; the
                        // m_clientLoadedFromSave flag also tells startGame to skip the
                        // inventory wipe + starting-kit grant that the New-Join path uses.
                        m_clientLoadedFromSave = true;
                        Net::setLocalPlayerClass(static_cast<u8>(m_playerClass));
                        m_menu.subState = 0;
                        m_menu.msg = nullptr;
                        m_splitPlayerCount = 1;
                        if (Net::connectToServer(m_menu.connectAddress)) {
                            m_gameState = GameState::CONNECTING;
                            m_connectingElapsed = 0.0f;
                            LOG_INFO("Connecting (continue slot %u) to %s as class %u...",
                                     selectedSlot, m_menu.connectAddress,
                                     static_cast<u32>(m_playerClass));
                        } else {
                            m_netRole = NetRole::NONE;
                            m_clientLoadedFromSave = false;
                        }
                        return;
                    }
                    if (m_netRole == NetRole::SERVER) {
                        if (!Net::hostServer(DEFAULT_PORT, m_menu.hostOnline)) {
                            m_netRole = NetRole::NONE; return;
                        }
                        LOG_INFO("Hosting game (continue slot %u, mode=%s)...",
                                 selectedSlot, m_menu.hostOnline ? "Online/UPnP" : "LAN");
                    }
                    // Singleplayer: route through the couch lobby (subState 4) so Player 2 can join
                    // and pick their OWN save. A legacy 2-player bundle already restored both heroes
                    // (m_splitPlayerCount==2), so it starts directly — back-compat. A 1-player save
                    // becomes Player 1 of a potential couch game; the world is this hero's floor.
                    if (m_splitPlayerCount >= 2) {
                        m_menu.subState = 0;
                        m_menu.msg = nullptr;
                        startGame(GameStart::CONTINUE);
                        positionLocalPlayersAtSpawn();
                    } else {
                        m_menu.p1Continue   = true;
                        m_menu.subState     = 4;   // couch lobby — P2 may join, or P1 starts solo
                        m_menu.subSelection = 0;
                        m_menu.msg          = nullptr;
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
                // New game — record chosen slot and proceed to class selection. This
                // covers all three flows (singleplayer, host, join) — the class-select
                // confirm handler branches on m_netRole.
                m_activeSaveSlot    = selectedSlot;
                m_playerSaveSlot[0] = selectedSlot; // P1's per-character slot
                m_menu.p1Continue   = false;        // fresh hero → NEW_GAME world
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
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)
            || mouseConfirm) {
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
            if (m_netRole == NetRole::CLIENT) {
                // Joining a remote game: advertise the chosen class so the server sets
                // up this slot correctly (no more forced Warrior), then connect. The
                // local loadout/health were already applied above from the class.
                Net::setLocalPlayerClass(static_cast<u8>(m_playerClass));
                m_menu.subState = 0;
                m_splitPlayerCount = 1;
                if (Net::connectToServer(m_menu.connectAddress)) {
                    m_gameState = GameState::CONNECTING;
                    m_connectingElapsed = 0.0f; // (M10) start the bail-out timer fresh
                    LOG_INFO("Connecting to %s as class %u...",
                             m_menu.connectAddress, static_cast<u32>(m_playerClass));
                } else {
                    m_netRole = NetRole::NONE; // connection failed — drop back to menu role
                }
            } else if (m_netRole == NetRole::SERVER) {
                // Network hosting — skip couch co-op, start server directly
                if (!Net::hostServer(DEFAULT_PORT, m_menu.hostOnline)) {
                    m_netRole = NetRole::NONE;
                    LOG_WARN("Failed to start server");
                    return;
                }
                LOG_INFO("Hosting game (mode=%s)...",
                         m_menu.hostOnline ? "Online/UPnP" : "LAN");
                m_menu.subState = 0;
                m_splitPlayerCount = 1;
                startGame(GameStart::NEW_GAME); // wipes + grants starting loadout
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

    // Couch co-op lobby (subState 4) — Player 1 is set up (New or Continue); wait for Player 2.
    // Every character now owns its own save, so P2 picks their OWN slot rather than just a class.
    if (m_menu.subState == 4) {
        // P2 presses A on their controller to join → they choose New/Continue + a slot of their own.
        if (Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_A)) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            scanSaveSlots();
            m_menu.subState = 11;        // P2 New/Continue chooser
            m_menu.subSelection = 0;
            return;
        }
        // +/Start or Enter/Space = start solo (skip P2). The world follows Player 1's mode.
        if (Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_START) ||
            Input::isKeyPressed(SDL_SCANCODE_RETURN) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            m_splitPlayerCount = 1;
            Input::setSplitScreen(false);
            m_menu.subState = 0;
            // Solo: CONTINUE keeps the loaded hero, NEW_GAME wipes+grants P1. Single lane, so the
            // default (lanesPrepared=false) legacy path is correct.
            startGame(m_menu.p1Continue ? GameStart::CONTINUE : GameStart::NEW_GAME);
            return;
        }
        // ESC/B: a continued P1 returns to its slot list; a fresh P1 returns to class select.
        if (Input::isActionPressed(GameAction::MENU_BACK)) {
            AudioSystem::play(SfxId::UI_BACK);
            if (m_menu.p1Continue) { m_menu.subState = 6; m_menu.msg = "continue"; }
            else                   { m_menu.subState = 2; }
            m_menu.subSelection = 0;
        }
        return;
    }

    // P2 class selection (subState 5) — P2 navigates with their own controller. Reached from the
    // P2 slot screen (subState 12) once a New slot is chosen, so m_playerSaveSlot[1] is already set.
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
            Input::isKeyPressed(SDL_SCANCODE_RETURN) || Input::isKeyPressed(SDL_SCANCODE_SPACE)
            || mouseConfirm) {
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

            // Fresh P2 lane: init inventory + grant the class loadout (class base stats were set
            // just above; equipFreshLane re-applies them harmlessly), then begin the couch game.
            equipFreshLane(1);
            startCouchGame();
        }
        return;
    }

    // Player 2 New/Continue chooser (subState 11) — driven by P2's controller (index 1), with a
    // keyboard fallback for desktop testing. Mirrors subState 1 but seats the result in lane 1.
    if (m_menu.subState == 11) {
        if (Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_DPAD_UP) || Input::isKeyPressed(SDL_SCANCODE_UP)) {
            if (m_menu.subSelection > 0) { m_menu.subSelection--; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || Input::isKeyPressed(SDL_SCANCODE_DOWN)) {
            if (m_menu.subSelection < 1) { m_menu.subSelection++; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_B) || Input::isActionPressed(GameAction::MENU_BACK)) {
            AudioSystem::play(SfxId::UI_BACK);
            m_menu.subState = 4;            // back to the lobby
            m_menu.subSelection = 0;
            return;
        }
        if (Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_A) || Input::isKeyPressed(SDL_SCANCODE_RETURN)) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            m_menu.p2Continue = (m_menu.subSelection == 1); // 0 = New, 1 = Continue
            scanSaveSlots();
            m_menu.subState = 12;          // P2 slot select
            m_menu.subSelection = 0;
        }
        return;
    }

    // Player 2 slot select (subState 12) — driven by P2's controller. P2 cannot pick Player 1's
    // slot (that would share one file). Continue loads the hero into lane 1 WITHOUT touching the
    // world (it stays Player 1's floor); New goes to P2 class select (overwrite-confirm if occupied).
    if (m_menu.subState == 12) {
        if (Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_DPAD_UP) || Input::isKeyPressed(SDL_SCANCODE_UP)) {
            if (m_menu.subSelection > 0) { m_menu.subSelection--; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || Input::isKeyPressed(SDL_SCANCODE_DOWN)) {
            if (m_menu.subSelection < MAX_SAVE_SLOTS - 1) { m_menu.subSelection++; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_B) || Input::isActionPressed(GameAction::MENU_BACK)) {
            AudioSystem::play(SfxId::UI_BACK);
            m_menu.subState = 11;
            m_menu.subSelection = 0;
            return;
        }
        if (Input::isButtonPressed(1, SDL_CONTROLLER_BUTTON_A) || Input::isKeyPressed(SDL_SCANCODE_RETURN)) {
            u8 sel        = static_cast<u8>(m_menu.subSelection);
            u8 slot       = static_cast<u8>(sel + 1);
            bool slotUsed = m_saveSlots[sel].exists;
            // Can't share Player 1's slot — both would write the same file.
            if (slot == m_playerSaveSlot[0]) { AudioSystem::play(SfxId::UI_BACK); return; }

            if (m_menu.p2Continue) {
                if (!slotUsed) { AudioSystem::play(SfxId::UI_BACK); return; }       // can't continue empty
                AudioSystem::play(SfxId::UI_CONFIRM);
                if (!loadCharacterIntoLane(slot, 1)) { AudioSystem::play(SfxId::UI_BACK); return; }
                startCouchGame();                                                   // world stays P1's
            } else if (slotUsed) {
                // New P2 over an existing save — confirm overwrite (subState 8, lane 1).
                AudioSystem::play(SfxId::UI_CONFIRM);
                m_menu.overwriteSlot = sel;
                m_menu.overwriteLane = 1;
                m_menu.subState = 8;
                m_menu.subSelection = 0;
            } else {
                AudioSystem::play(SfxId::UI_CONFIRM);
                m_playerSaveSlot[1] = slot;
                m_menu.subState = 5;            // P2 class select
                m_menu.subSelection = 0;
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
            if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)
                || mouseConfirm) {
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
                    Input::setStickSensitivity(1.25f);
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

    // Overwrite save confirmation (subState 8) — Yes/No menu
    if (m_menu.subState == 8) {
        if (Input::isActionPressed(GameAction::MENU_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
            if (m_menu.subSelection > 0) { m_menu.subSelection--; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isActionPressed(GameAction::MENU_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
            if (m_menu.subSelection < 1) { m_menu.subSelection++; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        // overwriteLane routes the back/No/Yes paths to the right player's slot+class screens.
        u8 slotScreen = (m_menu.overwriteLane == 1) ? 12 : 6;
        if (Input::isActionPressed(GameAction::MENU_BACK)) {
            AudioSystem::play(SfxId::UI_BACK);
            m_menu.subState = slotScreen;
            m_menu.subSelection = m_menu.overwriteSlot;
            m_menu.msg = "new";
            return;
        }
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)
            || mouseConfirm) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            if (m_menu.subSelection == 0) {
                // Yes — proceed with overwrite, routed by which local lane is being set up.
                u8 slot = static_cast<u8>(m_menu.overwriteSlot + 1);
                if (m_menu.overwriteLane == 1) {
                    m_playerSaveSlot[1] = slot;     // Player 2's chosen slot
                    m_menu.subState = 5;            // → P2 class select
                    m_menu.subSelection = 0;
                    m_menu.msg = nullptr;
                } else {
                    m_activeSaveSlot    = slot;
                    m_playerSaveSlot[0] = slot;     // Player 1's chosen slot
                    m_menu.p1Continue   = false;
                    m_level.currentFloor = 1;
                    m_menu.subState = 2;            // → P1 class select
                    m_menu.subSelection = 0;
                    m_menu.msg = nullptr;
                }
            } else {
                // No — back to slot selection
                m_menu.subState = slotScreen;
                m_menu.subSelection = m_menu.overwriteSlot;
                m_menu.msg = "new";
            }
        }
        return;
    }

    // Host-IP entry (subState 9) — joiners only. Type a dotted-quad IPv4, a bracketed
    // IPv6 literal (e.g. [::1] or [2001:db8::1]), or a hostname-like string, then Enter
    // to confirm; advances to the New/Continue chooser that the Host path also uses.
    // No mouse, no nav arrows — pure text entry.
    if (m_menu.subState == 9) {
        // Numeric + IPv6 input mapped from SDL scancodes. Both the top-row digits and
        // numpad produce the same character; period and KP_PERIOD both produce '.'.
        // R12 added the hex digits a-f, colon, and square brackets so the user can
        // type IPv6 literals. We don't track shift state — the colon, bracket, and
        // hex-letter scancodes emit their unshifted bare characters straight into
        // the buffer, which is what inet_pton expects.
        struct KeyMap { s32 scancode; char ch; };
        static const KeyMap kKeyMap[] = {
            {SDL_SCANCODE_0, '0'}, {SDL_SCANCODE_1, '1'}, {SDL_SCANCODE_2, '2'},
            {SDL_SCANCODE_3, '3'}, {SDL_SCANCODE_4, '4'}, {SDL_SCANCODE_5, '5'},
            {SDL_SCANCODE_6, '6'}, {SDL_SCANCODE_7, '7'}, {SDL_SCANCODE_8, '8'},
            {SDL_SCANCODE_9, '9'},
            {SDL_SCANCODE_KP_0, '0'}, {SDL_SCANCODE_KP_1, '1'}, {SDL_SCANCODE_KP_2, '2'},
            {SDL_SCANCODE_KP_3, '3'}, {SDL_SCANCODE_KP_4, '4'}, {SDL_SCANCODE_KP_5, '5'},
            {SDL_SCANCODE_KP_6, '6'}, {SDL_SCANCODE_KP_7, '7'}, {SDL_SCANCODE_KP_8, '8'},
            {SDL_SCANCODE_KP_9, '9'},
            {SDL_SCANCODE_PERIOD, '.'}, {SDL_SCANCODE_KP_PERIOD, '.'},
            // R12: IPv6 syntax — hex digits, colon (as `:`), and brackets.
            {SDL_SCANCODE_A, 'a'}, {SDL_SCANCODE_B, 'b'}, {SDL_SCANCODE_C, 'c'},
            {SDL_SCANCODE_D, 'd'}, {SDL_SCANCODE_E, 'e'}, {SDL_SCANCODE_F, 'f'},
            {SDL_SCANCODE_SEMICOLON,    ':'},
            {SDL_SCANCODE_LEFTBRACKET,  '['},
            {SDL_SCANCODE_RIGHTBRACKET, ']'},
        };

        for (const auto& km : kKeyMap) {
            if (!Input::isKeyPressed(km.scancode)) continue;
            // First content keystroke since entering this screen wipes the (likely-default)
            // address so the user can type fresh without nine backspaces.
            if (m_menu.connectAddressClearOnType) {
                m_menu.connectAddress[0] = '\0';
                m_menu.connectAddressClearOnType = false;
            }
            size_t len = std::strlen(m_menu.connectAddress);
            if (len < sizeof(m_menu.connectAddress) - 1) {
                m_menu.connectAddress[len]     = km.ch;
                m_menu.connectAddress[len + 1] = '\0';
                AudioSystem::play(SfxId::MENU_HOVER);
            }
        }

        if (Input::isKeyPressed(SDL_SCANCODE_BACKSPACE)) {
            // Backspace edits the live buffer; treat first edit as committing the user's
            // intent so subsequent digit presses don't wipe what's left.
            m_menu.connectAddressClearOnType = false;
            size_t len = std::strlen(m_menu.connectAddress);
            if (len > 0) {
                m_menu.connectAddress[len - 1] = '\0';
                AudioSystem::play(SfxId::MENU_HOVER);
            }
        }

        if (Input::isActionPressed(GameAction::MENU_BACK) ||
            Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) {
            AudioSystem::play(SfxId::UI_BACK);
            // Cancel the Join intent — back to main menu, clear the network role we set
            // when the user picked "Join". connectAddress retains whatever was typed so
            // re-entering Join in the same session shows the previous attempt.
            m_netRole = NetRole::NONE;
            m_clientLoadedFromSave = false;
            m_menu.subState = 0;
            m_menu.subSelection = 0;
            return;
        }

        if (Input::isActionPressed(GameAction::MENU_CONFIRM) ||
            Input::isKeyPressed(SDL_SCANCODE_RETURN) ||
            Input::isKeyPressed(SDL_SCANCODE_KP_ENTER)) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            // Empty buffer (e.g. user hit Enter without typing) falls back to localhost
            // so the screen always advances to a usable state.
            if (m_menu.connectAddress[0] == '\0') {
                std::snprintf(m_menu.connectAddress, sizeof(m_menu.connectAddress), "127.0.0.1");
            }
            m_menu.subState = 1; // New/Continue chooser (shared with Host path)
            m_menu.subSelection = 0;
        }
        return;
    }

    if (Input::isActionPressed(GameAction::MENU_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
        if (m_menu.selection > 0) m_menu.selection--;
    }
    if (Input::isActionPressed(GameAction::MENU_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
        if (m_menu.selection < 5) m_menu.selection++;
    }
    if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_SPACE)
        || mouseConfirm) {
        AudioSystem::play(SfxId::UI_CONFIRM);
        // A fresh game-setup begins here — clear any split-screen state left over from a previous
        // couch session so a single-player New/Continue stays single (fixes the "Continue spawns a
        // 2nd player" bug). The couch lobby raises the count back to 2 only when P2 actively joins.
        m_splitPlayerCount  = 1;
        Input::setSplitScreen(false);
        m_playerSaveSlot[1] = 0;
        m_menu.p1Continue   = false;
        m_menu.p2Continue   = false;
        switch (m_menu.selection) {
        case 0: // Singleplayer — show sub-menu
            scanSaveSlots(); // scan early so "Continue" is available if saves exist
            m_menu.subState = 1;
            m_menu.subSelection = 0;
            break;
        case 1: // Host — first pick LAN vs Online (subState 10), then fall into the
                // shared New/Continue → class selection flow (same as singleplayer).
            m_netRole = NetRole::SERVER;
            m_localPlayerIndex = 0;
            scanSaveSlots();
            m_menu.subState = 10;
            m_menu.subSelection = 0;     // default highlight: LAN
            break;
        case 2: // Join — prompt for host IP first, then New/Continue → save slot → class
            m_netRole = NetRole::CLIENT;
            // Lane stays 0 on a client (networking forces split count 1). The server-assigned
            // net slot lands in m_clientNetSlot at SV_JOIN_ACCEPT; net access uses activeNetSlot().
            m_localPlayerIndex = 0;
            scanSaveSlots(); // populate m_saveSlots so the Continue option is meaningful
            m_clientLoadedFromSave = false; // reset; set later in subState 6 on successful load
#ifdef __SWITCH__
            {
                // Switch has no physical keyboard — pop the system swkbd to type the IP, then
                // skip the SDL-scancode entry screen (subState 9) and go straight to the
                // New/Continue chooser. Cancel → back to the main menu (clear the netRole).
                char buf[sizeof(m_menu.connectAddress)];
                std::strncpy(buf, m_menu.connectAddress, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                if (Input::openVirtualKeyboard("Host IP (e.g. 192.168.1.10 or [::1]:7777)",
                                               buf, buf, sizeof(buf))) {
                    std::strncpy(m_menu.connectAddress, buf, sizeof(m_menu.connectAddress) - 1);
                    m_menu.connectAddress[sizeof(m_menu.connectAddress) - 1] = '\0';
                    m_menu.subState = 1; // proceed straight to New/Continue chooser
                    m_menu.subSelection = 0;
                } else {
                    m_netRole = NetRole::NONE;
                    m_menu.subState = 0;
                    m_menu.subSelection = 0;
                }
            }
#else
            m_menu.subState = 9;    // IP entry → on confirm advances to 1 (New/Continue chooser)
            m_menu.subSelection = 0;
            m_menu.connectAddressClearOnType = true;
#endif
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
    if (m_gameState == GameState::CONNECTING) {
        // (M10) Bail out instead of hanging on the pulsing-dot screen if the join fails
        // (SV_JOIN_REJECT, server full → pre-accept disconnect, or any handshake drop) or if
        // the server never accepts us within a reasonable window (10 s).
        m_connectingElapsed += dt;
        const f32 kConnectTimeout = 10.0f;
        if (Net::joinFailed() || m_connectingElapsed > kConnectTimeout) {
            LOG_WARN("Connect failed (%s); returning to menu",
                     Net::joinFailed() ? "rejected/disconnected" : "timeout");
            Net::disconnect();
            m_netRole = NetRole::NONE;
            m_clientLoadedFromSave = false; // join didn't take — don't push on a future attempt
            m_gameState = GameState::MENU;
            m_menu.subState = 0;
            m_menu.subSelection = 0;
            return;
        }
        // Wait for join accept — check if we got assigned a player index
        u8 idx = Net::getLocalPlayerIndex();
        if (idx != 0 || Net::getConnectedCount() > 0) {
            // We're connected and got a slot. The host is authoritative for our
            // inventory (it runs onPlayerJoin + syncs via snapshots), so locally we
            // start without wiping or granting a loadout — CONTINUE semantics.
            // Store the server-assigned slot as the NET slot; leave m_localPlayerIndex at 0
            // (the split-screen lane, set above) — net-array access goes through
            // activeNetSlot(). Overwriting m_localPlayerIndex here would be clobbered by
            // swapInPlayer(0) every frame anyway and would mis-index the per-lane arrays.
            m_clientNetSlot = idx;
            // Adopt the server's per-run dungeon seed, floor, and difficulty (from
            // SV_JOIN_ACCEPT) so startGame regenerates the IDENTICAL dungeon as the host
            // instead of rolling its own from local rand().
            m_level.levelSeed    = Net::getServerLevelSeed();
            m_level.currentFloor = Net::getServerLevelFloor();
            m_difficulty         = Net::getServerLevelDifficulty();
            startGame(GameStart::CONTINUE);
            // Continue-Join: push the loaded inventory to the host now that we have an
            // assigned slot. The host's onPlayerJoin granted a starting kit moments ago;
            // onInventorySync overwrites it with our saved gear. Clear the flag so we
            // don't re-push on a future reconnect within this menu session.
            if (m_clientLoadedFromSave) {
                sendInventorySync();
                m_clientLoadedFromSave = false;
            }
        }
    }
}

