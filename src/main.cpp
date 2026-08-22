#define SDL_MAIN_HANDLED
#include <SDL.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#ifndef __SWITCH__
#include <cstring>
#ifdef _WIN32
#include <windows.h>
// timeBeginPeriod/timeEndPeriod declared in windows.h (MinGW) or timeapi.h (MSVC)
#elif defined(__APPLE__)
#include <unistd.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#include <libgen.h>
#endif
#endif

#include "engine/engine.h"
// CLI launch flags. On Switch these arrive via `nxlink --args`, which is the only way to drive the
// console into a specific scenario for a measurement; launch_options.cpp was already in the Switch
// source list, only this include was gated.
#include "engine/launch_options.h"
#include "platform/window.h"  // --res: pre-init window size override

// Change CWD to the directory containing the executable so that relative
// asset paths ("assets/...") resolve correctly regardless of where the
// binary is launched from (e.g. double-click from file manager).
static void setCwdToExeDir([[maybe_unused]] const char* argv0) {
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* last = std::strrchr(path, '\\');
    if (last) { *last = '\0'; SetCurrentDirectoryA(path); }
#elif defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char* dir = dirname(buf);
        // .app bundle: exe at Contents/MacOS/, assets at Contents/Resources/assets/
        char resDir[1100];
        snprintf(resDir, sizeof(resDir), "%s/../Resources", dir);
        char checkRes[1200];
        snprintf(checkRes, sizeof(checkRes), "%s/assets", resDir);
        if (access(checkRes, F_OK) == 0) {
            (void)chdir(resDir);
        } else {
            // Flat layout (local dev build): assets next to binary
            char check[1100];
            snprintf(check, sizeof(check), "%s/assets", dir);
            if (access(check, F_OK) == 0) (void)chdir(dir);
        }
    }
#elif !defined(__SWITCH__)
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        char* dir = dirname(buf);
        // Only chdir if assets exist next to the binary (packaged build).
        // In dev builds the binary is in build/src/ but assets are at repo root.
        char check[1100];
        snprintf(check, sizeof(check), "%s/assets", dir);
        if (access(check, F_OK) == 0) {
            (void)chdir(dir);
        }
    }
#endif
}

int main(int argc, char* argv[]) {
#ifndef __SWITCH__
    setCwdToExeDir(argv[0]);
#endif

#ifdef __SWITCH__
    // Initialize Switch services before anything else
    romfsInit();
    socketInitializeDefault();
    nxlinkStdio(); // redirect stdout/stderr to nxlink console (debug)
    fprintf(stderr, "=== Switch main() entered ===\n");
    fflush(stderr);
#endif

#ifdef _WIN32
    // Set Windows timer resolution to 1ms so vsync blocking and the OS scheduler
    // wake the game precisely. Without this, default 15.6ms granularity causes
    // frame delivery jitter and visible microstutters in the fixed-timestep loop.
    timeBeginPeriod(1);
#endif

    SDL_SetMainReady();

    // Heap-allocate Engine to avoid ~500KB on the stack (Switch stack is limited)
    Engine* engine = new Engine();
    // Parse launch flags BEFORE init: --res must reach Window::init, which runs inside
    // engine->init() — applying options afterwards (the old order) could never size the window.
    const LaunchOptions launchOpts = parseLaunchArgs(argc, argv);
    if (launchOpts.resW > 0) Window::overrideInitialSize(launchOpts.resW, launchOpts.resH);
    engine->init();
    // Launch flags jump straight into a requested state (host/join/single + load/new, --vhall,
    // --autoplay, ...). No flags, bad flags or --help fall through to the normal menu boot.
    //
    // ON SWITCH these arrive from `nxlink --args "..."`, and they are the only way to put the
    // console into a specific, repeatable scenario. Without them a device measurement depends on a
    // human walking to the right kind of floor and holding still, which is neither repeatable nor
    // fair to ask for. Harmless in a normal launch from the homebrew menu: argc is 1.
    engine->applyLaunchOptions(launchOpts);
    engine->run();
    engine->shutdown();
    delete engine;

#ifdef _WIN32
    timeEndPeriod(1);
#endif

#ifdef __SWITCH__
    socketExit();
    romfsExit();
#endif
    return 0;
}

