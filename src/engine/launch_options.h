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
//   --floor <n>              starting floor for --new (default 1)
//   --difficulty <0-2>       difficulty for --new (0=Normal,1=Nightmare,2=Hell)
//   --port <n>               host/join port (default DEFAULT_PORT)
//   --lan                    host: LAN-only, skip UPnP (default attempts UPnP, like the menu)
//   --help                   print usage
//
// Examples:
//   DungeonEngine --host --load 1
//   DungeonEngine --host --new warrior --floor 5 --lan
//   DungeonEngine --join 192.168.1.5 --load 2
//   DungeonEngine --load 3

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
    u32 floor = 1;                             // for Save::NEW
    u8  difficulty = 0;                        // for Save::NEW (CONTINUE takes it from the save)

    u16  port = DEFAULT_PORT;                  // host/join
    bool upnp = true;                          // host: --lan clears this
    char address[64] = "127.0.0.1";            // for Role::JOIN

    bool active = false;   // any launch directive present (else: normal menu boot)
    bool valid  = true;    // parse succeeded (else: warn + fall back to menu)
};

// Parse argv into LaunchOptions. Pure aside from logging (usage on --help / bad input). On any
// error sets valid=false so the caller falls back to the menu instead of crashing.
LaunchOptions parseLaunchArgs(int argc, char** argv);
