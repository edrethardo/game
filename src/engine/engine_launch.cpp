// engine_launch.cpp — applies developer launch flags (parsed in launch_options.cpp) to boot the
// game directly into a playable state, skipping the menu. It deliberately reuses the SAME start
// primitives the menu does (loadGame / Net::hostServer / Net::connectToServer / startGame) so
// there is one code path into gameplay; this file only sequences them from CLI options instead
// of from interactive screens.
//
// applyClassToLane0 is the shared class-stat setup extracted from engine_menu.cpp's class-select
// confirm handler, so the menu and the CLI configure a fresh hero identically.

#include "engine/engine.h"
#include "engine/launch_options.h"
#include "game/game_constants.h"   // GameConst::kDemoBuild — gate --host/--join in the demo

#include "core/log.h"
#include "net/net.h"
#include "platform/window.h"

#include <cstring>

// Configure lane 0 for a freshly-chosen class: base HP/move/energy, the 4 class skill states, and
// the split-screen mirror arrays. Mirrors engine_menu.cpp (subState 2 confirm) exactly — keep the
// two in sync. Called by the menu and by applyLaunchOptions(Save::NEW).
void Engine::applyClassToLane0(PlayerClass cls) {
    m_playerClass = cls;
    m_activeClassSkill = 0;

    const ClassDef& def = kClassDefs[static_cast<u8>(cls)];
    m_localPlayer.maxHealth = def.baseHealth;
    m_localPlayer.health = def.baseHealth;
    m_localPlayer.moveSpeed = def.baseMoveSpeed;
    m_skillStates[m_localPlayerIndex].maxEnergy = def.baseEnergy;
    m_skillStates[m_localPlayerIndex].energy = def.baseEnergy;
    // Warrior passive: 30% damage reduction (matches the menu's class setup).
    m_localPlayer.damageReduction = (cls == PlayerClass::WARRIOR) ? 0.3f : 0.0f;

    for (u32 s = 0; s < 4; s++) {
        m_classSkillStates[s] = SkillState{};
        m_classSkillStates[s].activeSkill = def.skills[s];
        m_classSkillStates[s].maxEnergy = def.baseEnergy;
        m_classSkillStates[s].energy = def.baseEnergy;
    }

    // Store P1 state into the split-screen arrays (lane 0).
    m_localPlayers[0] = m_localPlayer;
    m_playerClasses[0] = m_playerClass;
    std::memcpy(m_classSkillStatesPerPlayer[0], m_classSkillStates, sizeof(m_classSkillStates));
}

void Engine::applyLaunchOptions(const LaunchOptions& opt) {
    if (!opt.valid) return;  // parse failed → normal menu boot

    // Display / capture modifiers apply whether or not a game-jump (host/join/load/new) was asked.
    if (opt.fullscreen) Window::enterFullscreenExternal();
    m_shotInterval = (f64)opt.shotInterval;
    if (opt.shotInterval > 0)
        LOG_INFO("Launch: auto-screenshot every %us -> screenshot_NNNN.png in the run dir", opt.shotInterval);

    if (!opt.active) return;  // no game-jump directive → normal menu boot

    // The demo build exposes ONLY singleplayer + local couch co-op, so the CLI must not open a
    // network session either — the menu hides Host/Join, but the --host/--join launch flags are a
    // separate entry point that calls Net::hostServer/connectToServer directly. Reject them and
    // fall back to the menu rather than start an online game.
    if (GameConst::kDemoBuild &&
        (opt.role == LaunchOptions::Role::HOST || opt.role == LaunchOptions::Role::JOIN)) {
        LOG_WARN("Launch: --host/--join are disabled in the demo build — staying at menu");
        return;
    }

    // --- Resolve the hero + the start mode (CONTINUE for a save, NEW_GAME for a fresh class) ---
    GameStart mode;
    if (opt.save == LaunchOptions::Save::LOAD) {
        if (!loadGame(opt.slot)) {
            LOG_WARN("Launch: save slot %u missing/incompatible — staying at menu", opt.slot);
            return;
        }
        m_level.currentFloor = m_level.savedFloor;   // adopt the saved run's floor (as the menu does)
        mode = GameStart::CONTINUE;
    } else {  // Save::NEW
        applyClassToLane0(opt.cls);
        m_difficulty = opt.difficulty;
        m_level.currentFloor = opt.floor;
        mode = GameStart::NEW_GAME;
    }

    m_splitPlayerCount = 1;  // CLI launch is always single local player

    // --- JOIN: connect as a client and enter CONNECTING; the server drives us into the game ---
    if (opt.role == LaunchOptions::Role::JOIN) {
        m_netRole = NetRole::CLIENT;
        std::strncpy(m_menu.connectAddress, opt.address, sizeof(m_menu.connectAddress) - 1);
        m_menu.connectAddress[sizeof(m_menu.connectAddress) - 1] = '\0';
        Net::setLocalPlayerClass(static_cast<u8>(m_playerClasses[0]));
        // A loaded hero must re-sync its inventory to the host once accepted (CL_INVENTORY_SYNC).
        if (opt.save == LaunchOptions::Save::LOAD) m_clientLoadedFromSave = true;

        if (Net::connectToServer(opt.address, opt.port)) {
            m_gameState = GameState::CONNECTING;
            m_connectingElapsed = 0.0f;
            LOG_INFO("Launch: joining %s:%u as class %u...",
                     opt.address, opt.port, static_cast<u32>(m_playerClasses[0]));
        } else {
            m_netRole = NetRole::NONE;
            m_clientLoadedFromSave = false;
            LOG_WARN("Launch: failed to connect to %s:%u — staying at menu", opt.address, opt.port);
        }
        return;  // never call startGame for a client
    }

    // --- HOST: bring up the listen-server before entering the game ---
    if (opt.role == LaunchOptions::Role::HOST) {
        m_netRole = NetRole::SERVER;
        if (!Net::hostServer(opt.port, opt.upnp, 1)) {
            m_netRole = NetRole::NONE;
            LOG_WARN("Launch: failed to host on port %u — staying at menu", opt.port);
            return;
        }
        LOG_INFO("Launch: hosting on port %u (%s)...", opt.port, opt.upnp ? "UPnP" : "LAN-only");
    }

    // --- SINGLE or HOST: enter the game (startGame sets m_gameState = IN_GAME) ---
    startGame(mode);
    LOG_INFO("Launch: entered game (%s, %s)",
             opt.role == LaunchOptions::Role::HOST ? "host" : "single-player",
             mode == GameStart::CONTINUE ? "continue" : "new");
}
