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

static SDL_Window* s_window = nullptr;
static s32 s_width = 0;
static s32 s_height = 0;
static bool s_shouldClose = false;

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
#ifdef ENGINE_DEBUG
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif

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
    LOG_INFO("Window now %dx%d (drawable)", s_width, s_height);
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
