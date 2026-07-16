#define SDL_MAIN_HANDLED
#include <SDL.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#include "platform/window.h"
#include "platform/input.h"
#include "core/log.h"
#include "core/assert.h"

#include <cstring>
#include <cstdio>

static SDL_Window* s_window = nullptr;
static s32 s_width = 0;
static s32 s_height = 0;
static bool s_shouldClose = false;
static Window::DisplayMode s_displayMode = Window::DisplayMode::WINDOWED;  // user Display setting; persisted in video settings

bool Window::init(const char* title, s32 width, s32 height) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    // Audio is optional — don't fail if no audio device is available
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOG_WARN("SDL audio init failed (no audio device?): %s — continuing without audio", SDL_GetError());
    }

#ifdef __SWITCH__
    // Docked = 1080p, handheld = 720p
    if (appletGetOperationMode() == AppletOperationMode_Console) {
        width = 1920; height = 1080;
    } else {
        width = 1280; height = 720;
    }
#endif

    // Must be set BEFORE SDL_CreateWindow
#ifdef __SWITCH__
    // Switch mesa provides OpenGL via a compatibility context — no core profile
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    // Request a GL *debug* context when this is a debug engine build OR when DE_GLDEBUG is set at runtime.
    // The latter is an opt-in triage switch for the Intel-driver in-draw crash on the Steam/Release build:
    // with a debug context, gl_context.cpp routes the driver's own diagnostics to the log (naming the
    // offending GL call), and the driver often demotes the faulting draw to a logged no-op. Must be set
    // before SDL_CreateWindow. Normal players never set the env var, so they get a plain context.
    bool wantGlDebug = false;
#ifdef ENGINE_DEBUG
    wantGlDebug = true;
#endif
    if (SDL_getenv("DE_GLDEBUG")) wantGlDebug = true;
    if (wantGlDebug) SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);

    s_window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width, height,
#ifdef __SWITCH__
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN
#else
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
#endif
    );

    if (!s_window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    s_width = width;
    s_height = height;
    s_shouldClose = false;

    LOG_INFO("Window created: %dx%d", width, height);
    return true;
}

void Window::enterFullscreenExternal() {
    if (!s_window) return;
    const int n = SDL_GetNumVideoDisplays();
    int target = -1;
    SDL_Rect best{};
    for (int i = 0; i < n; ++i) {
        SDL_Rect b;
        if (SDL_GetDisplayBounds(i, &b) != 0) continue;
        const char* name = SDL_GetDisplayName(i);
        const bool isEdp = name && (std::strstr(name, "eDP") || std::strstr(name, "edp"));
        const bool landscape = b.w >= b.h;          // skip portrait monitors
        if (isEdp || !landscape) continue;          // want an external widescreen panel
        // Prefer the left-most qualifying display (the external monitor sits left of eDP here).
        if (target < 0 || b.x < best.x) { target = i; best = b; }
    }
    if (target < 0) target = 0;                     // fallback: primary display
    SDL_GetDisplayBounds(target, &best);
    const char* tname = SDL_GetDisplayName(target);
    LOG_INFO("Fullscreen on display %d '%s' %dx%d @ (%d,%d)",
             target, tname ? tname : "?", best.w, best.h, best.x, best.y);

    // Move onto the target display (windowed first), then go fullscreen-desktop = its native res.
    SDL_SetWindowFullscreen(s_window, 0);
    SDL_SetWindowPosition(s_window, best.x, best.y);
    SDL_SetWindowSize(s_window, best.w, best.h);
    SDL_SetWindowFullscreen(s_window, SDL_WINDOW_FULLSCREEN_DESKTOP);

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(s_window, &dw, &dh);      // GL drawable = what glViewport/glReadPixels use
    if (dw > 0 && dh > 0) { s_width = dw; s_height = dh; }
    s_displayMode = Window::DisplayMode::BORDERLESS; // keep the Display row in sync with reality
    LOG_INFO("Window now %dx%d (drawable)", s_width, s_height);
}

void Window::setDisplayMode(DisplayMode mode) {
    if (!s_window) return;
    // Switching BETWEEN the two fullscreen kinds goes through windowed first — SDL2's direct
    // FULLSCREEN <-> FULLSCREEN_DESKTOP transition is driver-dependent; via 0 it is not.
    if (s_displayMode != DisplayMode::WINDOWED && mode != s_displayMode)
        SDL_SetWindowFullscreen(s_window, 0);

    u32 flag = 0;
    if (mode == DisplayMode::BORDERLESS) {
        // Borderless windowed fullscreen at the desktop resolution on the current display —
        // no exclusive mode switch, so alt-tab is instant.
        flag = SDL_WINDOW_FULLSCREEN_DESKTOP;
    } else if (mode == DisplayMode::FULLSCREEN) {
        // Exclusive fullscreen AT THE DESKTOP MODE: the engine always renders at native res,
        // so exclusive buys direct scanout (compositor bypass, lower present latency) — never
        // a resolution switch that would rearrange the player's desktop.
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(getDisplayIndex(), &dm) == 0)
            SDL_SetWindowDisplayMode(s_window, &dm);
        flag = SDL_WINDOW_FULLSCREEN;
    }
    // flag 0 returns to the normal resizable window — SDL restores its pre-fullscreen size/pos.
    if (SDL_SetWindowFullscreen(s_window, flag) != 0) {
        LOG_WARN("Window: SDL_SetWindowFullscreen failed: %s", SDL_GetError());
        return;
    }
    s_displayMode = mode;
    // Refresh the cached size from the GL drawable so the renderer/HUD pick up the new viewport.
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(s_window, &dw, &dh);
    if (dw > 0 && dh > 0) { s_width = dw; s_height = dh; }
    LOG_INFO("Window: display mode %s (%dx%d)", displayModeName(mode), s_width, s_height);
}

Window::DisplayMode Window::getDisplayMode() { return s_displayMode; }

const char* Window::displayModeName(DisplayMode mode) {
    switch (mode) {
        case DisplayMode::BORDERLESS: return "Borderless";
        case DisplayMode::FULLSCREEN: return "Fullscreen";
        default:                      return "Windowed";
    }
}

int Window::getDisplayCount() {
    int n = SDL_GetNumVideoDisplays();
    return n > 0 ? n : 1;
}

int Window::getDisplayIndex() {
    if (!s_window) return 0;
    int idx = SDL_GetWindowDisplayIndex(s_window);
    return idx >= 0 ? idx : 0;
}

const char* Window::getDisplayName(int index) {
    const char* name = SDL_GetDisplayName(index);
    if (name && name[0]) return name;
    // Some drivers report no name — fall back to a stable 1-based label for the menu.
    static char fallback[24];
    std::snprintf(fallback, sizeof(fallback), "Display %d", index + 1);
    return fallback;
}

void Window::setDisplay(int index) {
    if (!s_window) return;
    const int n = SDL_GetNumVideoDisplays();
    if (n <= 0) return;
    if (index < 0)  index = 0;
    if (index >= n) index = n - 1;
    // A window can't be moved between displays while it's in fullscreen, so drop to windowed,
    // recenter on the target display, then restore the mode — setDisplayMode re-fetches the NEW
    // display's desktop mode, which is what makes exclusive fullscreen land at the right res.
    const DisplayMode was = s_displayMode;
    if (was != DisplayMode::WINDOWED) {
        SDL_SetWindowFullscreen(s_window, 0);
        s_displayMode = DisplayMode::WINDOWED;   // reflect reality so setDisplayMode re-applies cleanly
    }
    SDL_SetWindowPosition(s_window,
                          SDL_WINDOWPOS_CENTERED_DISPLAY(index),
                          SDL_WINDOWPOS_CENTERED_DISPLAY(index));
    if (was != DisplayMode::WINDOWED) setDisplayMode(was);
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(s_window, &dw, &dh);
    if (dw > 0 && dh > 0) { s_width = dw; s_height = dh; }
    LOG_INFO("Window: moved to display %d '%s' (%dx%d, %s)", index, getDisplayName(index),
             s_width, s_height, displayModeName(s_displayMode));
}

void Window::saveVideoSettings(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) { LOG_WARN("Window: could not open %s for writing", path); return; }
    // Line format: "<mode 0|1|2> <displayIndex>" — 0 windowed, 1 borderless, 2 exclusive
    // fullscreen. Pre-existing files wrote 0|1 with the SAME meanings (the old two-state
    // toggle), so no migration is needed; a one-int file still loads (mode only).
    std::fprintf(f, "%d %d\n", static_cast<int>(s_displayMode), getDisplayIndex());
    std::fclose(f);
    LOG_INFO("Window: saved video settings to %s", path);
}

void Window::loadVideoSettings(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return;  // no saved video settings yet — keep the windowed default
    int fs = 0, disp = 0;
    int got = std::fscanf(f, "%d %d", &fs, &disp);
    std::fclose(f);
    // Move to the saved display FIRST so fullscreen lands on it. disp<=0 is the default primary —
    // skip the reposition so single-monitor / primary users aren't nudged from the centered spawn.
    if (got >= 2 && disp > 0) setDisplay(disp);
    if (got >= 1 && fs > 0) {
        // Clamp unknown future values to BORDERLESS rather than guessing exclusive — the
        // gentler of the two fullscreens is the safe misread.
        if (fs > static_cast<int>(DisplayMode::FULLSCREEN)) fs = static_cast<int>(DisplayMode::BORDERLESS);
        setDisplayMode(static_cast<DisplayMode>(fs));  // apply now, before the first frame
        LOG_INFO("Window: loaded video settings from %s (%s)", path,
                 displayModeName(static_cast<DisplayMode>(fs)));
    }
}

void Window::shutdown() {
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = nullptr;
        LOG_INFO("Window destroyed");
    }
    SDL_Quit();
}

void Window::pollEvents() {
#ifdef __SWITCH__
    // Detect dock/undock transitions and adjust resolution
    AppletOperationMode mode = appletGetOperationMode();
    s32 newW = (mode == AppletOperationMode_Console) ? 1920 : 1280;
    s32 newH = (mode == AppletOperationMode_Console) ? 1080 : 720;
    if (newW != s_width || newH != s_height) {
        s_width = newW;
        s_height = newH;
        SDL_SetWindowSize(s_window, s_width, s_height);
        LOG_INFO("Switch mode change: %dx%d (%s)", s_width, s_height,
                 mode == AppletOperationMode_Console ? "docked" : "handheld");
    }
#endif

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                s_shouldClose = true;
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    s_width = event.window.data1;
                    s_height = event.window.data2;
                    LOG_INFO("Window resized: %dx%d", s_width, s_height);
                }
                break;

            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED:
                Input::handleControllerEvent(event);
                break;

            case SDL_MOUSEWHEEL:
                Input::handleMouseWheel(event.wheel.y);
                break;
        }
    }
}

bool Window::shouldClose() {
    return s_shouldClose;
}

SDL_Window* Window::getHandle() {
    return s_window;
}

s32 Window::getWidth() {
    return s_width;
}

s32 Window::getHeight() {
    return s_height;
}
