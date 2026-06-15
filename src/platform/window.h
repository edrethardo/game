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

    // Move the window to fullscreen-desktop on the preferred EXTERNAL WIDESCREEN display — skips the
    // laptop eDP panel and any portrait monitors. Used by the --fullscreen launch flag so capture
    // runs at the monitor's native resolution (1080p). Updates getWidth()/getHeight().
    void enterFullscreenExternal();
}
