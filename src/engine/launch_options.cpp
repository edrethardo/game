// launch_options.cpp — argv parser for the developer launch flags (see launch_options.h).
// Deliberately C-style (strcmp/strtol, no exceptions) to match the codebase, which has no
// CLI/utility layer. Validation is lenient: any problem logs a warning + usage and sets
// valid=false so main() falls back to the normal menu rather than aborting.

#include "engine/launch_options.h"

#include "core/log.h"
#include "game/game_constants.h"   // GameConst::FINAL_FLOOR — demo caps --floor at 20
#include "game/free_play.h"        // DIFFICULTY_COUNT — the tier bound --difficulty accepts
#include "game/zone_def.h"         // Zone::FLOOR_MIN/MAX — the band --zone accepts

#include <cstring>
#include <cstdlib>
#include <cstdio>   // snprintf — the --record path copy

namespace {

// Case-insensitive equality (portable — avoids POSIX strcasecmp / Win _stricmp split).
bool ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return *a == *b;
}

// CLI name → PlayerClass. Order/spelling mirrors the PlayerClass enum (item.h).
struct ClassName { const char* name; PlayerClass cls; };
const ClassName kClassNames[] = {
    {"warrior",         PlayerClass::WARRIOR},
    {"ranger",          PlayerClass::RANGER},
    {"sorcerer",        PlayerClass::SORCERER},
    {"rogue",           PlayerClass::ROGUE},
    {"paladin",         PlayerClass::PALADIN},
    {"combat_engineer", PlayerClass::COMBAT_ENGINEER},
    {"marksman",        PlayerClass::MARKSMAN},
    {"tinkerer",        PlayerClass::TINKERER},
    {"wanderer",        PlayerClass::WANDERER},
};

bool parseClass(const char* s, PlayerClass& out) {
    for (const ClassName& c : kClassNames) {
        if (ieq(s, c.name)) { out = c.cls; return true; }
    }
    return false;
}

// Parse a base-10 integer with full-string validation. Returns false on junk/empty.
bool parseInt(const char* s, long& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    out = std::strtol(s, &end, 10);
    return end && *end == '\0';
}

void logUsage() {
    LOG_INFO("Launch flags (desktop dev): skip the menu and boot into a state.");
    LOG_INFO("  --host | --join <ip>   role (neither = single-player)");
    LOG_INFO("  --load <slot>          continue save_<slot>.dat (1-20)");
    LOG_INFO("  --new <class>          fresh run (warrior, ranger, sorcerer, rogue, paladin,");
    LOG_INFO("                         combat_engineer, marksman, tinkerer, wanderer)");
    LOG_INFO("  --floor <n>            starting floor for --new (default 1)");
    LOG_INFO("  --difficulty <0-3>     difficulty for --new (0=Normal 1=Nightmare 2=Hell 3=Inferno)");
    LOG_INFO("  --port <n>  --lan      host/join port; --lan skips UPnP");
    LOG_INFO("  --fullscreen           real fullscreen on the external widescreen monitor");
    LOG_INFO("  --res <WxH>            window creation size (native-res --record captures)");
    LOG_INFO("  --screenshot-interval <s>  auto-save a 1080p screenshot every <s> seconds in-game");
    LOG_INFO("  --menu <page>              open the character menu on inventory|character|quests (dev)");
    LOG_INFO("  --record <dir>             lockstep the sim and dump every frame as PNG (trailer capture)");
    LOG_INFO("  --chakram-room [n]         trailer stage: sealed room, n Infinity Chakrams in flight (dev)");
    LOG_INFO("  --camera <spec>            cinematic camera: orbit:cx,cz,r,lapSec[,h[,lookY]] | glide:a:b:secs[:look]");
    LOG_INFO("  --stage <file>             shot-dressing script: spawn/loot/equip lines at world entry (dev)");
    LOG_INFO("  --hidehud                  start with the HUD hidden (F10) — for clean bot-played takes");
    LOG_INFO("  --net-loss <0-90>      drop this %% of packets both directions (netcode stress rig)");
    LOG_INFO("  --net-latency <ms>     add one-way fake latency to every send (0-1000)");
    LOG_INFO("  --net-jitter <ms>      add per-packet [0,ms] jitter on top of latency (0-500)");
    LOG_INFO("  --bot-walk             deterministic movement bot (netcode divergence probe)");
    LOG_INFO("  --help                 this message");
    LOG_INFO("  e.g.  DungeonEngine --host --load 1   |   --join 1.2.3.4 --load 2");
}

} // namespace

LaunchOptions parseLaunchArgs(int argc, char** argv) {
    LaunchOptions opt;

    // Fetch the value following a value-flag at index i; advances i. Returns nullptr if missing.
    auto nextVal = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            LOG_WARN("Launch arg '%s' needs a value", argv[i]);
            opt.valid = false;
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];

        if (ieq(a, "--help") || ieq(a, "-h")) {
            logUsage();
            opt.valid = false;        // show menu rather than launching into a state
            return opt;
        } else if (ieq(a, "+connect_lobby")) {
            // Steam cold-start: friend accepted an invite / clicked Join while the game was closed.
            const char* v = nextVal(i); if (!v) break;
            opt.connectLobbyId = std::strtoull(v, nullptr, 10);   // stays at the menu (not `active`)
        } else if (ieq(a, "--host")) {
            opt.role = LaunchOptions::Role::HOST; opt.active = true;
        } else if (ieq(a, "--join")) {
            const char* v = nextVal(i); if (!v) break;
            opt.role = LaunchOptions::Role::JOIN; opt.active = true;
            std::strncpy(opt.address, v, sizeof(opt.address) - 1);
            opt.address[sizeof(opt.address) - 1] = '\0';
        } else if (ieq(a, "--load")) {
            const char* v = nextVal(i); if (!v) break;
            long n; if (!parseInt(v, n) || n < 1 || n > 20) {
                LOG_WARN("--load expects a slot 1-20 (got '%s')", v); opt.valid = false; break;
            }
            opt.save = LaunchOptions::Save::LOAD; opt.slot = (u8)n; opt.active = true;
        } else if (ieq(a, "--new")) {
            const char* v = nextVal(i); if (!v) break;
            if (!parseClass(v, opt.cls)) {
                LOG_WARN("--new: unknown class '%s'", v); opt.valid = false; break;
            }
            opt.save = LaunchOptions::Save::NEW; opt.active = true;
        } else if (ieq(a, "--floor")) {
            const char* v = nextVal(i); if (!v) break;
            // Cap at FINAL_FLOOR so the demo (20) can't be launched out of scope; full game = 50.
            long n; if (!parseInt(v, n) || n < 1 || n > (long)GameConst::FINAL_FLOOR) {
                LOG_WARN("--floor expects 1-%u (got '%s')", GameConst::FINAL_FLOOR, v); opt.valid = false; break;
            }
            opt.floor = (u32)n;
        } else if (ieq(a, "--difficulty")) {
            const char* v = nextVal(i); if (!v) break;
            long n; if (!parseInt(v, n) || n < 0 || n >= (long)FreePlay::DIFFICULTY_COUNT) {
                LOG_WARN("--difficulty expects 0-%u (got '%s')",
                         FreePlay::DIFFICULTY_COUNT - 1, v); opt.valid = false; break;
            }
            opt.difficulty = (u8)n;
        } else if (ieq(a, "--port")) {
            const char* v = nextVal(i); if (!v) break;
            long n; if (!parseInt(v, n) || n < 1 || n > 65535) {
                LOG_WARN("--port expects 1-65535 (got '%s')", v); opt.valid = false; break;
            }
            opt.port = (u16)n;
        } else if (ieq(a, "--town")) {
            opt.town   = true;
            opt.active = true;
        } else if (ieq(a, "--arena")) {
            opt.arena  = true;
            opt.active = true;
        } else if (ieq(a, "--source")) {
            // Dev door into The Source. Reaching it legitimately means a clean 50-floor run holding
            // all ten shards, so without this the secret fight could only be exercised by accident.
            opt.source = true;
            opt.active = true;
        } else if (ieq(a, "--zone")) {
            const char* v = nextVal(i); if (!v) break;
            long n; if (!parseInt(v, n) || n < Zone::FLOOR_MIN || n > Zone::FLOOR_MAX) {
                LOG_WARN("--zone expects an overworld floor %u-%u (got '%s')",
                         Zone::FLOOR_MIN, Zone::FLOOR_MAX, v); opt.valid = false; break;
            }
            opt.zoneFloor = (u8)n;
            opt.active    = true;
        } else if (ieq(a, "--endgame")) {
            // Gear the hero for post-Inferno content. Pairs with --zone: the acts are balanced
            // against a hero who has finished the ladder, so anything else is not a test of them.
            opt.endgame = true;
            opt.active  = true;
        } else if (ieq(a, "--quests-done")) {
            // Dev door onto a hero who has ALREADY finished both acts — the state that decides
            // whether the overworld bot roams or ends its run.
            opt.questsDone = true;
            opt.active     = true;
        } else if (ieq(a, "--stage")) {
            const char* v = (i + 1 < argc) ? argv[++i] : "";
            if (!v[0]) { LOG_WARN("--stage expects a file"); opt.valid = false; break; }
            std::snprintf(opt.stageFile, sizeof(opt.stageFile), "%s", v);
            opt.active = true;
        } else if (ieq(a, "--hidehud")) {
            opt.hideHud = true;
            opt.active  = true;
        } else if (ieq(a, "--camera")) {
            const char* v = (i + 1 < argc) ? argv[++i] : "";
            if (!v[0]) { LOG_WARN("--camera expects a spec (orbit:... or glide:...)"); opt.valid = false; break; }
            std::snprintf(opt.camera, sizeof(opt.camera), "%s", v);
            opt.active = true;
        } else if (ieq(a, "--chakram-room")) {
            // Optional count; a bare flag means the default storm.
            opt.chakramRoom = 40;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                long n = std::strtol(argv[++i], nullptr, 10);
                if (n < 1 || n > 60) {
                    LOG_WARN("--chakram-room expects 1-60 discs (got %ld)", n);
                    opt.valid = false; break;
                }
                opt.chakramRoom = static_cast<u32>(n);
            }
            opt.active = true;
        } else if (ieq(a, "--record")) {
            const char* v = (i + 1 < argc) ? argv[++i] : "";
            if (!v[0]) { LOG_WARN("--record expects a directory"); opt.valid = false; break; }
            std::snprintf(opt.recordDir, sizeof(opt.recordDir), "%s", v);
            opt.active = true;
        } else if (ieq(a, "--menu")) {
            // Named rather than numbered: "--menu 2" would silently follow the enum if a page were
            // ever inserted, and the whole point of a screenshot door is that it names the screen.
            const char* v = (i + 1 < argc) ? argv[++i] : "";
            if      (ieq(v, "inventory")) opt.menuPage = 0;
            else if (ieq(v, "character")) opt.menuPage = 1;
            else if (ieq(v, "quests"))    opt.menuPage = 2;
            else {
                LOG_WARN("--menu expects inventory|character|quests (got '%s')", v);
                opt.valid = false; break;
            }
            opt.active = true;
        } else if (ieq(a, "--victory")) {
            // Dev door onto the ENDING screens: builds the world, then rolls the standard ending's
            // credits on the spot. Exercises credits -> victory -> (autoplay) next-run continuation
            // without the 50-floor clear that is the only other way to see them.
            opt.victory = true;
            opt.active  = true;
        } else if (ieq(a, "--autoplay-couch")) {
            // Couch co-op AUTOPLAY: split-screen with BOTH local players driven by their own bot.
            // Implies --autoplay. Lane 0 takes --new's class; lane 1 takes an OPTIONAL class argument
            // here, defaulting to Marksman so the pair is melee + ranged. The argument is only
            // consumed when it is a real class name — otherwise `--autoplay-couch --fourstory` would
            // eat the next flag.
            opt.autoplayCouch = true; opt.autoplay = true; opt.active = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                PlayerClass c2;
                if (parseClass(argv[i + 1], c2)) { opt.cls2 = c2; i++; }
                else { LOG_WARN("--autoplay-couch: unknown class '%s'", argv[i + 1]); opt.valid = false; }
            }
        } else if (ieq(a, "--arena-couch")) {
            opt.arenaCouch = true;
            opt.active     = true;
        } else if (ieq(a, "--devperf")) {
            opt.devPerf = true; opt.active = true;
        } else if (ieq(a, "--vhall")) {
            opt.verticalHall = true;   // modifier on normal play (needs --new/--load); not a separate entry
            opt.active       = true;
        } else if (ieq(a, "--fourstory")) {
            opt.fourStory = true;      // modifier on normal play (needs --new/--load); mirrors --vhall
            opt.active    = true;
        } else if (ieq(a, "--lava")) {
            opt.lava   = true;         // modifier on normal play; only bites on floors 31-40
            opt.active = true;
        } else if (ieq(a, "--autoloot")) {
            opt.autoLoot = true;       // modifier on normal play (needs --new/--load)
            opt.active   = true;
        } else if (ieq(a, "--autoplay")) {
            opt.autoplay = true;
            opt.autoLoot = true;       // autoplay needs Auto Loot as the gear brain
            opt.active   = true;
        } else if (ieq(a, "--lan")) {
            opt.upnp = false;
        } else if (ieq(a, "--fullscreen")) {
            opt.fullscreen = true;          // display modifier — not a game-jump directive
        } else if (ieq(a, "--arena-map")) {
            const char* v = nextVal(i); if (!v) break;
            long n; if (!parseInt(v, n) || n < 0 || n > 3) {
                LOG_WARN("--arena-map expects 0-3 (got '%s')", v); opt.valid = false; break;
            }
            opt.arenaMap = (u8)n;
        } else if (ieq(a, "--res")) {
            // WxH creation size, e.g. --res 1920x1080. Bounded to keep a typo from asking SDL for
            // a 100000-pixel surface; the small end refuses sizes the HUD layout can't survive.
            const char* v = nextVal(i); if (!v) break;
            int w = 0, h = 0;
            if (std::sscanf(v, "%dx%d", &w, &h) != 2 || w < 640 || h < 360 || w > 7680 || h > 4320) {
                LOG_WARN("--res expects <W>x<H> between 640x360 and 7680x4320 (got '%s')", v);
                opt.valid = false; break;
            }
            opt.resW = (u16)w; opt.resH = (u16)h;   // display modifier — not a game-jump directive
        } else if (ieq(a, "--screenshot-interval") || ieq(a, "--shot-interval")) {
            const char* v = nextVal(i); if (!v) break;
            long n; if (!parseInt(v, n) || n < 1 || n > 3600) {
                LOG_WARN("--screenshot-interval expects 1-3600 seconds (got '%s')", v); opt.valid = false; break;
            }
            opt.shotInterval = (u32)n;      // display modifier — not a game-jump directive
        } else if (ieq(a, "--net-loss")) {
            const char* v = nextVal(i); if (!v) break;
            // 90 cap: 100% loss is a disconnect test, not a netcode test — ENet just times out.
            long n; if (!parseInt(v, n) || n < 0 || n > 90) {
                LOG_WARN("--net-loss expects 0-90 percent (got '%s')", v); opt.valid = false; break;
            }
            opt.netLossPct = (u8)n;         // adversity harness — not a game-jump directive
        } else if (ieq(a, "--net-latency")) {
            const char* v = nextVal(i); if (!v) break;
            long n; if (!parseInt(v, n) || n < 0 || n > 1000) {
                LOG_WARN("--net-latency expects 0-1000 ms (got '%s')", v); opt.valid = false; break;
            }
            opt.netLatencyMs = (u32)n;      // adversity harness — not a game-jump directive
        } else if (ieq(a, "--net-jitter")) {
            const char* v = nextVal(i); if (!v) break;
            // 500 cap: jitter is spread on top of the latency floor; beyond this the link is
            // effectively broken rather than long-haul, which the loss/timeout tests already cover.
            long n; if (!parseInt(v, n) || n < 0 || n > 500) {
                LOG_WARN("--net-jitter expects 0-500 ms (got '%s')", v); opt.valid = false; break;
            }
            opt.netJitterMs = (u32)n;       // adversity harness — not a game-jump directive
        } else if (ieq(a, "--bot-walk")) {
            opt.botWalk = true;             // adversity harness — not a game-jump directive
        } else {
            LOG_WARN("Unknown launch arg '%s' (try --help)", a);
            opt.valid = false; break;
        }
    }

    // A role with no hero source is meaningless — flag it (applyLaunchOptions also guards).
    if (opt.active && opt.valid && opt.save == LaunchOptions::Save::NONE) {
        LOG_WARN("Launch flags set a role but no --load <slot> or --new <class>; ignoring");
        opt.valid = false;
    }
    if (opt.active && !opt.valid) logUsage();
    return opt;
}
