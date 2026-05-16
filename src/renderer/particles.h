#pragma once
// Particle pool — lightweight visual FX for blood, sparks, magic, smoke, debris.
// Static array of MAX_PARTICLES particles, rendered via Renderer::submit() each frame.
// Billboard particles use a camera-facing quad; geometric particles use the cube mesh.

#include "core/types.h"
#include "core/math.h"

constexpr u32 MAX_PARTICLES = 256;

enum ParticleType : u8 {
    PTYPE_BILLBOARD = 0,  // camera-facing quad (smoke, magic glow)
    PTYPE_GEOMETRIC = 1,  // tiny colored cube (sparks, blood, debris)
};

enum ParticleFlags : u8 {
    PFLAG_GRAVITY = 1 << 0,
    PFLAG_FADE    = 1 << 1,
    PFLAG_SHRINK  = 1 << 2,
};

struct Particle {
    Vec3 position;
    Vec3 velocity;
    f32  life;        // counts down to 0
    f32  maxLife;     // initial lifetime (for fade/shrink ratio)
    f32  size;        // world-space half-extent
    f32  baseAlpha;   // starting alpha (0-1)
    u8   r, g, b;    // vertex color RGB
    u8   type;        // ParticleType
    u8   flags;       // ParticleFlags bitmask
    bool active;
};

struct ParticlePool {
    Particle particles[MAX_PARTICLES];
    u32 activeCount;  // cached for fast skip in render
};

struct Camera;
struct Shader;
struct Mesh;

namespace ParticleSystem {
    void init(ParticlePool& pool);
    void update(ParticlePool& pool, f32 dt);
    void render(const ParticlePool& pool, const Camera& cam,
                const Shader& unlitShader, const Mesh& cubeMesh,
                u8 blobMaterialId, u8 sparkMaterialId);
    void clear(ParticlePool& pool);

    bool spawn(ParticlePool& pool, const Particle& p);

    void spawnBlood(ParticlePool& pool, Vec3 pos, Vec3 dir, u8 count);
    void spawnSparks(ParticlePool& pool, Vec3 pos, Vec3 dir, u8 count);
    void spawnMagicBurst(ParticlePool& pool, Vec3 pos, u8 r, u8 g, u8 b, u8 count);
    void spawnSmoke(ParticlePool& pool, Vec3 pos, u8 count);
    void spawnExplosion(ParticlePool& pool, Vec3 pos, f32 radius);
    void spawnDebris(ParticlePool& pool, Vec3 pos, u8 count);
}
