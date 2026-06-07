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
static s32    s_locGroupTint = -1;  // u_groupTint — per-material-group tint for multi-color meshes

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
    s_locGroupTint = glGetUniformLocation(s_shader.program, "u_groupTint");

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
                                 const MeshDef* meshDefs, u32 meshDefCount,
                                 u8 arrowMeshId, u8 boltMeshId)
{
    if (!s_shader.program || !s_instanceVBO) return;

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
    // Default group tint = identity; per-material-group meshes (arrow/bolt) override it per group.
    if (s_locGroupTint >= 0) glUniform4f(s_locGroupTint, 1.0f, 1.0f, 1.0f, 1.0f);

    // Bucketed single-pass: collect all mesh-based projectiles into per-mesh buckets.
    // Skip meshId 0 (generic cubes → FX path) and FX-flagged projectiles.
    static constexpr u32 MAX_BUCKETS = 64;
    u32 bucketCount[MAX_BUCKETS] = {};
    u32 totalInstances = 0;

    // Early out if no projectiles active
    if (pool.activeCount == 0) { glBindVertexArray(0); glUseProgram(0); return; }

    // Pass 1: count per mesh (scan only up to activeCount found)
    u32 seen = 0;
    for (u32 i = 0; i < MAX_PROJECTILES && seen < pool.activeCount; i++) {
        const Projectile& p = pool.projectiles[i];
        if (!p.active) continue;
        seen++;
        if (p.meshId == 0 || p.meshId >= meshDefCount || p.meshId >= MAX_BUCKETS) continue;
        if (p.projFlags & (PROJ_ORB | PROJ_SPARK | PROJ_SPLASH)) continue;
        if (totalInstances >= MAX_INSTANCES_PER_BATCH) break;
        bucketCount[p.meshId]++;
        totalInstances++;
    }
    if (totalInstances == 0) { glBindVertexArray(0); glUseProgram(0); return; }

    // Prefix sum for bucket start offsets
    u32 bucketStart[MAX_BUCKETS] = {};
    u32 off = 0;
    for (u32 m = 0; m < MAX_BUCKETS; m++) { bucketStart[m] = off; off += bucketCount[m]; }

    // Pass 2: fill instance data with proper rotation + scale
    u32 bucketCur[MAX_BUCKETS] = {};
    seen = 0;
    for (u32 i = 0; i < MAX_PROJECTILES && seen < pool.activeCount; i++) {
        const Projectile& p = pool.projectiles[i];
        if (!p.active) continue;
        seen++;
        if (p.meshId == 0 || p.meshId >= meshDefCount || p.meshId >= MAX_BUCKETS) continue;
        if (p.projFlags & (PROJ_ORB | PROJ_SPARK | PROJ_SPLASH)) continue;
        u32 mid = p.meshId;
        u32 slot = bucketStart[mid] + bucketCur[mid];
        if (slot >= MAX_INSTANCES_PER_BATCH) continue;
        bucketCur[mid]++;

        // Compute scale from mesh bounds (match per-projectile path)
        const AABB& mb = meshDefs[mid].bounds;
        f32 maxDim = fmaxf(fmaxf(mb.max.x - mb.min.x, mb.max.y - mb.min.y),
                           mb.max.z - mb.min.z);
        bool isArrow = (mid == arrowMeshId || mid == boltMeshId);
        f32 projScale = isArrow ? (1.2f / fmaxf(maxDim, 0.001f))
                                : (0.4f / fmaxf(maxDim, 0.001f));

        // Compute rotation from velocity (yaw + pitch for arrows, spin for thrown)
        f32 spd = length(p.velocity);
        f32 flyYaw = atan2f(-p.velocity.x, -p.velocity.z);
        f32 rotX;
        if (isArrow) {
            rotX = (spd > 0.01f) ? -asinf(p.velocity.y / spd) : 0.0f;
        } else {
            rotX = p.lifetime * 15.0f; // spinning throw
        }

        // Build model = translate * rotateY(flyYaw) * rotateX(rotX) * scale
        // Instance rows map to GL columns: row0=col0, row1=col1, row2=col2, row3=col3
        f32 cy = cosf(flyYaw), sy = sinf(flyYaw);
        f32 cx = cosf(rotX),   sx = sinf(rotX);
        f32 s = projScale;

        InstanceData& inst = s_instanceBuf[slot];
        // Column 0
        inst.modelRow0[0] = cy * s;
        inst.modelRow0[1] = 0.0f;
        inst.modelRow0[2] = -sy * s;
        inst.modelRow0[3] = 0.0f;
        // Column 1
        inst.modelRow1[0] = sy * sx * s;
        inst.modelRow1[1] = cx * s;
        inst.modelRow1[2] = cy * sx * s;
        inst.modelRow1[3] = 0.0f;
        // Column 2
        inst.modelRow2[0] = sy * cx * s;
        inst.modelRow2[1] = -sx * s;
        inst.modelRow2[2] = cy * cx * s;
        inst.modelRow2[3] = 0.0f;
        // Column 3 (translation)
        inst.modelRow3[0] = p.position.x;
        inst.modelRow3[1] = p.position.y;
        inst.modelRow3[2] = p.position.z;
        inst.modelRow3[3] = 1.0f;

        // Color tint. Void weapons force a purple glow. Multi-material meshes (arrow/bolt)
        // carry their colors in per-group tints, so use a white instance tint to let those
        // render true; everything else keeps the neutral grey.
        if (p.projFlags & PROJ_VOID) {
            inst.color[0]=0.6f; inst.color[1]=0.2f; inst.color[2]=1.0f; inst.color[3]=1.0f;
        } else if (meshDefs[mid].mesh.materialGroupCount > 0) {
            inst.color[0]=1.0f; inst.color[1]=1.0f; inst.color[2]=1.0f; inst.color[3]=1.0f;
        } else {
            inst.color[0]=0.8f; inst.color[1]=0.8f; inst.color[2]=0.8f; inst.color[3]=1.0f;
        }
    }

    // Single VBO upload for all buckets
    glBindBuffer(GL_ARRAY_BUFFER, s_instanceVBO);
    glBufferData(GL_ARRAY_BUFFER, totalInstances * sizeof(InstanceData), s_instanceBuf, GL_STREAM_DRAW);

    // Draw each mesh bucket
    for (u32 mid = 1; mid < meshDefCount && mid < MAX_BUCKETS; mid++) {
        if (bucketCount[mid] == 0) continue;
        const Mesh& mesh = meshDefs[mid].mesh;
        if (mesh.vao == 0 || mesh.indexCount == 0) continue;

        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_instanceVBO);
        for (u32 loc = 3; loc <= 7; loc++) {
            glEnableVertexAttribArray(loc);
            glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, sizeof(InstanceData),
                                  (void*)(bucketStart[mid] * sizeof(InstanceData) + (loc - 3) * 16));
            glVertexAttribDivisor(loc, 1);
        }

        if (mesh.materialGroupCount > 0) {
            // Multi-color mesh (arrow/bolt): draw each usemtl group with its own texture +
            // tint so the shaft (wood), tip (steel) and fletching (white) read as distinct
            // colors. Each group is a contiguous index sub-range, drawn instanced.
            for (u8 g = 0; g < mesh.materialGroupCount; g++) {
                const MeshMaterialGroup& grp = mesh.materials[g];
                const Material* mat = MaterialSystem::get(grp.materialId);
                glBindTexture(GL_TEXTURE_2D, mat->texture.handle);
                if (s_locGroupTint >= 0)
                    glUniform4f(s_locGroupTint, mat->tint.x, mat->tint.y, mat->tint.z, mat->tint.w);
                glDrawElementsInstanced(GL_TRIANGLES, grp.indexCount, GL_UNSIGNED_INT,
                                        (const void*)(size_t)(grp.indexStart * sizeof(u32)),
                                        bucketCount[mid]);
            }
            // Restore defaults so the next (non-grouped) bucket is unaffected.
            if (s_locGroupTint >= 0) glUniform4f(s_locGroupTint, 1.0f, 1.0f, 1.0f, 1.0f);
            glBindTexture(GL_TEXTURE_2D, MaterialSystem::get(0)->texture.handle);
        } else {
            glDrawElementsInstanced(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT,
                                    nullptr, bucketCount[mid]);
        }

        for (u32 loc = 3; loc <= 7; loc++) {
            glVertexAttribDivisor(loc, 0);
            glDisableVertexAttribArray(loc);
        }
    }

    glBindVertexArray(0);
    glUseProgram(0);
}
