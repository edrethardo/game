// engine_launch.cpp — applies developer launch flags (parsed in launch_options.cpp) to boot the
// game directly into a playable state, skipping the menu. It deliberately reuses the SAME start
// primitives the menu does (loadGame / Net::hostServer / Net::connectToServer / startGame) so
// there is one code path into gameplay; this file only sequences them from CLI options instead
// of from interactive screens.
//
// applyClassToLane0 is the shared class-stat setup extracted from engine_menu.cpp's class-select
// confirm handler, so the menu and the CLI configure a fresh hero identically.

#include "engine/engine.h"
#include "game/build_score.h"   // DEFAULT_BUILD_CELL for the --autoloot dev door
#include "engine/launch_options.h"
#include "game/game_constants.h"   // GameConst::kDemoBuild — gate --host/--join in the demo
#include "game/player.h"           // PlayerController::setBotWalk — the --bot-walk probe
#include "game/free_play.h"        // saveCleared — cleared heroes launch into the town

#include "core/log.h"
#include "net/net.h"
#include "platform/window.h"
#include "platform/input.h"   // Input::setSplitScreen — the --arena-couch dev door
#include "platform/steam.h"   // Steam::joinLobby for +connect_lobby cold-start

#include <cstring>

// Configure lane 0 for a freshly-chosen class: base HP/move/energy, the 4 class skill states, and
// the split-screen mirror arrays. Mirrors engine_menu.cpp (subState 2 confirm) exactly — keep the
// two in sync. Called by the menu and by applyLaunchOptions(Save::NEW).
void Engine::applyClassToLane0(PlayerClass cls) {
    m_playerClass = cls;
    m_activeClassSkill = 0;

    const ClassDef& def = kClassDefs[static_cast<u8>(cls)];
    m_localPlayer.baseMaxHealth = def.baseHealth;
    m_localPlayer.maxHealth     = def.baseHealth;
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

    // Netcode adversity harness: the ONLY runtime path that arms the fake-loss / fake-latency
    // cvars (serverNetPre/clientNetPre push them into Net:: each frame). Applied regardless of
    // game-jump so `--host --net-loss 10` and a menu-hosted session behave identically.
    m_netFakeLossPct   = opt.netLossPct;
    m_netFakeLatencyMs = opt.netLatencyMs;
    m_netFakeJitterMs  = opt.netJitterMs;

    // Dev door: force the two-story VERTICAL_HALL layout on every non-boss floor so the feature is
    // playtestable without waiting for its ~12% weighted roll (see startGame). Applied like the net
    // knobs — regardless of game-jump — so `--new warrior --floor 6 --vhall` lands straight in one.
    m_forceVerticalHall = opt.verticalHall;

    // Dev door (--fourstory): force the four-story FOUR_STORY "Descent" layout on every non-boss floor
    // so it is playtestable without waiting for its weighted roll (see startGame). Mirrors --vhall.
    m_forceFourStory = opt.fourStory;

    // Dev door (--lava): force the molten Hellforge theme on any floor in the 31-40 range, so the
    // tier's few lava floors are reachable on demand instead of by seed roll.
    m_forceLava = opt.lava;

    // Dev door (--autoloot): lane 0 plays Auto Loot & Equip from the first frame — the menu chooser
    // is unreachable from a CLI launch. Set BEFORE the game-jump below; every NEW_GAME wipe
    // preserves the two fields, so ordering is not load-bearing (belt and braces).
    if (opt.autoLoot) {
        m_inventories[0].autoMode  = 1;
        m_inventories[0].buildCell = BuildScore::DEFAULT_BUILD_CELL;
    }
    if (opt.netLossPct > 0 || opt.netLatencyMs > 0 || opt.netJitterMs > 0)
        LOG_INFO("Launch: NET ADVERSITY ON — %u%% loss, +%ums one-way latency, +/-%ums jitter (net-graph: F9)",
                 (u32)opt.netLossPct, opt.netLatencyMs, opt.netJitterMs);
    if (opt.botWalk) {
        PlayerController::setBotWalk(true);
        LOG_INFO("Launch: BOT-WALK ON — deterministic movement pattern (divergence probe)");
    }

    // Steam cold-start: a friend accepted an invite / clicked Join while the game was closed. Join that
    // lobby now; the lobby-entered callback (initCallbacks) routes it into the join flow once Steam
    // confirms. Stays at the menu (not a `--host/--join` game-jump). No-op if Steam isn't available;
    // gated off in the demo (no online).
    if (opt.connectLobbyId != 0 && !GameConst::kDemoBuild) {
        LOG_INFO("Launch: Steam +connect_lobby %llu", (unsigned long long)opt.connectLobbyId);
        Steam::joinLobby(opt.connectLobbyId);
    }

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
        // A CLEARED hero's Continue lands in the TOWN, exactly like the menu route — without
        // this, --load on a cleared save would feed the marker floor (51) into the generator.
        if (opt.role != LaunchOptions::Role::JOIN && !opt.arena && !opt.arenaCouch &&
            FreePlay::saveCleared(m_level.savedFloor, m_difficulty)) {
            if (opt.role == LaunchOptions::Role::HOST) {
                if (!Net::hostServer(opt.port, opt.upnp, 1)) {
                    m_netRole = NetRole::NONE;
                    LOG_WARN("Launch: failed to host on port %u — staying at menu", opt.port);
                    return;
                }
                m_netRole = NetRole::SERVER;
            }
            m_splitPlayerCount = 1;
            enterTown();
            LOG_INFO("Launch: cleared save -> entered the TOWN hub");
            return;
        }
    } else {  // Save::NEW
        applyClassToLane0(opt.cls);
        m_difficulty = opt.difficulty;
        m_level.currentFloor = opt.floor;
        mode = GameStart::NEW_GAME;
    }

    // --- Dev door (--arena-couch): local-versus PvP with two fresh lanes of opt.cls. ---
    if (opt.arenaCouch) {
        m_playerClasses[1] = opt.cls;         // both lanes fight as the same class
        equipFreshLane(0);
        equipFreshLane(1);
        m_splitPlayerCount = 2;
        Input::setSplitScreen(true);
        enterArena();
        LOG_INFO("Launch: entered the ARENA (local versus, --arena-couch)");
        return;
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
    if (opt.town) {
        // Dev door (--town): land ANY hero in the town hub — no clear required. startGame is
        // skipped entirely; enterTown builds the world and places the player.
        (void)mode;
        enterTown();
        LOG_INFO("Launch: entered the TOWN hub (--town)");
        return;
    }
    if (opt.arena) {
        // Dev door (--arena): straight into the PvP arena (optionally hosting — the HOST
        // block above already brought the listen-server up, and enterArena broadcasts the
        // sentinel seed so joiners follow).
        (void)mode;
        enterArena();
        LOG_INFO("Launch: entered the ARENA (--arena)");
        return;
    }
    startGame(mode);
    LOG_INFO("Launch: entered game (%s, %s)",
             opt.role == LaunchOptions::Role::HOST ? "host" : "single-player",
             mode == GameStart::CONTINUE ? "continue" : "new");
}
