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
                    m_localPlayers[p].maxHealth *= 1.015f;
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

        // Tick the spawn-calm window once per frame (here, not in gameUpdate, so
        // couch co-op — which calls gameUpdate per player — doesn't double-decrement).
        if (m_spawnCalmTimer > 0.0f) m_spawnCalmTimer -= dt;

        // Split-screen: update each local player in turn
        for (u8 sp = 0; sp < m_splitPlayerCount; sp++) {
            swapInPlayer(sp);             // sets m_localPlayerIndex = sp (the active player)
            Input::setActivePlayer(sp);

            if (m_playerDead[sp]) {
                // Dead player: check for respawn input, skip gameplay. Gamepad JUMP is routed
                // to this player's controller (setActivePlayer above); keyboard Space counts
                // only for P0 (L4 — Space is P0's key and must not respawn the controller P2).
                if (Input::isActionPressed(GameAction::JUMP) ||
                    (sp == 0 && Input::isKeyPressed(SDL_SCANCODE_SPACE))) {
                    // (M4) Enemies are already sent home when the last player dies (the co-op
                    // death path calls resetEnemiesToRooms), so nothing to reset on respawn —
                    // the old allDead computation here was dead code.
                    m_localPlayer.health = m_localPlayer.maxHealth;
                    m_localPlayer.position = m_players[sp].spawnPosition;
                    m_localPlayer.velocity = {0, 0, 0};
                    m_localPlayer.invulnTimer = 2.5f;
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
        tickSharedSystems(dt);

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

    // CLIENT ghost-sim guard (R7-4 (a)): snapshot the local player's HP/status, restore after,
    // so the locally-run ghost AI/projectiles can't overwrite the authoritative snapshot HP.
    const u8   localIdx   = (m_localPlayerIndex < MAX_LOCAL_PLAYERS) ? m_localPlayerIndex : 0;
    const bool ghostGuard = (m_netRole == NetRole::CLIENT);
    f32 sH=0, sInv=0, sPoi=0, sBur=0, sFrz=0, sSlo=0, sFla=0;
    if (ghostGuard) {
        Player& lp = m_localPlayers[localIdx];
        sH=lp.health; sInv=lp.invulnTimer; sPoi=lp.poisonTimer; sBur=lp.burnTimer;
        sFrz=lp.freezeTimer; sSlo=lp.slowTimer; sFla=lp.damageFlashTimer;
    }

    // Enemy AI — enemies target the nearest of primary + extras.
    {
        PROFILE_SCOPE(1, "AI");
        bool spawnCalm = m_spawnCalmTimer > 0.0f; // floor-start calm window
        EnemyAI::update(m_entities, m_level.grid, primary, m_projectiles, dt, &m_level.squads,
                        extraCount > 0 ? extras : nullptr, extraCount, &m_level.dungeon, spawnCalm);
        SquadSystem::update(m_level.squads, m_level.dungeon, m_entities, primary.position, dt);
    }

    // Decay enemy speech timers + log fresh speech to chat (shared entity state → once/frame;
    // the old per-player placement double-ticked these in split-screen).
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        u32 idx = m_entities.activeList[a];
        Entity& e = m_entities.entities[idx];
        if (e.speechTimer > 0.0f) {
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
    for (u32 i = 0; i < MAX_CHAT_LINES; i++) {
        if (m_chatLog[i].timer > 0.0f) m_chatLog[i].timer -= dt;
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
    // CLIENT: discard any HP/status the ghost sim just inflicted (snapshot is authoritative).
    if (ghostGuard) {
        Player& lp = m_localPlayers[localIdx];
        lp.health=sH; lp.invulnTimer=sInv; lp.poisonTimer=sPoi; lp.burnTimer=sBur;
        lp.freezeTimer=sFrz; lp.slowTimer=sSlo; lp.damageFlashTimer=sFla;
    }

    EntitySystem::tickTimers(m_entities, dt);
    WorldItemSystem::update(m_worldItems, dt);

    // Shared FX pools + shared skill world-systems (once/frame).
    tickSharedFX(dt);
    SkillSystem::updateOrbProjectiles(m_projectiles, m_skillDefs, m_skillDefCount, dt);
    // Meteors: pass both local players so kill-heals credit the casting player.
    Player* meteorPlayers[MAX_LOCAL_PLAYERS];
    for (u8 p = 0; p < MAX_LOCAL_PLAYERS; p++) meteorPlayers[p] = &m_localPlayers[p];
    SkillSystem::updateMeteors(m_entities, meteorPlayers, m_splitPlayerCount, dt);
    ParticleSystem::update(m_particles, dt);
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
            m_localPlayer.markTimer       = 0.0f;
            m_localPlayer.deathsDanceTimer = 0.0f;
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
        AudioSystem::play(SfxId::PLAYER_DEATH);
        return;
    }

    PROFILE_SCOPE(0, "Update");

    handleDebugKeys();

    // Quickbar slot switching (mouse wheel only — keys 1-4 are for class skills)
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
    SkillSystem::setCastingPlayer(m_localPlayerIndex); // credit per-player buffs to this player
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
    // CLIENT: pickups are SERVER-AUTHORITATIVE (N5). The client never consumes globes or
    // removes world items locally — instead it requests a pickup of the aimed item via
    // CL_PICKUP_ITEM, and the server validates proximity/ownership, applies the effect, and
    // removes the item (which propagates back via the next snapshot). Globe auto-pickup for
    // the client is handled server-side in serverNetPost (the client is a "remote" slot there).
    if (m_netRole == NetRole::CLIENT) {
        if (!m_inventoryOpen && Input::isActionPressed(GameAction::PICKUP))
            sendPickupRequest();
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
            // Loot-ownership window: skip items reserved to another local player (3s, then free).
            // (L8) Now active — drops are stamped with the killer's slot (Combat::s_attackingPlayer
            // → Entity::killerSlot → WorldItem::ownerSlot). Environmental/AoE kills stay 0xFF
            // (free-for-all). So in split-screen each player's kills are briefly theirs to grab.
            if (w.ownerSlot != 0xFF && w.ownerSlot != m_localPlayerIndex && w.exclusiveTimer > 0.0f)
                continue;
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

// CLIENT-only (N5): pick the world item the local player is aiming at (same aim logic as
// the host's updatePlayerPickup) and send its uid to the server as CL_PICKUP_ITEM. The
// server validates and removes it; the removal arrives back via the next snapshot, so we
// do NOT touch m_worldItems locally here (it's mirrored from the server).
void Engine::sendPickupRequest() {
    Vec3 fwd = m_localPlayer.forward;
    f32 bestScore = -1.0f;
    s32 bestIdx = -1;
    for (u32 wi = 0; wi < MAX_WORLD_ITEMS; wi++) {
        const WorldItem& w = m_worldItems.items[wi];
        if (!w.active) continue;
        if (isGlobe(w.item)) continue;            // globes are auto-picked server-side
        if (w.item.defId >= m_itemDefCount) continue;
        Vec3 toItem = w.position - m_localPlayer.position;
        f32 hDist = sqrtf(toItem.x * toItem.x + toItem.z * toItem.z);
        if (hDist > 3.5f) continue;
        f32 hLen = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
        f32 dot = (hDist > 0.1f && hLen > 0.01f)
            ? (fwd.x * toItem.x + fwd.z * toItem.z) / (hDist * hLen)
            : 1.0f;
        if (dot < 0.3f) continue;
        f32 score = dot - hDist * 0.1f;
        if (w.item.rarity == Rarity::LEGENDARY) score += 0.5f;
        if (score > bestScore) { bestScore = score; bestIdx = static_cast<s32>(wi); }
    }
    if (bestIdx < 0) return;

    u32 uid = m_worldItems.items[bestIdx].item.uid;
    // Packet: header(4) + uid(4). Reliable so a dropped pickup request still lands.
    u8 buf[sizeof(PacketHeader) + 4];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_PICKUP_ITEM;
    hdr->flags = 0;
    hdr->seq   = 0;
    std::memcpy(buf + sizeof(PacketHeader), &uid, 4);
    Net::sendToServer(buf, sizeof(buf), true);
}

// SERVER-only (N5): apply a validated CL_PICKUP_ITEM request. Finds the world item by uid,
// checks the requesting player's proximity + ownership against AUTHORITATIVE state, then
// moves it into that player's inventory and frees the slot (propagates via the next
// snapshot). Mirrors the host's local pickup rules so clients and host behave identically.
void Engine::handlePickupRequest(u8 playerSlot, u32 uid) {
    if (playerSlot >= MAX_PLAYERS) return;
    NetPlayer& np = m_players[playerSlot];
    if (!np.active || np.isDead) return;

    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = m_worldItems.items[i];
        if (!wi.active || wi.item.uid != uid) continue;
        if (isGlobe(wi.item)) return; // globes are server-auto-picked, not requestable

        // Proximity check against the authoritative net player position (XZ, matches aim path).
        Vec3 delta = np.position - wi.position;
        f32 hDist = sqrtf(delta.x * delta.x + delta.z * delta.z);
        if (hDist > 3.5f) return; // too far — reject (snapshot keeps the item present)

        // Ownership: free-for-all, owned by this player, or exclusive window expired.
        bool canPickup = (wi.ownerSlot == 0xFF)
                      || (wi.ownerSlot == playerSlot)
                      || (wi.exclusiveTimer <= 0.0f);
        if (!canPickup) return;

        ItemInstance picked = wi.item;
        if (!Inventory::addToBackpack(m_inventories[playerSlot], picked))
            return; // backpack full — leave the item in the world

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
        return;
    }
}

// True while a milestone boss is still alive on this floor. Used to lock the floor
// exit so the player must defeat the boss before descending. A boss mid-death-fade
// (ENT_DEAD set, deathTimer running) counts as dead. Malachar counts as alive through
// his false-death phases until the real kill.
bool Engine::floorBossAlive() const {
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
            if (Input::isActionPressed(GameAction::PICKUP)) {
                // Server-authoritative descent: a remote CLIENT must never advance its
                // own floor / regenerate the level locally — that would desync from the
                // still-on-floor-N host. The host (SERVER) and offline/split-screen
                // (NONE) run the local descend below; the host then BROADCASTS it so
                // clients follow via onLevelSeed -> FLOOR_TRANSITION. (Remote-initiated
                // descent — a client requesting a descend at the door — is deferred;
                // descent is currently host-initiated only. See CLAUDE.md.)
                if (m_netRole == NetRole::CLIENT) {
                    m_bossLockNotifyTimer = 0.0f; // no UI nag; just wait for the host
                    return false;
                }
                // Exit is sealed only on boss floors, and only until the boss is dead.
                if (m_level.floorHasBoss && floorBossAlive()) {
                    m_bossLockNotifyTimer = 2.0f;
                    return false;
                }
                m_level.currentFloor++;
                // All players grow 1.5% stronger each floor (multiplicative).
                // In split-screen the FLOOR_TRANSITION block grows BOTH local players once
                // (regardless of who opened the door), so suppress the per-trigger growth
                // here to avoid double-growing whoever reached the exit.
                if (m_splitPlayerCount <= 1) {
                    m_localPlayer.maxHealth *= 1.015f;
                    m_localPlayer.health = m_localPlayer.maxHealth;
                    m_skillStates[m_localPlayerIndex].maxEnergy *= 1.015f;
                    m_skillStates[m_localPlayerIndex].energy = m_skillStates[m_localPlayerIndex].maxEnergy;
                }
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
                m_level.savedSeed = m_level.levelSeed; // persist the run seed (not a fresh draw)
                saveGame(m_activeSaveSlot);
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
                // Host tells clients to follow into the same floor. Broadcast the FINAL
                // floor/difficulty (already resolved above, incl. the floor-50->1 loop
                // where difficulty++ and currentFloor reset to 1) plus the run seed, so
                // clients regenerate the IDENTICAL dungeon. Skipped on VICTORY (no next
                // floor) since that branch leaves m_gameState != FLOOR_TRANSITION.
                if (m_netRole == NetRole::SERVER &&
                    m_gameState == GameState::FLOOR_TRANSITION) {
                    Net::broadcastLevelSeed(static_cast<u8>(m_level.currentFloor),
                                            m_difficulty, m_level.levelSeed);
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

