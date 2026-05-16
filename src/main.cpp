#define SDL_MAIN_HANDLED
#include <SDL.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#ifndef __SWITCH__
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <libgen.h>
#endif
#endif

#include "engine/engine.h"

// Change CWD to the directory containing the executable so that relative
// asset paths ("assets/...") resolve correctly regardless of where the
// binary is launched from (e.g. double-click from file manager).
static void setCwdToExeDir([[maybe_unused]] const char* argv0) {
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* last = std::strrchr(path, '\\');
    if (last) { *last = '\0'; SetCurrentDirectoryA(path); }
#elif !defined(__SWITCH__)
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        char* dir = dirname(buf);
        (void)chdir(dir);
    }
#endif
}

int main(int argc, char* argv[]) {
    (void)argc;

#ifndef __SWITCH__
    setCwdToExeDir(argv[0]);
#else
    (void)argv;
#endif

#ifdef __SWITCH__
    // Initialize Switch services before anything else
    romfsInit();
    socketInitializeDefault();
    nxlinkStdio(); // redirect stdout/stderr to nxlink console (debug)
#endif

    SDL_SetMainReady();

    Engine engine;
    engine.init();
    engine.run();
    engine.shutdown();

#ifdef __SWITCH__
    socketExit();
    romfsExit();
#endif
    return 0;
}

