#define SDL_MAIN_HANDLED
#include <SDL.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "engine/engine.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

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

