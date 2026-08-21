#pragma once

// launch_options — desktop developer convenience for booting the game straight into a playable
// state, skipping the main menu. Parsed once in main() from argv and handed to
// Engine::applyLaunchOptions (engine_launch.cpp), which replays the same start primitives the
// menu uses (loadGame / Net::hostServer / Net::connectToServer / startGame).
//
// Flags:
//   --host | --join <ip>     role (neither = single-player)
//   --load <slot>            continue save_<slot>.dat (1-20)
//   --new <class>            fresh run as <class> (warrior, ranger, sorcerer, rogue, paladin,
//                            combat_engineer, marksman, tinkerer, wanderer)
//   --town                   land in the TOWN hub instead of the dungeon (works with any
//                            --load/--new hero — the dev door for town testing)
//   --floor <n>              starting floor for --new (default 1)
//   --difficulty <0-2>       difficulty for --new (0=Normal,1=Nightmare,2=Hell)
//   --port <n>               host/join port (default DEFAULT_PORT)
//   --lan                    host: LAN-only, skip UPnP (default attempts UPnP, like the menu)
//   --fullscreen             real fullscreen on the external widescreen monitor (native res)
//   --screenshot-interval <s>  auto-save a screenshot every <s> seconds while in-game
//   --help                   print usage
//
// Examples:
//   DungeonEngine --host --load 1
//   DungeonEngine --host --new warrior --floor 5 --lan
//   DungeonEngine --join 192.168.1.5 --load 2
//   DungeonEngine --load 3
//   DungeonEngine --fullscreen --screenshot-interval 60   (play + auto-capture 1080p store shots)

#include "core/types.h"
#include "game/item.h"   // PlayerClass
#include "net/net.h"     // DEFAULT_PORT

struct LaunchOptions {
    enum struct Role : u8 { SINGLE, HOST, JOIN };
    enum struct Save : u8 { NONE, LOAD, NEW };

    Role role = Role::SINGLE;
    Save save = Save::NONE;

    u8  slot = 0;                              // 1-20, for Save::LOAD
    PlayerClass cls = PlayerClass::WARRIOR;    // for Save::NEW
    // Lane 1's class for --autoplay-couch. Defaults to MARKSMAN so the pair is melee + ranged out of
    // the box: two lanes of the same class exercise one doctrine twice, and the whole point of couch
    // autoplay is watching two DIFFERENT builds run their own targeting and navigation side by side.
    PlayerClass cls2 = PlayerClass::MARKSMAN;
    u32 floor = 1;                             // for Save::NEW
    u8  difficulty = 0;                        // for Save::NEW (CONTINUE takes it from the save)

    u16  port = DEFAULT_PORT;                  // host/join
    bool upnp = true;                          // host: --lan clears this
    char address[64] = "127.0.0.1";            // for Role::JOIN
    // --record <dir>: trailer capture. Locksteps the sim (exactly ONE tick per rendered frame,
    // wall speed irrelevant) and writes every presented frame as <dir>/frame_NNNNNN.png, so an
    // offline 60 fps encode (tools/encode_trailer.sh) is bit-perfect real-time footage no matter
    // how slowly the machine rendered while capturing. Empty = off.
    char recordDir[200] = "";

    bool town = false;                         // --town: after --load/--new, enter the TOWN hub
    bool arena = false;                        // --arena: after --load/--new, enter the PvP ARENA
    // --source: after --load/--new, go straight into The Source (the secret Engine fight). Normally
    // it costs a full 50-floor run with all ten shards, which made the one world an autoplay bot can
    // enter but not play effectively untestable — a 3 h soak reached it three times by accident.
    bool source = false;
    // --victory: after --load/--new, roll the STANDARD ending's credits immediately. Same reasoning
    // as --source: the ending costs a full clear to reach (a 3 h soak produced exactly one), so the
    // credits-park strand and the run-continuation below it were effectively untestable.
    bool victory = false;
    // --zone <floor>: after --load/--new, drop straight into an OVERWORLD zone (52-96). The zones
    // sit behind an Inferno clear, so without this door the only way to see one is a full ladder.
    u8   zoneFloor = 0;   // 0 = not requested
    // --endgame: gear the hero as if they had just broken Inferno. The overworld spawns TIER 5 at
    // effective floor 200, so a fresh character is killed in about two seconds and never sees a
    // zone at all — measured, and it is why every early overworld probe went silent within a second
    // of arriving. Without this there is no way to soak-test the acts.
    bool endgame = false;
    // --quests-done: mark every act quest complete on the loaded hero. The post-acts FREE ROAM
    // (engine_autoplay_zone.cpp) only engages for a hero who arrives with the chain already
    // finished, which otherwise costs a full two-act playthrough to reach — the same reason
    // --endgame and --victory exist. Compose it: --load <slot> --endgame --zone 52 --autoplay
    // --quests-done reproduces "my finished hero walks into the overworld with the bot driving".
    bool questsDone = false;
    // --menu <inventory|character|quests>: open the tabbed character menu on that PAGE as soon as
    // the world is up. 0xFF = do not open.
    //
    // A dev door onto a SCREEN rather than a game state, and it exists for the same reason
    // --victory does: the alternative is driving the UI by hand, and on a headless or unfocused
    // display there is no hand to drive it with — the window-focus input gate zeroes every
    // synthetic keypress, so the menu pages were literally uncapturable without this.
    u8 menuPage = 0xFF;

    bool arenaCouch = false;                   // --arena-couch: local-versus arena, two fresh lanes
    bool autoplayCouch = false;                // --autoplay-couch: split-screen, BOTH lanes bot-driven
    // --devperf: emit the 1 Hz [DEVPERF] measurement line. OFF by default so a build you actually
    // PLAY is silent — the probe exists for tools/switch_perf_run.sh, which passes this flag through
    // nxlink --args. Printing it unconditionally put dev instrumentation in a release console build.
    bool devPerf      = false;
    bool verticalHall = false;                 // --vhall: force the two-story VERTICAL_HALL layout on non-boss floors (dev)
    bool fourStory   = false;                  // --fourstory: force the four-story FOUR_STORY "Descent" layout on non-boss floors (dev)
    bool lava        = false;                  // --lava: force the molten Hellforge theme on any floor 31-40 (dev)
    bool autoLoot    = false;                  // --autoloot: start lane 0 in Auto Loot & Equip mode (dev)
    bool autoplay = false;  // --autoplay: bot plays the run (dev door; implies autoloot; needs --new/--load)
    bool fullscreen = false;                   // --fullscreen: external-widescreen fullscreen
    u32  shotInterval = 0;                     // --screenshot-interval seconds (0 = off)

    // Netcode adversity harness (M14/D5). These are the ONLY way to turn the fake-loss /
    // fake-latency cvars on: they existed since M14 but nothing ever set them, so the whole
    // loss-resilience test rig was unreachable at runtime and the net-graph read "loss 0" forever.
    u8  netLossPct   = 0;                      // --net-loss <0-90>: % of packets dropped, both directions
    u32 netLatencyMs = 0;                      // --net-latency <0-1000>: one-way ms added to every send
    u32 netJitterMs  = 0;                      // --net-jitter <0-500>: per-packet [0,ms] jitter on top of latency
    bool botWalk     = false;                  // --bot-walk: deterministic movement bot (divergence probe)

    // Steam cold-start: `+connect_lobby <id>` (Steam appends this when a friend accepts an invite /
    // clicks Join while the game is closed). Non-zero → Engine::applyLaunchOptions joins that lobby,
    // which routes into the join flow. Handled separately from `active` (it stays at the menu).
    u64 connectLobbyId = 0;

    bool active = false;   // any GAME-JUMP directive present (else: normal menu boot)
    bool valid  = true;    // parse succeeded (else: warn + fall back to menu)
};

// Parse argv into LaunchOptions. Pure aside from logging (usage on --help / bad input). On any
// error sets valid=false so the caller falls back to the menu instead of crashing.
LaunchOptions parseLaunchArgs(int argc, char** argv);
