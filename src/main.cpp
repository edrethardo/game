#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    SDL_SetMainReady();

    Engine engine;
    engine.init();
    engine.run();
    engine.shutdown();
    return 0;
}
