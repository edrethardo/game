// Engine module — see engine.h for class definition.
// Split from engine.cpp for manageability. All methods are Engine:: members.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
#include "engine/credits.h"   // credits roll rows + scroll end (CREDITS state)
#include "platform/window.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "platform/user_paths.h"
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
#include "world/raycast.h"     // client-side chakram bounce prediction (predicted-ghost tick)
#include "world/combat_query.h"
#include "game/player.h"
#include "game/combat.h"
#include "game/enemy_ai.h"
#include "game/squad.h"
#include "game/limb_system.h"
#include "game/projectile.h"
#include "game/item.h"
#include "game/shrine.h"
#include "game/champion.h"  // cycle-driven champion affixes (tickChampions)
#include "game/floor_event.h"  // Goblin:: tunables (loot bleed)
#include "game/skill.h"
#include "game/inventory_ui.h"
#include "game/game_constants.h"
#include "net/net.h"
#include "net/server.h"
#include "net/client.h"
#include "platform/steam.h"   // Steam::currentLobbyId / closeLobby for the host's pause-menu "Close Lobby" row
#include "net/snapshot.h"
#include "net/packet.h"
#include "net/render_offset.h"
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
extern u16  s_sourceShards;   // secret superboss key — session-only set of collected shards
extern bool s_engineSlain;    // secret superboss — Engine defeated this session (victory variant)

// ---------------------------------------------------------------------------
// Death-screen mouse hit-tests. Layout MUST match the renderer in engine_render.cpp
// (the SP GAME_OVER screen): options at optY = sh*0.35 with -25 px per row
// (0=Respawn, 1=Reload, 2=Quit); the quit-confirm Yes/No sit at cy = sh*0.26. HUD coords
// are y-up, so flip the mouse Y (hudY = sh - my) — same convention as menuMouseHit
// (engine_menu.cpp). Returns the index under the cursor, or -1.
// ---------------------------------------------------------------------------
static s8 deathOptionHit(u32 sw, u32 sh) {
    s32 mx, my; Input::getMousePosition(mx, my);
    f32 hudX = static_cast<f32>(mx);
    f32 hudY = static_cast<f32>(sh) - static_cast<f32>(my);
    f32 cx   = static_cast<f32>(sw) * 0.5f;
    if (hudX < cx - 90.0f || hudX > cx + 200.0f) return -1; // key icon (cx-80) + label span
    const f32 base = static_cast<f32>(sh) * 0.35f;
    for (s8 i = 0; i < 3; i++) {
        f32 rowY = base - static_cast<f32>(i) * 25.0f; // matches render: optY -= 25 per option
        if (hudY >= rowY - 6.0f && hudY <= rowY + 18.0f) return i;
    }
    return -1;
}
// Quit-confirm overlay: 0 = Yes (≈cx-60), 1 = No (≈cx+15), -1 = none.
static s8 deathConfirmHit(u32 sw, u32 sh) {
    s32 mx, my; Input::getMousePosition(mx, my);
    f32 hudX = static_cast<f32>(mx);
    f32 hudY = static_cast<f32>(sh) - static_cast<f32>(my);
    f32 cx   = static_cast<f32>(sw) * 0.5f;
    f32 cy   = static_cast<f32>(sh) * 0.26f;
    if (hudY < cy - 6.0f || hudY > cy + 18.0f) return -1;
    if (hudX >= cx - 65.0f && hudX <= cx + 5.0f)  return 0; // Yes
    if (hudX >= cx + 10.0f && hudX <= cx + 85.0f) return 1; // No
    return -1;
}
// Networked-MP dead overlay: is the cursor over the "Press … to respawn" prompt
// (centered, y = vpH*0.4)? Networked MP viewport == full window.
static bool deathRespawnPromptHit(u32 sw, u32 sh) {
    s32 mx, my; Input::getMousePosition(mx, my);
    f32 hudX = static_cast<f32>(mx);
    f32 hudY = static_cast<f32>(sh) - static_cast<f32>(my);
    f32 cx   = static_cast<f32>(sw) * 0.5f;
    f32 y    = static_cast<f32>(sh) * 0.4f;
    return hudX >= cx - 220.0f && hudX <= cx + 220.0f && hudY >= y - 8.0f && hudY <= y + 28.0f;
}
// In-game pause menu (m_menu.confirmQuit while IN_GAME): option rows centered at cx —
// [Continue, (Close Lobby), (Menagerie), Options, Save and Quit]. Geometry comes from pauseMenuLayout
// (engine.h), the SAME source the renderer draws from, so the clickable rects cannot drift
// from the drawn boxes. Returns the option under the cursor, or -1.
static s8 pauseMenuHit(u32 sw, u32 sh, u8 optCount) {
    s32 mx, my; Input::getMousePosition(mx, my);
    f32 hudX = static_cast<f32>(mx);
    f32 hudY = static_cast<f32>(sh) - static_cast<f32>(my);
    f32 cx   = static_cast<f32>(sw) * 0.5f;
    f32 cy   = static_cast<f32>(sh) * 0.5f;
    const PauseMenuLayout L = pauseMenuLayout(sh);
    if (hudX < cx - L.rowW * 0.5f || hudX > cx + L.rowW * 0.5f) return -1;
    // Rows are laid out top-down; optCount varies (3-5: +1 when the host can close its Steam
    // lobby, +1 once the Menagerie is unlocked) so the hit-test must match the renderer's
    // dynamic option list exactly.
    for (s8 i = 0; i < static_cast<s8>(optCount); i++) {
        f32 y = cy + L.firstRowOffset - static_cast<f32>(i) * L.rowStep;
        if (hudY >= y && hudY <= y + L.rowH) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Update (60 Hz fixed timestep) — dispatches based on role
// ---------------------------------------------------------------------------
void Engine::update(f32 dt) {
    // (N6) Networking was historically mutually exclusive with split-screen. ONLINE COUCH CO-OP
    // (m_netCouch) is the deliberate exception: two local players share one connection, so the
    // split-screen lane loop runs under a net role. Everywhere else the guard still forces a single
    // local player under a net role — defensive against a 2-player CONTINUE leaking into a normal
    // Host/Join (which would cross-wire the lane-indexed arrays).
    if (m_netRole != NetRole::NONE && !m_netCouch && m_splitPlayerCount != 1) m_splitPlayerCount = 1;

    // Death screen input — handle before the generic ESC check so ESC goes to menu
    if (m_gameState == GameState::GAME_OVER) {
        // Mouse control: the cursor was freed on death entry. Hover highlights an option
        // (m_deathHover, read by the renderer) and a left-click activates it, alongside the
        // keyboard/gamepad bindings. The hit-test layout mirrors engine_render.cpp.
        const u32  dsw    = Window::getWidth();
        const u32  dsh    = Window::getHeight();
        // Last-input-device gate: the pointer hovers/clicks the death screen only when the mouse is
        // actually in use (see updateMenuMouseActive); keyboard/gamepad bindings always work, and the
        // cursor stays hidden while they drive.
        const bool mouseActive = updateMenuMouseActive();
        const bool dclick = mouseActive && Input::isMouseButtonPressed(MOUSE_LEFT);
        if (m_menu.confirmQuit) {
            const s8 ch = mouseActive ? deathConfirmHit(dsw, dsh) : static_cast<s8>(-1);
            m_deathHover = ch;
            // "Are you sure?" confirmation before quitting to menu
            if (Input::isActionPressed(GameAction::MENU_CONFIRM) || Input::isKeyPressed(SDL_SCANCODE_Y)
                || (dclick && ch == 0)) {
                m_menu.confirmQuit = false;
                m_deathHover = -1;
                m_gameState = GameState::MENU;
                AudioSystem::stopMusic();
                Input::setRelativeMouseMode(false);
            }
            if (Input::isActionPressed(GameAction::MENU_BACK) || Input::isKeyPressed(SDL_SCANCODE_N)
                || (dclick && ch == 1)) {
                m_menu.confirmQuit = false;
                m_deathHover = -1;
            }
        } else {
            const s8 hov = mouseActive ? deathOptionHit(dsw, dsh) : static_cast<s8>(-1);
            if (hov != m_deathHover) { m_deathHover = hov; if (hov >= 0) AudioSystem::play(SfxId::MENU_HOVER); }
            // A / Space / ENTER / click = revive at entrance.
            //
            // Enter respawns now too. It used to trigger "reload last save" — i.e. the most natural
            // confirm key on the screen did the DESTRUCTIVE thing, and reloading silently wipes the
            // run's collected source shards (loadGame resets them), so an instinctive Enter after a
            // death quietly ended any attempt at the secret superboss. Respawn is the safe default;
            // reloading a save is still available from the mouse and from the PICKUP action.
            if (Input::isActionPressed(GameAction::JUMP)
                || Input::isKeyPressed(SDL_SCANCODE_SPACE)
                || Input::isKeyPressed(SDL_SCANCODE_RETURN)
                || Input::isKeyPressed(SDL_SCANCODE_KP_ENTER)
                || (dclick && hov == 0)) {
                m_localPlayer.health = m_localPlayer.maxHealth;
                m_localPlayer.position = m_players[activeNetSlot()].spawnPosition; // local player's net slot
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
                    // Client: request respawn (reliable CL_RESPAWN). A client never actually
                    // reaches GAME_OVER — death is handled in the IN_GAME dead-branch — but keep
                    // this path correct rather than leaving the old dropped-input hack.
                    sendRespawnRequest(activeNetSlot());
                }
                m_deathHover = -1;
                Input::setRelativeMouseMode(true); // back to gameplay — re-capture the cursor
                m_gameState = GameState::IN_GAME;
            }
            // TAB / gamepad X / click = reload last save (singleplayer only).
            //
            // Deliberately NOT Enter, and no longer the PICKUP action either. This is the
            // DESTRUCTIVE option — loadGame silently wipes the run's collected source shards, so an
            // instinctive Enter (or E) after a death quietly ends any attempt at the secret
            // superboss. It now sits on a key nobody presses by reflex to dismiss a death screen.
            if (m_netRole == NetRole::NONE &&
                (Input::isKeyPressed(SDL_SCANCODE_TAB)
                 || Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_X)
                 || (dclick && hov == 1))) {
                // Reload restores the CHARACTER from disk but restarts the floor the player
                // DIED on — never the save header's floor. The header floor cannot be adopted
                // here: a cleared character's header is deliberately pinned past 50 (floor 51
                // is the FreePlay::saveCleared marker, kept alive by the no-downgrade guard),
                // and a Free-Play run's chosen floor/difficulty never matches the header at
                // all — adopting it teleported a finished hero onto nonexistent "floor 51".
                // For an ordinary mid-run death the two agree anyway (descent auto-saves).
                // Clamped ≤50 as a backstop (in-memory floor 51 is the victory signal; a
                // Source-run death reloads at 50, matching the quit-out path).
                const u32 diedFloor      = (m_level.currentFloor > 50) ? 50 : m_level.currentFloor;
                const u8  diedDifficulty = m_difficulty;
                if (loadGame(m_activeSaveSlot)) {
                    m_level.currentFloor = diedFloor;
                    m_difficulty         = diedDifficulty;   // loadGame adopted the header's — keep the run's
                } else {
                    m_level.currentFloor = 1;
                }
                m_localPlayer.health = m_localPlayer.maxHealth;
                m_localPlayer.invulnTimer = 2.0f;
                m_inventoryOpen = false;
                m_deathHover = -1;
                Input::setRelativeMouseMode(true); // re-capture the cursor for gameplay
                startGame(GameStart::CONTINUE); // loadGame already restored gear/HP
                m_gameState = GameState::IN_GAME;
            }
            // ESC/B / click = ask to quit
            if (Input::isActionPressed(GameAction::PAUSE) || (dclick && hov == 2)) {
                m_menu.confirmQuit = true;
                m_deathHover = -1;
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

    // Pet menagerie overlay (opened from the pause menu). Any back/confirm press returns TO
    // the pause menu, not to gameplay, so the player lands back where they left. View-only —
    // no selection state to manage.
    if (m_menagerieOpen) {
        if (Input::isActionPressed(GameAction::MENU_BACK) ||
            Input::isActionPressed(GameAction::MENU_CONFIRM) ||
            Input::isKeyPressed(SDL_SCANCODE_RETURN) ||
            Input::isKeyPressed(SDL_SCANCODE_KP_ENTER) ||
            Input::isKeyPressed(SDL_SCANCODE_SPACE)) {   // Space works wherever Enter does
            AudioSystem::play(SfxId::UI_BACK);
            m_menagerieOpen     = false;
            m_menu.confirmQuit  = true;
            m_menu.subSelection = 0;
            // Consume this edge fully: the confirmQuit handler below reads the SAME
            // MENU_BACK/MENU_CONFIRM press, and letting it fall through would close (or
            // re-confirm) the pause menu we just returned to. One skipped tick, not a pause.
            return;
        }
        // SP: the overlay freezes the game exactly like the pause it came from. MP: NEVER —
        // same R12 policy as the pause overlay below. This unconditional return used to make
        // a HOST browsing the menagerie skip serverNetPre/Post, which starved every client's
        // input drain and froze the whole session; a guest starved its own prediction/ack
        // stream. Fall through instead so the world keeps running under the page.
        if (m_netRole == NetRole::NONE) return;
    }

    // Pause/quit selection menu
    if (m_menu.confirmQuit) {
        // Mouse control: the cursor was freed when the pause opened. Hover selects an option
        // and a left-click confirms it, alongside the keyboard/gamepad bindings. Layout-matched
        // hit-test in pauseMenuHit (mirrors engine_hud.cpp).
        // Last-input-device gate (see updateMenuMouseActive): a resting pointer must not hijack the
        // pause selection while the player uses keyboard/controller; the cursor hides while they do.
        const bool mouseActive = updateMenuMouseActive();
        // The host of an open Steam lobby gets an extra middle option, "Close Lobby" (stop new joiners
        // without ending the game). currentLobbyId()==0 for SP / ENet hosts / clients / non-Steam builds,
        // so the option auto-hides everywhere else and the menu stays 2 rows. Layout must match the
        // renderer in engine_hud.cpp: [Continue, (Close Lobby), Save and Quit].
        const bool canCloseLobby = (m_netRole == NetRole::SERVER && Steam::currentLobbyId() != 0);
        // The Menagerie row exists only once the profile has summoned at least one minipet —
        // an empty museum row would spoil the collection's existence before the first jackpot.
        const bool hasMenagerie = menagerieUnlocked();
        // Row indices are derived, not hardcoded, so the renderer and this handler cannot drift when
        // a row appears or disappears. Order: [Continue, (Close Lobby), (Menagerie), Options, Save and Quit].
        u8 pauseIdx = 0;
        const u8 iContinue   = pauseIdx++;
        const u8 iCloseLobby = canCloseLobby ? pauseIdx++ : 0xFF;
        const u8 iMenagerie  = hasMenagerie ? pauseIdx++ : 0xFF;
        const u8 iOptions    = pauseIdx++;
        const u8 iSaveQuit   = pauseIdx++;
        const u8 pauseOptCount = pauseIdx;
        (void)iSaveQuit;
        const s8   ph     = mouseActive ? pauseMenuHit(Window::getWidth(), Window::getHeight(), pauseOptCount) : static_cast<s8>(-1);
        const bool pclick = mouseActive && Input::isMouseButtonPressed(MOUSE_LEFT);
        if (ph >= 0 && static_cast<u8>(ph) != m_menu.subSelection) {
            m_menu.subSelection = static_cast<u8>(ph);
            AudioSystem::play(SfxId::MENU_HOVER);
        }
        // Hover sound on move — the pause menu was silent here for the same reason the main menu was.
        if (Input::isActionPressed(GameAction::MENU_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
            if (m_menu.subSelection > 0) { m_menu.subSelection--; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isActionPressed(GameAction::MENU_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
            if (m_menu.subSelection < pauseOptCount - 1) { m_menu.subSelection++; AudioSystem::play(SfxId::MENU_HOVER); }
        }
        if (Input::isActionPressed(GameAction::MENU_BACK)) {
            m_menu.confirmQuit = false;          // ESC/B = resume
            Input::setRelativeMouseMode(true);   // recapture the cursor for gameplay
        }
        // Enter and Space always confirm here too (matches menuConfirmPressed() in engine_menu.cpp).
        if (Input::isActionPressed(GameAction::MENU_CONFIRM)
            || Input::isKeyPressed(SDL_SCANCODE_SPACE)
            || Input::isKeyPressed(SDL_SCANCODE_RETURN)
            || Input::isKeyPressed(SDL_SCANCODE_KP_ENTER)
            || (pclick && ph >= 0)) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            if (m_menu.subSelection == iContinue) {
                // Continue Playing
                m_menu.confirmQuit = false;
                Input::setRelativeMouseMode(true); // recapture the cursor for gameplay
            } else if (hasMenagerie && m_menu.subSelection == iMenagerie) {
                // Menagerie — view-only collection page (handled at the top of update()).
                // confirmQuit is cleared for the same swallow-every-input reason as Options;
                // the overlay's back press restores it. Cursor stays free (still "paused").
                m_menagerieOpen    = true;
                m_menu.confirmQuit = false;
            } else if (m_menu.subSelection == iOptions) {
                // Options — the same screens the main menu uses (audio / bindings / display), opened
                // mid-run. They live in GameState::MENU, so we leave IN_GAME and flag WHY, and the
                // BACK handler in engine_menu.cpp brings us straight back to this paused game.
                // The world is untouched: nothing is torn down by the state switch.
                // Stay in IN_GAME: the options screens are drawn as an OVERLAY over the live, frozen
                // scene (see render() and the optionsFromPause branch at the top of update()).
                // Switching to GameState::MENU would have worked, but renderTransitionScreens
                // early-outs on MENU and never draws the world — the player would be looking at the
                // title backdrop, which reads as "I left my run" rather than "I paused it".
                m_menu.optionsFromPause = true;
                // confirmQuit MUST be cleared: the pause handler runs early in update() and RETURNS
                // whenever it is set, so leaving it true would swallow every input before the options
                // screen ever saw it — and would keep drawing the pause overlay on top of it.
                m_menu.confirmQuit  = false;
                m_menu.subState     = 3;   // options category list
                m_menu.subSelection = 0;
                m_menu.bindCapture  = false;
                Input::setRelativeMouseMode(false);   // the options screens are cursor-driven
            } else if (canCloseLobby && m_menu.subSelection == iCloseLobby) {
                // Close Lobby — stop advertising + refuse new joiners; the game continues over the relay
                // with whoever's already in. Resume gameplay afterward (this isn't a quit).
                Steam::closeLobby();
                addChatMessage("System", "Lobby closed — no new players can join.", {1.0f, 0.85f, 0.4f});
                m_menu.confirmQuit = false;
                m_menu.subSelection = 0;            // reset highlight; the option is gone next time
                Input::setRelativeMouseMode(true);  // recapture the cursor for gameplay
            } else {
                // Save and Quit (always the last row)
                m_menu.confirmQuit = false;
                m_menu.optionsFromPause = false;   // leaving the game: never resume into it later
                saveAllCharacters();  // per-character: each local lane to its own slot
                if (m_netRole != NetRole::NONE) {
                    Net::disconnect();
                    m_netRole = NetRole::NONE;
                }
                m_gameState = GameState::MENU;
                AudioSystem::stopMusic();
                Input::setRelativeMouseMode(false);
            }
        }
        // R12 — SP: the pause menu freezes the world (the early-return below skips
        // gameUpdate for the whole frame). MP: never freeze — remote peers can't be
        // held hostage by anyone's pause press. The menu still renders and accepts
        // input on top of the running world; the pausing player's character stays
        // in the world (still hittable, still processed by the server). Matches
        // Quake / Hellgate / Diablo II MP semantics: pause is a personal UI, not a
        // session-wide gate.
        if (m_netRole == NetRole::NONE) {
            return;
        }
        // MP fall-through: continue into the gameState switch so gameUpdate + net
        // pre/post run normally with the menu overlaid.
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
    // Options opened from the pause menu. This sits BEFORE the pause handler below, which would
    // otherwise eat ESC and re-open the pause overlay instead of letting MENU_BACK leave the
    // options screen.
    // SP: the world is frozen under the overlay (return skips gameUpdate + shared systems).
    // MP: NEVER freeze — R12, same as the pause overlay. The old unconditional return meant a
    // HOST sitting in audio settings skipped serverNetPre/Post and froze every client via input
    // starvation (and a guest froze its own sim). The menu above already consumed this frame's
    // UI navigation, so drop the pause-press edge — ESC must page back through the options
    // screens, not stack the pause overlay on top of them.
    if (m_gameState == GameState::IN_GAME && m_menu.optionsFromPause) {
        updateMenu(dt);
        if (m_netRole == NetRole::NONE) return;
        anyPause = false;
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
                   m_gameState != GameState::VICTORY &&
                   m_gameState != GameState::CREDITS) {   // ESC in credits = skip (handled in its case)
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
            // Co-op carries a player who died on the previous floor to the next one (HP is
            // refilled below), so clear death flags — otherwise that player would arrive
            // frozen behind the "YOU DIED" overlay despite being alive again.
            for (u8 p = 0; p < m_splitPlayerCount; p++) m_playerDead[p] = false;
            // In split-screen, reposition both players at the new spawn (M5 helper).
            if (m_splitPlayerCount > 1) {
                positionLocalPlayersAtSpawn();
                // Single growth point for split-screen: each local player +1.5% HP/energy
                // exactly once per floor (the door-path growth is suppressed in split).
                // Runs AFTER positionLocalPlayersAtSpawn (which set m_localPlayers[0]) so
                // P0's growth isn't clobbered.
                for (u8 p = 0; p < m_splitPlayerCount; p++) {
                    // Grow the BASE, not maxHealth: maxHealth is derived (base + gear + buffs), so
                    // growing it directly would be overwritten by the next refresh — and, worse, a
                    // derived value that anything may permanently nudge is exactly what let a leaked
                    // shrine buff compound into the save.
                    m_localPlayers[p].baseMaxHealth *= 1.015f;
                    Inventory::refreshMaxHealth(m_localPlayers[p], m_inventories[p]);
                    m_localPlayers[p].health = m_localPlayers[p].maxHealth;
                    m_skillStates[p].maxEnergy *= 1.015f;
                    m_skillStates[p].energy = m_skillStates[p].maxEnergy;
                }
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
        // Singleplayer/split-screen have no net pre-step, so advance the sim tick here. The R17
        // tick-based skill/potion cooldown gate reads it via currentLocalTick() (= m_serverTick for
        // NONE); without this it stays 0 and every cooldown jams after the first use (the gate then
        // computes (0 - lastActivationTick + grace) which u32-underflows below any real cooldown).
        // SERVER advances m_serverTick in serverNetPre (engine_net.cpp:71); CLIENT advances
        // m_clientTick in clientNetPost. Once per fixed update — m_serverTick is the shared sim clock.
        if (m_netRole == NetRole::NONE) m_serverTick++;

        // M4: decay each local lane's smooth-correction offset once per CLIENT frame so the camera
        // position smoothly converges toward the server-corrected sim position over ~150 ms.
        if (m_netRole == NetRole::CLIENT)
            for (u8 lane = 0; lane < m_splitPlayerCount; lane++) RenderOffsetOps::tick(m_renderOffset[lane], dt);

        // M14 + net-diagnostics: 1 Hz net-graph window. Runs for SERVER and CLIENT so the F9
        // overlay (engine_hud.cpp) and this log read fresh per-second bandwidth/snapshot metrics.
        // Net::tickMetricsWindow folds the raw counters into per-slot NetMetrics and resets the
        // window. m_divergenceCount is bumped in clientNetPost on each reconcile mismatch.
        if (m_netRole != NetRole::NONE) {
            f64 nowSec = Clock::getElapsedSeconds();
            if (nowSec - m_lastDebugLogSec >= 1.0) {
                Net::tickMetricsWindow(static_cast<f32>(nowSec - m_lastDebugLogSec));
                if (m_netRole == NetRole::CLIENT) {
                    // Baseline age = how far the estimated server clock is ahead of the newest
                    // applied snapshot (engine-side state Net can't see, so computed here).
                    NetMetrics m = Net::getMetrics();
                    u32 bage = NetMetricsOps::baselineAgeTicks(
                        static_cast<u32>(m_clockSync.serverTickEst), m_lastAppliedSnap.serverTick);
                    // Shaky-FOV diagnostic: div= corrections/s (each snaps the camera → shake when
                    // frequent). mean/max = correction magnitude. near= how many fired while brushing
                    // an enemy (root-cause signature). idelay= client's adaptive interp delay — the
                    // wider it is vs the server's fixed 33 ms rewind, the bigger the obstacle-time
                    // mismatch that drives the divergence. Smooth floor ≈ div=0; shaky floor = high
                    // div, near≈div, idelay>33 ms.
                    f32 divMean = (m_divergenceCount > 0)
                                  ? m_divergenceSumM / static_cast<f32>(m_divergenceCount) : 0.0f;
                    LOG_INFO("[NET-GRAPH] rtt=%.1fms est=%.1f div=%u(near=%u mean=%.2fm max=%.2fm) "
                             "idelay=%.0fms in=%.1fKB/s(wire~%.1f) snap=%.1fHz bage=%ut",
                             m_clockSync.oneWayTripMs * 2.0f,
                             static_cast<f64>(m_clockSync.serverTickEst),
                             m_divergenceCount, m_divergenceNearEnemyCount, divMean, m_divergenceMaxM,
                             Client::getInterpDelaySec() * 1000.0f,
                             m.kbInTotal, m.wireKbIn, m.snapsInPerSec, bage);
                } else {
                    // SERVER: one line per connected remote client — the M12 read-off surface.
                    const NetPlayerSlot* slots = Net::getSlots();
                    for (u32 slot = 0; slot < MAX_PLAYERS; slot++) {
                        if (slot == static_cast<u32>(m_localPlayerIndex)) continue;
                        if (!slots || slots[slot].state != SlotState::ACTIVE) continue;
                        NetMetrics m = Net::getMetricsForSlot(static_cast<u8>(slot));
                        u32 bage = NetMetricsOps::baselineAgeTicks(m_serverTick, m_clientAckedSnap[slot]);
                        LOG_INFO("[NET-GRAPH] cli s%u out=%.1fKB/s(wire~%.1f) delta=%.0f%% loss=%.1f%% bage=%ut",
                                 slot, m.kbOutTotal, m.wireKbOut, m.deltaFullRatio * 100.0f,
                                 m.packetLoss * 100.0f, bage);
                    }
                }
                m_lastDebugLogSec = nowSec;
                m_divergenceCount = 0;
                // Reset the shaky-FOV diagnostic accumulators alongside the count so each
                // [NET-GRAPH] line reports a fresh 1 s window.
                m_divergenceSumM = 0.0f;
                m_divergenceMaxM = 0.0f;
                m_divergenceNearEnemyCount = 0;
            }
        }

        // Tick the spawn-calm window once per frame (here, not in gameUpdate, so
        // couch co-op — which calls gameUpdate per player — doesn't double-decrement).
        if (m_spawnCalmTimer > 0.0f) m_spawnCalmTimer -= dt;

        // Floor/total play-time clocks tick HERE, once per sim tick, for the same reason as the
        // spawn-calm window above: they used to accumulate inside gameUpdate, which runs once PER
        // LOCAL PLAYER — so a whole split-screen session's clock ran at double speed. This spot
        // also pins the semantics to "time the character is actually in the world": update()
        // returns before this switch for menus, the SP pause overlay, the death screen and floor
        // transitions, so none of those count. (The MP pause overlay deliberately DOES count —
        // R12 keeps the pausing player's character live and hittable in the shared world.)
        m_transition.floorTime     += dt;
        m_transition.totalPlayTime += dt;

        // Split-screen: update each local player in turn
        for (u8 sp = 0; sp < m_splitPlayerCount; sp++) {
            swapInPlayer(sp);             // sets m_localPlayerIndex = sp (the active player)
            Input::setActivePlayer(sp);

            // Networked-MP dead overlay cursor: free while this (single, local) player is dead so
            // the "Respawn" prompt is clickable, re-capture once alive. Single owner, edge-driven
            // via m_deathCursorFree so we only hit the OS on a transition. Gated to networked play
            // (split-screen shares one mouse with a still-alive teammate — leave it captured), and
            // reset on startGame so a stale flag from a prior session can't suppress the free.
            if (m_netRole != NetRole::NONE) {
                if (m_playerDead[sp] && !m_deathCursorFree) {
                    Input::setRelativeMouseMode(false); m_deathCursorFree = true;
                } else if (!m_playerDead[sp] && m_deathCursorFree) {
                    Input::setRelativeMouseMode(true);  m_deathCursorFree = false;
                }
            }

            if (m_playerDead[sp]) {
                // Networked-MP dead overlay: the cursor is freed at the top of the loop, so a
                // click on the "Respawn" prompt counts as respawn input (split-screen leaves the
                // cursor captured — see the loop-top block — so this stays false there).
                const bool deadClickRespawn = (m_netRole != NetRole::NONE) &&
                    Input::isMouseButtonPressed(MOUSE_LEFT) &&
                    deathRespawnPromptHit(Window::getWidth(), Window::getHeight());
                // Dead player: check for respawn input, skip gameplay. Gamepad JUMP is routed
                // to this player's controller (setActivePlayer above); keyboard Space counts
                // only for P0 (L4 — Space is P0's key and must not respawn the controller P2).
                if (Input::isActionPressed(GameAction::JUMP) ||
                    (sp == 0 && Input::isKeyPressed(SDL_SCANCODE_SPACE)) || deadClickRespawn) {
                    // (M4) Enemies are already sent home when the last player dies (the co-op
                    // death path calls resetEnemiesToRooms), so nothing to reset on respawn —
                    // the old allDead computation here was dead code.
                    if (m_netRole == NetRole::CLIENT) {
                        // CLIENT: send the reliable respawn request and immediately predict the
                        // transition locally (M9). The server confirms via the next snapshot; if
                        // the server rejects (e.g. duplicate packet), it returns isDead=true and
                        // clientNetPre will re-set m_playerDead — harmless one-frame flicker.
                        // Mirrors handleRespawnRequest() field assignments (engine_update.cpp).
                        // activeNetSlot() is THIS dying lane's net slot (swapInPlayer(sp) above), so
                        // the server revives the right couch player — P2's respawn revives P2.
                        sendRespawnRequest(activeNetSlot());
                        {
                            NetPlayer& np = m_players[activeNetSlot()];
                            m_localPlayer.health        = np.maxHealth;
                            m_localPlayer.position      = np.spawnPosition;
                            m_localPlayer.velocity      = {0, 0, 0};
                            m_localPlayer.invulnTimer   = 1.5f;  // matches server's handleRespawnRequest
                            m_localPlayer.damageFlashTimer = 0.0f;
                            m_localPlayer.hurtVignette  = 0.0f;
                            // Clear the net-layer isDead so clientNetPre doesn't immediately
                            // re-gate us dead on this same frame before the snapshot arrives.
                            np.isDead = false;
                        }
                        m_playerDead[sp] = false;
                        snapCameraToPlayer();  // prevent camera-interp smear from death position
                    } else {
                        // SERVER + split-screen: direct local revive (authoritative locally).
                        m_localPlayer.health = m_localPlayer.maxHealth;
                        m_localPlayer.position = m_players[activeNetSlot()].spawnPosition; // local net slot (sp is the lane)
                        m_localPlayer.velocity = {0, 0, 0};
                        m_localPlayer.invulnTimer = 2.0f;
                        m_localPlayer.hurtVignette = 0.0f; // no red lingering on co-op respawn
                        snapCameraToPlayer();              // no camera-interp smear on co-op respawn
                        m_playerDead[sp] = false;
                        if (m_netRole == NetRole::SERVER) {
                            NetPlayer& np = m_players[m_localPlayerIndex];
                            np.health = np.maxHealth;
                            np.position = np.spawnPosition;
                            np.velocity = {0, 0, 0};
                            np.invulnTimer = 2.0f;
                            np.isDead = false;
                        }
                    }
                }
                swapOutPlayer(sp);
                // A dead player runs no gameplay. The shared world systems still tick once
                // after the loop (tickSharedSystems below), so enemies/projectiles/FX keep
                // running and enemies return home when everyone is down — no special-casing
                // needed here anymore (M3 removed the duplicated dead-branch shared block).
                continue;
            }

            gameUpdate(dt);
            swapOutPlayer(sp);
        }

        // Shared world systems run EXACTLY once, after every local player has been updated
        // (M3) — independent of who is alive, so FX/AI/projectiles never freeze when P1 is
        // dead, and they're no longer duplicated across the alive/dead code paths.
        //
        // Singleplayer pause: while the character inspect screen is open, freeze the world
        // (enemy AI, projectiles, combat) so the player can't be attacked/killed while looking
        // at their stats. The inspect-model rotation lives in gameUpdate (already run above) so
        // it keeps responding. MP never pauses — remote peers can't be held hostage by one
        // player's screen (mirrors the m_menu.confirmQuit pause policy).
        if (m_netRole != NetRole::NONE || !m_characterScreenOpen) {
            tickSharedSystems(dt);
        }

        if (m_netRole == NetRole::SERVER) serverNetPost(dt);
        if (m_netRole == NetRole::CLIENT) clientNetPost(dt);
        break;
    case GameState::GAME_OVER:
        break; // handled above
    case GameState::CREDITS:
        // Scrolling credits (post-Engine portal / Hell-complete). Runs identically on every
        // machine — the net session stays connected (Net::poll is state-independent) and is
        // torn down when VICTORY returns to the menu. Any confirm key skips to VICTORY;
        // otherwise the scroll's end (checked against the row count in renderCredits) lands
        // there too via m_creditsScroll.
        m_creditsScroll += dt * 40.0f;   // ~40 px/s at 720p reference scale
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) ||
            Input::isActionPressed(GameAction::MENU_BACK) ||   // ESC/B skip, like every menu
            Input::isActionPressed(GameAction::JUMP) ||
            Input::isKeyPressed(SDL_SCANCODE_SPACE) ||
            Input::isKeyPressed(SDL_SCANCODE_RETURN) ||
            Input::isKeyPressed(SDL_SCANCODE_ESCAPE) ||
            m_creditsScroll > Credits::scrollEnd()) {
            m_gameState = GameState::VICTORY;
        }
        break;
    case GameState::VICTORY:
        // Final victory (Hell floor 50 cleared) — "You conquered the Dungeon Engine."
        // MENU_BACK is in the set so ESC/B dismiss this screen like every other menu.
        if (Input::isActionPressed(GameAction::MENU_CONFIRM) ||
            Input::isActionPressed(GameAction::MENU_BACK) ||
            Input::isActionPressed(GameAction::JUMP) ||
            Input::isKeyPressed(SDL_SCANCODE_SPACE) ||
            Input::isKeyPressed(SDL_SCANCODE_RETURN)) {
            // The Engine-slayer's ending leads HOME, not to the menu: persist the cleared
            // marker (Continue must land in town forever after) and walk out into the town.
            // Clients keep the menu path — a guest that waits is pulled in by the host's
            // town sentinel; pressing the key is explicitly leaving the session.
            if (s_engineSlain && m_netRole != NetRole::CLIENT) {
                if (m_level.currentFloor <= 50) {
                    m_level.currentFloor = 51;              // the FreePlay::saveCleared marker
                    m_level.savedFloor   = 51;
                }
                unlockTown();   // account-wide: every hero (incl. future ones) starts at home now
                saveAllCharacters();
                s_engineSlain = false;
                enterTown();
                break;
            }
            m_gameState = GameState::MENU;
            AudioSystem::stopMusic();
            Input::setRelativeMouseMode(false);
            // Tear the net session down on the way out — the ending now REACHES clients (the
            // credits broadcast), so both roles pass through here with a live session. Without
            // this the host's server lingered under the main menu and a guest reconnect met a
            // ghost lobby.
            if (m_netRole != NetRole::NONE) {
                Net::disconnect();
                m_netRole = NetRole::NONE;
            }
            // Belt-and-suspenders: clear the session-only secret-superboss state on return to menu
            // (NEW_GAME / loadGame also reset it; this guards a lingering s_engineSlain ending flag).
            s_sourceShards = 0;
            s_engineSlain  = false;
            m_level.inSourceChamber    = false;
            m_level.sourcePortalActive = false;
            m_level.exitPortalActive   = false;
        }
        break;
    }
}


// ---------------------------------------------------------------------------
// Shared-world systems — run EXACTLY once per frame (M3), AFTER every local player has been
// updated. This used to live inline in gameUpdate gated on m_localPlayerIndex==0 AND was
// duplicated in the dead-player branch of the update loop. The gated copy never ran when P1
// (sp=0) was dead, so FX/meteors/orb-shards/particles froze even while P2 kept playing.
// Centralizing here fixes that freeze and removes the copy-paste drift.
//
// Primary AI/projectile reference = first ALIVE local player (fallback P0 if all dead); the
// other living locals ride along as extras so enemies chase the nearest. On a SERVER the
// extras are throwaway views of the remote NetPlayers (R7-3); on a CLIENT the local player's
// HP/status is saved/restored around the passes so the ghost sim can't fight the
// authoritative snapshot HP (R7-4 option (a)).
// ---------------------------------------------------------------------------
// tickChampions — the champion affixes that fire on a CYCLE rather than on a hit or a death:
// MOLTEN's fire eruptions, THUNDERING's lightning novas, TELEPORTING's blink.
//
// All three are timed off the entity's own animTimer instead of new per-entity timer fields: Entity
// has no spare bytes (the champion fields already consumed its tail padding), and a phase derived
// from a value the host already ticks needs no state at all. The edge test is "did the phase wrap
// this frame", which fires exactly once per period regardless of frame rate.
//
// Server/SP only — the caller gates it. Every player-facing effect goes through emitNovaFX, which
// BROADCASTS, so a guest sees the thing that is hurting them rather than taking damage from thin
// air. (Entity.hasAuraBuff is the standing counter-example in this codebase: it drives a tint, is
// not replicated, and so is invisible to every guest.)
// ---------------------------------------------------------------------------
void Engine::tickChampions(f32 dt) {
    if (dt <= 0.0f) return;

    // Fires once per `period`, on the frame the phase wraps.
    auto cyclePassed = [dt](f32 animTimer, f32 period) -> bool {
        if (period <= 0.0f) return false;
        const f32 prev = fmodf(animTimer - dt, period);
        const f32 cur  = fmodf(animTimer, period);
        return cur < prev;   // wrapped
    };

    // AoE onto every player — local lanes directly, remotes through the throwaway-view path so the
    // hit rides the snapshot (same shape as the BOMBER explosion in engine_death.cpp).
    auto novaDamagePlayers = [&](Vec3 pos, f32 radius, f32 dmg) {
        for (u8 p = 0; p < m_splitPlayerCount; p++) {
            if (m_playerDead[p]) continue;
            Player& lp = m_localPlayers[p];
            Vec3 d = lp.position - pos;
            if (sqrtf(d.x * d.x + d.z * d.z) < radius)
                Combat::applyDamageToPlayer(lp, dmg, &pos);
        }
        if (m_netRole == NetRole::SERVER) {
            for (u8 s = 0; s < MAX_PLAYERS; s++) {
                if (s == m_localPlayerIndex) continue;
                const NetPlayer& np = m_players[s];
                if (!np.active || np.isDead) continue;
                Vec3 d = np.position - pos;
                if (sqrtf(d.x * d.x + d.z * d.z) >= radius) continue;
                Player view;
                buildRemotePlayerView(s, view);
                Combat::applyDamageToPlayer(view, dmg, &pos);
                applyRemotePlayerView(view, s);
            }
        }
    };

    for (u32 a = 0; a < m_entities.activeCount; a++) {
        Entity& e = m_entities.entities[m_entities.activeList[a]];
        if (!(e.flags & ENT_CHAMPION) || (e.flags & ENT_DEAD) || e.champAffixes == 0) continue;

        if ((e.champAffixes & ChampAffix::MOLTEN) &&
            cyclePassed(e.animTimer, Champion::MOLTEN_ERUPT_SEC)) {
            novaDamagePlayers(e.position, Champion::MOLTEN_ERUPT_RAD,
                              e.damage * Champion::MOLTEN_ERUPT_PCT);
            emitNovaFX(e.position, Champion::MOLTEN_ERUPT_RAD, {1.0f, 0.45f, 0.12f});
        }

        if ((e.champAffixes & ChampAffix::THUNDERING) &&
            cyclePassed(e.animTimer, Champion::THUNDER_PERIOD_SEC)) {
            novaDamagePlayers(e.position, Champion::THUNDER_RADIUS,
                              e.damage * Champion::THUNDER_DMG_PCT);
            emitNovaFX(e.position, Champion::THUNDER_RADIUS, {0.55f, 0.85f, 1.0f});
        }

        if ((e.champAffixes & ChampAffix::TELEPORTING) &&
            cyclePassed(e.animTimer, Champion::TELEPORT_PERIOD_SEC)) {
            // Only blinks if you have actually opened a gap — otherwise it would teleport on top of
            // a player it is already meleeing, which reads as a bug rather than a threat.
            Vec3 target{}; f32 bestSq = Champion::TELEPORT_MIN_DIST * Champion::TELEPORT_MIN_DIST;
            bool found = false;
            for (u8 p = 0; p < m_splitPlayerCount; p++) {
                if (m_playerDead[p]) continue;
                Vec3 d = m_localPlayers[p].position - e.position;
                f32 dSq = d.x * d.x + d.z * d.z;
                if (dSq > bestSq) { bestSq = dSq; target = m_localPlayers[p].position; found = true; }
            }
            if (m_netRole == NetRole::SERVER) {
                for (u8 s = 0; s < MAX_PLAYERS; s++) {
                    if (s == m_localPlayerIndex) continue;
                    const NetPlayer& np = m_players[s];
                    if (!np.active || np.isDead) continue;
                    Vec3 d = np.position - e.position;
                    f32 dSq = d.x * d.x + d.z * d.z;
                    if (dSq > bestSq) { bestSq = dSq; target = np.position; found = true; }
                }
            }
            if (found) {
                emitNovaFX(e.position, 1.5f, {0.70f, 0.35f, 1.0f});   // departure flash
                Vec3 toChamp = e.position - target;
                f32  len = sqrtf(toChamp.x * toChamp.x + toChamp.z * toChamp.z);
                Vec3 dir = (len > 0.001f) ? Vec3{toChamp.x / len, 0.0f, toChamp.z / len}
                                          : Vec3{1.0f, 0.0f, 0.0f};
                Vec3 dest = target + dir * Champion::TELEPORT_LAND_DIST;
                dest.y = e.position.y;
                // Never blink into geometry — a champion inside a wall is unkillable and unfair.
                Collision::ensureNotInWall(dest, e.halfExtents, m_level.grid);
                e.position = dest;
                e.velocity = {0.0f, 0.0f, 0.0f};
                emitNovaFX(e.position, 1.5f, {0.70f, 0.35f, 1.0f});   // arrival flash
            }
        }
    }
}

// ---------------------------------------------------------------------------
// tickLootGoblins — drip the goblin's loot while you chase it.
//
// Bleeding the sack over time (rather than dumping it all on death) does two jobs: every second of
// the chase visibly pays, so the pursuit feels worth it even if it escapes; and it spreads the
// drops out instead of bursting them, which is what would otherwise overrun the world-item pool.
// Authoritative sim only — the caller gates it.
// ---------------------------------------------------------------------------
void Engine::tickLootGoblins(f32 dt) {
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        Entity& e = m_entities.entities[m_entities.activeList[a]];
        if (!(e.flags & ENT_LOOT_GOBLIN) || (e.flags & ENT_DEAD)) continue;
        // Only a FLEEING goblin bleeds. The loot shakes out of the sack as it runs — an un-provoked
        // one sitting on its hoard would otherwise quietly drip items onto the floor for free, and
        // the player could just stand back and farm it without ever swinging.
        if (e.aiState != AIState::FLEE) continue;
        if (e.resurrectCount >= Goblin::BLEED_MAX) continue;   // out of pocket (reused as a counter)

        e.tacticalTimer -= dt;
        if (e.tacticalTimer > 0.0f) continue;
        e.tacticalTimer = Goblin::BLEED_SECONDS;

        u8 lvl = static_cast<u8>(e.level > 255 ? 255 : e.level);
        if (lvl < 1) lvl = 1;
        ItemInstance drop = ItemGen::rollItem(lvl, m_itemDefs, m_itemDefCount,
                                              m_affixDefs, m_affixDefCount);
        if (isItemEmpty(drop)) continue;

        Vec3 pos = e.position + Vec3{0.0f, 0.3f, 0.0f};
        // Check the return — a full pool makes spawn() fail silently, which for a chase reward would
        // read as "the goblin dropped nothing", i.e. a broken feature rather than a full pool.
        if (!WorldItemSystem::spawn(m_worldItems, drop, pos, &m_level.grid, 0xFF)) {
            LOG_WARN("LootGoblin: bled item lost — world-item pool full");
            continue;
        }
        // No broadcastLootSpawn here (it is a file-static in engine_death.cpp): that packet is only
        // an *early* notify for kill-feed/minimap. World items replicate through the snapshot
        // regardless (Client::mirrorWorldItems), so a guest still sees every bled item.
        e.resurrectCount++;   // free on a goblin: only SUMMONER resurrection reads it
    }
}

// ---------------------------------------------------------------------------
// grantShrineBuff — apply a shrine's buff to a player. ONE implementation, deliberately templated
// over Player/NetPlayer, because the host grants onto its local Player and the server grants onto a
// remote's NetPlayer — and if those two drifted, a shrine would mean different things depending on
// who touched it.
//
// VITALITY is the fiddly one. It raises maxHealth, and SnapPlayer sends health as a RATIO of
// maxHealth: a bare max bump would leave the player's absolute HP unchanged while the denominator
// grew, so every HP bar (theirs and, in co-op, the one everyone else sees) would visibly LURCH
// DOWNWARD at the exact moment they picked up a health buff. So heal by the same absolute amount:
// the ratio is preserved and the bar grows instead of dropping.
// ---------------------------------------------------------------------------
void Engine::grantShrineBuff(Player& p, u8 buff)    { Shrine::apply(p, buff); }
void Engine::grantShrineBuff(NetPlayer& p, u8 buff) { Shrine::apply(p, buff); }

void Engine::tickSharedSystems(f32 dt) {
    // (L8) Default kill-credit to "none" so AI / skill / DoT entity kills drop free-for-all;
    // projectile.cpp re-asserts each player projectile's own firer around its damage below.
    Combat::setAttackingPlayer(0xFF);

    // Choose the primary reference (first alive local) + the living-local extra targets.
    u8 primaryIdx = 0;
    for (u8 p = 0; p < m_splitPlayerCount; p++) {
        if (!m_playerDead[p]) { primaryIdx = p; break; }
    }
    Player& primary = m_localPlayers[primaryIdx];

    Player* extras[MAX_PLAYERS - 1];
    u32     extraCount = 0;

    // SERVER: build throwaway views of the remote NetPlayers, copied back after projectiles.
    Player  remoteViews[MAX_PLAYERS - 1];
    Player* remotePtrs[MAX_PLAYERS - 1];
    u8      remoteSlots[MAX_PLAYERS - 1];
    u32     remoteViewCount = 0;
    if (m_netRole == NetRole::SERVER) {
        remoteViewCount = buildRemotePlayerViews(remoteViews, remotePtrs, remoteSlots);
        for (u32 i = 0; i < remoteViewCount; i++) extras[extraCount++] = remotePtrs[i];
    } else {
        // Split-screen: the other living local players are the extra targets.
        for (u8 p = 0; p < m_splitPlayerCount; p++) {
            if (p == primaryIdx || m_playerDead[p]) continue;
            extras[extraCount++] = &m_localPlayers[p];
        }
    }

    // N4 ghost-sim removal: on CLIENT, the snapshot is the authoritative source for entities,
    // projectiles, world items, and timers. Running the local sim here just produces a
    // divergent "ghost" world that blocks player movement against entities the host already
    // killed and fake-hits the local player (Combat::applyDamageToPlayer flashes through
    // hurtVignette/sound/camera-kick even though the HP it subtracts is overwritten by the
    // next snapshot reconcile). With the obstacle source now coming from m_renderInterp.entities
    // (engine_update.cpp gameUpdate above) the ghost no longer has to be alive for collision,
    // so we can finally skip every authoritative pass here. Cosmetic-only systems (chat log,
    // shared FX, particles) hoist out below and keep running on CLIENT.
    if (m_netRole != NetRole::CLIENT) {
        // Enemy AI — enemies target the nearest of primary + extras.
        {
            PROFILE_SCOPE(1, "AI");
            bool spawnCalm = m_spawnCalmTimer > 0.0f; // floor-start calm window
            // Pass m_players + MAX_PLAYERS so the friendly-tether code in EnemyAI::update can
            // resolve `Entity::ownerNetSlot` against the right NetPlayer for remote-cast minions
            // (N4). Harmless on SP/split since all `m_players[]` slots are inactive there.
            EnemyAI::update(m_entities, m_level.grid, primary, m_projectiles, dt, &m_level.squads,
                            extraCount > 0 ? extras : nullptr, extraCount, &m_level.dungeon, spawnCalm,
                            m_players, MAX_PLAYERS);
            SquadSystem::update(m_level.squads, m_level.dungeon, m_entities, primary.position, dt);
            // Champion affixes that fire on a cycle (Molten eruptions, Thundering novas, blinks).
            // Runs right after the AI that advances animTimer, which is what their phase is derived
            // from. Authoritative-sim only — a client would double-apply these and desync.
            tickChampions(dt);
            tickLootGoblins(dt);   // drip the sack while it runs (same authoritative-only reason)
        }

        // Decay enemy speech timers + log fresh speech to chat (shared entity state → once/frame;
        // the old per-player placement double-ticked these in split-screen).
        // Pool by role: a CLIENT's speech state lives on the interp entities (planted there by
        // the SV_EVENT::SPEECH handler — its own m_entities is the gated-off ghost pool), and it
        // only DECAYS here: the chat line already arrived with the event, name/color resolved
        // server-side (nameTag doesn't replicate), so re-logging locally would double-post.
        {
            EntityPool& speechPool = (m_netRole == NetRole::CLIENT) ? m_renderInterp.entities
                                                                    : m_entities;
            // CLIENT iterates every slot, not the active list: the interp pool's active list is
            // rebuilt from each snapshot, so a speaker that drops out of snapshot coverage (death,
            // record-count clamp) would otherwise FREEZE its timer — and the stale bubble would
            // reappear, arbitrarily later, on whatever entity next used that pool index.
            const u32 speechScan = (m_netRole == NetRole::CLIENT) ? MAX_ENTITIES
                                                                  : speechPool.activeCount;
            for (u32 a = 0; a < speechScan; a++) {
                u32 idx = (m_netRole == NetRole::CLIENT) ? a : speechPool.activeList[a];
                Entity& e = speechPool.entities[idx];
                if (e.speechTimer <= 0.0f) continue;
                if (m_netRole != NetRole::CLIENT && e.speechText && e.speechTimer > 1.9f) {
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
                    } else if (e.flags & ENT_LOOT_GOBLIN) {
                        name = "Loot Goblin";   // hoard mutters — not an anonymous "???"
                    }
                    Vec3 chatCol = (e.flags & ENT_LOOT_GOBLIN)
                        ? Vec3{1.0f, 0.84f, 0.25f}   // hoard gold, matching the chase trail
                        : (e.flags & ENT_FRIENDLY)
                            ? Vec3{0.4f, 1.0f, 0.5f}
                            : Vec3{1.0f, 0.3f, 0.3f};
                    addChatMessage(name, e.speechText, chatCol);
                    // Ship the line to guests: bubble + chat on their side (SV_EVENT::SPEECH).
                    // Fires exactly once per line — same edge as the chat log above. Strings ride
                    // the wire literally so every current and future speech site replicates
                    // without a registry; the client bounds-checks hard on receipt.
                    if (m_netRole == NetRole::SERVER) {
                        u8 buf[sizeof(PacketHeader) + 6 + 24 + 64];
                        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
                        hdr->type  = NetPacketType::SV_EVENT;
                        hdr->flags = 0;
                        hdr->seq   = 0;
                        u32 off = sizeof(PacketHeader);
                        buf[off++] = static_cast<u8>(NetEventType::SPEECH);
                        buf[off++] = static_cast<u8>(idx);                       // server pool index
                        buf[off++] = static_cast<u8>(chatCol.x * 255.0f);
                        buf[off++] = static_cast<u8>(chatCol.y * 255.0f);
                        buf[off++] = static_cast<u8>(chatCol.z * 255.0f);
                        u8 nameLen = static_cast<u8>(std::strlen(name));
                        if (nameLen > 23) nameLen = 23;
                        buf[off++] = nameLen;
                        std::memcpy(buf + off, name, nameLen); off += nameLen;
                        u8 lineLen = static_cast<u8>(std::strlen(e.speechText));
                        if (lineLen > 63) lineLen = 63;
                        buf[off++] = lineLen;
                        std::memcpy(buf + off, e.speechText, lineLen); off += lineLen;
                        Net::broadcastReliable(buf, off);
                    }
                    e.speechTimer = 1.8f; // prevent re-logging on next tick
                }
                e.speechTimer -= dt;
                if (e.speechTimer <= 0.0f) {
                    e.speechText  = nullptr;
                    e.speechTimer = 0.0f;
                }
            }
        }

        // Projectiles — primary + extras are collidable so enemy projectiles damage everyone.
        {
            PROFILE_SCOPE(2, "Projectiles");
            SpatialGridSystem::rebuild(m_spatialGrid, m_entities);
            ProjectileSystem::update(m_projectiles, m_level.grid, m_entities, primary, dt, &m_spatialGrid,
                                     extraCount > 0 ? extras : nullptr, extraCount);
        }

        // SERVER: write AI + projectile damage/status back to the authoritative NetPlayers
        // (serverNetPost then ticks DoT/death and the snapshot carries it to the owning client).
        if (remoteViewCount > 0)
            applyRemotePlayerViews(remoteViews, remoteSlots, remoteViewCount);

        EntitySystem::tickTimers(m_entities, dt);
        WorldItemSystem::update(m_worldItems, dt, m_itemDefs, m_itemDefCount);   // def-aware: pet drops never despawn

        SkillSystem::updateOrbProjectiles(m_projectiles, m_skillDefs, m_skillDefCount, dt);
        // Meteors: pass both local players so kill-heals credit the casting player.
        Player* meteorPlayers[MAX_LOCAL_PLAYERS];
        for (u8 p = 0; p < MAX_LOCAL_PLAYERS; p++) meteorPlayers[p] = &m_localPlayers[p];
        // Pass the NetPlayer array on a SERVER so a remote caster's meteor/pillar heal lands on
        // the caster's NetPlayer (H4), not silently re-routed to local lane 0.

        // D2 — AOE lag-comp for meteor / holy-pillar explosions.
        // On the SERVER, check which meteors are about to explode this tick (timer <= dt)
        // and compute the maximum lag-comp ticks across their remote casters. Using the
        // maximum means a simultaneous host-cast meteor (lagComp=0) and a remote-cast meteor
        // (lagComp=N) correctly rewind for the remote's explosion without affecting host
        // accuracy (beginLagComp with the remote's N rewound view is still correct for the
        // host's meteor since the host sees entities at the same present-time positions).
        // For host-only or singleplayer runs lagCompTicks stays 0 → beginLagComp is a no-op.
        u32 meteorLagCompTicks = 0;
        if (m_netRole == NetRole::SERVER) {
            extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
            for (u32 mi = 0; mi < MAX_PENDING_METEORS; mi++) {
                const PendingMeteor& pm = s_meteors[mi];
                if (!pm.active) continue;
                if (pm.timer > dt) continue; // won't explode this tick
                // Only rewind for remote casters — host-local lanes (slots 0..count-1) have no
                // network delay, so their meteors hit live entity positions, no lag-comp rewind.
                if (pm.caster < m_splitPlayerCount) continue;
                u32 ticks = computeLagCompTicks(pm.caster);
                if (ticks > meteorLagCompTicks) meteorLagCompTicks = ticks;
            }
            if (meteorLagCompTicks > 0) beginLagComp(meteorLagCompTicks);
        }

        SkillSystem::updateMeteors(m_entities, meteorPlayers, m_splitPlayerCount, dt,
                                    m_netRole == NetRole::SERVER ? m_players : nullptr);

        // D2 — Restore present-time entity poses after the meteor explosion pass.
        if (meteorLagCompTicks > 0) endLagComp();
    }

    // Chat log timers, shared FX, and particles are cosmetic — keep ticking on CLIENT so the
    // chat log lines vanish on schedule, FX overlays decay, and particles dissipate.
    for (u32 i = 0; i < MAX_CHAT_LINES; i++) {
        if (m_chatLog[i].timer > 0.0f) m_chatLog[i].timer -= dt;
    }
    tickSharedFX(dt);
    ParticleSystem::update(m_particles, dt);

    // Ambient monster cries — every ~12 s ONE living hostile calls out, distance-attenuated from
    // where it stands, so the soundscape hints (by loudness) how close the danger is. Pure
    // per-machine ambience: each peer rolls its own pick from its own view of the world (CLIENT:
    // the interpolated snapshot pool — its ghost sim is gone), so nothing goes on the wire, and
    // peers hearing different cries is fine — they are standing in different places anyway.
    m_monsterCryTimer -= dt;
    if (m_monsterCryTimer <= 0.0f) {
        m_monsterCryTimer = 9.0f + (std::rand() % 61) * 0.1f;   // 9–15 s, jittered — never a metronome
        const EntityPool& cryPool = (m_netRole == NetRole::CLIENT) ? m_renderInterp.entities
                                                                   : m_entities;
        // Reservoir-sample a uniformly random living hostile in one pass (no allocation).
        const Entity* crier = nullptr;
        u32 seen = 0;
        for (u32 a = 0; a < cryPool.activeCount; a++) {
            const Entity& e = cryPool.entities[cryPool.activeList[a]];
            if (e.flags & (ENT_DEAD | ENT_FRIENDLY)) continue;
            if (e.enemyType == EnemyType::PROP) continue;
            if (e.health <= 0.0f) continue;
            seen++;
            if ((std::rand() % seen) == 0) crier = &e;
        }
        if (crier) {
            // The nearest living local player listens — in split-screen whoever stands closer
            // hears it louder, matching how every other positional SFX picks its listener.
            Vec3 listener = m_localPlayers[0].position;
            f32 best = lengthSq(crier->position - listener);
            for (u8 p = 1; p < m_splitPlayerCount; p++) {
                if (m_playerDead[p]) continue;
                f32 d = lengthSq(crier->position - m_localPlayers[p].position);
                if (d < best) { best = d; listener = m_localPlayers[p].position; }
            }
            static const SfxId kCries[3] = {SfxId::MONSTER_CRY_1, SfxId::MONSTER_CRY_2,
                                            SfxId::MONSTER_CRY_3};
            // 24 m audible radius: a bit past enemy detection range (18 m), so a cry you can
            // barely hear means something is close to noticing you.
            AudioSystem::playAt(kCries[std::rand() % 3], crier->position, listener, 24.0f);
        }
    }

    // Loot-goblin chase breadcrumbs — the goblin is FAST and serpentines through door after
    // door; once it breaks line of sight the chase used to be over. Two trails: its coin sack
    // rattles on every sharp turn (each jink of the FLEE serpentine), positional from where it
    // runs; and a rate-limited chat line calls out its direction relative to you. Same
    // per-machine pattern as the ambient cries above: each peer detects turns from its own view
    // of the goblin (ENT_LOOT_GOBLIN and velocity are both in SnapEntity), nothing on the wire.
    {
        m_goblinJingleCd -= dt;
        m_goblinChatCd   -= dt;
        const EntityPool& gobPool = (m_netRole == NetRole::CLIENT) ? m_renderInterp.entities
                                                                   : m_entities;
        const Entity* gob = nullptr;
        u32 gobIdx = 0;
        for (u32 a = 0; a < gobPool.activeCount; a++) {
            const u32 idx = gobPool.activeList[a];
            const Entity& e = gobPool.entities[idx];
            if ((e.flags & ENT_LOOT_GOBLIN) && !(e.flags & ENT_DEAD) && e.health > 0.0f) {
                gob = &e;
                gobIdx = idx;
                break;
            }
        }
        const f32 gobSpeedSq = gob ? (gob->velocity.x * gob->velocity.x +
                                      gob->velocity.z * gob->velocity.z) : 0.0f;
        if (!gob || gobSpeedSq < 1.0f) {
            m_goblinHeadingValid = false;   // parked on its hoard (or gone) — no trail to leave

            // Hoard chatter: a SITTING goblin (IDLE — it hasn't been provoked yet) mutters to
            // itself and its sack clinks. Gated to earshot (22 m) so the tell rewards walking
            // near, never announces the goblin from across the floor. The rustle is per-machine
            // (aiState rides SnapEntity, so guests hear it too); the muttered line goes through
            // the ordinary entity-speech pipeline — bubble + auto chat line — which only the
            // authoritative sim owns, same as every other NPC's speech.
            if (gob && gob->aiState == AIState::IDLE) {
                m_goblinMutterTimer -= dt;
                if (m_goblinMutterTimer <= 0.0f) {
                    m_goblinMutterTimer = 5.0f + (std::rand() % 60) * 0.1f;   // 5-11 s, jittered
                    Vec3 listener = m_localPlayers[0].position;
                    f32  best = lengthSq(gob->position - listener);
                    for (u8 p = 1; p < m_splitPlayerCount; p++) {
                        if (m_playerDead[p]) continue;
                        f32 d = lengthSq(gob->position - m_localPlayers[p].position);
                        if (d < best) { best = d; listener = m_localPlayers[p].position; }
                    }
                    if (best < 22.0f * 22.0f) {
                        AudioSystem::playAt(SfxId::GOBLIN_JINGLE, gob->position, listener, 18.0f);
                        if (m_netRole != NetRole::CLIENT && (std::rand() % 2) == 0) {
                            // gobPool IS m_entities here (only CLIENT reads the interp pool),
                            // so writing speech through gobIdx mutates the authoritative entity.
                            static const char* kMutters[] = {
                                "mine, mine, all mine...",
                                "shiny... so shiny...",
                                "one, two... three? count again.",
                                "hee hee... no one finds us.",
                                "the sack stays SHUT.",
                            };
                            Entity& g = m_entities.entities[gobIdx];
                            g.speechText  = kMutters[std::rand() % 5];
                            g.speechTimer = 2.4f;
                        }
                    }
                }
            }
        } else {
            const f32 heading = atan2f(gob->velocity.z, gob->velocity.x);
            if (m_goblinHeadingValid) {
                f32 turn = heading - m_goblinTrackHeading;
                while (turn >  3.14159265f) turn -= 6.2831853f;   // shortest angular difference
                while (turn < -3.14159265f) turn += 6.2831853f;
                // ~30°+ in one frame = a jink, not steering noise. Debounced: the serpentine
                // re-rolls at most ~3×/s and each roll is one sharp discontinuity.
                if (fabsf(turn) > 0.52f && m_goblinJingleCd <= 0.0f) {
                    m_goblinJingleCd = 0.3f;
                    // Nearest living local player listens (split-screen: closest lane hears it
                    // loudest) — 30 m radius, wider than the cries: the trail must outrange sight.
                    Vec3 listener = m_localPlayers[0].position;
                    f32  listenerYaw = m_localPlayers[0].yaw;
                    f32  best = lengthSq(gob->position - listener);
                    for (u8 p = 1; p < m_splitPlayerCount; p++) {
                        if (m_playerDead[p]) continue;
                        f32 d = lengthSq(gob->position - m_localPlayers[p].position);
                        if (d < best) {
                            best = d;
                            listener    = m_localPlayers[p].position;
                            listenerYaw = m_localPlayers[p].yaw;
                        }
                    }
                    AudioSystem::playAt(SfxId::GOBLIN_JINGLE, gob->position, listener, 30.0f);

                    if (m_goblinChatCd <= 0.0f) {
                        m_goblinChatCd = 4.0f;
                        // Direction words relative to where the listener FACES (an on-screen
                        // compass doesn't exist, "to your left" always means something).
                        const Vec3 fwd = {-sinf(listenerYaw), 0.0f, -cosf(listenerYaw)};
                        const f32 dx = gob->position.x - listener.x;
                        const f32 dz = gob->position.z - listener.z;
                        const f32 rel = atan2f(fwd.x * dz - fwd.z * dx,   // + = to the right
                                               fwd.x * dx + fwd.z * dz);
                        static const char* kDirs[8] = {
                            "just ahead", "ahead, to your right", "to your right",
                            "behind you, right", "right behind you", "behind you, left",
                            "to your left", "ahead, to your left"};
                        const u32 sector =
                            static_cast<u32>((rel + 3.14159265f + 0.3926991f) / 0.7853982f) & 7;
                        // sector 0 starts at rel=-π (behind); rotate so 0 = ahead
                        char msg[80];
                        std::snprintf(msg, sizeof(msg), "*jingle jingle* — %s!",
                                      kDirs[(sector + 4) & 7]);
                        addChatMessage("Loot Goblin", msg, {1.0f, 0.84f, 0.25f});   // hoard gold
                    }
                }
            }
            m_goblinTrackHeading = heading;
            m_goblinHeadingValid = true;
        }
    }

    // V2 fire prediction: on CLIENT, the N4 gate above skips ProjectileSystem::update, so
    // locally-predicted ghost projectiles (spawned by handleWeaponFire on this frame) would
    // freeze at spawn and never animate. Tick them here with a minimal update — position
    // + gravity + lifetime — no collision, no damage (server is authoritative for both).
    // Predicted ghosts despawn when (a) their lifetime expires, (b) the predictedLife cap
    // is reached (0.5 s safety net if the matching authoritative snapshot never arrives —
    // e.g. server rejected the fire or UDP loss), or (c) the matching snapshot arrives
    // (handled in clientNetPost's match-and-despawn pass).
    if (m_netRole == NetRole::CLIENT) {
        // Client-side VISUAL-only meteor tick. Every player PREDICTS THEIR OWN meteors, so on a
        // CLIENT s_meteors is fed by:
        //   • Its OWN skill casts (class / boot / helm via tryActivate) — deterministic, predicted.
        //   • Its OWN weapon on-hit procs — melee/hitscan (engine_combat.cpp) and projectile (the
        //     predicted-impact site just above). The proc roll is a local std::rand() nobody else
        //     can reproduce, so the firing player owns it and reports it via CL_METEOR; the server
        //     spawns the one authoritative damaging copy.
        //   • OTHER players' meteors, relayed as SV_EVENT::METEOR (never our own — the server skips
        //     the caster when relaying, so our prediction is never double-spawned).
        // SkillSystem::updateMeteors is server-gated (runs only under `!CLIENT` above), so without
        // this tick every one of those would just freeze — telegraph never resolves, no impact ("the
        // meteor doesn't work on the client"). Advance their timers and detonate VISUALLY
        // (spawnSplashFX = fire ring + explosion + shake) with NO damage: damage is always the
        // server's, and enemies die via the snapshot.
        {
            extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
            for (u32 mi = 0; mi < MAX_PENDING_METEORS; mi++) {
                PendingMeteor& pm = s_meteors[mi];
                if (!pm.active) continue;
                pm.timer -= dt;
                if (pm.timer <= 0.0f) {
                    spawnSplashFX(pm.position, pm.radius); // visual impact only (no damage on CLIENT)
                    pm.active = false;
                }
            }
        }

        for (u32 i = 0; i < MAX_PROJECTILES; i++) {
            Projectile& p = m_projectiles.projectiles[i];
            if (!p.active || !p.predicted) continue;

            // Lifetime — mirrors the server's special case (projectile.cpp): an Infinity Chakram
            // (PROJ_INFINITE_BOUNCE) never times out, its `lifetime` counts UP as an age used by the
            // per-owner cap. Decrementing it here (as this did) killed the ghost a few seconds in,
            // and with it the locally-simulated bounces below.
            if (p.projFlags & PROJ_INFINITE_BOUNCE) p.lifetime += dt;
            else                                    p.lifetime -= dt;
            p.predictedLife += dt;

            // Chakram wall ricochet — SIMULATED locally rather than replicated. The client can't
            // hear the server's "pling" (ProjectileSystem::update, which plays it, is gated off on
            // CLIENT) and the bounce never crosses the wire (guests just interpolate the already-
            // reflected path), so the ghost used to sail straight THROUGH walls in silence.
            //
            // Re-simulating is safe because the bounce is deterministic and cheap to agree on: the
            // client holds the identical LevelGrid, and reflecting off an axis-aligned face flips
            // exactly one velocity component — so client and server produce the same outgoing
            // DIRECTION even if their impact points differ by a few centimetres. Residual position
            // error stays inside the speed-relative ghost-handoff tolerance in clientNetPost.
            // Same reflection as projectile.cpp: v' = v - 2(v·n)n, sat just off the struck face so
            // the next tick's ray doesn't re-hit it and burn another bounce.
            bool bounced = false;
            if (p.projFlags & PROJ_BOUNCE) {
                const f32 speed = length(p.velocity);
                const f32 travel = speed * dt;
                const bool infinite = (p.projFlags & PROJ_INFINITE_BOUNCE) != 0;
                if (travel > 0.0001f && (infinite || p.bouncesLeft > 0)) {
                    Vec3 dir = p.velocity * (1.0f / speed);
                    RayHit wallHit = Raycast::cast(m_level.grid, p.position, dir, travel + p.radius);
                    if (wallHit.hit && wallHit.distance <= travel + p.radius) {
                        if (!infinite) p.bouncesLeft--;
                        p.position = wallHit.position + wallHit.normal * (p.radius + 0.02f);
                        p.velocity = p.velocity - wallHit.normal * (2.0f * dot(p.velocity, wallHit.normal));
                        AudioSystem::playAt(SfxId::RICOCHET, wallHit.position, m_localPlayer.position);
                        bounced = true;   // already repositioned — skip this frame's integrate
                    }
                }
            }
            if (!bounced) {
                p.position = p.position + p.velocity * dt;
                if (p.gravity > 0.0f) p.velocity.y -= p.gravity * dt;
            }
            // Despawn timeout. A ghost that the match-and-keep pass (clientNetPost) has CONFIRMED —
            // i.e. found its authoritative snapshot copy and is keeping THIS ghost as the canonical
            // render — has its predictedLife reset every frame the authoritative still matches, so
            // predictedLife only grows once the server's copy is GONE (real impact / expiry). Despawn
            // it after a short CONFIRMED_LOST window then = a clean handoff with no fly-through-walls.
            // An UNCONFIRMED ghost (its authoritative never arrived) keeps the 0.5 s safety net: the
            // server likely rejected the fire (cooldown drift / origin clamp) or the snapshot was lost.
            constexpr f32 CONFIRMED_LOST_SEC = 0.1f;
            const f32 ghostTimeout = p.confirmed ? CONFIRMED_LOST_SEC : 0.5f;
            if (p.lifetime <= 0.0f || p.predictedLife >= ghostTimeout) {
                p.active = false;
                continue;
            }
            // Phase 2.3 — Predicted projectile-vs-entity collision against the INTERPOLATED
            // (rendered) pool. Without this, the predicted ghost flies through the rendered
            // enemy and the user sees the projectile pass through visibly before the
            // server's snapshot finally despawns it. Now: spawn the impact spark / blood
            // FX the instant the ghost reaches a rendered enemy AABB, and despawn the
            // ghost early. The server is still authoritative on damage; this is purely a
            // visual prediction. Skip ORB (phases through) to mirror server behavior.
            if (!p.fromPlayer) continue;       // only the local player's predicted ghosts
            if (p.projFlags & PROJ_ORB) continue;
            // Grace window: if we collision-check on frame 1, the ghost's spawn position
            // (eyePos + forward * ~0.5 m) is still inside any enemy standing within ~1 m
            // of the player — common in melee range — so the ghost despawns on its very
            // first tick before the renderer draws it. Visible as "fire weapon, see
            // nothing, then a tracer arrives mid-air from the snapshot". Give the ghost
            // 50 ms (≈3 frames at 60 Hz, ≈1-2 m at typical projectile speeds) to clear
            // the player's immediate neighborhood before predict-impact engages.
            if (p.predictedLife < 0.05f) continue;
            AABB projBox = {
                p.position - Vec3{p.radius, p.radius, p.radius},
                p.position + Vec3{p.radius, p.radius, p.radius}
            };
            for (u32 a = 0; a < m_renderInterp.entities.activeCount; a++) {
                u32 idx = m_renderInterp.entities.activeList[a];
                Entity& e = m_renderInterp.entities.entities[idx];
                if (e.flags & ENT_DEAD) continue;
                if (e.flags & ENT_FRIENDLY) continue;
                if (e.enemyType == EnemyType::PROP) continue;
                if (!CombatQuery::aabbOverlap(projBox, entityAABB(e))) continue;
                for (u32 fx = 0; fx < MAX_IMPACT_FX; fx++) {
                    if (!m_fx.impactFX[fx].active) {
                        Vec3 nrm = p.position - e.position; nrm.y = 0;
                        f32 l = lengthSq(nrm);
                        nrm = (l > 1e-6f) ? normalize(nrm) : Vec3{0,0,1};
                        m_fx.impactFX[fx] = {e.position, nrm, 0.3f, true, true};
                        break;
                    }
                }
                // Each player predicts their OWN weapon on-hit proc meteors. A CLIENT's projectile
                // impact is only ever detected HERE — ProjectileSystem::update (and with it the
                // host's proc hit-callback) is gated off on CLIENT — so this is the one place a
                // PROJECTILE-weapon METEOR_STRIKE proc can be rolled and predicted client-side.
                // Same roll + 10% chance as the melee/hitscan path (engine_combat.cpp) and the
                // host's projectile callback (engine_init_callbacks.cpp). predictProcMeteor spawns
                // the instant telegraph locally and reports it via CL_METEOR, so the server spawns
                // the single authoritative damaging meteor and relays it to the other players.
                if (m_weaponProc == SkillId::METEOR_STRIKE &&
                    (static_cast<u32>(std::rand()) % 100) < 10) {
                    const SkillDef* msd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount,
                                                                    SkillId::METEOR_STRIKE);
                    if (msd) predictProcMeteor(p.position, msd->damage, msd->radius, msd->delay);
                }
                p.active = false;              // ghost has done its visual job
                break;
            }
        }

        // Phase M7 / D3.2 — Predicted incoming-projectile hit on the local player.
        // Iterate the authoritative interpolated projectile pool (m_renderInterp.projectiles)
        // rather than the local ghost pool. For each enemy-owned projectile that is
        // approaching within PLAYER_HIT_RADIUS of the player's center:
        //   • fire visual feedback (damageFlash + hurtVignette) immediately;
        //   • decrement local HP by the server's expectedDamage (D3.1 wire field) raw —
        //     no defence/armor applied because we don't have that pipeline client-side.
        //     The next snapshot's authoritative HP (M4 reconcile + M13 renderedHealth lerp)
        //     absorbs any over/under-prediction smoothly.
        //   • record the key so M10 can ack the damage against the server's SV_DAMAGE_TO_ME.
        // Per-lane (online couch co-op): predict incoming projectile damage for EACH local player.
        // This block runs in tickSharedSystems (once/frame), so without the swap it would only cover
        // the last-swapped lane. swapInPlayer(sp) makes m_localPlayer / activeNetSlot() / the HP +
        // vignette writes resolve to lane sp; the single m_pendingDamage ring is keyed by the inbound
        // projectile (distinct per shot, each hitting one lane), so both lanes share it safely.
        // (Body indent left shallow to keep the diff readable.)
        for (u8 sp = 0; sp < m_splitPlayerCount; sp++) {
        swapInPlayer(sp);
        if (m_localPlayer.health > 0.0f) {
            static constexpr f32 PLAYER_HIT_RADIUS = 0.7f;
            // Player center at mid-body height so approaching ground-level projectiles register
            Vec3 playerCenter = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight * 0.5f, 0};
            for (u32 a = 0; a < m_renderInterp.projectiles.activeCount; a++) {
                u32 idx = m_renderInterp.projectiles.activeList[a];
                const Projectile& proj = m_renderInterp.projectiles.projectiles[idx];
                if (!proj.active) continue;
                // Only enemy-owned projectiles can damage us; skip our own
                if (proj.ownerSlot == activeNetSlot()) continue;
                // Build a stable key: high byte = owner slot, low 24 bits = projectile clientTick
                u32 key = (static_cast<u32>(proj.ownerSlot) << 24) | (proj.clientTick & 0xFFFFFFu);
                // Skip if we already predicted this exact projectile (avoid per-frame re-flash)
                bool alreadyPredicted = false;
                for (u32 i = 0; i < m_pendingDamage.count; i++) {
                    if (m_pendingDamage.entries[i].projectileSrcKey == key) {
                        alreadyPredicted = true; break;
                    }
                }
                if (alreadyPredicted) continue;
                Vec3 toPlayer = playerCenter - proj.position;
                f32 distSq = lengthSq(toPlayer);
                if (distSq >= (PLAYER_HIT_RADIUS * PLAYER_HIT_RADIUS)) continue;
                // Confirm the projectile is actually moving toward us before predicting impact
                // (avoids false positives from just-spawned or receding projectiles).
                Vec3 nextStep = proj.velocity * dt;
                f32 approachSpeed = -dot(normalize(toPlayer), nextStep);
                if (approachSpeed <= 0.0f) continue;
                // Predicted hit: trigger the same visual feedback path that a real hit would
                // (damageFlashTimer first-tick fires the hit sound + camera shake + rumble
                //  in tickVisualFeedback). Set hurtVignette for a brief red edge flash.
                m_localPlayer.damageFlashTimer = 0.15f;
                m_localPlayer.hurtVignette = fmaxf(m_localPlayer.hurtVignette, 0.4f);
                // D3.2 — Predicted HP decrement. proj.damage carries the server's authoritative
                // value decoded from expectedDamageQ (D3.1). No defence/armor reduction applied
                // here — worst-case mispredict (missed shot, shield block) is ~5–30 hp low for
                // ≤ 80 ms before the next snapshot reconciles upward via Client::reconcile.
                f32 predictedDmg = proj.damage;
                m_localPlayer.health -= predictedDmg;
                if (m_localPlayer.health < 0.0f) m_localPlayer.health = 0.0f;
                PendingDamageRingOps::record(m_pendingDamage, m_clientTick, key);
            }
        }
        swapOutPlayer(sp);
        } // end per-lane incoming-damage loop
        swapInPlayer(0); // restore lane-0 aliases
    }
}

// ---------------------------------------------------------------------------
// Singleplayer update (unchanged from Phase 3)
// ---------------------------------------------------------------------------
void Engine::gameUpdate(f32 dt) {
    // (The floor/total play-time clocks tick once per sim tick in update()'s IN_GAME case —
    // NOT here: gameUpdate runs once per local player, which double-counted in split-screen.)

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
    // Reset the near-death lifesaver grace once the player has recovered to a safe HP (>85%): the
    // grace is an emergency low-HP i-frame, so a regen/heal build shouldn't keep riding it while
    // already healthy. graceInvuln tags ONLY the lifesaver-sourced invuln (combat.cpp), so dodge /
    // spawn / skill i-frames are untouched. Authoritative side only — a CLIENT adopts invuln from
    // the snapshot (the host clears it server-side and it decays down via reconcile). Also drop the
    // tag on natural expiry so it can never leak onto a subsequent dodge's i-frame.
    if (m_localPlayer.graceInvuln) {
        if (m_localPlayer.invulnTimer <= 0.0f) {
            m_localPlayer.graceInvuln = false;
        } else if (m_netRole != NetRole::CLIENT &&
                   m_localPlayer.health > m_localPlayer.maxHealth * 0.85f) {
            m_localPlayer.invulnTimer = 0.0f;
            m_localPlayer.graceInvuln = false;
        }
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
        // CLIENT: death is server-authoritative — m_playerDead is driven from the snapshot's
        // isDead in clientNetPre. Don't set m_playerDead or trigger GAME_OVER from local HP here
        // (that fought reconcile and caused a death-loop/flicker); just stop this frame's
        // gameplay and let the authoritative snapshot drive the death overlay/respawn.
        if (m_netRole == NetRole::CLIENT) return;
        // Clear the damage vignette the instant the player dies so no red lingers
        // into the game-over / respawn screen. (The low-HP vignette term is already
        // 0 at 0 HP, and 0 again once respawn restores full HP, so this closes the
        // whole death->respawn window.)
        m_localPlayer.hurtVignette = 0.0f;
        // Reset Wanderer transient state on death so timers/marks don't persist to respawn
        if (m_playerClass == PlayerClass::WANDERER) {
            m_localPlayer.dodgeState      = {};
            m_localPlayer.deflectTimer    = 0.0f;
            m_localPlayer.markTimer       = 0.0f;
            m_localPlayer.deathsDanceTimer = 0.0f;
        }
        // "Butchered" achievement: the killing blow came from The Butcher (floor-5 boss).
        // Reads the never-cleared killing-blow tracker (combat.cpp) and matches by boss
        // nameTag, which only the authoritative sim has — so this fires for SP/host/split
        // lanes; a network GUEST's machine can't know its killer (damage arrives as HP
        // adoption with no attacker identity on the wire) and self-skips via 0xFFFF.
        if (m_localPlayer.lastAttackerEntity < MAX_ENTITIES) {
            const Entity& killer = m_entities.entities[m_localPlayer.lastAttackerEntity];
            if (killer.nameTag && std::strstr(killer.nameTag, "Butcher"))
                Steam::unlockAchievement("ACH_BUTCHERED");
        }
        if (m_splitPlayerCount > 1 || m_netRole != NetRole::NONE) {
            // Multiplayer or co-op: this player dies, game keeps running
            m_playerDead[m_localPlayerIndex] = true;
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
        // Free the cursor so the death-screen options are clickable (the screen is a full-screen
        // takeover; re-captured on respawn/reload). Mirrors the menu's setRelativeMouseMode(false).
        Input::setRelativeMouseMode(false);
        m_deathHover = -1;
        AudioSystem::play(SfxId::PLAYER_DEATH);
        return;
    }

    PROFILE_SCOPE(0, "Update");

    handleDebugKeys();

    // ---- Quickbar slot selection ----
    // KB/M: the mouse wheel cycles the active slot (there are no number keys — 1-4 are the class
    // skills) and middle-click equips it (updateTargetLock). The wheel is a GLOBAL device while
    // gameUpdate runs once per split-screen lane, so it must be gated to lane 0 — exactly as
    // player.cpp gates the mouse look delta — or P1 scrolling would move P2's active slot too.
    s32 wheel = (Input::getActivePlayer() == 0) ? Input::getMouseWheelDelta() : 0;
    if (wheel != 0) {
        s32 slot = static_cast<s32>(m_quickbars[m_localPlayerIndex].activeSlot);
        slot -= wheel; // scroll up = previous slot, down = next
        if (slot < 0) slot = QUICKBAR_SLOTS - 1;
        if (slot >= static_cast<s32>(QUICKBAR_SLOTS)) slot = 0;
        m_quickbars[m_localPlayerIndex].activeSlot = static_cast<u8>(slot);
    }

    // Gamepad: L + D-pad picks a slot DIRECTLY (one direction per slot, same Up/Right/Down/Left
    // order as the bare-D-pad class skills) and equips it in the same press. Cycling with a
    // separate use button used to be the plan, but a pad has only four D-pad directions and the
    // bar has exactly four slots — so select-and-equip is both the natural mapping and the reason
    // no gamepad "use" button is needed. Before this the pad could only CYCLE the bar and had no
    // way to use it at all, which made the quickbar dead on the entire Switch build.
    static constexpr GameAction kQuickbarSlots[QUICKBAR_SLOTS] = {
        GameAction::QUICKBAR_SLOT_1, GameAction::QUICKBAR_SLOT_2,
        GameAction::QUICKBAR_SLOT_3, GameAction::QUICKBAR_SLOT_4,
    };
    for (u8 i = 0; i < QUICKBAR_SLOTS; i++) {
        if (!Input::isActionPressed(kQuickbarSlots[i])) continue;
        m_quickbars[m_localPlayerIndex].activeSlot = i;
        useQuickbarSlot(i);
        break;  // one slot per frame — a chord can't legitimately claim two directions at once
    }

    // Healing potion (Q key) — restores 60% HP + 30% energy
    // R17 — tick-based gate using m_potionLastActivationTick. Both client and server
    // evaluate `(currentTick - lastActivationTick) >= cooldownTicks` with currentTick
    // = client's m_clientTick locally, input->clientTick server-side. Divergence-free
    // by construction. m_potionCooldown is HUD-derived below.
    const f32 potionCdr  = m_inventories[m_localPlayerIndex].bonusCooldownReduction * 0.1f;
    const u32 potionCdTicks = static_cast<u32>(GameConst::POTION_COOLDOWN * (1.0f - potionCdr) * 60.0f + 0.5f);
    const u32 nowTickPotion = currentLocalTick();
    // Lenient gate (shared with the server) so the local heal predicts even if a snapshot
    // nudged m_potionLastActivationTick forward by a tick — feel over exactness.
    const bool potionReady  = GameConst::cooldownReady(nowTickPotion, m_potionLastActivationTick, potionCdTicks);
    if (Input::isActionPressed(GameAction::POTION) && potionReady) {
        f32 healAmount = m_localPlayer.maxHealth * GameConst::POTION_HEAL_PCT;
        m_localPlayer.health += healAmount;
        if (m_localPlayer.health > m_localPlayer.maxHealth)
            m_localPlayer.health = m_localPlayer.maxHealth;
        SkillState& ss = m_skillStates[m_localPlayerIndex];
        f32 energyAmt = ss.maxEnergy * GameConst::POTION_ENERGY_PCT;
        ss.energy += energyAmt;
        if (ss.energy > ss.maxEnergy) ss.energy = ss.maxEnergy;
        m_potionLastActivationTick = (nowTickPotion == 0) ? 1u : nowTickPotion;
        AudioSystem::play(SfxId::POTION_USE);
        LOG_INFO("Used potion: +%.0f HP, +%.0f EN", healAmount, energyAmt);
    }
    // HUD: derive m_potionCooldown from authoritative tick state.
    {
        const u32 e = nowTickPotion - m_potionLastActivationTick;
        m_potionCooldown = (m_potionLastActivationTick != 0 && e < potionCdTicks)
                           ? static_cast<f32>(potionCdTicks - e) * (1.0f / 60.0f)
                           : 0.0f;
    }

    // Player movement/aiming — disabled while a blocking UI is open (inventory or, in MP, the
    // pause menu). gameplayInputFrozen() generalizes the old !m_inventoryOpen so a paused MP
    // player stands still like an inventory-open one (dodge activation lives in
    // PlayerController::update below, so it's frozen too).
    if (!gameplayInputFrozen()) {
        // Speed modifiers: blocking slows, buffs speed up
        f32 savedSpeed = m_localPlayer.moveSpeed;
        if (m_localPlayer.blocking) m_localPlayer.moveSpeed *= 0.4f;
        if (m_localPlayer.shadowDanceTimer > 0.0f) m_localPlayer.moveSpeed *= 1.2f;
        // Shrine of Speed. Now gated on the timer too — the old line checked only the buff id, and
        // since nothing ever cleared it (there was no duration field at all), a buff would have
        // lasted the rest of the run.
        if (m_localPlayer.shrineBuff == ShrineBuff::SPEED && m_localPlayer.shrineBuffTimer > 0.0f)
            m_localPlayer.moveSpeed *= (1.0f + m_localPlayer.shrineBuffValue);
        if (m_localPlayer.overdriveTimer > 0.0f) m_localPlayer.moveSpeed *= 1.3f;
        PlayerController::update(m_localPlayer, dt);
        if (m_localPlayer.dodgeState.rolling) m_dodgeRolledOnce = true;
        m_localPlayer.moveSpeed = savedSpeed;
        if (!m_localPlayer.noclip) {
            // N4 fix: on CLIENT, source obstacles from the AUTHORITATIVE interpolated pool
            // (m_renderInterp.entities) instead of the local ghost sim (m_entities). The
            // ghost diverges from the host — host-side kills stay alive in the ghost; ghost
            // entities the host already killed/never had still block movement — and used to
            // produce "I get blocked by invisible enemies" on the client. The render path
            // already uses the same (CLIENT ? m_renderInterp.entities : m_entities) switch
            // (engine_render_entities.cpp:66) and Client::interpolateEntities populates
            // flags/enemyType/halfExtents so these filters still apply.
            const EntityPool& obsPool = (m_netRole == NetRole::CLIENT)
                                        ? m_renderInterp.entities
                                        : m_entities;
            auto* obstacles = static_cast<CollisionObstacle*>(
                s_frameAllocator.alloc(MAX_ENTITIES * sizeof(CollisionObstacle)));
            u32 obsCount = 0;
            for (u32 a = 0; a < obsPool.activeCount; a++) {
                const Entity& e = obsPool.entities[obsPool.activeList[a]];
                if (e.flags & ENT_DEAD) continue;
                if (e.flags & ENT_FRIENDLY) continue;
                if (e.enemyType == EnemyType::PROP) continue;
                if (e.flags & ENT_BURROWED) continue;   // underground — walk right over it
                obstacles[obsCount++] = {e.position, e.halfExtents};
            }
            Collision::moveAndSlide(m_localPlayer, m_level.grid, dt, obstacles, obsCount);
        }
    }

    // Sync to NetPlayer for consistent rendering
    syncLocalPlayerToNetPlayer();

    // Target lock and weapon fire — disabled while a blocking UI is open (inventory / pause menu).
    if (!gameplayInputFrozen()) {
        updateTargetLock(dt);
        handleWeaponFire(dt);
    }

    // Update viewmodel animation timers — faithful Doom weapon bob algorithm
    // Doom: bob = (momx² + momy²) >> 2, clamped to MAXBOB.
    // Weapon swings at FINEANGLES/70 per tic (period = 2s at 35Hz).
    // View bobs at FINEANGLES/20 per tic (period = 0.571s at 35Hz).
    {
        // Bob amplitude from speed squared (Doom: momentum² / 4, capped)
        // Freeze bob while a blocking UI is open (inventory / pause menu) so the view stays still
        f32 vx = gameplayInputFrozen() ? 0.0f : m_localPlayer.velocity.x;
        f32 vz = gameplayInputFrozen() ? 0.0f : m_localPlayer.velocity.z;
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

    // Shared world systems (AI, projectiles, entity timers, world items, shared FX, meteors,
    // particles, enemy speech/chat decay) moved to Engine::tickSharedSystems, which runs ONCE
    // per frame after the per-player loop (M3) — they no longer ride the first player's pass.

    // Per-local-player FX/buff decay + skill cooldowns (run once per swap).
    tickPlayerFX(dt);
    tickSkillCooldowns(dt);

    tickPassiveEquipment();

    // eyePos is computed here (after view-bob has updated eyeHeight) and threaded
    // into both skill-activation helpers that need it.
    Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
    SkillSystem::setCastingPlayer(activeNetSlot()); // local player's net slot — keeps client-side Marksman overcharge prediction targeting the right slot
    handleClassSkillActivation(dt, eyePos);
    handleEquipmentSkillActivation(dt, eyePos);

    // --- Shield blocking (Ctrl / Left Trigger) ---
    {
        bool wantsBlock = Input::isActionDown(GameAction::BLOCK) && !gameplayInputFrozen();
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

    updatePlayerPickup(dt);

    // updateFloorDoor returns true when the player descends — skip remainder of tick
    if (updateFloorDoor()) return;

    // The Source (secret superboss): spawn the hidden portal once floor 50 is cleared with the
    // full shard set, and enter when the player steps into it. Returns true on entry (world rebuilt).
    if (updateSourcePortal()) return;

    // Exit portal (post-Engine): entering starts the shared credits sequence — skip the rest of
    // this player's tick exactly like a floor descent (the world is about to stop mattering).
    if (updateExitPortal()) return;

    // Town portal: opens the Free-Play select over the (kept-alive) town world.
    if (updateTownPortal()) return;

    // Toggle inventory (Tab key). Co-op decision (L3): opening an inventory does NOT pause
    // the game — the other player and all enemies keep running. This is intentional couch
    // co-op behavior (a shared session can't freeze for one player); inventory is simply not
    // a "safe" moment in split-screen. Mouse capture is toggled only for P0, the mouse owner
    // — otherwise P2 (controller-only) opening their inventory would release P1's mouse-look.
    if (Input::isActionPressed(GameAction::INVENTORY)) {
        m_inventoryOpen = !m_inventoryOpen;
        if (m_localPlayerIndex == 0) Input::setRelativeMouseMode(!m_inventoryOpen);
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
    // Stash rides the inventory screen: whichever path closed the inventory (ESC, B, Tab,
    // respawn, drop-all) also closes the stash — and closing flushes stash.dat (atomic,
    // no-op when nothing changed). ONE reconciler instead of a hook in every close path.
    if (m_stashOpen && !m_inventoryOpen) {
        m_stashOpen = false;
        saveStash();
    }

    // Toggle character inspect screen (C / LB+Plus). Freezes gameplay input via
    // gameplayInputFrozen(). Unlike the inventory (which frees the cursor for slot
    // clicks), the inspect screen has no clickable UI and instead uses mouse-drag to
    // rotate the model, so it KEEPS the cursor captured (relative mode ON) while open.
    // Closing restores relative mode unless the inventory is open (which wants it free).
    if (Input::isActionPressed(GameAction::CHARACTER_SCREEN)) {
        m_characterScreenOpen = !m_characterScreenOpen;
        if (m_localPlayerIndex == 0)
            Input::setRelativeMouseMode(m_characterScreenOpen || !m_inventoryOpen);
        AudioSystem::play(SfxId::UI_CONFIRM);
    }
    if (Input::isActionPressed(GameAction::MENU_BACK) && m_characterScreenOpen) {
        m_characterScreenOpen = false;
        if (m_localPlayerIndex == 0 && !m_inventoryOpen)
            Input::setRelativeMouseMode(true);
    }
    if (m_characterScreenOpen) {
        // Rotate the inspect model from mouse-X drag (relative-mode delta) + right-stick X.
        // The gentle idle auto-spin only applies when the player ISN'T actively rotating,
        // so manual input always wins and the model stops where you leave it.
        s32 mdx = 0, mdy = 0;
        Input::getMouseDelta(mdx, mdy);
        f32 stick  = Input::getStickX(true);                          // right-stick X (deadzone applied)
        f32 manual = static_cast<f32>(mdx) * 0.01f + stick * 0.04f;
        m_inspectYaw += manual;
        if (mdx == 0 && stick == 0.0f) m_inspectYaw += 0.0025f;       // idle showcase spin only
    }

    updateInventoryInteraction(dt);

    // Debug: F7 gives random item
    if (Input::isKeyPressed(SDL_SCANCODE_F7)) {
        ItemInstance item = ItemGen::rollItem(1, m_itemDefs, m_itemDefCount,
                                              m_affixDefs, m_affixDefCount);
        if (!isItemEmpty(item)) {
            if (Inventory::addToBackpack(m_inventories[m_localPlayerIndex], item) >= 0) {
                LOG_INFO("Debug: gave %s (rarity %u, damage %.1f)",
                         m_itemDefs[item.defId].name, (u32)item.rarity, item.damage);
            }
        }
    }

    tickVisualFeedback(dt);
    tickMiscTimers(dt);

    pushPlayerFromEntities();

    // Update fog-of-war — on CLIENT use the authoritative interp pool so visible-enemy
    // markers track snapshot positions rather than the frozen ghost sim (N4).
    Minimap::updateVisited(m_level.grid, m_localPlayer.position,
                           (m_netRole == NetRole::CLIENT) ? m_renderInterp.entities
                                                          : m_entities);

    syncLocalPlayerToNetPlayer();
}

// ---------------------------------------------------------------------------
// gameUpdate sub-functions
// ---------------------------------------------------------------------------

// Auto-pickup health/energy globes (walk-over) and E-key item pickup.
// The Engine's lore, leaked one fragment per source shard. Indexed by floor/5-1 (floor 5→[0] …
// floor 50→[9]). Kept ≤47 chars to fit a ChatLine. The arc reveals that the player is themselves
// an iteration of the curse — see ~/.claude/plans (the-dungeon-engine).
static const char* kShardLore[10] = {
    "Draft 001. It only knew how to cut.",            // 5  Butcher
    "I taught the brood to multiply.",                // 10 Ygara
    "Death? Merely a feature I shipped.",             // 15 Sethrak
    "He begged not to be recompiled.",                // 20 Malachar
    "Every cavern, every child: my syntax.",          // 25 Ixara
    "I raise walls so you will break them.",          // 30 Korvath
    "The blade was mine before his.",                 // 35 Azhar
    "You have fought this one before.",               // 40 DiaBRO
    "The Void is only my empty memory.",              // 45 Nyx
    "The Reaper was my first draft. You are my last.",// 50 Reaper
};

// Record a collected source shard and whisper its lore line. Idempotent per floor.
void Engine::collectSourceShard(const ItemInstance& shard) {
    if (shard.itemLevel < 5 || shard.itemLevel > 50 || shard.itemLevel % 5 != 0) return;
    u8 bit = static_cast<u8>(shard.itemLevel / 5 - 1);
    if (s_sourceShards & (1u << bit)) return;  // already held this session
    s_sourceShards |= (1u << bit);
    AudioSystem::play(SfxId::ITEM_PICKUP);     // a soft chime; same cue as item pickup
    addChatMessage("\?\?\?", kShardLore[bit], Vec3{0.62f, 0.30f, 0.95f}); // void-violet whisper
    LOG_INFO("Source shard collected (floor %u); set = 0x%03X", shard.itemLevel, s_sourceShards);
}

// Globes are consumed immediately; regular items go to the backpack.
// ---------------------------------------------------------------------------
// resolveInteractTargets — the ONE answer to "what is this player pointing at?"
//
// Every consumer (host pickup, client pickup request, the on-screen prompt, the exit door) reads
// this instead of running its own scan. They used to run four, and the four had drifted.
// ---------------------------------------------------------------------------

// Open the account stash: it rides the inventory screen (backpack beside the stash panel).
// Purely local — contents live in this machine's stash.dat; co-op inventory changes flow
// through the ordinary CL_INVENTORY_SYNC that every transfer sends.
void Engine::openStashUI() {
    m_stashOpen     = true;
    m_stash.page    = 0;
    m_inventoryOpen = true;
    m_inventoryOpenArr[m_localPlayerIndex] = true;
    Input::setRelativeMouseMode(false);   // free the cursor for slot clicks, like the inventory
    m_dragState = {};
    m_dblClickState = {};
}

void Engine::resolveInteractTargets(InteractState& st) {
    st.itemIdx = -1;
    st.shrineIdx = -1;
    st.mimicIdx = -1;
    st.chestIdx = -1;
    st.stashIdx = -1;
    st.nearTownPortal = false;
    st.nearExit = false;
    if (m_inventoryOpen) return;

    const Vec3 fwd  = m_localPlayer.forward;
    const f32  hLen = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
    f32 bestItem = -1.0f, bestShrine = -1.0f, bestChest = -1.0f, bestStash = -1.0f;

    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        const WorldItem& w = m_worldItems.items[i];
        if (!w.active) continue;
        if (isGlobe(w.item) || isSourceShard(w.item)) continue;   // walk-over pickups, never aimed

        const bool shrine = isShrine(w.item);
        const bool chest  = isChest(w.item);
        const bool stash  = isStash(w.item);   // the town's account-stash chest (fixture)
        // The defId bound check must NOT be applied to a shrine or a chest: their sentinel
        // defIds (0xFFFB…/0xFFF8) sit far outside the real item range, so this exact line —
        // copied into the client's scan — is what made shrines impossible to activate as a
        // guest. A chest skipped here would be un-openable the same silent way.
        if (!shrine && !chest && !stash && w.item.defId >= m_itemDefCount) continue;
        // Loot-ownership window: another player's kill is theirs for 3 s. Fixtures are never owned.
        if (!shrine && !chest && !stash && w.ownerSlot != 0xFF && w.ownerSlot != activeNetSlot() && w.exclusiveTimer > 0.0f)
            continue;

        Vec3 to = w.position - m_localPlayer.position;
        f32 hDist = sqrtf(to.x * to.x + to.z * to.z);
        // Horizontal-only dot so floor items stay reachable while looking down at them.
        f32 dot = (hDist > 0.01f && hLen > 0.01f)
                ? (fwd.x * to.x + fwd.z * to.z) / (hDist * hLen)
                : 1.0f;   // exactly underfoot — no meaningful direction to compare against
        // Interact::inReach owns the rule (and the reason): the aim cone applies only BEYOND
        // INTERACT_GRAB_RADIUS, so the item at your feet can never be refused for the way you face.
        if (!Interact::inReach(hDist, dot, GameConst::INTERACT_RANGE,
                               GameConst::INTERACT_GRAB_RADIUS, GameConst::INTERACT_MIN_DOT))
            continue;

        f32 score = dot - hDist * 0.1f;   // prefer what you're looking straight at, then what's near
        if (shrine) {
            if (score > bestShrine) { bestShrine = score; st.shrineIdx = static_cast<s32>(i); }
        } else if (stash) {
            if (score > bestStash) { bestStash = score; st.stashIdx = static_cast<s32>(i); }
        } else if (chest) {
            if (score > bestChest) { bestChest = score; st.chestIdx = static_cast<s32>(i); }
        } else {
            if (w.item.rarity == Rarity::LEGENDARY) score += 0.5f;   // legendaries win ties
            if (score > bestItem) { bestItem = score; st.itemIdx = static_cast<s32>(i); }
        }
    }

    // Dormant mimic "chests" are E-interactable exactly like loot — opening one IS the trap
    // firing. Scanned with the SAME reach rule as items so the prompt and the button can
    // never disagree. Runs identically on the CLIENT: enemyType and aiState are replicated,
    // and the guest's entity mirror is written in-place at the SERVER's pool index, so the
    // index found here is the very name the server validates against. The full-pool scan
    // (not activeList) is deliberate — the client mirror doesn't maintain activeList.
    f32 bestMimic = -1.0f;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& ent = m_entities.entities[i];
        if (!(ent.flags & ENT_ACTIVE) || (ent.flags & ENT_DEAD)) continue;
        if (ent.enemyType != EnemyType::MIMIC || ent.aiState != AIState::DORMANT) continue;

        Vec3 to = ent.position - m_localPlayer.position;
        f32 hDist = sqrtf(to.x * to.x + to.z * to.z);
        f32 mdot = (hDist > 0.01f && hLen > 0.01f)
                 ? (fwd.x * to.x + fwd.z * to.z) / (hDist * hLen)
                 : 1.0f;
        if (!Interact::inReach(hDist, mdot, GameConst::INTERACT_RANGE,
                               GameConst::INTERACT_GRAB_RADIUS, GameConst::INTERACT_MIN_DOT))
            continue;

        f32 score = mdot - hDist * 0.1f;
        if (score > bestMimic) { bestMimic = score; st.mimicIdx = static_cast<s32>(i); }
    }

    st.nearExit = m_level.floorDoorActive &&
                  lengthSq(m_level.floorDoorPos - m_localPlayer.position) < 4.0f;
    st.nearPortal = m_level.sourcePortalActive &&
                    lengthSq(m_level.sourcePortalPos - m_localPlayer.position) < 4.0f;
    st.nearExitPortal = m_level.exitPortalActive &&
                        lengthSq(m_level.exitPortalPos - m_localPlayer.position) < 4.0f;
    st.nearTownPortal = m_level.townPortalActive &&
                        lengthSq(m_level.townPortalPos - m_localPlayer.position) < 4.0f;
}

void Engine::updatePlayerPickup(f32 dt) {
    m_descendRequested = false;   // updateFloorDoor consumes this later in the same tick
    m_portalRequested  = false;   // ...and updateSourcePortal right after it
    m_creditsRequested = false;   // ...and updateExitPortal after that
    m_townPortalRequested = false; // ...and updateTownPortal last

    InteractState& st = m_interact[m_localPlayerIndex];
    resolveInteractTargets(st);

    // The button rule itself is pure and lives in game/interact.h (and is unit-tested there): an
    // item always wins a tap; a hold reaches past it to the shrine, then the exit.
    // The exit and The Source portal are one priority class: both are "leave the floor", both are
    // outranked by loot, and both are reachable by holding.
    const bool hasExitTarget = st.nearExit || st.nearPortal || st.nearExitPortal || st.nearTownPortal;
    const bool hasHoldTarget = (st.shrineIdx >= 0) || hasExitTarget;
    // Chests — real (world-item sentinel) or fake (dormant mimic entity) — compete in the
    // ITEM class of the tap rule: opening one is a tap, exactly like grabbing loot. Real
    // loot still outranks both below (a chest must never steal the grab aimed at an item
    // lying beside it).
    const bool hasItemClass  = (st.itemIdx >= 0) || (st.chestIdx >= 0) || (st.mimicIdx >= 0) ||
                               (st.stashIdx >= 0);
    const bool down = !m_inventoryOpen && Input::isActionDown(GameAction::PICKUP);
    const Interact::Intent intent =
        Interact::poll(st.hold, down, hasHoldTarget, dt, GameConst::INTERACT_HOLD_SEC);
    const Interact::Target target =
        Interact::choose(intent, hasItemClass, st.shrineIdx >= 0, hasExitTarget);

    const bool wantItem   = (target == Interact::Target::ITEM);
    const bool wantShrine = (target == Interact::Target::SHRINE);
    if (target == Interact::Target::EXIT) {
        // The portals win a tie: each only exists where you deliberately went looking for it
        // (and the exit portal never coexists with a floor door — the chamber has none).
        if (st.nearExitPortal)  m_creditsRequested = true;
        else if (st.nearTownPortal) m_townPortalRequested = true;
        else if (st.nearPortal) m_portalRequested = true;
        else                    m_descendRequested = true;   // updateFloorDoor owns the boss gate + net path
    }

    // CLIENT: pickups are SERVER-AUTHORITATIVE (N5). The client never consumes globes or
    // removes world items locally — instead it requests a pickup of the aimed item via
    // CL_PICKUP_ITEM, and the server validates proximity/ownership, applies the effect, and
    // removes the item (which propagates back via the next snapshot). Globe auto-pickup for
    // the client is handled server-side in serverNetPost (the client is a "remote" slot there).
    if (m_netRole == NetRole::CLIENT) {
        if (wantItem) {
            if (st.itemIdx >= 0)       sendPickupRequest(st.itemIdx);
            else if (st.stashIdx >= 0) openStashUI();                   // stash is local-only UI
            else if (st.chestIdx >= 0) sendPickupRequest(st.chestIdx);  // real chest → server opens it
            else if (st.mimicIdx >= 0) sendMimicInteract(st.mimicIdx);  // "opened" a chest → server springs it
        }
        else if (wantShrine) sendPickupRequest(st.shrineIdx);   // same packet; server grants the buff
        return;
    }

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

    // Auto-pickup source shards (secret superboss key) — walk-over, like globes. Host/SP only;
    // remote lanes are handled server-side in serverNetPost (mirrors the globe split above).
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = m_worldItems.items[i];
        if (!wi.active || !isSourceShard(wi.item)) continue;
        Vec3 delta = m_localPlayer.position - wi.position;
        f32 dist = sqrtf(delta.x * delta.x + delta.z * delta.z);
        if (dist < 3.0f) {
            collectSourceShard(wi.item);
            wi.active = false;
            if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
        }
    }

    // Shrine: activate rather than pick up — it is a fixture, not loot, and must never reach the
    // backpack. Reached on a HOLD, or on a tap when there is no item competing for the button.
    if (wantShrine) {
        const u8 buff = Shrine::buffOf(m_worldItems.items[st.shrineIdx].item);
        grantShrineBuff(m_localPlayer, buff);
        m_worldItems.items[st.shrineIdx].active = false;
        if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
        AudioSystem::play(SfxId::SHRINE_ACTIVATE);
        addChatMessage("", Shrine::nameOf(buff), Vec3{0.6f, 0.9f, 1.0f});
        return;
    }

    // Real chest (SP/host lanes; a guest's request lands in handlePickupRequest instead).
    // Only reached when no real item won the tap.
    if (wantItem && st.itemIdx < 0 && st.stashIdx >= 0) {
        openStashUI();
        return;
    }
    if (wantItem && st.itemIdx < 0 && st.chestIdx >= 0) {
        openChest(static_cast<u32>(st.chestIdx));
        return;
    }

    // Mimic "chest" (SP/host lanes; a guest's request lands in onEntityInteract instead).
    // Only reached when no real item won the tap — opening it IS the trap firing.
    if (wantItem && st.itemIdx < 0 && st.mimicIdx >= 0) {
        Entity& mim = m_entities.entities[st.mimicIdx];
        if ((mim.flags & ENT_ACTIVE) && !(mim.flags & ENT_DEAD) &&
            mim.enemyType == EnemyType::MIMIC && mim.aiState == AIState::DORMANT) {
            EnemyAI::wakeAmbusher(mim);
        }
        return;
    }

    // Item pickup (E / X) — the aimed item resolved above.
    if (wantItem) {
        const s32 bestIdx = st.itemIdx;
        ItemInstance picked = m_worldItems.items[bestIdx].item;
        m_worldItems.items[bestIdx].active = false;
        if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
        if (!isItemEmpty(picked)) {
            s8 bpSlot = Inventory::addToBackpack(m_inventories[m_localPlayerIndex], picked);
            if (bpSlot >= 0) {
                AudioSystem::play(SfxId::ITEM_PICKUP);
                // First WORLD pickup (SP/host lane; a guest's fires in onPickupResult).
                // Starting-loadout grants and the F7 debug give deliberately don't count.
                Steam::unlockAchievement("ACH_FIRST_ITEM");
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

// CLIENT-only (N5): pick the world item the local player is aiming at (same aim logic as
// the host's updatePlayerPickup) and send its uid to the server as CL_PICKUP_ITEM.
//
// M8/D4 prediction: we predict BOTH the world-item disappearance AND the inventory add here
// so the player sees the change immediately without waiting ~RTT/2 for the server response.
// On SV_PICKUP_RESULT accept: clear the predicted flag (item is real).
// On SV_PICKUP_RESULT reject: removeFromBackpack rolls back the add; mirrorWorldItems restores
// the world item from the next authoritative snapshot.
// If backpack is already full we skip the send entirely — no point requesting a pickup the
// server would reject (and the server would reject it), so we just show the "backpack full"
// notify without sending CL_PICKUP_ITEM.
void Engine::sendPickupRequest(s32 worldIdx) {
    if (worldIdx < 0 || worldIdx >= static_cast<s32>(MAX_WORLD_ITEMS)) return;
    const WorldItem& target = m_worldItems.items[worldIdx];
    if (!target.active) return;

    const u32 uid = target.item.uid;

    // A SHRINE or a CHEST is requested through the same packet, but predicts NOTHING. Neither
    // grants a backpack slot: the shrine's buff lands on the authoritative NetPlayer and returns
    // through SnapPlayer, and the chest's loot is ROLLED server-side at open time (we cannot
    // predict an item we haven't rolled). (The shrine half of this branch replaced a scan that
    // rejected any defId past the real item range, which made shrines unreachable in co-op —
    // a chest would have hit the identical wall.)
    if (isShrine(target.item) || isChest(target.item)) {
        sendPickupPacket(uid);
        return;
    }

    // M8/D4: predict the inventory add before sending so the bag slot appears immediately.
    s8 predictedSlot = -1;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = m_worldItems.items[i];
        if (!wi.active || wi.item.uid != uid) continue;

        predictedSlot = Inventory::addToBackpack(m_inventories[m_localPlayerIndex], wi.item);
        if (predictedSlot < 0) {
            // Backpack full — don't even send the request; avoid a pointless round-trip
            // that the server would reject. Show the notify so the player knows why.
            m_fullBackpackNotifyTimer = 2.0f;
            return;
        }
        wi.active = false;  // hide world item locally (M8); restored by mirrorWorldItems on reject
        if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
        break;
    }
    // uid disappeared from local world before we could copy it (rare race: two clients grabbing
    // the same item in the same frame). Don't send an orphaned request.
    if (predictedSlot < 0) return;

    // Lane rides along so a rejected pickup rolls back THIS lane's backpack — onPickupResult
    // fires during Net::poll, when m_localPlayerIndex is whatever lane was swapped in last.
    PendingPickupRingOps::record(m_pendingPickups, m_clientTick, uid, predictedSlot,
                                 m_localPlayerIndex);
    sendPickupPacket(uid);
}

// The wire half of a pickup request, shared by the item path (which predicts first) and the shrine
// path (which predicts nothing). Packet: header(4) + uid(4) + targetSlot(1). Reliable, so a dropped
// request doesn't silently cost the player an item. The trailing byte (v18) names WHICH local lane
// is picking up — must be called with the acting lane swapped in (updatePlayerPickup's per-lane
// loop), because activeNetSlot() reads m_localPlayerIndex.
void Engine::sendPickupPacket(u32 uid) {
    u8 buf[sizeof(PacketHeader) + 5];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_PICKUP_ITEM;
    hdr->flags = 0;
    hdr->seq   = 0;
    std::memcpy(buf + sizeof(PacketHeader), &uid, 4);
    buf[sizeof(PacketHeader) + 4] = activeNetSlot();
    Net::sendToServer(buf, sizeof(buf), true);
}

// CLIENT: pet-consumable use. Reliable, like the pickup request above — a discrete UI click
// must not be lost to packet loss, and the payload names WHICH pet def to toggle (there is one
// per enemy now, so the old payload-less input bit could not carry the choice).
void Engine::sendUsePetPacket(u16 petDefId) {
    u8 buf[sizeof(PacketHeader) + 3];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_USE_PET;
    hdr->flags = 0;
    hdr->seq   = 0;
    std::memcpy(buf + sizeof(PacketHeader), &petDefId, 2);
    buf[sizeof(PacketHeader) + 2] = activeNetSlot();   // v18: which local lane is using it
    Net::sendToServer(buf, sizeof(buf), true);
}

// CLIENT: ask the server to spring the dormant mimic this lane just "opened" (E on the
// chest). Entities have no world-item uid, so the wire name is the u8 SERVER pool index —
// valid cross-machine because the client's entity mirror is written in-place at that same
// index. Reliable: losing the packet would leave a player who deliberately opened a chest
// staring at an inert prop. No prediction — the spring comes back as replicated aiState
// within a snapshot, and predicting a wake we don't own could disagree with a wake the
// weeping-angel rule already fired server-side.
void Engine::sendMimicInteract(s32 poolIdx) {
    if (poolIdx < 0 || poolIdx >= static_cast<s32>(MAX_ENTITIES)) return;
    u8 buf[sizeof(PacketHeader) + 2];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_INTERACT_ENTITY;
    hdr->flags = 0;
    hdr->seq   = 0;
    buf[sizeof(PacketHeader)]     = static_cast<u8>(poolIdx);
    buf[sizeof(PacketHeader) + 1] = activeNetSlot();   // v18: which local lane opened it
    Net::sendToServer(buf, sizeof(buf), true);
}

// R11 — CLIENT: tell the server about an inventory drop. Wire layout matches
// Engine::onDropItem's decode: header(4) + slotKind(1) + slotIndex(1) + dropPos(12) +
// ItemInstance(sizeof ItemInstance). Without this packet the client's local-only drop
// (Inventory::dropFromBackpack + WorldItemSystem::spawn) is silently wiped on the next
// mirrorWorldItems / inventory sync pass — the item disappears and the bag drifts from
// the server's view. Reliable so a dropped request doesn't lose the player an item.
void Engine::sendDropRequest(u8 slotKind, u8 slotIndex, const ItemInstance& it, Vec3 dropPos) {
    constexpr u32 kFixed = sizeof(PacketHeader) + 1 + 1 + 12;
    u8 buf[kFixed + sizeof(ItemInstance) + 1];   // +1: v18 trailing target slot
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_DROP_ITEM;
    hdr->flags = 0;
    hdr->seq   = 0;
    u8* p = buf + sizeof(PacketHeader);
    p[0] = slotKind;
    p[1] = slotIndex;
    std::memcpy(p + 2, &dropPos, 12);
    std::memcpy(p + 2 + 12, &it, sizeof(ItemInstance));
    // v18: trailing byte names WHICH local lane is dropping (CL_INVENTORY_SYNC's trailing-byte
    // form) so a couch P2's drop removes from P2's server-side inventory, not P1's.
    buf[kFixed + sizeof(ItemInstance)] = activeNetSlot();
    Net::sendToServer(buf, sizeof(buf), /*reliable=*/true);
}

// R11 — SERVER: handle a client's CL_DROP_ITEM. Zero the named inventory slot (so the
// server's view stops thinking the player has the item) and spawn a world item at
// dropPos. No validation — co-op trust model (mirrors onInventorySync).
void Engine::handleDropRequest(u8 playerSlot, u8 slotKind, u8 slotIndex,
                                const ItemInstance& it, Vec3 dropPos) {
    if (playerSlot >= MAX_PLAYERS) return;
    PlayerInventory& inv = m_inventories[playerSlot];
    // Hold a named default-constructed empty so the assignments below copy from a
    // real lvalue. The `ItemInstance{}` temporary trips a GCC 13 gimplifier ICE
    // (Ubuntu 24.04 CI: "internal compiler error: in gimple_add_tmp_var") when the
    // struct contains the brace-initialized `Affix affixes[MAX_AFFIXES_PER_ITEM] = {}`
    // default member init. The named-local form sidesteps the affected codegen path
    // and is semantically identical on every compiler.
    ItemInstance emptySlot;
    if (slotKind == 0) {
        // Backpack drop
        if (slotIndex >= MAX_INVENTORY_ITEMS) return;
        inv.backpack[slotIndex] = emptySlot;   // zero the slot
    } else if (slotKind == 1) {
        // Equipped-slot drop
        if (slotIndex >= static_cast<u32>(ItemSlot::COUNT)) return;
        inv.equipped[slotIndex] = emptySlot;
    } else {
        return; // unknown kind
    }
    // Spawn the world item carrying the client's rolled stats. The snapshot's
    // SnapWorldItem (R8/D8) propagates damage/affixes/etc. to all clients including
    // the requester so the dropped item is pickable again.
    WorldItemSystem::spawn(m_worldItems, it, dropPos);
}

// CLIENT: request respawn after death. A header-only reliable CL_RESPAWN packet — NOT routed
// through the input ring buffer (whose monotonic-tick guard silently dropped the old
// INPUT_EX_RESPAWN input when it shared a tick with that frame's regular input). The server
// respawns us authoritatively and the revival comes back in the next snapshot.
void Engine::sendRespawnRequest(u8 targetSlot) {
    u8 buf[sizeof(PacketHeader) + 1];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_RESPAWN;
    hdr->flags = 0;
    hdr->seq   = 0;
    // Online couch co-op: stamp which local player is respawning so the server revives the right
    // one of this peer's two slots (was header-only, which always respawned the peer's primary —
    // so the 2nd couch player could never respawn). The server validates peer ownership.
    buf[sizeof(PacketHeader)] = targetSlot;
    Net::sendToServer(buf, sizeof(buf), true);
}

// CLIENT: ask the host to trigger a floor descent at the portal. Header-only reliable
// CL_REQUEST_DESCEND (no payload — the slot identifies the requester on the server side).
// The server re-validates proximity + boss-dead and runs triggerFloorDescent(), which
// broadcasts SV_LEVEL_SEED so we (and every other client) transition in lockstep.
void Engine::sendDescendRequest() {
    u8 buf[sizeof(PacketHeader) + 1];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_REQUEST_DESCEND;
    hdr->flags = 0;
    hdr->seq   = 0;
    // v18: byte names WHICH local lane pressed E at the portal — the server checks door
    // proximity against the REQUESTER, and header-only always meant the peer's primary, so a
    // couch client's P2 standing at the door could never descend the party unless P1 was
    // also inside the 2 m radius.
    buf[sizeof(PacketHeader)] = activeNetSlot();
    Net::sendToServer(buf, sizeof(buf), true);
}

// SERVER-only: handle a remote client's CL_REQUEST_DESCEND. The requesting client could be a
// cheater or a stale request, so we re-check the same gates the host's local press would
// pass through: the floor door must be active, the requesting NetPlayer must be within the
// door radius, and (on boss floors) the boss must be dead. If all pass, run the shared
// descent flow. Boss-locked rejections are silent today (no client-side notify) — adding
// feedback is a follow-up SV_EVENT.
void Engine::handleDescendRequest(u8 playerSlot) {
    if (playerSlot >= MAX_PLAYERS) return;
    // Post-Engine exit portal: in the Source chamber there is no floor door, so a guest's
    // "leave" request resolves against the portal instead — same slot byte, same proximity
    // discipline. First one in rolls the credits for everyone (beginCreditsSequence is
    // idempotent once CREDITS/VICTORY is reached).
    if (m_level.exitPortalActive) {
        const NetPlayer& np = m_players[playerSlot];
        if (!np.active || np.isDead) return;
        if (lengthSq(m_level.exitPortalPos - np.position) >= 4.0f) return;
        LOG_INFO("CL_REQUEST_DESCEND from slot %u accepted — exit portal, rolling credits", playerSlot);
        beginCreditsSequence(true);   // the portal only exists after the Engine fell
        return;
    }
    if (!m_level.floorDoorActive) return;
    const NetPlayer& np = m_players[playerSlot];
    if (!np.active || np.isDead) return;
    Vec3 toDoor = m_level.floorDoorPos - np.position;
    if (lengthSq(toDoor) >= 4.0f) return; // outside the 2 m portal radius
    if (m_level.floorHasBoss && floorBossAlive()) return; // boss still gating exit
    LOG_INFO("CL_REQUEST_DESCEND from slot %u accepted — triggering descent", playerSlot);
    triggerFloorDescent();
}

// SERVER-only: respawn a dead client's NetPlayer in response to CL_RESPAWN. Idempotent (only
// acts on an active, dead slot), so a duplicate/late packet is harmless. The revived state
// (health, position, invuln, isDead=false) propagates to the client via the next snapshot.
void Engine::handleRespawnRequest(u8 playerSlot) {
    if (playerSlot >= MAX_PLAYERS) return;
    NetPlayer& np = m_players[playerSlot];
    if (!np.active || !np.isDead) return;
    np.health     = np.maxHealth;
    np.position   = np.spawnPosition;
    np.velocity   = {0, 0, 0};
    np.invulnTimer = 1.5f;
    np.isDead     = false;
    LOG_INFO("Player %u respawned (CL_RESPAWN)", playerSlot);
}

// D1.2 helper: send SV_PICKUP_RESULT to the requesting client (accept=1 or accept=0).
// 6-byte payload: u8 accept + u8 reserved + u32 uid.
static void sendPickupResult(u8 playerSlot, u8 accept, u32 uid) {
    u8 buf[sizeof(PacketHeader) + 6];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::SV_PICKUP_RESULT;
    hdr->flags = 0;
    hdr->seq   = 0;
    u32 off = sizeof(PacketHeader);
    buf[off++] = accept;
    buf[off++] = 0; // reserved
    std::memcpy(buf + off, &uid, 4); off += 4;
    Net::sendReliable(playerSlot, buf, off);
}

// SERVER-only (N5): apply a validated CL_PICKUP_ITEM request. Finds the world item by uid,
// checks the requesting player's proximity + ownership against AUTHORITATIVE state, then
// moves it into that player's inventory and frees the slot (propagates via the next
// snapshot). D1.2: always responds with SV_PICKUP_RESULT so the client can ack its
// pending-pickup ring and resolve the predicted disappearance.
void Engine::handlePickupRequest(u8 playerSlot, u32 uid) {
    if (playerSlot >= MAX_PLAYERS) return;
    NetPlayer& np = m_players[playerSlot];
    if (!np.active || np.isDead) { sendPickupResult(playerSlot, 0, uid); return; }

    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = m_worldItems.items[i];
        if (!wi.active || wi.item.uid != uid) continue;
        if (isGlobe(wi.item)) { sendPickupResult(playerSlot, 0, uid); return; } // globes not requestable

        // Shrine: a guest pressed E on it. Grant the buff onto the AUTHORITATIVE NetPlayer — this is
        // what makes a shrine-buffed guest actually move faster on the server too, instead of
        // predicting a speed the server never simulates and rubber-banding every tick. The buff then
        // reaches them through the snapshot.
        if (isShrine(wi.item)) {
            Vec3 d = np.position - wi.position;
            if (sqrtf(d.x * d.x + d.z * d.z) > 3.5f) { sendPickupResult(playerSlot, 0, uid); return; }
            const u8 buff = Shrine::buffOf(wi.item);
            grantShrineBuff(np, buff);
            wi.active = false;
            if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
            sendPickupResult(playerSlot, 1, uid);
            LOG_INFO("Shrine: slot %u activated %s", playerSlot, Shrine::nameOf(buff));
            return;
        }

        // Real chest: a guest opened it. Same reach rule as the shrine; the rolled loot
        // spawns server-side and reaches every player through the ordinary snapshot mirror
        // (the guest predicted nothing, so accept/reject only settles their request state).
        if (isChest(wi.item)) {
            Vec3 d = np.position - wi.position;
            if (sqrtf(d.x * d.x + d.z * d.z) > 3.5f) { sendPickupResult(playerSlot, 0, uid); return; }
            openChest(i);
            sendPickupResult(playerSlot, 1, uid);
            return;
        }

        // Proximity check against the authoritative net player position (XZ, matches aim path).
        Vec3 delta = np.position - wi.position;
        f32 hDist = sqrtf(delta.x * delta.x + delta.z * delta.z);
        if (hDist > 3.5f) { sendPickupResult(playerSlot, 0, uid); return; } // too far — reject

        // Ownership: free-for-all, owned by this player, or exclusive window expired.
        bool canPickup = (wi.ownerSlot == 0xFF)
                      || (wi.ownerSlot == playerSlot)
                      || (wi.exclusiveTimer <= 0.0f);
        if (!canPickup) { sendPickupResult(playerSlot, 0, uid); return; }

        ItemInstance picked = wi.item;
        if (Inventory::addToBackpack(m_inventories[playerSlot], picked) < 0) {
            sendPickupResult(playerSlot, 0, uid); return; // backpack full — reject
        }

        // Auto-equip an empty weapon slot (matches the host local-pickup convenience).
        if (picked.defId < m_itemDefCount &&
            m_itemDefs[picked.defId].slot == ItemSlot::WEAPON) {
            u8 bpIdx = 0xFF;
            for (u8 bi = 0; bi < MAX_INVENTORY_ITEMS; bi++) {
                if (m_inventories[playerSlot].backpack[bi].uid == picked.uid) { bpIdx = bi; break; }
            }
            if (bpIdx != 0xFF) {
                const ItemInstance& eqWpn = m_inventories[playerSlot].equipped[static_cast<u32>(ItemSlot::WEAPON)];
                if (isItemEmpty(eqWpn))
                    Inventory::equip(m_inventories[playerSlot], bpIdx, m_itemDefs);
            }
        }

        wi.active = false; // removal propagates to all clients via the next snapshot
        if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
        // D1.2: notify the requesting client that pickup was accepted.
        sendPickupResult(playerSlot, 1, uid);
        return;
    }
    // uid not found in world (already gone or never existed): reject so the client cleans up.
    sendPickupResult(playerSlot, 0, uid);
}

// True while a milestone boss is still alive on this floor. Used to lock the floor
// exit so the player must defeat the boss before descending. A boss mid-death-fade
// (ENT_DEAD set, deathTimer running) counts as dead. Malachar counts as alive through
// his false-death phases until the real kill.
//
// R9: on CLIENT the local m_entities pool is the prediction ghost (N4 gated client-side
// AI off, so it stops updating once the server takes authority). The authoritative
// post-snapshot world lives in m_renderInterp.entities, populated by Client::interpolate
// Entities. Entity.isBoss is mirrored from SnapEntity.bossStatus bit 4 (R9) so the
// client can identify the milestone boss there. Without this branch, the portal stayed
// the locked color forever — the ghost pool never saw the boss die.
bool Engine::floorBossAlive() const {
    if (m_netRole == NetRole::CLIENT) {
        // Walk the render pool's active list — interpolateEntities rebuilds it each
        // frame, one entry per SnapEntity received.
        for (u32 a = 0; a < m_renderInterp.entities.activeCount; a++) {
            const Entity& e = m_renderInterp.entities.entities[m_renderInterp.entities.activeList[a]];
            if (e.isBoss && !(e.flags & ENT_DEAD)) return true;
        }
        return false;
    }
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        const Entity& e = m_entities.entities[m_entities.activeList[a]];
        if (e.isBoss && !(e.flags & ENT_DEAD)) return true;
    }
    return false;
}

// Floor door interaction — descend to next floor when near and E is pressed.
// Returns true if the player descended (caller must return immediately to skip
// the rest of the tick with the now-regenerated level state).
bool Engine::updateFloorDoor() {
    if (m_level.floorDoorActive) {
        Vec3 toDoor = m_level.floorDoorPos - m_localPlayer.position;
        if (lengthSq(toDoor) < 4.0f) {
            // NOT a raw button read any more: the exit is the LOWEST-priority interact target, so
            // updatePlayerPickup arbitrates the button against nearby loot first and sets this flag
            // only when the exit actually won (a tap with no loot in reach, or a deliberate hold).
            // Reading the button here directly would let one press both grab an item AND descend.
            if (m_descendRequested) {
                // Server-authoritative descent: a remote CLIENT must never advance its own
                // floor / regenerate the level locally (that desyncs from the still-on-floor-N
                // host). Instead it asks the host to trigger descent via CL_REQUEST_DESCEND.
                // The host re-validates proximity + boss-dead on its side and broadcasts
                // SV_LEVEL_SEED back so every client transitions in lockstep.
                if (m_netRole == NetRole::CLIENT) {
                    sendDescendRequest();
                    m_bossLockNotifyTimer = 0.0f; // no local nag — server gates silently
                    return false;
                }
                // Local (host or offline/split) path: gate on boss, then run the shared flow.
                // Exit is sealed only on boss floors, and only until the boss is dead.
                if (m_level.floorHasBoss && floorBossAlive()) {
                    m_bossLockNotifyTimer = 2.0f;
                    return false;
                }
                // Demo ends after clearing the final floor (FINAL_FLOOR=20). Intercept here —
                // before triggerFloorDescent bumps currentFloor and saves — so the run ends
                // without persisting a phantom "floor 21" save; the player's save stays at
                // floor 20. Reuses the VICTORY state (its press-any-key->MENU handler is exactly
                // what we want); the victory render shows the demo "thanks for playing" text.
                if (GameConst::kDemoBuild && m_level.currentFloor >= GameConst::FINAL_FLOOR) {
                    m_gameState = GameState::VICTORY;
                    AudioSystem::stopMusic();
                    Input::setRelativeMouseMode(false);
                    return true;
                }
                return triggerFloorDescent();
            }
        }
    }
    return false;
}

// Shared floor-descent flow: bump currentFloor, grow all players, refresh cooldowns,
// save, schedule FLOOR_TRANSITION, broadcast SV_LEVEL_SEED. Called by two paths:
//   1. Local host-press (updateFloorDoor on SERVER/NONE) — after its own boss/proximity gate.
//   2. Remote client request (handleDescendRequest on SERVER) — after server-side re-validation.
// Never called on a CLIENT (the early CL_REQUEST_DESCEND branch in updateFloorDoor short-circuits).
bool Engine::triggerFloorDescent() {
    m_level.currentFloor++;
    // All players grow 1.5% stronger each floor (multiplicative).
    // In split-screen the FLOOR_TRANSITION block grows BOTH local players once
    // (regardless of who opened the door), so suppress the per-trigger growth
    // here to avoid double-growing whoever reached the exit.
    if (m_splitPlayerCount <= 1) {
        m_localPlayer.baseMaxHealth *= 1.015f;       // grow the base; maxHealth is derived from it
        Inventory::refreshMaxHealth(m_localPlayer, m_inventories[m_localPlayerIndex]);
        m_localPlayer.health = m_localPlayer.maxHealth;
        // Write-through to the per-player ARRAY, not just the alias: this function has a second
        // caller — handleDescendRequest, fired from a net callback when a REMOTE client presses
        // the door — and net callbacks run OUTSIDE the swapIn/swapOut window, where m_localPlayer
        // is a dead copy that the next swapInPlayer overwrites from the array. An alias-only write
        // here silently cost the HOST both the descend heal and the +1.5% growth whenever a client
        // triggered the descent (the local door press, inside gameUpdate, never hit this).
        m_localPlayers[m_localPlayerIndex].baseMaxHealth = m_localPlayer.baseMaxHealth;
        m_localPlayers[m_localPlayerIndex].maxHealth     = m_localPlayer.maxHealth;
        m_localPlayers[m_localPlayerIndex].health        = m_localPlayer.health;
        m_skillStates[m_localPlayerIndex].maxEnergy *= 1.015f;
        m_skillStates[m_localPlayerIndex].energy = m_skillStates[m_localPlayerIndex].maxEnergy;
    }
    // Scale all networked players too
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (!m_players[pi].active) continue;
        m_players[pi].baseMaxHealth *= 1.015f;
        Inventory::refreshMaxHealth(m_players[pi], m_inventories[pi]);
        m_players[pi].health = m_players[pi].maxHealth;
        m_players[pi].invulnTimer = 2.0f;
        m_players[pi].isDead = false;
    }
    // Upgrade equipment for NPCs that survived this floor
    upgradeNpcEquipment(static_cast<u8>(m_level.currentFloor));
    // Save progress before descending so death respawn returns here
    m_level.savedFloor = m_level.currentFloor;
    m_level.savedSeed = m_level.levelSeed; // persist the run seed (not a fresh draw)
    saveAllCharacters();  // per-character: each local lane to its own slot
    LOG_INFO("Descending to floor %u", m_level.currentFloor);

    // Refresh all cooldowns on floor descend. m_skillStates is a direct
    // [MAX_PLAYERS] array. Class-skill cooldowns live in the per-local-player
    // store m_classSkillStatesPerPlayer[MAX_LOCAL_PLAYERS][4] (m_classSkillStates
    // is only the swapped-in alias), so clear the store — not the alias — or the
    // non-descending split player keeps its cooldowns into the next floor.
    for (u32 p = 0; p < MAX_PLAYERS; p++)
        m_skillStates[p].cooldownTimer = 0.0f;
    for (u32 lp = 0; lp < MAX_LOCAL_PLAYERS; lp++)
        for (u32 s = 0; s < 4; s++) m_classSkillStatesPerPlayer[lp][s].cooldownTimer = 0.0f;
    // Keep the currently swapped-in player's active alias in sync.
    for (u32 s = 0; s < 4; s++) m_classSkillStates[s].cooldownTimer = 0.0f;
    // Per-player: clear every local player's equipment-skill cooldowns
    // (boot/helmet states are indexed per slot, not swapped in/out).
    for (u32 p = 0; p < MAX_PLAYERS; p++) {
        m_bootSkillStates[p].cooldownTimer = 0.0f;
        m_helmetSkillStates[p].cooldownTimer = 0.0f;
    }

    // Snapshot floor stats for transition screen
    m_transition.snapshotKills = m_transition.floorKillCount;
    m_transition.snapshotTime = m_transition.floorTime;

    // Floor 50 is the final floor of each difficulty. (The demo ends earlier, at FINAL_FLOOR=20,
    // but it's intercepted at the door in updateFloorDoor() before this point so no phantom
    // next-floor save is written — this full-game branch is never reached in a demo build.)
    if (m_level.currentFloor > 50) {
        if (m_difficulty < 2) {
            // Advance to next difficulty — reset to floor 1, keep gear
            m_difficulty++;
            m_highestUnlocked = m_difficulty;
            char diffPath[512];  // per-user data dir (Steam-Cloud-synced); CWD on Switch
            FILE* uf = std::fopen(Platform::userDataPath("difficulty_unlock.dat", diffPath, sizeof(diffPath)), "wb");
            if (uf) { std::fwrite(&m_highestUnlocked, 1, 1, uf); std::fclose(uf); }
            m_level.currentFloor = 1;
            m_level.savedFloor = 1;
            saveAllCharacters();  // per-character: each local lane to its own slot
            // Show transition with difficulty name
            m_transition.timer = 3.0f;
            m_gameState = GameState::FLOOR_TRANSITION;
            AudioSystem::play(SfxId::LEVEL_UP);
            LOG_INFO("Advancing to %s difficulty",
                     m_difficulty == 1 ? "Nightmare" : "Hell");
        } else {
            // Hell complete — the run's standard ending. Rolls credits on every machine (the
            // old direct VICTORY flip was host-local and hung co-op clients, same bug class as
            // the Engine kill) and falls through to the VICTORY screen after.
            beginCreditsSequence(false);
        }
    } else {
        m_transition.timer = 2.0f;
        m_gameState = GameState::FLOOR_TRANSITION;
        AudioSystem::play(SfxId::LEVEL_UP);
    }
    // Host tells clients to follow into the same floor. Broadcast the FINAL
    // floor/difficulty (already resolved above, incl. the floor-50->1 loop
    // where difficulty++ and currentFloor reset to 1) plus the run seed, so
    // clients regenerate the IDENTICAL dungeon. Skipped on VICTORY (no next
    // floor) since that branch leaves m_gameState != FLOOR_TRANSITION.
    if (m_netRole == NetRole::SERVER &&
        m_gameState == GameState::FLOOR_TRANSITION) {
        Net::broadcastLevelSeed(static_cast<u8>(m_level.currentFloor),
                                m_difficulty, m_level.levelSeed);
        // Republish the Steam lobby metadata so the public browser's "Floor N" reflects the floor we
        // just descended to instead of the one we started on (no-op off a Steam-relay host).
        updateSteamLobbyRoster();
        // (M11) Advance Server's authoritative level NOW so a client joining during
        // the ~2 s FLOOR_TRANSITION window receives the NEW floor/seed in
        // SV_JOIN_ACCEPT — otherwise it generates the previous floor and desyncs.
        Server::updateLevel(m_level.levelSeed,
                            static_cast<u8>(m_level.currentFloor), m_difficulty);
    }
    return true;
}

// ===========================================================================
// The Source — the secret superboss chamber (The Dungeon Engine).
// See ~/.claude/plans (the-dungeon-engine). Session-only key, no save change.
// ===========================================================================

// Carve a fixed, fully-enclosed void arena into the grid and rebuild the render mesh + nav fields.
// Deterministic (fixed dimensions, no RNG) so host and client build identical geometry. Returns
// the arena centre (where the Engine spawns and the nav flow-field converges).
Vec3 Engine::buildSourceChamber() {
    constexpr u32 W = 28, D = 28;          // grid cells → ~26x26 m open interior, 1-cell solid border
    constexpr f32 CS = 1.0f;
    LevelGridSystem::init(m_level.grid, W, D, CS);  // calloc → all cells empty; we fill every one below

    u8 vFloor = MaterialSystem::getIdByName("void_floor");
    u8 vWall  = MaterialSystem::getIdByName("void_wall");
    u8 vCeil  = MaterialSystem::getIdByName("void_ceiling");
    for (u32 z = 0; z < D; z++) {
        for (u32 x = 0; x < W; x++) {
            GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
            bool border = (x == 0 || z == 0 || x == W - 1 || z == D - 1);
            if (border) {
                c.flags = CELL_SOLID;          // closed walls — no corridor leaks out of The Source
                c.wallMaterialId = vWall;
            } else {
                c.flags = CELL_FLOOR | CELL_CEILING;
                c.floorHeight   = 0;
                c.ceilingHeight = 20;          // tall (5 m) — clears the 3.2 m Engine + grand feel
                c.floorMaterialId = vFloor;
                c.wallMaterialId  = vWall;
                c.ceilMaterialId  = vCeil;
            }
        }
    }
    // Per-floor mesh seed (levelSeed + floor) — matches startGame so a mid-run rebuild keeps
    // the same baked tile-shade + prop layout for this floor.
    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid,
                             m_level.levelSeed + m_level.currentFloor * 7919u,
                             m_level.sections, MAX_LEVEL_SECTIONS);
    LevelGridSystem::buildClearanceField(m_level.grid);
    // Re-init the minimap to the new grid size — it caches grid dimensions + a visited mask, so
    // without this it keeps the floor-50 48x48 bounds and reads OOB against the 28x28 grid (an
    // assert-crash in Debug). Mirrors the normal floor path in startGame.
    Minimap::init(m_level.grid.width, m_level.grid.depth);
    Vec3 center = {(W * 0.5f) * CS, 0.0f, (D * 0.5f) * CS};
    LevelGridSystem::buildFlowField(m_level.grid, center);   // wave-adds path toward the Engine
    return center;
}

// Host: transition into The Source. Wipe the floor-50 world, build the arena, move players in,
// spawn the Engine, and tell clients to follow via the sentinel-floor seed. Does NOT bump
// currentFloor or save (nothing about The Source touches disk — quitting reloads at floor 50).
void Engine::enterSourceChamber() {
    EntitySystem::init(m_entities);          // clear the dead Reaper + any leftover floor-50 enemies
    ProjectileSystem::init(m_projectiles);
    WorldItemSystem::init(m_worldItems);
    Vec3 center = buildSourceChamber();

    m_level.inSourceChamber    = true;
    m_level.sourcePortalActive = false;      // consumed
    m_level.floorDoorActive    = false;      // no ordinary exit in The Source (also disarms updateFloorDoor)
    m_level.floorHasBoss       = false;      // Engine death routes to victory, not the exit lock

    // Players enter from the south edge facing the Engine (centre, to the north). The lane that
    // triggered entry is the live m_localPlayer alias (swapOut writes it back to its array slot at
    // the end of this frame's per-player pass); other couch lanes are positioned in their slots.
    Vec3 base = {center.x, 0.0f, center.z + 10.0f};
    const f32 faceEngine = 3.14159f;         // yaw to face -Z toward the centre
    m_localPlayer.position    = base;
    m_localPlayer.yaw         = faceEngine;
    m_localPlayer.pitch       = 0.0f;
    m_localPlayer.invulnTimer = 2.0f;
    for (u8 lane = 0; lane < m_splitPlayerCount && lane < MAX_LOCAL_PLAYERS; lane++) {
        if (lane == m_localPlayerIndex) continue;
        m_localPlayers[lane].position    = base + Vec3{(f32)lane * 1.6f, 0.0f, 0.0f};
        m_localPlayers[lane].yaw         = faceEngine;
        m_localPlayers[lane].pitch       = 0.0f;
        m_localPlayers[lane].invulnTimer = 2.0f;
    }
    snapCameraToPlayer();                     // no interp smear from the floor-50 camera

    // Networked players (remote slots) enter too.
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (!m_players[pi].active) continue;
        m_players[pi].position    = base + Vec3{(f32)pi * 1.2f - 0.6f, 0.0f, 0.0f};
        m_players[pi].invulnTimer = 2.0f;
        m_players[pi].isDead      = false;
    }

    spawnSourceBoss(center);
    AudioSystem::play(SfxId::BOSS_ROAR);
    addChatMessage("\?\?\?", "So. You assembled me. Then meet the others.", Vec3{0.62f, 0.30f, 0.95f});

    // Tell clients to build the same chamber (sentinel floor 99 — no packet-format change). They
    // do NOT bump currentFloor; the Engine + waves replicate over the normal snapshot path.
    if (m_netRole == NetRole::SERVER) {
        Net::broadcastLevelSeed(GameConst::SOURCE_SENTINEL_FLOOR, m_difficulty, m_level.levelSeed);
        Server::updateLevel(m_level.levelSeed, GameConst::SOURCE_SENTINEL_FLOOR, m_difficulty);
    }
    LOG_INFO("Entered The Source (host).");
}

// Client: mirror of enterSourceChamber driven by the sentinel-floor SV_LEVEL_SEED (see onLevelSeed).
// Builds the identical deterministic geometry and moves the local player in; the Engine + its
// summoned waves are server-authoritative and arrive via snapshots — the client spawns nothing.
void Engine::enterSourceChamberClient() {
    Vec3 center = buildSourceChamber();
    m_level.inSourceChamber    = true;
    m_level.floorDoorActive    = false;
    m_level.sourcePortalActive = false;

    Vec3 base = {center.x, 0.0f, center.z + 10.0f};
    m_localPlayer.position    = base;
    m_localPlayer.yaw         = 3.14159f;
    m_localPlayer.pitch       = 0.0f;
    m_localPlayer.invulnTimer = 2.0f;
    m_localPlayers[0]         = m_localPlayer;   // client is single-lane (m_localPlayerIndex == 0)
    snapCameraToPlayer();
    addChatMessage("\?\?\?", "So. You assembled me. Then meet the others.", Vec3{0.62f, 0.30f, 0.95f});
    LOG_INFO("Entered The Source (client).");
}

// Per-tick (host/SP only): once floor 50 is cleared with the full shard set, spawn the hidden
// second portal beside the exit; then enter The Source when a player stands in it and presses
// pickup. Returns true if the player entered (caller returns immediately — the world is rebuilt).
bool Engine::updateSourcePortal() {
    if (GameConst::kDemoBuild) return false;          // secret is absent from the demo
    if (m_netRole == NetRole::CLIENT) return false;   // host-authoritative; clients follow the broadcast
    if (m_level.inSourceChamber) return false;        // already inside

    if (!m_level.sourcePortalActive) {
        // Open the portal once: on HELL floor 50, boss dead, and every shard collected this
        // session. The difficulty term is belt-and-suspenders — shards only DROP in Hell
        // (engine_death.cpp shard gate), so a full set can't exist below it; this makes the
        // intent explicit at the consumer too.
        if (m_level.currentFloor == 50 && m_difficulty == 2 &&
            s_sourceShards == 0x03FFu && !floorBossAlive()) {
            // A few metres off the (now-open) exit portal — far enough that their 2 m pickup zones
            // don't overlap, so the player chooses between descending and entering The Source. The
            // floor-50 boss arena is a 4x major arena, so this stays well inside open floor.
            m_level.sourcePortalPos    = m_level.floorDoorPos + Vec3{6.0f, 0.0f, 0.0f};
            m_level.sourcePortalActive = true;
            AudioSystem::play(SfxId::BOSS_ROAR);
            addChatMessage("\?\?\?", "The Engine has noticed you.", Vec3{0.62f, 0.30f, 0.95f});
            LOG_INFO("The Source portal opened on floor 50.");
        }
        return false;
    }

    // Arbitrated, not a raw button read (see updatePlayerPickup): entering The Source is
    // irreversible, and a tap aimed at the loot on the floor must never trigger it.
    if (m_portalRequested) {
        enterSourceChamber();
        return true;
    }
    return false;
}

// Post-Engine exit portal — consume the arbitrated enter request (same discipline as
// updateSourcePortal above: entering the ending is irreversible, so it must never ride a raw
// button read aimed at loot). SERVER/SP starts the shared credits sequence directly; a CLIENT
// asks the server via CL_REQUEST_DESCEND, whose handler resolves proximity against the portal.
bool Engine::updateExitPortal() {
    if (!m_level.exitPortalActive) return false;
    if (!m_creditsRequested) return false;
    m_creditsRequested = false;
    if (m_netRole == NetRole::CLIENT) {
        sendDescendRequest();
        return false;   // keep ticking until the server's SV_EVENT::CREDITS arrives
    }
    beginCreditsSequence(true);   // the portal only exists after the Engine fell
    return true;
}

// The town's to-dungeon portal — opens the Free-Play level select OVER the town (the world
// stays built; MENU_BACK in the select simply returns to IN_GAME). In co-op only the HOST
// drives the descent, exactly like ordinary floor doors.
bool Engine::updateTownPortal() {
    if (!m_level.townPortalActive) return false;
    if (!m_townPortalRequested) return false;
    m_townPortalRequested = false;
    if (m_netRole == NetRole::CLIENT) return false;   // host picks; guests ride the descent
    m_menu.freePlayDifficulty = (m_difficulty > 2) ? 2 : m_difficulty;
    m_menu.freePlayFloor      = 1;
    m_menu.freePlayFromTown   = true;
    m_menu.subState           = 14;   // Free-Play level select
    m_menu.subSelection       = 0;
    m_gameState = GameState::MENU;
    Input::setRelativeMouseMode(false);
    return true;
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
// Finds the first inactive slot in the damage number pool. On SERVER, also broadcasts
// SV_EVENT::DAMAGE_NUMBER so connected clients spawn the same number locally — without
// this the N4 ghost-sim removal leaves clients with no damage feedback (Combat::applyDamage
// doesn't run on CLIENT anymore, so the local-spawn path is never reached).
void Engine::spawnDamageNumber(Vec3 pos, f32 amount, bool isHeal, bool isCrit) {
    for (u32 i = 0; i < MAX_DAMAGE_NUMBERS; i++) {
        if (!m_fx.damageNumbers[i].active) {
            m_fx.damageNumbers[i] = {pos, amount, 1.0f, true, isHeal, isCrit};
            break;
        }
    }
    // Replicate to clients. Skipped on CLIENT (no remote peers to broadcast to) and on
    // NONE (offline / split-screen) — only the host fires this branch.
    if (m_netRole == NetRole::SERVER) {
        u8 buf[sizeof(PacketHeader) + 18]; // hdr(4) + eventType(1) + pos(12) + amount(4) + flags(1)
        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
        hdr->type = NetPacketType::SV_EVENT;
        hdr->flags = 0;
        hdr->seq   = 0;
        u32 off = sizeof(PacketHeader);
        buf[off++] = static_cast<u8>(NetEventType::DAMAGE_NUMBER);
        std::memcpy(buf + off, &pos.x, 4);    off += 4;
        std::memcpy(buf + off, &pos.y, 4);    off += 4;
        std::memcpy(buf + off, &pos.z, 4);    off += 4;
        std::memcpy(buf + off, &amount, 4);   off += 4;
        u8 flagsByte = (isHeal ? 1u : 0u) | (isCrit ? 2u : 0u);
        buf[off++] = flagsByte;
        // Phase 2.1 — Reliable. Cosmetic, but the user-perceived "I hit, where's my
        // damage number?" gap when an unreliable packet drops is jarring at 5%+ loss.
        // ENet reliable retransmit (~RTT-paced) gives a worst-case ~100 ms extra delay
        // on lossy paths versus the prior "silently lost forever". Mirrors HITSCAN_IMPACT
        // which is already reliable. AoE bursts (~10 numbers per frame) are <0.3 KB and
        // fit comfortably in ENet's reliable window.
        Net::broadcastReliable(buf, off);
    }
}

// Thin wrapper over Collision::tryPushXZ (which owns the rule and the reasoning) that supplies the
// player's XZ footprint. A push that would drive the player into geometry is refused.
static bool tryPushPlayerXZ(Player& p, const LevelGrid& grid, f32 dx, f32 dz) {
    const Vec3 half = {PLAYER_HALF_WIDTH, PLAYER_HEIGHT * 0.5f, PLAYER_HALF_WIDTH};
    return Collision::tryPushXZ(p.position, half, grid, dx, dz);
}

// Pushes the local player out of all active hostile entity AABBs.
// Uses the minimal-penetration axis to avoid tunneling on corners.
// Pushes that would land the player inside solid geometry are refused (see tryPushPlayerXZ).
void Engine::pushPlayerFromEntities() {
    AABB playerBox = {
        m_localPlayer.position + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
        m_localPlayer.position + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
    };
    // N4: on CLIENT, source pushout obstacles from the authoritative interp pool, not
    // the (now-frozen) ghost sim. Mirrors the moveAndSlide obstacle-source switch in
    // gameUpdate — without this the safety pushout drives the player out of stale
    // ghost positions and they can't traverse tiles where a ghost enemy was spawned.
    // (The enemyPush factor below is 0, so writes to e.position are no-ops; safe to
    // touch the interp pool even though it gets overwritten on the next snapshot.)
    EntityPool& pushPool = (m_netRole == NetRole::CLIENT)
                           ? m_renderInterp.entities
                           : m_entities;
    // Use activeList instead of scanning all 128 entity slots
    for (u32 a = 0; a < pushPool.activeCount; a++) {
        u32 idx = pushPool.activeList[a];
        Entity& e = pushPool.entities[idx];
        if (e.flags & ENT_DEAD) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;
        if (e.flags & ENT_BURROWED) continue;   // underground — never shoves the player
        AABB entBox = entityAABB(e);
        if (CombatQuery::aabbOverlap(playerBox, entBox)) {
            Vec3 toPlayer = m_localPlayer.position - e.position;
            f32 pushX = (e.halfExtents.x + PLAYER_HALF_WIDTH) - fabsf(toPlayer.x);
            f32 pushZ = (e.halfExtents.z + PLAYER_HALF_WIDTH) - fabsf(toPlayer.z);
            if (pushX > 0.0f && pushZ > 0.0f) {
                // A RUNNING loot goblin sheds the player SIDEWAYS, never along its heading. The
                // minimal-penetration push below re-resolves along the goblin's travel axis every
                // tick while it advances, so a head-on goblin used to bulldoze the player in front
                // of it for the whole contact — a free stationary kill riding its nose. Pushing
                // perpendicular to its velocity (toward whichever flank the player is already on)
                // pops the player off its path and lets it slip past. Seated/stationary goblins
                // (velocity ~0) fall through to the normal axis push like everything else.
                if (e.flags & ENT_LOOT_GOBLIN) {
                    Vec3 v = {e.velocity.x, 0.0f, e.velocity.z};
                    if (lengthSq(v) > 0.01f) {
                        v = normalize(v);
                        const f32 along = toPlayer.x * v.x + toPlayer.z * v.z;
                        Vec3 lat = {toPlayer.x - v.x * along, 0.0f, toPlayer.z - v.z * along};
                        // Player dead-center on the path has no lateral component — pick a flank.
                        if (lengthSq(lat) < 1e-4f) lat = {v.z, 0.0f, -v.x};
                        lat = normalize(lat);
                        const f32 depth = fminf(pushX, pushZ);
                        tryPushPlayerXZ(m_localPlayer, m_level.grid, lat.x * depth, lat.z * depth);
                        continue;
                    }
                }
                // Safety net — entities are solid, push the player out fully. The push is applied
                // through tryPushPlayerXZ, so an enemy can crowd the player against a wall but can
                // never drive them into it; when the minimal-penetration axis is blocked by geometry
                // the two are simply left overlapping until the player walks out.
                f32 playerPush = 1.0f;
                f32 enemyPush  = 0.0f;
                if (pushX < pushZ) {
                    f32 dir = (toPlayer.x > 0) ? 1.0f : -1.0f;
                    tryPushPlayerXZ(m_localPlayer, m_level.grid, dir * pushX * playerPush, 0.0f);
                    e.position.x -= dir * pushX * enemyPush;
                } else {
                    f32 dir = (toPlayer.z > 0) ? 1.0f : -1.0f;
                    tryPushPlayerXZ(m_localPlayer, m_level.grid, 0.0f, dir * pushZ * playerPush);
                    e.position.z -= dir * pushZ * enemyPush;
                }
            }
        }
    }

    // Player-to-player push in split-screen — gentle separation so they don't overlap.
    // L1: each player pushes only ITSELF (the active alias) away from the other, during its
    // own update pass — pushing both here would be clobbered by swapOutPlayer for the
    // non-active player. Both passes together separate the pair; it converges over a couple
    // of frames rather than snapping, which is the intended "gentle" feel. The wall push-out
    // below then resolves any overlap a push created with level geometry.
    if (m_splitPlayerCount > 1) {
        u8 otherP = (m_localPlayerIndex == 0) ? 1 : 0;
        if (!m_playerDead[otherP]) {
            Vec3 toMe = m_localPlayer.position - m_localPlayers[otherP].position;
            f32 dist = sqrtf(toMe.x * toMe.x + toMe.z * toMe.z);
            f32 minSep = 0.7f; // minimum separation (2 × player half-width)
            if (dist > 0.01f && dist < minSep) {
                f32 push = (minSep - dist) * 0.5f;
                Vec3 dir = {toMe.x / dist, 0, toMe.z / dist};
                // Same rule as the entity push: co-op partners can crowd each other, but neither may
                // shove the other into geometry. Without this, two players squeezing through a
                // doorway push each other into the door frame.
                tryPushPlayerXZ(m_localPlayer, m_level.grid, dir.x * push, dir.z * push);
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

