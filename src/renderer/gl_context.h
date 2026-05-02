#pragma once

struct SDL_Window;

namespace GLContext {
    bool init(SDL_Window* window);
    void shutdown();
    void swapBuffers(SDL_Window* window);
}
