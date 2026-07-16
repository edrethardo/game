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

    // User-facing display mode — three states, cycled from the Display options row:
    //   WINDOWED   — decorated resizable window (SDL restores the pre-fullscreen size/position).
    //   BORDERLESS — borderless window at desktop resolution on the current display
    //                (SDL_WINDOW_FULLSCREEN_DESKTOP: no exclusive mode switch, instant alt-tab).
    //   FULLSCREEN — EXCLUSIVE fullscreen, deliberately at the display's desktop mode: the engine
    //                always renders at native res, so exclusive buys direct scanout (compositor
    //                bypass, lower present latency) — never a resolution switch.
    // Distinct from enterFullscreenExternal(), which hunts for a specific capture monitor.
    // setDisplayMode applies immediately and updates getWidth()/getHeight() from the GL drawable.
    enum struct DisplayMode : u8 { WINDOWED = 0, BORDERLESS = 1, FULLSCREEN = 2, COUNT = 3 };
    void        setDisplayMode(DisplayMode mode);
    DisplayMode getDisplayMode();
    const char* displayModeName(DisplayMode mode);   // "Windowed" / "Borderless" / "Fullscreen"

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
