#include "renderer/renderer.h"
#include "renderer/shader.h"
#include "renderer/texture.h"
#include "renderer/mesh.h"
#include "renderer/camera.h"
#include "core/log.h"

#include <glad/glad.h>
#include <algorithm>

struct RenderCommand {
    u64       sortKey;      // packed: (shader << 42) | (texture << 21) | mesh
    u32       shaderProg;
    u32       texHandle;
    u32       meshVAO;
    u32       meshIndexCount;
    Mat4      mvp;
    Mat4      model;
    Vec4      color;        // per-object tint (passed as u_color if shader has it)
};

static constexpr u32 MAX_COMMANDS = 4096;

static RenderCommand s_commands[MAX_COMMANDS];
static u32           s_commandCount = 0;
static u32           s_drawCalls    = 0;
static u32           s_visible      = 0;
static u32           s_submitted    = 0;

static Mat4    s_viewProjection;
static Frustum s_frustum;

static Vec3 s_lightDir     = normalize(Vec3{-0.3f, -1.0f, -0.5f});
static Vec3 s_lightColor   = {1.0f, 0.95f, 0.9f};
static Vec3 s_lightAmbient = {0.15f, 0.15f, 0.2f};

void Renderer::init() {
    LOG_INFO("Renderer initialized (MAX_COMMANDS=%u)", MAX_COMMANDS);
}

void Renderer::shutdown() {
    LOG_INFO("Renderer shut down");
}

void Renderer::setDirectionalLight(Vec3 direction, Vec3 color, Vec3 ambient) {
    s_lightDir     = normalize(direction);
    s_lightColor   = color;
    s_lightAmbient = ambient;
}

void Renderer::beginFrame(const Camera& camera) {
    s_commandCount = 0;
    s_drawCalls    = 0;
    s_visible      = 0;
    s_submitted    = 0;
    s_viewProjection = camera.viewProjection;
    s_frustum = extractFrustum(s_viewProjection);
}

void Renderer::submit(const Shader& shader, const Texture& texture,
                      const Mesh& mesh, const Mat4& modelMatrix,
                      const AABB& bounds, Vec4 color) {
    s_submitted++;

    if (!isVisible(s_frustum, bounds)) return;
    if (s_commandCount >= MAX_COMMANDS) {
        LOG_WARN("Renderer: MAX_COMMANDS reached, skipping object");
        return;
    }

    s_visible++;
    RenderCommand& cmd = s_commands[s_commandCount++];

    // Pack sort key: upper 21 bits = shader, middle 21 = texture, lower 22 = mesh VAO
    cmd.sortKey   = (static_cast<u64>(shader.program & 0x1FFFFF) << 42)
                  | (static_cast<u64>(texture.handle & 0x1FFFFF) << 21)
                  | (static_cast<u64>(mesh.vao       & 0x3FFFFF));
    cmd.shaderProg     = shader.program;
    cmd.texHandle      = texture.handle;
    cmd.meshVAO        = mesh.vao;
    cmd.meshIndexCount = mesh.indexCount;
    cmd.model          = modelMatrix;
    cmd.mvp            = s_viewProjection * modelMatrix;
    cmd.color          = color;
}

void Renderer::flush() {
    // Sort to minimise state changes
    std::sort(s_commands, s_commands + s_commandCount,
              [](const RenderCommand& a, const RenderCommand& b) {
                  return a.sortKey < b.sortKey;
              });

    u32 boundShader  = 0;
    u32 boundTex     = 0;
    u32 boundVAO     = 0;
    bool lightSet    = false;

    for (u32 i = 0; i < s_commandCount; i++) {
        const RenderCommand& cmd = s_commands[i];

        if (cmd.shaderProg != boundShader) {
            glUseProgram(cmd.shaderProg);
            boundShader = cmd.shaderProg;
            lightSet    = false; // re-set light uniforms for new shader
        }

        if (!lightSet) {
            // Light uniforms (same for all objects, set once per shader bind)
            s32 locLD = glGetUniformLocation(cmd.shaderProg, "u_lightDir");
            s32 locLC = glGetUniformLocation(cmd.shaderProg, "u_lightColor");
            s32 locAC = glGetUniformLocation(cmd.shaderProg, "u_ambientColor");
            if (locLD >= 0) glUniform3f(locLD, s_lightDir.x, s_lightDir.y, s_lightDir.z);
            if (locLC >= 0) glUniform3f(locLC, s_lightColor.x, s_lightColor.y, s_lightColor.z);
            if (locAC >= 0) glUniform3f(locAC, s_lightAmbient.x, s_lightAmbient.y, s_lightAmbient.z);
            lightSet = true;
        }

        if (cmd.texHandle != boundTex) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, cmd.texHandle);
            boundTex = cmd.texHandle;
            s32 locTex = glGetUniformLocation(cmd.shaderProg, "u_texture0");
            if (locTex >= 0) glUniform1i(locTex, 0);
        }

        // Per-object uniforms
        s32 locMVP   = glGetUniformLocation(cmd.shaderProg, "u_mvp");
        s32 locModel = glGetUniformLocation(cmd.shaderProg, "u_model");
        s32 locColor = glGetUniformLocation(cmd.shaderProg, "u_color");
        if (locMVP   >= 0) glUniformMatrix4fv(locMVP,   1, GL_FALSE, cmd.mvp.ptr());
        if (locModel >= 0) glUniformMatrix4fv(locModel, 1, GL_FALSE, cmd.model.ptr());
        if (locColor >= 0) glUniform4f(locColor, cmd.color.x, cmd.color.y, cmd.color.z, cmd.color.w);

        if (cmd.meshVAO != boundVAO) {
            glBindVertexArray(cmd.meshVAO);
            boundVAO = cmd.meshVAO;
        }

        glDrawElements(GL_TRIANGLES, cmd.meshIndexCount, GL_UNSIGNED_INT, 0);
        s_drawCalls++;
    }
}

u32 Renderer::getDrawCallCount()  { return s_drawCalls; }
u32 Renderer::getVisibleCount()   { return s_visible; }
u32 Renderer::getTotalSubmitted() { return s_submitted; }
