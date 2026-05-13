# Particle System + Screen Shake Design

## Context

The game lacks visual feedback ("juice") on impacts, explosions, and skill activations. Hits feel flat — no sparks, blood, debris, or camera shake. This spec adds a lightweight particle system and screen shake that fits the Switch budget (16.6ms frame, 300-500 draw calls, OpenGL 3.3).

## Particle System

### Pool

`MAX_PARTICLES = 512` static array in `ParticleSystem` namespace. ~32 bytes per particle, 16KB total.

```cpp
// src/renderer/particles.h

constexpr u32 MAX_PARTICLES = 512;

enum ParticleType : u8 {
    PTYPE_BILLBOARD = 0,  // 2D quad facing camera (smoke, magic)
    PTYPE_GEOMETRIC = 1,  // tiny colored cube (sparks, blood, debris)
};

enum ParticleFlags : u8 {
    PFLAG_GRAVITY = 1 << 0,  // apply downward acceleration
    PFLAG_FADE    = 1 << 1,  // alpha fades to 0 over lifetime
    PFLAG_SHRINK  = 1 << 2,  // size shrinks to 0 over lifetime
};

struct Particle {
    Vec3 position;
    Vec3 velocity;
    f32  life;       // counts down to 0
    f32  maxLife;    // initial lifetime (for fade/shrink ratio)
    f32  size;       // world-space radius
    u8   r, g, b, a; // vertex color
    u8   type;       // ParticleType
    u8   flags;      // ParticleFlags bitmask
    bool active;
};
```

### API

```cpp
namespace ParticleSystem {
    void init();
    void shutdown();
    void update(f32 dt);                   // tick all active particles
    void render(const Camera& cam);        // 2 instanced draw calls

    // Spawn a single particle (returns false if pool is full)
    bool spawn(const Particle& p);

    // Emitter presets — spawn N particles with themed params
    void spawnBlood(Vec3 pos, Vec3 dir, u8 count);        // red geo cubes, gravity
    void spawnSparks(Vec3 pos, Vec3 dir, u8 count);       // yellow/orange geo, fast, short life
    void spawnMagicBurst(Vec3 pos, u8 r, u8 g, u8 b, u8 count);  // colored billboards, fade+shrink
    void spawnSmoke(Vec3 pos, u8 count);                   // grey billboards, slow rise, fade
    void spawnExplosion(Vec3 pos, f32 radius);             // sparks + smoke combined
    void spawnDebris(Vec3 pos, u8 count);                  // brown/grey geo cubes, gravity

    void clear();  // kill all particles (call on floor transition)
}
```

### Rendering

Two batched instanced draw calls per frame:

1. **Billboard particles**: Collect all `PTYPE_BILLBOARD` into a temp buffer, upload as instance data, draw instanced quads using the `unlit` shader. Each quad is camera-facing (billboard matrix from view inverse). Vertex color from particle RGBA. Uses a single 4x4 white texture so the shader multiplies color through.

2. **Geometric particles**: Collect all `PTYPE_GEOMETRIC`, draw instanced unit cubes using the `basic` shader with no texture (material 0 fallback, tinted by vertex color). Each instance is a `Mat4::translate(pos) * Mat4::scale(size)`.

Instance data per particle: `{ Mat4 model, Vec4 color }` = 80 bytes. For 512 particles worst case: 40KB uploaded per frame. On Switch this fits in a single buffer update.

### Update Logic

```
for each active particle:
    life -= dt
    if life <= 0: deactivate, continue
    position += velocity * dt
    if GRAVITY: velocity.y -= 9.8 * dt
    if FADE:    alpha = baseAlpha * (life / maxLife)
    if SHRINK:  size = baseSize * (life / maxLife)
```

### Emitter Preset Details

| Preset | Type | Count | Life | Size | Velocity | Flags | Color |
|--------|------|-------|------|------|----------|-------|-------|
| Blood | Geo | 8-12 | 0.3-0.6s | 0.03-0.06 | dir*2 + random spread | GRAVITY | Red (180-220, 0-30, 0-20) |
| Sparks | Geo | 6-10 | 0.2-0.4s | 0.02-0.04 | dir*4 + random spread | GRAVITY | Yellow-orange (255, 180-255, 0-80) |
| MagicBurst | Billboard | 10-16 | 0.4-0.8s | 0.08-0.15 | random sphere * 1.5 | FADE+SHRINK | Caller-specified |
| Smoke | Billboard | 4-8 | 0.6-1.2s | 0.1-0.2 | up 0.3-0.8 + random | FADE | Grey (120-180, alpha 150) |
| Explosion | Mixed | 20 | varies | varies | radial | varies | Sparks + Smoke combined |
| Debris | Geo | 6-10 | 0.4-0.8s | 0.03-0.06 | random sphere * 2 | GRAVITY | Brown-grey (100-160, 80-130, 60-100) |

### Switch Budget

- 2 draw calls (billboard batch + geo batch)
- 512 particles max × 32 bytes = 16KB pool
- 40KB instance buffer upload worst case
- No new textures (white 4x4 for billboards, untextured for geo)
- Update is O(512) simple arithmetic — negligible

---

## Screen Shake

### Data

Added to `Camera` struct in `src/renderer/camera.h`:

```cpp
struct ScreenShake {
    f32 intensity = 0.0f;   // current amplitude (world units)
    f32 decay     = 0.0f;   // decay rate (units/sec)
    f32 frequency = 25.0f;  // oscillation speed (Hz)
    f32 timer     = 0.0f;   // phase accumulator
};
```

### API

```cpp
// Trigger a new shake. If already shaking, takes the stronger of current vs new.
void ScreenShake::trigger(f32 intensity, f32 duration);
// Tick the shake, returns offset to add to camera position.
Vec3 ScreenShake::update(f32 dt);
```

### Update Logic

```
timer += dt * frequency
intensity -= decay * dt
if intensity < 0: intensity = 0

offset.x = sin(timer * 1.0) * intensity * pseudoRandom(timer, 0)
offset.y = sin(timer * 1.3) * intensity * pseudoRandom(timer, 1)
offset.z = sin(timer * 0.7) * intensity * pseudoRandom(timer, 2)
return offset
```

Different frequencies per axis (1.0, 1.3, 0.7 multipliers) prevent the shake from feeling like a simple oscillation. `pseudoRandom` uses a cheap hash of the timer to add jitter.

### Application

In `Engine::render()`, after camera interpolation (line ~481 of `engine_render.cpp`):

```cpp
Vec3 shakeOffset = m_camera.shake.update(dt);
m_camera.position = m_camera.position + shakeOffset;
```

The offset is purely visual — it doesn't affect collision, raycast, or gameplay position.

### Shake Presets

| Event | Intensity | Duration | Where triggered |
|-------|-----------|----------|-----------------|
| Player takes damage | 0.03 | 0.2s | `engine_update.cpp` damage handler |
| Melee hit connects | 0.02 | 0.15s | `Combat::fireMelee()` on hit |
| Hitscan hit | 0.015 | 0.1s | `Combat::fireHitscan()` on hit |
| Explosion/splash | 0.08 | 0.4s | `ProjectileSystem::update()` splash |
| Meteor impact | 0.1 | 0.5s | `SkillSystem::updateMeteors()` on land |
| Boss stomp | 0.1 | 0.5s | `enemy_ai.cpp` boss stomp attack |
| Earthquake skill | 0.12 | 0.6s | `SkillSystem::tryActivate()` EARTHQUAKE |
| Thunderclap skill | 0.06 | 0.3s | `SkillSystem::tryActivate()` THUNDERCLAP |

---

## Integration Points

### Where particles spawn

| Event | Preset | Location in code |
|-------|--------|-----------------|
| Enemy takes damage | `spawnBlood(hitPos, hitDir, 8)` | `Combat::applyDamage()` |
| Player takes damage | `spawnBlood(hitPos, hitDir, 6)` | `engine_update.cpp` damage handler |
| Melee hit (any) | `spawnSparks(hitPos, hitDir, 8)` | `Combat::fireMelee()` |
| Hitscan hit | `spawnSparks(hitPos, hitDir, 6)` | `Combat::fireHitscan()` |
| Projectile wall hit | `spawnDebris(hitPos, 6)` | `ProjectileSystem::update()` wall collision |
| Projectile splash | `spawnExplosion(hitPos, radius)` | `ProjectileSystem::update()` splash |
| Meteor lands | `spawnExplosion(pos, radius)` | `SkillSystem::updateMeteors()` |
| Skill: fire-themed | `spawnMagicBurst(pos, 255, 120, 30, 12)` | `SkillSystem::tryActivate()` FIREBALL/CONSECRATION |
| Skill: ice-themed | `spawnMagicBurst(pos, 100, 200, 255, 12)` | `SkillSystem::tryActivate()` FROZEN_ORB |
| Skill: lightning | `spawnMagicBurst(pos, 255, 255, 200, 10)` | `SkillSystem::tryActivate()` CHAIN_LIGHTNING/SHOCK_BOLT |
| Skill: blood nova | `spawnBlood(pos, random, 20)` | `SkillSystem::tryActivate()` BLOOD_NOVA |
| Skill: phase dash | `spawnSmoke(startPos, 8)` | `SkillSystem::tryActivate()` PHASE_DASH |
| Enemy death | `spawnBlood(pos, up, 12)` | death callback in `engine_init.cpp` |
| Molotov impact | `spawnExplosion(pos, 1.5)` | projectile collision |

### Floor transition cleanup

`ParticleSystem::clear()` called at the start of `startGame()` to kill lingering particles from the previous floor.

---

## Asset Generation

Billboard particles need a soft circle texture for the alpha falloff. Generate it with `tools/gen_texture.py` by adding a `particle_blob` texture type:

**Texture: `particle_blob_16.png`** (16x16, RGBA)
- White circle with radial alpha gradient: alpha = 1.0 at center, 0.0 at edge
- RGB is (255, 255, 255) everywhere — color comes from vertex color at runtime
- Smooth falloff: `alpha = max(0, 1.0 - (dist_from_center / radius)^2)`

**Texture: `particle_spark_16.png`** (16x16, RGBA)
- Elongated diamond/star shape — brighter along horizontal axis
- White RGB, alpha falloff from center
- Used for spark/impact billboards when you want a directional look

Both are 16x16 (tiny, fits the low-poly aesthetic) and go into `assets/textures/`. Add a material entry in `assets/materials.json` for each:

```json
{ "id": <next_id>, "name": "particle_blob", "texture": "textures/particle_blob_16.png", "tint": [1,1,1,1] },
{ "id": <next_id>, "name": "particle_spark", "texture": "textures/particle_spark_16.png", "tint": [1,1,1,1] }
```

Geometric particles (blood, debris, sparks cubes) use no texture — just vertex color through the existing `basic` shader with material 0 (default fallback).

The texture generation should be added as a new type in `gen_texture.py` so it's reproducible and not hand-authored.

---

## Files

| File | Action | Purpose |
|------|--------|---------|
| `src/renderer/particles.h` | **New** | Particle struct, pool, API declarations |
| `src/renderer/particles.cpp` | **New** | Pool management, update, instanced rendering, emitter presets |
| `src/renderer/camera.h` | **Modify** | Add `ScreenShake` struct to `Camera` |
| `src/engine/engine_init.cpp` | **Modify** | Call `ParticleSystem::init()` / `shutdown()` |
| `src/engine/engine_update.cpp` | **Modify** | Call `ParticleSystem::update(dt)`, trigger shake on player damage |
| `src/engine/engine_render.cpp` | **Modify** | Apply shake offset, call `ParticleSystem::render()` |
| `src/engine/engine_startgame.cpp` | **Modify** | Call `ParticleSystem::clear()` |
| `src/game/combat.cpp` | **Modify** | Spawn blood/sparks on hits, trigger shake |
| `src/game/skill.cpp` | **Modify** | Spawn magic particles on skill activation, trigger shake for big skills |
| `src/game/projectile.h` or `engine_update.cpp` | **Modify** | Spawn debris/explosion on projectile collisions |
| `tools/gen_texture.py` | **Modify** | Add `particle_blob` and `particle_spark` texture types |
| `assets/materials.json` | **Modify** | Add particle_blob and particle_spark material entries |
| `assets/textures/particle_blob_16.png` | **Generated** | Soft circle billboard texture (16x16 RGBA) |
| `assets/textures/particle_spark_16.png` | **Generated** | Elongated spark billboard texture (16x16 RGBA) |

---

## Verification

1. Build PC: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build`
2. Build Switch: Docker Switch build
3. In-game: swing sword at enemy — sparks + blood + light screen shake
4. In-game: fire projectile at wall — debris particles
5. In-game: use Meteor Strike — explosion particles + strong shake
6. In-game: take damage from enemy — blood + mild shake
7. In-game: descend floor — particles clear, no stale effects
8. Performance: F3 profiler overlay — particle update + render should be < 0.5ms on PC
