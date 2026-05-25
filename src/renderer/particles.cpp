// ParticleSystem — lightweight visual FX pool.
// update() ticks all active particles (gravity, fade, shrink, lifetime).
// render() batches billboard particles into a single draw call using a dynamic
// VBO with per-vertex color (particle.vert/frag shader). Geometric particles
// (cubes) are still submitted individually via Renderer::submit().

#include "renderer/particles.h"
#include "renderer/camera.h"
#include "renderer/shader.h"
#include "renderer/mesh.h"
#include "renderer/material.h"
#include "renderer/renderer.h"
#include "renderer/frustum.h"
#include <glad/glad.h>
#include <cstdlib>
#include <cmath>
#include <cstring>

static f32 randf(f32 lo, f32 hi) {
    return lo + (hi - lo) * (std::rand() / static_cast<f32>(RAND_MAX));
}

static Vec3 randomSpread(Vec3 dir, f32 spread) {
    return {
        dir.x + randf(-spread, spread),
        dir.y + randf(-spread, spread),
        dir.z + randf(-spread, spread)
    };
}

// CPU-side vertex staging buffer for billboard batch (reused each frame)
static ParticleVertex s_billboardVerts[MAX_PARTICLES * 4];

void ParticleSystem::init(ParticlePool& pool) {
    std::memset(&pool, 0, sizeof(pool));
}

void ParticleSystem::initBatchBuffers(ParticlePool& pool) {
    // Create VAO/VBO/IBO for batched billboard particles.
    // VBO is dynamic (updated each frame), IBO is static (quad index pattern).
    glGenVertexArrays(1, &pool.batchVAO);
    glGenBuffers(1, &pool.batchVBO);
    glGenBuffers(1, &pool.batchIBO);

    glBindVertexArray(pool.batchVAO);

    // VBO — allocate for max capacity, filled each frame via glBufferSubData
    glBindBuffer(GL_ARRAY_BUFFER, pool.batchVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 MAX_PARTICLES * 4 * sizeof(ParticleVertex),
                 nullptr, GL_DYNAMIC_DRAW);

    // Vertex layout: position (vec3), color (vec4), uv (vec2) = 36 bytes
    // location 0: position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex),
                          (void*)offsetof(ParticleVertex, position));
    // location 1: color (vec4 — matches particle.vert aColor)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex),
                          (void*)offsetof(ParticleVertex, color));
    // location 2: uv
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex),
                          (void*)offsetof(ParticleVertex, uv));

    // IBO — static quad indices (0,1,2, 2,3,0) repeated for each particle
    u32 indices[MAX_PARTICLES * 6];
    for (u32 i = 0; i < MAX_PARTICLES; i++) {
        u32 base = i * 4;
        u32 idx  = i * 6;
        indices[idx + 0] = base + 0;
        indices[idx + 1] = base + 1;
        indices[idx + 2] = base + 2;
        indices[idx + 3] = base + 2;
        indices[idx + 4] = base + 3;
        indices[idx + 5] = base + 0;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pool.batchIBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 MAX_PARTICLES * 6 * sizeof(u32),
                 indices, GL_STATIC_DRAW);

    glBindVertexArray(0);
}

void ParticleSystem::shutdownBatchBuffers(ParticlePool& pool) {
    if (pool.batchVAO) { glDeleteVertexArrays(1, &pool.batchVAO); pool.batchVAO = 0; }
    if (pool.batchVBO) { glDeleteBuffers(1, &pool.batchVBO);      pool.batchVBO = 0; }
    if (pool.batchIBO) { glDeleteBuffers(1, &pool.batchIBO);      pool.batchIBO = 0; }
}

void ParticleSystem::clear(ParticlePool& pool) {
    for (u32 i = 0; i < MAX_PARTICLES; i++) pool.particles[i].active = false;
    pool.activeCount = 0;
}

bool ParticleSystem::spawn(ParticlePool& pool, const Particle& p) {
    for (u32 i = 0; i < MAX_PARTICLES; i++) {
        if (!pool.particles[i].active) {
            pool.particles[i] = p;
            pool.particles[i].active = true;
            pool.activeCount++;
            return true;
        }
    }
    return false; // pool full — silently drop rather than assert in hot path
}

void ParticleSystem::update(ParticlePool& pool, f32 dt) {
    u32 active = 0;
    for (u32 i = 0; i < MAX_PARTICLES; i++) {
        Particle& p = pool.particles[i];
        if (!p.active) continue;
        p.life -= dt;
        if (p.life <= 0.0f) { p.active = false; continue; }
        p.position = p.position + p.velocity * dt;
        if (p.flags & PFLAG_GRAVITY) p.velocity.y -= 9.8f * dt;
        active++;
    }
    pool.activeCount = active;
}

void ParticleSystem::render(const ParticlePool& pool, const Camera& cam,
                             const Shader& particleShader, const Shader& unlitShader,
                             const Mesh& cubeMesh,
                             u8 blobMaterialId, u8 sparkMaterialId)
{
    if (pool.activeCount == 0) return;

    // Material textures for billboard (blob) and geometric (spark) particles
    const Texture& blobTex  = MaterialSystem::get(blobMaterialId)
                                ? MaterialSystem::get(blobMaterialId)->texture
                                : MaterialSystem::get(0)->texture;
    const Texture& sparkTex = MaterialSystem::get(sparkMaterialId)
                                ? MaterialSystem::get(sparkMaterialId)->texture
                                : MaterialSystem::get(0)->texture;

    // Camera basis for billboard orientation
    Vec3 right = cam.right;
    Vec3 up    = {0.0f, 1.0f, 0.0f};

    // --- Pass 1: Batch all billboard particles into the dynamic VBO ---
    u32 billboardCount = 0;

    for (u32 i = 0; i < MAX_PARTICLES; i++) {
        const Particle& p = pool.particles[i];
        if (!p.active) continue;
        if (p.type != PTYPE_BILLBOARD) continue;

        f32 t     = (p.maxLife > 0.0f) ? (p.life / p.maxLife) : 1.0f;
        f32 alpha = (p.flags & PFLAG_FADE)   ? p.baseAlpha * t : p.baseAlpha;
        f32 sz    = (p.flags & PFLAG_SHRINK) ? p.size * t      : p.size;
        if (sz < 0.001f || alpha < 0.01f) continue;

        Vec4 color = {p.r / 255.0f, p.g / 255.0f, p.b / 255.0f, alpha};

        // Build 4 world-space corners of the camera-facing quad
        Vec3 r_scaled = right * sz;
        Vec3 u_scaled = up * sz;

        u32 base = billboardCount * 4;
        // bottom-left, bottom-right, top-right, top-left
        s_billboardVerts[base + 0] = {p.position - r_scaled - u_scaled, color, {0.0f, 0.0f}};
        s_billboardVerts[base + 1] = {p.position + r_scaled - u_scaled, color, {1.0f, 0.0f}};
        s_billboardVerts[base + 2] = {p.position + r_scaled + u_scaled, color, {1.0f, 1.0f}};
        s_billboardVerts[base + 3] = {p.position - r_scaled + u_scaled, color, {0.0f, 1.0f}};

        billboardCount++;
    }

    // Upload and draw batched billboards in a single draw call
    if (billboardCount > 0 && pool.batchVAO) {
        glUseProgram(particleShader.program);

        // Upload VP matrix (vertices are already in world space, no model transform)
        if (particleShader.loc_vp >= 0)
            glUniformMatrix4fv(particleShader.loc_vp, 1, GL_FALSE, cam.viewProjection.ptr());
        // Fallback: some shader builds may use u_mvp instead of u_vp
        if (particleShader.loc_mvp >= 0)
            glUniformMatrix4fv(particleShader.loc_mvp, 1, GL_FALSE, cam.viewProjection.ptr());

        if (particleShader.loc_texture0 >= 0)
            glUniform1i(particleShader.loc_texture0, 0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, blobTex.handle);

        // Enable alpha blending for transparent particles
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE); // don't write to depth buffer for transparent particles

        glBindVertexArray(pool.batchVAO);
        glBindBuffer(GL_ARRAY_BUFFER, pool.batchVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        billboardCount * 4 * sizeof(ParticleVertex),
                        s_billboardVerts);

        glDrawElements(GL_TRIANGLES,
                       billboardCount * 6,
                       GL_UNSIGNED_INT, nullptr);

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    // --- Pass 2: Geometric (cube) particles — still individual submits ---
    // These are rare (blood, sparks, debris) and use the shared cube mesh.
    for (u32 i = 0; i < MAX_PARTICLES; i++) {
        const Particle& p = pool.particles[i];
        if (!p.active) continue;
        if (p.type != PTYPE_GEOMETRIC) continue;

        f32 t     = (p.maxLife > 0.0f) ? (p.life / p.maxLife) : 1.0f;
        f32 alpha = (p.flags & PFLAG_FADE)   ? p.baseAlpha * t : p.baseAlpha;
        f32 sz    = (p.flags & PFLAG_SHRINK) ? p.size * t      : p.size;
        if (sz < 0.001f || alpha < 0.01f) continue;

        Vec4 color = {p.r / 255.0f, p.g / 255.0f, p.b / 255.0f, alpha};
        AABB bounds;
        bounds.min = p.position - Vec3{sz, sz, sz};
        bounds.max = p.position + Vec3{sz, sz, sz};

        Mat4 model = Mat4::translate(p.position) * Mat4::scale({sz, sz, sz});
        Renderer::submit(unlitShader, sparkTex, cubeMesh, model, bounds, color);
    }
}

// ---------------------------------------------------------------------------
// Emitter presets — fire-and-forget convenience wrappers
// ---------------------------------------------------------------------------

void ParticleSystem::spawnBlood(ParticlePool& pool, Vec3 pos, Vec3 dir, u8 count) {
    for (u8 i = 0; i < count; i++) {
        Particle p = {};
        p.position = pos;
        p.velocity = randomSpread(dir * 2.0f, 1.5f);
        p.life = randf(0.3f, 0.6f); p.maxLife = p.life;
        p.size = randf(0.03f, 0.06f); p.baseAlpha = 0.9f;
        // Ash grey to match the spark/smoke particles — no red blood/gore.
        u8 grey = static_cast<u8>(randf(150, 200));
        p.r = grey; p.g = grey; p.b = grey;
        p.type  = PTYPE_GEOMETRIC;
        p.flags = PFLAG_GRAVITY;
        spawn(pool, p);
    }
}

void ParticleSystem::spawnSparks(ParticlePool& pool, Vec3 pos, Vec3 dir, u8 count) {
    for (u8 i = 0; i < count; i++) {
        Particle p = {};
        p.position = pos;
        p.velocity = randomSpread(dir * 4.0f, 2.0f);
        p.life = randf(0.2f, 0.4f); p.maxLife = p.life;
        p.size = randf(0.02f, 0.04f); p.baseAlpha = 1.0f;
        u8 grey = static_cast<u8>(randf(160, 210));
        p.r = grey; p.g = grey; p.b = grey;
        p.type  = PTYPE_GEOMETRIC;
        p.flags = PFLAG_GRAVITY | PFLAG_FADE;
        spawn(pool, p);
    }
}

void ParticleSystem::spawnMagicBurst(ParticlePool& pool, Vec3 pos, u8 r, u8 g, u8 b, u8 count) {
    for (u8 i = 0; i < count; i++) {
        Particle p = {};
        p.position = pos;
        p.velocity = {randf(-1.5f, 1.5f), randf(-1.5f, 1.5f), randf(-1.5f, 1.5f)};
        p.life = randf(0.4f, 0.8f); p.maxLife = p.life;
        p.size = randf(0.08f, 0.15f); p.baseAlpha = 0.8f;
        p.r = r; p.g = g; p.b = b;
        p.type  = PTYPE_BILLBOARD;
        p.flags = PFLAG_FADE | PFLAG_SHRINK;
        spawn(pool, p);
    }
}

void ParticleSystem::spawnSmoke(ParticlePool& pool, Vec3 pos, u8 count) {
    for (u8 i = 0; i < count; i++) {
        Particle p = {};
        p.position = pos + Vec3{randf(-0.2f, 0.2f), 0.0f, randf(-0.2f, 0.2f)};
        p.velocity = {randf(-0.1f, 0.1f), randf(0.3f, 0.8f), randf(-0.1f, 0.1f)};
        p.life = randf(0.6f, 1.2f); p.maxLife = p.life;
        p.size = randf(0.1f, 0.2f); p.baseAlpha = 0.5f;
        u8 grey = static_cast<u8>(randf(120, 180));
        p.r = grey; p.g = grey; p.b = grey;
        p.type  = PTYPE_BILLBOARD;
        p.flags = PFLAG_FADE;
        spawn(pool, p);
    }
}

void ParticleSystem::spawnExplosion(ParticlePool& pool, Vec3 pos, f32 radius) {
    // 12 fiery debris chunks
    for (u8 i = 0; i < 12; i++) {
        Particle p = {};
        p.position = pos;
        Vec3 dir = {randf(-1.0f, 1.0f), randf(-1.0f, 1.0f), randf(-1.0f, 1.0f)};
        p.velocity = dir * (radius * 3.0f);
        p.life = randf(0.2f, 0.5f); p.maxLife = p.life;
        p.size = randf(0.02f, 0.05f); p.baseAlpha = 1.0f;
        p.r = 255;
        p.g = static_cast<u8>(randf(100, 200));
        p.b = 0;
        p.type  = PTYPE_GEOMETRIC;
        p.flags = PFLAG_GRAVITY | PFLAG_FADE;
        spawn(pool, p);
    }
    spawnSmoke(pool, pos, 6);
}

void ParticleSystem::spawnDebris(ParticlePool& pool, Vec3 pos, u8 count) {
    for (u8 i = 0; i < count; i++) {
        Particle p = {};
        p.position = pos;
        p.velocity = {randf(-2.0f, 2.0f), randf(1.0f, 3.0f), randf(-2.0f, 2.0f)};
        p.life = randf(0.4f, 0.8f); p.maxLife = p.life;
        p.size = randf(0.03f, 0.06f); p.baseAlpha = 0.9f;
        p.r = static_cast<u8>(randf(100, 160));
        p.g = static_cast<u8>(randf(80, 130));
        p.b = static_cast<u8>(randf(60, 100));
        p.type  = PTYPE_GEOMETRIC;
        p.flags = PFLAG_GRAVITY;
        spawn(pool, p);
    }
}
