#define SDL_MAIN_HANDLED
#include <SDL.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#include "platform/window.h"
#include "platform/input.h"
#include "core/log.h"
#include "core/assert.h"

static SDL_Window* s_window = nullptr;
static s32 s_width = 0;
static s32 s_height = 0;
static bool s_shouldClose = false;

bool Window::init(const char* title, s32 width, s32 height) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
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
