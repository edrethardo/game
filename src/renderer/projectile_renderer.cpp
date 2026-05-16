// Instanced projectile renderer — batches projectiles by mesh for minimal draw calls.
// Each unique meshId in the active pool becomes one glDrawElementsInstanced call.
// Per-instance data (model matrix + color) is streamed via a shared instance VBO.

#include "renderer/projectile_renderer.h"
#include "renderer/renderer.h"
#include "renderer/shader.h"
#include "renderer/material.h"
#include "core/log.h"
#include "core/math.h"

#include <glad/glad.h>
#include <cstring>
#include <cmath>

// Per-instance data uploaded to GPU each frame
struct InstanceData {
    f32 modelRow0[4];
    f32 modelRow1[4];
    f32 modelRow2[4];
    f32 modelRow3[4];
    f32 color[4];
};
static_assert(sizeof(InstanceData) == 80, "InstanceData must be 80 bytes");

static constexpr u32 MAX_INSTANCES_PER_BATCH = 4096;

static Shader s_shader;
static u32    s_instanceVBO = 0;

// Scratch buffer for building instance data before upload (avoids per-frame alloc)
static InstanceData s_instanceBuf[MAX_INSTANCES_PER_BATCH];

void ProjectileRenderer::init() {
    s_shader = ShaderSystem::load(
        ASSET_PATH("assets/shaders/projectile.vert"),
        ASSET_PATH("assets/shaders/projectile.frag"));
    if (!s_shader.program) {
        LOG_ERROR("ProjectileRenderer: failed to load instanced shader");
        return;
    }

    // Create instance VBO (orphaned each frame via glBufferData)
    glGenBuffers(1, &s_instanceVBO);
    glBindBuffer(GL_ARRAY_BUFFER, s_instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, MAX_INSTANCES_PER_BATCH * sizeof(InstanceData),
                 nullptr, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    LOG_INFO("ProjectileRenderer: initialized (max %u instances, shader=%u)",
             MAX_INSTANCES_PER_BATCH, s_shader.program);
}

void ProjectileRenderer::shutdown() {
    if (s_instanceVBO) { glDeleteBuffers(1, &s_instanceVBO); s_instanceVBO = 0; }
    ShaderSystem::destroy(s_shader);
}

void ProjectileRenderer::render(const ProjectilePool& pool, const Mat4& vp,
                                 const MeshDef* meshDefs, u32 meshDefCount)
{
    if (!s_shader.program || !s_instanceVBO) return;

    // Group active projectiles by meshId. Use a simple bucket approach:
    // for each meshId, collect instance data into the scratch buffer then draw.
    // Since meshDefCount is small (≤32), iterate mesh IDs and scan pool per ID.
    // This is O(meshDefCount * MAX_PROJECTILES) in the worst case but pool
    // iteration is cache-friendly and meshDefCount is tiny.

    glUseProgram(s_shader.program);

    // Upload shared uniforms: VP matrix + lighting
    glUniformMatrix4fv(s_shader.loc_vp, 1, GL_FALSE, vp.m);
    Vec3 ld = Renderer::getLightDir();
    Vec3 lc = Renderer::getLightColor();
    Vec3 la = Renderer::getAmbientColor();
    glUniform3fv(s_shader.loc_lightDir, 1, (f32*)&ld);
    glUniform3fv(s_shader.loc_lightColor, 1, (f32*)&lc);
    glUniform3fv(s_shader.loc_ambientColor, 1, (f32*)&la);

    // Point lights
    Vec3 plPos[4], plCol[4];
    u32 plCount = Renderer::getPointLights(plPos, plCol);
    glUniform1i(s_shader.loc_pointLightCount, static_cast<s32>(plCount));
    for (u32 i = 0; i < plCount; i++) {
        glUniform3fv(s_shader.loc_pointLightPos[i], 1, (f32*)&plPos[i]);
        glUniform3fv(s_shader.loc_pointLightColor[i], 1, (f32*)&plCol[i]);
    }

    // Texture: use material 0 (default white) — projectile color comes from instance tint
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, MaterialSystem::get(0)->texture.handle);
    glUniform1i(s_shader.loc_texture0, 0);

    // For each mesh ID with active projectiles, batch and draw
    for (u32 mid = 0; mid < meshDefCount; mid++) {
        const Mesh& mesh = meshDefs[mid].mesh;
        if (mesh.vao == 0 || mesh.indexCount == 0) continue;

        // Collect instances for this mesh
        u32 count = 0;
        for (u32 i = 0; i < MAX_PROJECTILES && count < MAX_INSTANCES_PER_BATCH; i++) {
            const Projectile& p = pool.projectiles[i];
            if (!p.active || p.meshId != mid) continue;

            // Build model matrix: translate to position, uniform scale by radius
            f32 s = p.radius * 2.0f; // scale factor (radius → diameter)
            if (s < 0.05f) s = 0.1f; // minimum visible size
            InstanceData& inst = s_instanceBuf[count++];

            // Column-major model matrix (translate + uniform scale)
            inst.modelRow0[0] = s;    inst.modelRow0[1] = 0.0f; inst.modelRow0[2] = 0.0f; inst.modelRow0[3] = 0.0f;
            inst.modelRow1[0] = 0.0f; inst.modelRow1[1] = s;    inst.modelRow1[2] = 0.0f; inst.modelRow1[3] = 0.0f;
            inst.modelRow2[0] = 0.0f; inst.modelRow2[1] = 0.0f; inst.modelRow2[2] = s;    inst.modelRow2[3] = 0.0f;
            inst.modelRow3[0] = p.position.x; inst.modelRow3[1] = p.position.y;
            inst.modelRow3[2] = p.position.z; inst.modelRow3[3] = 1.0f;

            // Color tint based on projFlags
            if (p.projFlags & PROJ_ORB)        { inst.color[0]=0.3f; inst.color[1]=0.8f; inst.color[2]=1.0f; inst.color[3]=0.8f; }
            else if (p.projFlags & PROJ_ORB_SHARD) { inst.color[0]=0.5f; inst.color[1]=0.9f; inst.color[2]=1.0f; inst.color[3]=0.9f; }
            else if (p.projFlags & PROJ_SPARK) { inst.color[0]=0.7f; inst.color[1]=0.8f; inst.color[2]=1.0f; inst.color[3]=1.0f; }
            else if (p.projFlags & PROJ_SPLASH){ inst.color[0]=1.0f; inst.color[1]=0.5f; inst.color[2]=0.2f; inst.color[3]=1.0f; }
            else if (p.projFlags & PROJ_VOID)  { inst.color[0]=0.6f; inst.color[1]=0.2f; inst.color[2]=1.0f; inst.color[3]=1.0f; }
            else                                { inst.color[0]=1.0f; inst.color[1]=1.0f; inst.color[2]=1.0f; inst.color[3]=1.0f; }
        }

        if (count == 0) continue;

        // Upload instance data
        glBindBuffer(GL_ARRAY_BUFFER, s_instanceVBO);
        glBufferData(GL_ARRAY_BUFFER, count * sizeof(InstanceData), s_instanceBuf, GL_STREAM_DRAW);

        // Bind mesh VAO and configure instance attributes
        glBindVertexArray(mesh.vao);

        // Instance attributes at locations 3-7 (model matrix rows + color)
        glBindBuffer(GL_ARRAY_BUFFER, s_instanceVBO);
        for (u32 loc = 3; loc <= 7; loc++) {
            glEnableVertexAttribArray(loc);
            glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, sizeof(InstanceData),
                                  (void*)((loc - 3) * 16));  // 16 bytes per vec4
            glVertexAttribDivisor(loc, 1);  // advance once per instance
        }

        // Draw all instances in one call
        glDrawElementsInstanced(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT,
                                nullptr, count);

        // Clean up divisors so other rendering isn't affected
        for (u32 loc = 3; loc <= 7; loc++) {
            glVertexAttribDivisor(loc, 0);
            glDisableVertexAttribArray(loc);
        }
    }

    glBindVertexArray(0);
    glUseProgram(0);
}
