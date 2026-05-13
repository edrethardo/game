# Particle System + Screen Shake Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a lightweight particle system (blood, sparks, magic, smoke, debris) and screen shake to give combat visual feedback/"juice", while staying within the Switch performance budget.

**Architecture:** Particles are a static pool of 128 structs in the engine's existing `EffectsState`, rendered via the existing `Renderer::submit()` path (one draw call per particle using the cube mesh for geo particles and a billboard quad for billboard particles). Screen shake is a simple struct on Camera with intensity + decay. Both integrate at known combat/skill trigger points.

**Tech Stack:** C++17, OpenGL 3.3, existing Renderer::submit() pipeline

**Spec:** `docs/superpowers/specs/2026-05-13-particles-screenshake-design.md`

---

### Task 1: Generate particle textures and add materials

**Files:**
- Modify: `tools/gen_texture.py`
- Modify: `assets/materials.json`
- Generated: `assets/textures/particle_blob_16.png`, `assets/textures/particle_spark_16.png`

- [ ] **Step 1: Add particle texture types to gen_texture.py**

Add these two generator functions before the `TEXTURE_TYPES` dict at the end of `gen_texture.py`:

```python
def generate_particle_blob(size, seed, palette_name):
    """Soft circle with radial alpha falloff — used for billboard smoke/magic particles."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    cx, cy = size / 2.0, size / 2.0
    radius = size / 2.0
    for y in range(size):
        for x in range(size):
            dx = x + 0.5 - cx
            dy = y + 0.5 - cy
            dist = math.sqrt(dx * dx + dy * dy)
            t = dist / radius
            alpha = max(0.0, 1.0 - t * t)  # quadratic falloff
            a = int(alpha * 255)
            img.putpixel((x, y), (255, 255, 255, a))
    return img


def generate_particle_spark(size, seed, palette_name):
    """Elongated diamond/star — used for directional spark/impact billboards."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    cx, cy = size / 2.0, size / 2.0
    radius = size / 2.0
    for y in range(size):
        for x in range(size):
            dx = abs(x + 0.5 - cx) / radius
            dy = abs(y + 0.5 - cy) / radius
            # Diamond shape: heavier on horizontal axis
            t = dx * 0.6 + dy * 1.4
            alpha = max(0.0, 1.0 - t)
            a = int(alpha * 255)
            img.putpixel((x, y), (255, 255, 255, a))
    return img
```

Then add to the `TEXTURE_TYPES` dict:

```python
"particle_blob": generate_particle_blob,
"particle_spark": generate_particle_spark,
```

- [ ] **Step 2: Generate the textures**

```bash
cd /home/aaron/game
python3 tools/gen_texture.py --type particle_blob --size 16 --seed 1 --output assets/textures/particle_blob_16.png
python3 tools/gen_texture.py --type particle_spark --size 16 --seed 1 --output assets/textures/particle_spark_16.png
```

Verify files exist and are 16x16 RGBA PNGs.

- [ ] **Step 3: Add material entries to materials.json**

Add two entries at the end of the materials array (after the last entry, currently ID 121). The IDs must equal their array index:

```json
{ "id": 122, "name": "particle_blob", "texture": "textures/particle_blob_16.png", "tint": [1,1,1,1] },
{ "id": 123, "name": "particle_spark", "texture": "textures/particle_spark_16.png", "tint": [1,1,1,1] }
```

- [ ] **Step 4: Build and verify materials load**

```bash
cmake --build build 2>&1 | tail -5
timeout 3 ./build/src/DungeonEngine 2>&1 | grep -i "material\|particle" | head -5 || true
```

Expected: no errors loading the new materials.

- [ ] **Step 5: Commit**

```bash
git add tools/gen_texture.py assets/textures/particle_blob_16.png assets/textures/particle_spark_16.png assets/materials.json
git commit -m "add particle textures and materials (blob + spark, 16x16)"
```

---

### Task 2: Add particle pool and screen shake structs

**Files:**
- Create: `src/renderer/particles.h`
- Modify: `src/renderer/camera.h`
- Modify: `src/engine/engine.h`

- [ ] **Step 1: Create particles.h**

```cpp
#pragma once
// Particle pool — lightweight visual FX for blood, sparks, magic, smoke, debris.
// Static array of MAX_PARTICLES particles, rendered via Renderer::submit() each frame.
// Billboard particles use a camera-facing quad; geometric particles use the cube mesh.

#include "core/types.h"
#include "core/math.h"

constexpr u32 MAX_PARTICLES = 128;

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

    // Spawn a single particle. Returns false if pool is full.
    bool spawn(ParticlePool& pool, const Particle& p);

    // Emitter presets — fire-and-forget, spawn N particles with themed params
    void spawnBlood(ParticlePool& pool, Vec3 pos, Vec3 dir, u8 count);
    void spawnSparks(ParticlePool& pool, Vec3 pos, Vec3 dir, u8 count);
    void spawnMagicBurst(ParticlePool& pool, Vec3 pos, u8 r, u8 g, u8 b, u8 count);
    void spawnSmoke(ParticlePool& pool, Vec3 pos, u8 count);
    void spawnExplosion(ParticlePool& pool, Vec3 pos, f32 radius);
    void spawnDebris(ParticlePool& pool, Vec3 pos, u8 count);
}
```

- [ ] **Step 2: Add ScreenShake to camera.h**

Add after the Camera struct definition in `src/renderer/camera.h`:

```cpp
struct ScreenShake {
    f32 intensity = 0.0f;   // current amplitude (world units)
    f32 decay     = 0.0f;   // decay rate (units/sec)
    f32 frequency = 25.0f;  // oscillation speed (Hz)
    f32 timer     = 0.0f;   // phase accumulator

    void trigger(f32 newIntensity, f32 duration) {
        // Take the stronger of current vs new
        if (newIntensity > intensity) {
            intensity = newIntensity;
            decay = newIntensity / duration;
        }
    }

    Vec3 update(f32 dt) {
        if (intensity <= 0.0f) return {0, 0, 0};
        timer += dt * frequency;
        intensity -= decay * dt;
        if (intensity < 0.0f) intensity = 0.0f;
        // Different frequencies per axis to avoid simple oscillation
        f32 ox = sinf(timer * 1.0f) * intensity;
        f32 oy = sinf(timer * 1.3f) * intensity;
        f32 oz = sinf(timer * 0.7f) * intensity;
        return {ox, oy, oz};
    }
};
```

Also add a `ScreenShake shake;` member to the Camera struct.

- [ ] **Step 3: Add ParticlePool and material IDs to engine.h**

Add `#include "renderer/particles.h"` to the includes in `engine.h`.

Add to the Engine class private members (near the other FX pools):

```cpp
ParticlePool m_particles;
u8 m_particleBlobMatId  = 0;
u8 m_particleSparkMatId = 0;
```

- [ ] **Step 4: Build to verify headers compile**

```bash
cmake --build build 2>&1 | tail -5
```

Expected: compiles (particles.cpp doesn't exist yet, but the header is only included).

- [ ] **Step 5: Commit**

```bash
git add src/renderer/particles.h src/renderer/camera.h src/engine/engine.h
git commit -m "add particle pool and screen shake structs"
```

---

### Task 3: Implement particle system (update + render + presets)

**Files:**
- Create: `src/renderer/particles.cpp`
- Modify: `src/CMakeLists.txt` (add particles.cpp to build)

- [ ] **Step 1: Create particles.cpp**

```cpp
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

// Random float in [lo, hi]
static f32 randf(f32 lo, f32 hi) {
    return lo + (hi - lo) * (std::rand() / static_cast<f32>(RAND_MAX));
}

// Random unit-ish direction with spread around `dir`
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
    return false; // pool full
}

void ParticleSystem::update(ParticlePool& pool, f32 dt) {
    u32 active = 0;
    for (u32 i = 0; i < MAX_PARTICLES; i++) {
        Particle& p = pool.particles[i];
        if (!p.active) continue;

        p.life -= dt;
        if (p.life <= 0.0f) {
            p.active = false;
            continue;
        }

        p.position = p.position + p.velocity * dt;

        if (p.flags & PFLAG_GRAVITY) {
            p.velocity.y -= 9.8f * dt;
        }

        // fade and shrink are applied at render time from life/maxLife ratio
        active++;
    }
    pool.activeCount = active;
}

void ParticleSystem::render(const ParticlePool& pool, const Camera& cam,
                             const Shader& unlitShader, const Mesh& cubeMesh,
                             u8 blobMaterialId, u8 sparkMaterialId) {
    if (pool.activeCount == 0) return;

    for (u32 i = 0; i < MAX_PARTICLES; i++) {
        const Particle& p = pool.particles[i];
        if (!p.active) continue;

        f32 t = (p.maxLife > 0.0f) ? (p.life / p.maxLife) : 1.0f;
        f32 alpha = (p.flags & PFLAG_FADE)   ? p.baseAlpha * t : p.baseAlpha;
        f32 sz    = (p.flags & PFLAG_SHRINK) ? p.size * t      : p.size;
        if (sz < 0.001f || alpha < 0.01f) continue;

        Vec4 color = {p.r / 255.0f, p.g / 255.0f, p.b / 255.0f, alpha};
        Mat4 model = Mat4::translate(p.position) * Mat4::scale({sz, sz, sz});

        // Simple AABB for frustum culling
        AABB bounds;
        bounds.min = p.position - Vec3{sz, sz, sz};
        bounds.max = p.position + Vec3{sz, sz, sz};

        if (p.type == PTYPE_BILLBOARD) {
            // Billboard: use camera-facing rotation
            // Build billboard matrix from camera right/up vectors
            Vec3 right = cam.right;
            Vec3 up = {0, 1, 0};
            Vec3 look = Vec3::cross(right, up);
            Mat4 billboard = Mat4::identity();
            billboard.m[0] = right.x * sz; billboard.m[1] = right.y * sz; billboard.m[2]  = right.z * sz;
            billboard.m[4] = up.x * sz;    billboard.m[5] = up.y * sz;    billboard.m[6]  = up.z * sz;
            billboard.m[8] = look.x * sz;  billboard.m[9] = look.y * sz;  billboard.m[10] = look.z * sz;
            billboard.m[12] = p.position.x; billboard.m[13] = p.position.y; billboard.m[14] = p.position.z;

            const Texture& tex = MaterialSystem::getTexture(blobMaterialId);
            Renderer::submit(unlitShader, tex, cubeMesh, billboard, bounds, color);
        } else {
            // Geometric: simple scaled cube, no texture needed
            const Texture& tex = MaterialSystem::getTexture(0); // default fallback
            Renderer::submit(unlitShader, tex, cubeMesh, model, bounds, color);
        }
    }
}

// ---------------------------------------------------------------------------
// Emitter presets
// ---------------------------------------------------------------------------

void ParticleSystem::spawnBlood(ParticlePool& pool, Vec3 pos, Vec3 dir, u8 count) {
    for (u8 i = 0; i < count; i++) {
        Particle p = {};
        p.position  = pos;
        p.velocity  = randomSpread(dir * 2.0f, 1.5f);
        p.life      = randf(0.3f, 0.6f);
        p.maxLife   = p.life;
        p.size      = randf(0.03f, 0.06f);
        p.baseAlpha = 0.9f;
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
        p.position  = pos;
        p.velocity  = randomSpread(dir * 4.0f, 2.0f);
        p.life      = randf(0.2f, 0.4f);
        p.maxLife   = p.life;
        p.size      = randf(0.02f, 0.04f);
        p.baseAlpha = 1.0f;
        p.r = 255;
        p.g = static_cast<u8>(randf(180, 255));
        p.b = static_cast<u8>(randf(0, 80));
        p.type  = PTYPE_GEOMETRIC;
        p.flags = PFLAG_GRAVITY | PFLAG_FADE;
        spawn(pool, p);
    }
}

void ParticleSystem::spawnMagicBurst(ParticlePool& pool, Vec3 pos, u8 r, u8 g, u8 b, u8 count) {
    for (u8 i = 0; i < count; i++) {
        Particle p = {};
        p.position  = pos;
        p.velocity  = {randf(-1.5f, 1.5f), randf(-1.5f, 1.5f), randf(-1.5f, 1.5f)};
        p.life      = randf(0.4f, 0.8f);
        p.maxLife   = p.life;
        p.size      = randf(0.08f, 0.15f);
        p.baseAlpha = 0.8f;
        p.r = r; p.g = g; p.b = b;
        p.type  = PTYPE_BILLBOARD;
        p.flags = PFLAG_FADE | PFLAG_SHRINK;
        spawn(pool, p);
    }
}

void ParticleSystem::spawnSmoke(ParticlePool& pool, Vec3 pos, u8 count) {
    for (u8 i = 0; i < count; i++) {
        Particle p = {};
        p.position  = pos + Vec3{randf(-0.2f, 0.2f), 0.0f, randf(-0.2f, 0.2f)};
        p.velocity  = {randf(-0.1f, 0.1f), randf(0.3f, 0.8f), randf(-0.1f, 0.1f)};
        p.life      = randf(0.6f, 1.2f);
        p.maxLife   = p.life;
        p.size      = randf(0.1f, 0.2f);
        p.baseAlpha = 0.5f;
        p.r = p.g = p.b = static_cast<u8>(randf(120, 180));
        p.type  = PTYPE_BILLBOARD;
        p.flags = PFLAG_FADE;
        spawn(pool, p);
    }
}

void ParticleSystem::spawnExplosion(ParticlePool& pool, Vec3 pos, f32 radius) {
    // Sparks burst outward
    for (u8 i = 0; i < 12; i++) {
        Particle p = {};
        p.position  = pos;
        Vec3 dir = {randf(-1, 1), randf(-1, 1), randf(-1, 1)};
        p.velocity  = dir * (radius * 3.0f);
        p.life      = randf(0.2f, 0.5f);
        p.maxLife   = p.life;
        p.size      = randf(0.02f, 0.05f);
        p.baseAlpha = 1.0f;
        p.r = 255; p.g = static_cast<u8>(randf(100, 200)); p.b = 0;
        p.type  = PTYPE_GEOMETRIC;
        p.flags = PFLAG_GRAVITY | PFLAG_FADE;
        spawn(pool, p);
    }
    // Smoke cloud
    spawnSmoke(pool, pos, 6);
}

void ParticleSystem::spawnDebris(ParticlePool& pool, Vec3 pos, u8 count) {
    for (u8 i = 0; i < count; i++) {
        Particle p = {};
        p.position  = pos;
        p.velocity  = {randf(-2, 2), randf(1, 3), randf(-2, 2)};
        p.life      = randf(0.4f, 0.8f);
        p.maxLife   = p.life;
        p.size      = randf(0.03f, 0.06f);
        p.baseAlpha = 0.9f;
        p.r = static_cast<u8>(randf(100, 160));
        p.g = static_cast<u8>(randf(80, 130));
        p.b = static_cast<u8>(randf(60, 100));
        p.type  = PTYPE_GEOMETRIC;
        p.flags = PFLAG_GRAVITY;
        spawn(pool, p);
    }
}
```

- [ ] **Step 2: Add particles.cpp to CMakeLists.txt**

In `src/CMakeLists.txt`, find the renderer source files section and add `renderer/particles.cpp` to the list.

- [ ] **Step 3: Build**

```bash
cmake --build build 2>&1 | tail -5
```

Expected: compiles with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/renderer/particles.cpp src/CMakeLists.txt
git commit -m "implement particle system: update, render, emitter presets"
```

---

### Task 4: Wire particles + shake into engine init/update/render

**Files:**
- Modify: `src/engine/engine_init.cpp` (init + resolve material IDs)
- Modify: `src/engine/engine_update.cpp` (call ParticleSystem::update)
- Modify: `src/engine/engine_render.cpp` (apply shake + call ParticleSystem::render)
- Modify: `src/engine/engine_startgame.cpp` (clear particles on floor transition)

- [ ] **Step 1: Init particles and resolve material IDs in engine_init.cpp**

In `Engine::init()`, after `MaterialSystem::init(...)` completes, add:

```cpp
ParticleSystem::init(m_particles);
m_particleBlobMatId  = MaterialSystem::getIdByName("particle_blob");
m_particleSparkMatId = MaterialSystem::getIdByName("particle_spark");
```

Add `#include "renderer/particles.h"` at the top if not already included via engine.h.

- [ ] **Step 2: Update particles in engine_update.cpp**

In the `gameUpdate()` function (or `singleplayerUpdate()`), after the existing FX timer updates and before `PlayerController::applyToCamera`, add:

```cpp
ParticleSystem::update(m_particles, dt);
```

- [ ] **Step 3: Apply screen shake and render particles in engine_render.cpp**

In `Engine::render()`, after the camera interpolation block (around line 481, after `m_camera.pitch = ...`), add:

```cpp
// Apply screen shake — purely visual, doesn't affect gameplay position
Vec3 shakeOffset = m_camera.shake.update(static_cast<f32>(FIXED_DT));
m_camera.position = m_camera.position + shakeOffset;
```

In `renderProjectilesAndEffects()`, at the end (after all existing FX rendering), add:

```cpp
// Render particles
ParticleSystem::render(m_particles, m_camera, m_unlitShader, m_cubeMesh,
                       m_particleBlobMatId, m_particleSparkMatId);
```

- [ ] **Step 4: Clear particles on floor transition in engine_startgame.cpp**

In `Engine::startGame()`, near the top (after the existing resets around line 327), add:

```cpp
ParticleSystem::clear(m_particles);
m_camera.shake.intensity = 0.0f; // kill any lingering shake
```

- [ ] **Step 5: Build and run**

```bash
cmake --build build 2>&1 | tail -5
timeout 3 ./build/src/DungeonEngine 2>&1 | head -10 || true
```

Expected: builds and runs with no errors. No visible particles yet (nothing spawns them).

- [ ] **Step 6: Commit**

```bash
git add src/engine/engine_init.cpp src/engine/engine_update.cpp src/engine/engine_render.cpp src/engine/engine_startgame.cpp
git commit -m "wire particle system + screen shake into engine lifecycle"
```

---

### Task 5: Spawn particles and shake from combat

**Files:**
- Modify: `src/game/combat.cpp` (blood, sparks on hits + shake)
- Modify: `src/engine/engine_update.cpp` (blood on player damage + shake)
- Modify: `src/engine/engine_init.cpp` (blood on enemy death callback)

- [ ] **Step 1: Add particle includes to combat.cpp**

Add at the top of `src/game/combat.cpp`:

```cpp
#include "renderer/particles.h"
```

Combat functions need access to the particle pool. Since the engine passes pools by reference, add a `ParticlePool&` parameter to `fireMelee` and `fireHitscan`, or use a simpler approach: add a file-scope pointer set during init (matching the existing `s_engine` pattern used for the death callback).

Check the existing pattern — the death callback in `engine_init.cpp` uses a global `s_engine` pointer. Follow the same pattern: access particles via `s_engine->m_particles` (make `m_particles` accessible or add a getter).

Actually, the simplest approach: add a `ParticlePool*` and `ScreenShake*` extern in combat.cpp, set them from engine_init.cpp. This matches how `s_engine` is used for the death callback.

In `src/game/combat.cpp`, add near the top:

```cpp
// Set by Engine::init() so combat can spawn visual FX without coupling to Engine
static ParticlePool* s_particlePool = nullptr;
static ScreenShake*  s_screenShake  = nullptr;

namespace Combat {
    void setFXTargets(ParticlePool* particles, ScreenShake* shake) {
        s_particlePool = particles;
        s_screenShake  = shake;
    }
}
```

Add the declaration to `src/game/combat.h`:

```cpp
void setFXTargets(ParticlePool* particles, ScreenShake* shake);
```

In `Engine::init()` (in engine_init.cpp), after `ParticleSystem::init(m_particles)`:

```cpp
Combat::setFXTargets(&m_particles, &m_camera.shake);
```

- [ ] **Step 2: Spawn sparks + blood on melee hits**

In `Combat::fireMelee()`, after the `applyDamage()` call for each hit (around line 106), add:

```cpp
if (s_particlePool) {
    Vec3 hitPos = pool.entities[hits[i].index].position;
    hitPos.y += 0.5f; // aim at chest height
    ParticleSystem::spawnBlood(*s_particlePool, hitPos, forward, 8);
    ParticleSystem::spawnSparks(*s_particlePool, hitPos, forward * -1.0f, 6);
}
if (s_screenShake) s_screenShake->trigger(0.02f, 0.15f);
```

- [ ] **Step 3: Spawn sparks on hitscan hits**

In `Combat::fireHitscan()`, after applying damage on entity hit, add:

```cpp
if (s_particlePool) {
    ParticleSystem::spawnSparks(*s_particlePool, hitPos, forward * -1.0f, 6);
    ParticleSystem::spawnBlood(*s_particlePool, hitPos, forward, 6);
}
if (s_screenShake) s_screenShake->trigger(0.015f, 0.1f);
```

On wall hit (no entity), spawn debris:

```cpp
if (s_particlePool) {
    ParticleSystem::spawnDebris(*s_particlePool, wallHitPos, 4);
}
```

- [ ] **Step 4: Blood + shake when player takes damage**

In `src/engine/engine_update.cpp`, where `damageFlashTimer` is set (the damage handler around line 1027), add:

```cpp
ParticleSystem::spawnBlood(m_particles, m_localPlayer.position + Vec3{0, 0.8f, 0}, {0, 1, 0}, 6);
m_camera.shake.trigger(0.03f, 0.2f);
```

- [ ] **Step 5: Blood burst on enemy death**

In `src/engine/engine_init.cpp`, in the death callback (where loot is rolled, search for `s_engine` and `ItemGen::rollItem`), add:

```cpp
ParticleSystem::spawnBlood(s_engine->m_particles, position, {0, 1, 0}, 12);
```

Where `position` is the entity's death position (already available in the callback).

- [ ] **Step 6: Build and test**

```bash
cmake --build build 2>&1 | tail -5
```

Expected: compiles. In-game: hitting enemies should show blood + sparks + screen shake.

- [ ] **Step 7: Commit**

```bash
git add src/game/combat.cpp src/game/combat.h src/engine/engine_update.cpp src/engine/engine_init.cpp
git commit -m "spawn blood, sparks, debris on combat hits + screen shake"
```

---

### Task 6: Spawn particles from skills

**Files:**
- Modify: `src/game/skill.cpp`

- [ ] **Step 1: Add particle pool access to skill.cpp**

Same pattern as combat — add a file-scope pointer:

```cpp
#include "renderer/particles.h"

static ParticlePool* s_particlePool = nullptr;
static ScreenShake*  s_screenShake  = nullptr;

namespace SkillSystem {
    void setFXTargets(ParticlePool* particles, ScreenShake* shake) {
        s_particlePool = particles;
        s_screenShake  = shake;
    }
}
```

Add declaration to `src/game/skill.h`:

```cpp
void setFXTargets(ParticlePool* particles, ScreenShake* shake);
```

Wire in `Engine::init()`:

```cpp
SkillSystem::setFXTargets(&m_particles, &m_camera.shake);
```

- [ ] **Step 2: Add particle spawns alongside the audio switch**

In `tryActivate()`, after the audio switch block (after the closing `}` of the sound switch, before the main skill dispatch switch), add:

```cpp
// Spawn visual FX on skill activation
if (s_particlePool) {
    switch (ss.activeSkill) {
    case SkillId::FIREBALL:
    case SkillId::CONSECRATION:
        ParticleSystem::spawnMagicBurst(*s_particlePool, eyePos + forward * 0.5f, 255, 120, 30, 12);
        break;
    case SkillId::FROZEN_ORB:
        ParticleSystem::spawnMagicBurst(*s_particlePool, eyePos + forward * 0.5f, 100, 200, 255, 12);
        break;
    case SkillId::CHAIN_LIGHTNING:
    case SkillId::SHOCK_BOLT:
    case SkillId::TESLA_COIL:
        ParticleSystem::spawnMagicBurst(*s_particlePool, eyePos + forward * 0.5f, 255, 255, 200, 10);
        break;
    case SkillId::BLOOD_NOVA:
        ParticleSystem::spawnBlood(*s_particlePool, eyePos, {0, 1, 0}, 20);
        break;
    case SkillId::PHASE_DASH:
    case SkillId::SHADOW_STRIKE:
        ParticleSystem::spawnSmoke(*s_particlePool, eyePos, 8);
        break;
    case SkillId::METEOR_STRIKE:
    case SkillId::EARTHQUAKE:
    case SkillId::EXPLOSIVE_ROUND:
        ParticleSystem::spawnExplosion(*s_particlePool, eyePos + forward * 2.0f, 2.0f);
        if (s_screenShake) s_screenShake->trigger(0.1f, 0.5f);
        break;
    case SkillId::THUNDERCLAP:
        ParticleSystem::spawnSparks(*s_particlePool, eyePos, {0, 1, 0}, 10);
        if (s_screenShake) s_screenShake->trigger(0.06f, 0.3f);
        break;
    default: break;
    }
}
```

- [ ] **Step 3: Add shake to meteor landing**

In `SkillSystem::updateMeteors()`, where a meteor's timer expires and damage is applied, add:

```cpp
if (s_particlePool) ParticleSystem::spawnExplosion(*s_particlePool, m.position, m.radius);
if (s_screenShake) s_screenShake->trigger(0.1f, 0.5f);
```

- [ ] **Step 4: Build and test**

```bash
cmake --build build 2>&1 | tail -5
```

Expected: compiles. In-game: using skills should show colored particle bursts + shake on big skills.

- [ ] **Step 5: Commit**

```bash
git add src/game/skill.cpp src/game/skill.h src/engine/engine_init.cpp
git commit -m "spawn magic particles and screen shake from skill activation"
```

---

### Task 7: Build and verify on both platforms

- [ ] **Step 1: PC build and smoke test**

```bash
cd /home/aaron/game && cmake --build build 2>&1 | tail -5
timeout 3 ./build/src/DungeonEngine 2>&1 | head -10 || true
```

Expected: builds, loads 48/48 SFX, no errors.

- [ ] **Step 2: Switch build**

```bash
docker run --rm -u "$(id -u):$(id -g)" -v /home/aaron/game:/game -w /game devkitpro/devkita64 \
  bash -c "source /opt/devkitpro/switchvars.sh && cmake --build build-switch 2>&1" | tail -5
```

Expected: builds successfully, NRO generated with romfs.

- [ ] **Step 3: Commit any fixes if needed**

Only if builds revealed issues. Otherwise skip.
