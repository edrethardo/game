// ParticleSystem — lightweight visual FX pool.
// update() ticks all active particles (gravity, fade, shrink, lifetime).
// render() submits each particle via Renderer::submit() — billboard quads face
// the camera, geometric cubes use the existing cube mesh. Emitter presets
// (spawnBlood, spawnSparks, etc.) are fire-and-forget convenience functions.

#include "renderer/particles.h"
#include "renderer/camera.h"
#include "renderer/shader.h"
#include "renderer/mesh.h"
#include "renderer/material.h"
#include "renderer/renderer.h"
#include "renderer/frustum.h"
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

void ParticleSystem::init(ParticlePool& pool) {
    std::memset(&pool, 0, sizeof(pool));
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
                             const Shader& unlitShader, const Mesh& cubeMesh,
                             u8 blobMaterialId, u8 sparkMaterialId)
{
    if (pool.activeCount == 0) return;

    // Fallback to material 0 if the particle materials aren't loaded yet
    const Texture& blobTex  = MaterialSystem::get(blobMaterialId)
                                ? MaterialSystem::get(blobMaterialId)->texture
                                : MaterialSystem::get(0)->texture;
    const Texture& sparkTex = MaterialSystem::get(sparkMaterialId)
                                ? MaterialSystem::get(sparkMaterialId)->texture
                                : MaterialSystem::get(0)->texture;

    for (u32 i = 0; i < MAX_PARTICLES; i++) {
        const Particle& p = pool.particles[i];
        if (!p.active) continue;

        f32 t     = (p.maxLife > 0.0f) ? (p.life / p.maxLife) : 1.0f;
        f32 alpha = (p.flags & PFLAG_FADE)   ? p.baseAlpha * t : p.baseAlpha;
        f32 sz    = (p.flags & PFLAG_SHRINK) ? p.size * t      : p.size;
        if (sz < 0.001f || alpha < 0.01f) continue;

        Vec4 color = {p.r / 255.0f, p.g / 255.0f, p.b / 255.0f, alpha};
        AABB bounds;
        bounds.min = p.position - Vec3{sz, sz, sz};
        bounds.max = p.position + Vec3{sz, sz, sz};

        if (p.type == PTYPE_BILLBOARD) {
            // Camera-facing billboard — build orientation from camera right + world up
            Vec3 right = cam.right;
            Vec3 up    = {0.0f, 1.0f, 0.0f};
            Vec3 look  = cross(right, up);
            Mat4 billboard = Mat4::identity();
            billboard.m[0]  = right.x * sz; billboard.m[1]  = right.y * sz; billboard.m[2]  = right.z * sz;
            billboard.m[4]  = up.x    * sz; billboard.m[5]  = up.y    * sz; billboard.m[6]  = up.z    * sz;
            billboard.m[8]  = look.x  * sz; billboard.m[9]  = look.y  * sz; billboard.m[10] = look.z  * sz;
            billboard.m[12] = p.position.x;
            billboard.m[13] = p.position.y;
            billboard.m[14] = p.position.z;
            Renderer::submit(unlitShader, blobTex, cubeMesh, billboard, bounds, color);
        } else {
            // Geometric: axis-aligned spinning cube
            Mat4 model = Mat4::translate(p.position) * Mat4::scale({sz, sz, sz});
            Renderer::submit(unlitShader, sparkTex, cubeMesh, model, bounds, color);
        }
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
        p.r = static_cast<u8>(randf(180, 220));
        p.g = static_cast<u8>(randf(0, 30));
        p.b = static_cast<u8>(randf(0, 20));
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
