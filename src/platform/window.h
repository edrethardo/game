#pragma once

#include "core/types.h"

struct SDL_Window;

namespace Window {
    bool init(const char* title, s32 width, s32 height);
    void shutdown();
    void pollEvents();
    bool shouldClose();

    SDL_Window* getHandle();
    s32 getWidth();
    s32 getHeight();
}
