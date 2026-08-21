// engine_launch.cpp — applies developer launch flags (parsed in launch_options.cpp) to boot the
// game directly into a playable state, skipping the menu. It deliberately reuses the SAME start
// primitives the menu does (loadGame / Net::hostServer / Net::connectToServer / startGame) so
// there is one code path into gameplay; this file only sequences them from CLI options instead
// of from interactive screens.
//
// applyClassToLane0 is the shared class-stat setup extracted from engine_menu.cpp's class-select
// confirm handler, so the menu and the CLI configure a fresh hero identically.

#include "game/quest_def.h"   // --quests-done: mark the act chain complete
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
#include <cerrno>      // --record: mkdir failure reporting
#include <sys/stat.h>  // mkdir — the --record target directory

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


// Gear a lane as if it had just broken Inferno — the dev door (--endgame) that makes the overworld
// testable at all.
//
// WHY THIS HAS TO EXIST. The acts spawn TIER 5 evaluated at effective floor 200 (the ladder end), so
// a fresh character meets post-Inferno enemies immediately. Measured: a `--new warrior --zone 52` is
// dead in about two seconds and the run then sits on the death screen, where neither logStats nor the
// autoplay driver runs — which is why every early overworld probe went silent one second after
// arriving and looked like a hang. A soak of the acts is impossible without a hero the acts were
// balanced for.
//
// It rolls through the REAL ItemGen and equips through the REAL auto-equip, rather than stamping
// stats onto the player. That matters: a hand-stamped hero is not a hero the game can produce, so it
// would test the acts against a fiction. This one is exactly what a drop stream at ilvl 200 gives a
// build, which is the thing the balance was measured against.
void Engine::equipEndgameLoadout(u8 lane) {
    if (lane >= MAX_LOCAL_PLAYERS) return;
    PlayerInventory& inv = m_inventories[lane];

    // "As if they had just broken Inferno" is a PROGRESSION claim as much as a gear one. Without it
    // the hero is a fresh Normal character wearing endgame loot, and FreePlay::overworldUnlocked is
    // false — so the town's north gate refuses them. That matters the moment a bot drifts out of the
    // Blood Buffer's south edge into the town: it would be locked out of the acts it was playing,
    // with the only way onward being a dungeon run.
    m_difficulty       = FreePlay::FINAL_DIFFICULTY;
    m_highestUnlocked  = FreePlay::FINAL_DIFFICULTY;
    m_level.savedFloor = FreePlay::MAX_FLOOR + 1;   // the "cleared" marker floor

    // The gear brain does the choosing. Forced on regardless of --autoloot because a bag of loot the
    // hero never equips is not a loadout.
    inv.autoMode = 1;

    // ilvl 200 is the overworld's own scaling point (FreePlay::MAX_FLOOR + FINAL_DIFFICULTY * 50),
    // single-sourced from the same expression the spawner uses so the two cannot drift.
    const u8 ilvl = static_cast<u8>(200);

    // A DRIP, NOT ONE DUMP. The first version rolled into the bag until it was full and equipped
    // once — 24 items, which is a couple of hours of play, not a finished ladder. The hero it made
    // died three times in six minutes in the FIRST zone, so the soak was measuring an under-geared
    // character rather than the acts.
    //
    // Rolling in ROUNDS and re-equipping after each one models what actually produces an endgame
    // hero: a long drop stream where each upgrade is kept and the rest discarded. The bag is emptied
    // between rounds so a full bag can never throttle the stream — which is exactly what capped the
    // first version at 24.
    u32 taken = 0;
    for (u32 round = 0; round < 12; round++) {
        for (u32 i = 0; i < 64; i++) {
            const ItemInstance it = ItemGen::rollItem(ilvl, m_itemDefs, m_itemDefCount,
                                                      m_affixDefs, m_affixDefCount, Rarity::RARE);
            if (isItemEmpty(it)) continue;
            if (Inventory::addToBackpack(inv, it) < 0) break;
            taken++;
        }
        autoEquipBackpack(lane);
        // Drop what did not win, so the next round has room. autoEquipBackpack has already moved
        // every upgrade onto the character, so nothing of value is here.
        for (u32 sl = 0; sl < MAX_INVENTORY_ITEMS; sl++) inv.backpack[sl] = ItemInstance{};
    }
    LOG_INFO("Launch: --endgame geared lane %u from %u rolled items at ilvl %u",
             static_cast<u32>(lane), taken, static_cast<u32>(ilvl));
}

// --quests-done: hand the hero a finished act chain. Shared by every launch path that can reach the
// overworld, because the state it creates (objectiveZone() == 0) changes behaviour in the TOWN as
// well as in a zone — the town step's north-gate rule keys off it.
void Engine::applyQuestsDoneOption(const LaunchOptions& opt) {
    if (!opt.questsDone) return;
    m_questMask[0] = (Quest::COUNT >= 64) ? ~0ull : ((1ull << Quest::COUNT) - 1ull);
    // ...and FOLD it into the per-quest state, which is the authority. The mask alone is a raw
    // write, exactly like the save loader's, and Quest::Progress is what the Journal, the givers
    // and reevaluate() all read — so without this the door half-worked: ZoneRoute and the gate
    // refusals saw a finished chain while the quest log showed ten LOCKED rows and "0 of 5
    // complete". refreshQuestMask migrates the mask in before re-deriving, and migration only ever
    // ADDS completions, so this cannot lose progress.
    refreshQuestMask(0);
    LOG_INFO("Launch: --quests-done - both acts marked complete on lane 0 (%u quests)", Quest::COUNT);
}

void Engine::applyLaunchOptions(const LaunchOptions& opt) {
    if (!opt.valid) return;  // parse failed → normal menu boot

    // Display / capture modifiers apply whether or not a game-jump (host/join/load/new) was asked.
    if (opt.fullscreen) Window::enterFullscreenExternal();
    m_shotInterval = (f64)opt.shotInterval;
    // --camera: arm the cinematic path. Parsed HERE so a typo refuses the launch loudly —
    // discovered any later, it would have cost a whole capture run pointed the wrong way.
    if (opt.camera[0]) {
        if (!CineCam::parse(opt.camera, m_cinePath)) {
            LOG_ERROR("--camera: cannot parse '%s' — camera NOT armed", opt.camera);
        } else {
            m_cineTick = 0;
            // A cinematic take is HUD-free by definition (F10's flag): the path exists for staged
            // shots, and a health bar sliding through a dolly frame is a retake nobody wants.
            m_hideHud = true;
            LOG_INFO("Launch: --camera armed (%s) — HUD hidden, player input still live",
                     opt.camera);
        }
    }

    // --record: arm the trailer capture. The directory is created here, not lazily at the first
    // frame — a bad path should refuse at launch, when the message is readable, not one frame in.
    if (opt.recordDir[0]) {
        std::snprintf(m_recordDir, sizeof(m_recordDir), "%s", opt.recordDir);
        if (mkdir(m_recordDir, 0755) != 0 && errno != EEXIST) {
            LOG_ERROR("--record: cannot create '%s' (%s) — recording disabled",
                      m_recordDir, strerror(errno));
        } else {
            m_recordActive = true;
            LOG_INFO("Launch: --record armed -> %s (lockstep: 1 tick/frame, encode with "
                     "tools/encode_trailer.sh)", m_recordDir);
        }
    }
    // Deferred to the first IN_GAME frame (Engine::run) — every game-jump branch below returns
    // from a different place, and the menu can only open once a world exists.
    m_launchMenuPage = opt.menuPage;
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
    m_devPerf           = opt.devPerf;
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
    // --autoplay (arm the lane-0 bot) is deferred until AFTER the world is up (enterAutoplayRun below,
    // post startGame/enterTown) — a pre-load force would be overwritten by loadGame stamping the saved
    // autoMode, so a --autoplay --load of a Classic save would arm with the gear brain OFF.
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
            // Cleared-save Continue: an existing hero, so never re-seed its build cell.
            if (opt.autoplay) enterAutoplayRun(/*freshCharacter=*/false);
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

    // --- Dev door (--autoplay-couch): split-screen co-op with BOTH lanes bot-driven. ---
    // Autoplay was lane-0-only until the per-lane state split; this is the door that exercises the
    // other lane. Two fresh lanes of opt.cls, split-screen on, then the shared enterAutoplayRun
    // below arms EVERY lane (it seeds each lane's gear brain and build cell from that lane's class).
    if (opt.autoplayCouch) {
        m_playerClasses[1] = opt.cls2;        // lane 1 is a DIFFERENT build by default (melee + ranged)
        equipFreshLane(0);
        equipFreshLane(1);
        m_splitPlayerCount = 2;
        Input::setSplitScreen(true);
        Input::assignCouchPads();
        startGame(GameStart::NEW_GAME, /*lanesPrepared=*/true);
        positionLocalPlayersAtSpawn();
        enterAutoplayRun(/*freshCharacter=*/true);
        LOG_INFO("Launch: COUCH AUTOPLAY — P1 %s + P2 %s, both bot-driven (--autoplay-couch)",
                 kClassDefs[static_cast<u32>(opt.cls)].name,
                 kClassDefs[static_cast<u32>(opt.cls2)].name);
        // --victory composes with this door: the run continuation has a COUCH path of its own
        // (both lanes re-prepared, `lanesPrepared` start), and this is the only way to exercise it.
        if (opt.victory) {
            beginCreditsSequence(/*engineSlain=*/false);
            LOG_INFO("Launch: rolled the STANDARD ending (--victory, couch)");
        }
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
    if (opt.chakramRoom > 0) {
        // Trailer stage (WB-269): straight into the chakram room. Before --town in the chain only
        // because a stage is narrower than a world — nothing else composes with it.
        enterChakramRoom(opt.chakramRoom);
        return;
    }
    if (opt.town) {
        // Dev door (--town): land ANY hero in the town hub — no clear required. startGame is
        // skipped entirely; enterTown builds the world and places the player.
        // Quests BEFORE entering: the town step reads objectiveZone() to decide north gate vs
        // dungeon portal, so a mask applied afterwards would miss the first ticks that matter.
        applyQuestsDoneOption(opt);
        enterTown();
        // Arm the bot here too. This branch returns before the startGame() path's enterAutoplayRun
        // below, so `--autoplay --town` used to land an UNARMED hero in the hub — the one dev door
        // for the town's autoplay behaviour was the one place the bot was never switched on.
        // NEW_GAME = a fresh hero (seed its build cell from the class); a --load keeps its own.
        if (opt.endgame) equipEndgameLoadout(0);
        if (opt.autoplay) enterAutoplayRun(mode == GameStart::NEW_GAME);
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
    if (opt.source) {
        // Dev door (--source): build a normal run, then transition straight into The Source. Unlike
        // --town/--arena this needs startGame FIRST — enterSourceChamber wipes the current floor's
        // world and moves the live player in, so there has to be one. The bot is armed BEFORE the
        // transition for the same reason --town arms it early: this branch returns before the
        // arming below, and an unarmed hero in the one world we are here to test is useless.
        startGame(mode);
        if (opt.endgame) equipEndgameLoadout(0);
        if (opt.autoplay) enterAutoplayRun(mode == GameStart::NEW_GAME);
        enterSourceChamber();
        LOG_INFO("Launch: entered THE SOURCE (--source)");
        return;
    }
    if (opt.zoneFloor != 0) {
        // Dev door (--zone <52-96>): build a normal run, then walk straight into an OVERWORLD zone.
        // Needs startGame FIRST for the same reason --source does — enterZone replaces the live
        // world, so there has to be one and the hero's class/gear must already be set up. Without
        // this door the only way to see a zone is a full Inferno clear.
        startGame(mode);
        // Gear BEFORE arming the bot: enterAutoplayRun seeds the build cell from the class, and
        // autoEquipBackpack should run against that cell rather than re-gearing a moment later.
        if (opt.endgame) equipEndgameLoadout(0);
        if (opt.endgame) equipEndgameLoadout(0);
        if (opt.autoplay) enterAutoplayRun(mode == GameStart::NEW_GAME);
        if (opt.endgame) autoEquipBackpack(0);   // re-pick under the class's own build cell
        // --quests-done: hand the hero a finished act chain. Applied AFTER enterAutoplayRun (which
        // does not touch the mask) and BEFORE enterZone, so the very first tick in the zone already
        // sees the post-acts state — which is the tick that decides roam vs end-the-run.
        applyQuestsDoneOption(opt);
        enterZone(opt.zoneFloor, /*fromFloor=*/0);
        LOG_INFO("Launch: entered overworld zone %u (--zone)", (u32)opt.zoneFloor);
        return;
    }
    if (opt.victory) {
        // Dev door (--victory): build the world, arm the bot, then roll the STANDARD ending on the
        // spot. Needs startGame first for the same reason --source does — startCredits only fires
        // from IN_GAME. This is the only way to exercise the ending screens without a 50-floor
        // clear, which is why the credits park (and the run continuation after it) went unnoticed
        // until a 3 h soak happened to produce exactly one victory.
        startGame(mode);
        if (opt.endgame) equipEndgameLoadout(0);
        if (opt.autoplay) enterAutoplayRun(mode == GameStart::NEW_GAME);
        beginCreditsSequence(/*engineSlain=*/false);
        LOG_INFO("Launch: rolled the STANDARD ending (--victory)");
        return;
    }
    startGame(mode);
    // Arm the bot + force Auto Loot AFTER startGame so it sticks even for a CONTINUE (whose load
    // stamps the saved autoMode); no-op unless --autoplay. See enterAutoplayRun().
    // NEW_GAME = a fresh hero, so its build cell is seeded from the class; a CONTINUE keeps the
    // cell it saved (which may be a build the player deliberately picked).
    if (opt.endgame) equipEndgameLoadout(0);
    if (opt.autoplay) enterAutoplayRun(mode == GameStart::NEW_GAME);
    LOG_INFO("Launch: entered game (%s, %s)",
             opt.role == LaunchOptions::Role::HOST ? "host" : "single-player",
             mode == GameStart::CONTINUE ? "continue" : "new");
}
