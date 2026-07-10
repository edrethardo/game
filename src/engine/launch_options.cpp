// launch_options.cpp — argv parser for the developer launch flags (see launch_options.h).
// Deliberately C-style (strcmp/strtol, no exceptions) to match the codebase, which has no
// CLI/utility layer. Validation is lenient: any problem logs a warning + usage and sets
// valid=false so main() falls back to the normal menu rather than aborting.

#include "engine/launch_options.h"

#include "core/log.h"
#include "game/game_constants.h"   // GameConst::FINAL_FLOOR — demo caps --floor at 20

#include <cstring>
#include <cstdlib>

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
    LOG_INFO("  --difficulty <0-2>     difficulty for --new");
    LOG_INFO("  --port <n>  --lan      host/join port; --lan skips UPnP");
    LOG_INFO("  --fullscreen           real fullscreen on the external widescreen monitor");
    LOG_INFO("  --screenshot-interval <s>  auto-save a 1080p screenshot every <s> seconds in-game");
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
            long n; if (!parseInt(v, n) || n < 0 || n > 2) {
                LOG_WARN("--difficulty expects 0-2 (got '%s')", v); opt.valid = false; break;
            }
            opt.difficulty = (u8)n;
        } else if (ieq(a, "--port")) {
            const char* v = nextVal(i); if (!v) break;
            long n; if (!parseInt(v, n) || n < 1 || n > 65535) {
                LOG_WARN("--port expects 1-65535 (got '%s')", v); opt.valid = false; break;
            }
            opt.port = (u16)n;
        } else if (ieq(a, "--lan")) {
            opt.upnp = false;
        } else if (ieq(a, "--fullscreen")) {
            opt.fullscreen = true;          // display modifier — not a game-jump directive
        } else if (ieq(a, "--screenshot-interval") || ieq(a, "--shot-interval")) {
            const char* v = nextVal(i); if (!v) break;
            long n; if (!parseInt(v, n) || n < 1 || n > 3600) {
                LOG_WARN("--screenshot-interval expects 1-3600 seconds (got '%s')", v); opt.valid = false; break;
            }
            opt.shotInterval = (u32)n;      // display modifier — not a game-jump directive
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
