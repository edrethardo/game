#include "renderer/gl_context.h"
#include "core/log.h"

#include <glad/glad.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>

static SDL_GLContext s_context = nullptr;

#ifdef ENGINE_DEBUG
static void APIENTRY glDebugCallback(GLenum source, GLenum type, GLuint id,
                                      GLenum severity, GLsizei length,
                                      const char* message, const void* userParam) {
    (void)source; (void)type; (void)id; (void)length; (void)userParam;
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    const char* sevStr = "???";
    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:   sevStr = "HIGH"; break;
        case GL_DEBUG_SEVERITY_MEDIUM: sevStr = "MEDIUM"; break;
        case GL_DEBUG_SEVERITY_LOW:    sevStr = "LOW"; break;
    }
    LOG_WARN("GL [%s]: %s", sevStr, message);
}
#endif

bool GLContext::init(SDL_Window* window) {
    s_context = SDL_GL_CreateContext(window);
    if (!s_context) {
        LOG_ERROR("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return false;
    }

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        LOG_ERROR("GLAD failed to load GL functions");
        return false;
    }

    LOG_INFO("OpenGL %s", glGetString(GL_VERSION));
    LOG_INFO("Renderer: %s", glGetString(GL_RENDERER));

    // Default state
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);

    // Adaptive vsync: tear on late frames instead of blocking for next vsync
    if (SDL_GL_SetSwapInterval(-1) < 0) {
        SDL_GL_SetSwapInterval(1); // fallback to hard vsync if adaptive not supported
    }

#ifdef ENGINE_DEBUG
    // glDebugMessageCallback requires GL 4.3; macOS caps at 4.1, so guard against null
    if (glDebugMessageCallback) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDebugCallback, nullptr);
        LOG_INFO("GL debug output enabled");
    } else {
        LOG_INFO("GL debug output not available (GL < 4.3)");
    }
#endif

    return true;
}

void GLContext::shutdown() {
    if (s_context) {
        SDL_GL_DeleteContext(s_context);
        s_context = nullptr;
        LOG_INFO("GL context destroyed");
    }
}

void GLContext::swapBuffers(SDL_Window* window) {
    SDL_GL_SwapWindow(window);
}
