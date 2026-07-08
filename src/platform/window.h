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

    // User-facing borderless (desktop) fullscreen toggle: a borderless window at the desktop
    // resolution on whichever display the window is currently on — no exclusive video-mode switch,
    // so alt-tab is instant. Distinct from enterFullscreenExternal(), which hunts for a specific
    // capture monitor. Updates getWidth()/getHeight().
    void setBorderlessFullscreen(bool enable);
    bool isBorderlessFullscreen();

    // Multi-monitor selection (desktop). Displays are indexed 0..getDisplayCount()-1 in SDL's
    // order. setDisplay() moves the window — carrying its borderless-fullscreen state with it —
    // onto the chosen display and centers it there. Lets players on multi-monitor rigs pick which
    // screen the game runs on from the Display options.
    int         getDisplayCount();
    int         getDisplayIndex();          // display the window currently occupies (0 on error)
    const char* getDisplayName(int index);  // human-readable monitor name ("Display N" fallback)
    void        setDisplay(int index);

    // Persist / restore the borderless-fullscreen preference (plain text, one flag; mirrors the
    // audio + controls settings files). loadVideoSettings applies the saved state immediately.
    void saveVideoSettings(const char* path);
    void loadVideoSettings(const char* path);
}
