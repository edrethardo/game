#include "renderer/renderer.h"
#include "renderer/shader.h"
#include "renderer/texture.h"
#include "renderer/mesh.h"
#include "renderer/camera.h"
#include "core/log.h"

#include <glad/glad.h>
#include <algorithm>

struct RenderCommand {
    u64           sortKey;      // packed: (shader << 42) | (texture << 21) | mesh
    const Shader* shader;       // pointer to shader (stable lifetime) for cached locs
    u32           texHandle;
    u32           meshVAO;
    u32           meshIndexCount;
    Mat4          mvp;
    Mat4          model;
    Vec4          color;        // per-object tint (passed as u_color if shader has it)
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

static Vec3 s_pointLightPos[4];
static Vec3 s_pointLightColor[4];
static u32  s_pointLightCount = 0;

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

void Renderer::setPointLights(const Vec3* positions, const Vec3* colors, u32 count) {
    // Clamp to the shader's array size; caller is responsible for pre-sorting by distance
    s_pointLightCount = (count > 4) ? 4 : count;
    for (u32 i = 0; i < s_pointLightCount; i++) {
        s_pointLightPos[i]   = positions[i];
        s_pointLightColor[i] = colors[i];
    }
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
    cmd.shader         = &shader;
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
        const Shader& sh = *cmd.shader;

        if (sh.program != boundShader) {
            glUseProgram(sh.program);
            boundShader = sh.program;
            lightSet    = false;
        }

        if (!lightSet) {
            // Light uniforms — use cached locations, zero glGetUniformLocation calls
            if (sh.loc_lightDir >= 0)
                glUniform3f(sh.loc_lightDir, s_lightDir.x, s_lightDir.y, s_lightDir.z);
            if (sh.loc_lightColor >= 0)
                glUniform3f(sh.loc_lightColor, s_lightColor.x, s_lightColor.y, s_lightColor.z);
            if (sh.loc_ambientColor >= 0)
                glUniform3f(sh.loc_ambientColor, s_lightAmbient.x, s_lightAmbient.y, s_lightAmbient.z);

            // Point lights — count is always uploaded so the loop in the shader terminates correctly
            if (sh.loc_pointLightCount >= 0)
                glUniform1i(sh.loc_pointLightCount, static_cast<s32>(s_pointLightCount));
            for (u32 p = 0; p < s_pointLightCount; p++) {
                if (sh.loc_pointLightPos[p] >= 0)
                    glUniform3f(sh.loc_pointLightPos[p],
                                s_pointLightPos[p].x, s_pointLightPos[p].y, s_pointLightPos[p].z);
                if (sh.loc_pointLightColor[p] >= 0)
                    glUniform3f(sh.loc_pointLightColor[p],
                                s_pointLightColor[p].x, s_pointLightColor[p].y, s_pointLightColor[p].z);
            }
            lightSet = true;
        }

        if (cmd.texHandle != boundTex) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, cmd.texHandle);
            boundTex = cmd.texHandle;
            if (sh.loc_texture0 >= 0) glUniform1i(sh.loc_texture0, 0);
        }

        // Per-object uniforms — all cached, zero lookups
        if (sh.loc_mvp >= 0)
            glUniformMatrix4fv(sh.loc_mvp, 1, GL_FALSE, cmd.mvp.ptr());
        if (sh.loc_model >= 0)
            glUniformMatrix4fv(sh.loc_model, 1, GL_FALSE, cmd.model.ptr());
        if (sh.loc_color >= 0)
            glUniform4f(sh.loc_color, cmd.color.x, cmd.color.y, cmd.color.z, cmd.color.w);

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
