# Switch Performance Optimization — CPU + GPU Savings

## Problem

FPS on Nintendo Switch is consistently ~50 instead of 60, even in non-extreme scenarios. The Tegra X1's A57 cores at 1 GHz and shared 25.6 GB/s memory bandwidth are bottlenecked by:

1. **Per-particle draw calls** — up to 256 particles, each submitted as a separate `Renderer::submit()` call. In typical combat, 50-100 active particles = 50-100 draw calls (17-33% of the ~300 DC budget).
2. **Oversized projectile pool** — `MAX_PROJECTILES = 4096` allocates ~400 KB of scattered memory. Even with activeList early-exit, the struct footprint hurts cache utilization on Switch's weak cores.
3. **Uncapped enemy spawns** — floors 11+ allow 5 enemies/room, producing 80+ active entities that exceed the draw call budget (407 DC estimated at 80 enemies).

## Changes

### 1. Particle Batching

**Goal:** Reduce particle draw calls from 50-100 to 2 per frame.

**Current flow:** `ParticleSystem::render()` (particles.cpp:65) iterates all 256 particle slots. For each active particle, it builds a model matrix and calls `Renderer::submit()` individually — one draw call per particle.

**New flow:**
- Add a dynamic vertex buffer (similar to the HUD's `MAX_HUD_VERTS` approach) sized for `MAX_PARTICLES * 4` vertices (4 verts per billboard quad).
- In `ParticleSystem::render()`, instead of submitting each particle:
  1. Build the billboard quad vertices (position, UV, color+alpha) in a CPU-side array
  2. Separate into two buckets: billboard particles and geometric particles
  3. Upload each bucket's vertex data to GPU in one `glBufferSubData` call
  4. Draw each bucket with one `glDrawElements` call
- Billboard orientation uses the existing camera right/up basis vectors (already computed per-particle at lines 95-104).
- Geometric particles (spinning cubes) stay as individual submits for now — they're rare and the cube mesh can't be trivially batched. Only billboard batching is needed for the win.

**Vertex format per particle quad:**
```
struct ParticleVertex {
    Vec3 position;   // world-space corner
    Vec2 uv;         // 0-1 texture coords
    Vec4 color;      // RGBA with pre-multiplied alpha
};
```
4 vertices × 36 bytes = 144 bytes per particle. 256 max = ~36 KB upload per frame (negligible on 25.6 GB/s bus).

**Files:**
- `src/renderer/particles.h` — add `ParticleVertex` struct, batch VBO/VAO/IBO handles to `ParticlePool`
- `src/renderer/particles.cpp` — rewrite `render()` to batch billboard particles; add `initBatchBuffers()` / `shutdownBatchBuffers()` called from Engine init/shutdown

### 2. Projectile Pool Shrink (Switch only)

**Goal:** Reduce projectile pool memory footprint from ~400 KB to ~50 KB on Switch.

**Change:** In `src/game/projectile.h`, conditionally define:
```cpp
#ifdef __SWITCH__
static constexpr u32 MAX_PROJECTILES = 512;
#else
static constexpr u32 MAX_PROJECTILES = 4096;
#endif
```

512 is sufficient — typical gameplay rarely exceeds 30-40 active projectiles. The activeList pattern already prevents scanning dead slots, but the smaller pool improves cache locality for the `ProjectilePool` struct.

**Files:**
- `src/game/projectile.h` — conditional `MAX_PROJECTILES`

### 3. Spawn Cap (Switch only)

**Goal:** Keep entity count under ~48 on Switch to stay within draw call budget.

**Change:** In `src/engine/engine_startgame.cpp`, cap enemy spawns at 3/room on Switch for all floors:
```cpp
#ifdef __SWITCH__
    if (enemyCount > 3) enemyCount = 3;
#endif
```

This applies after the existing floor-based calculation. Floors 1-10 already cap at 3, so this only affects floors 11+ (which currently allow up to 5/room).

**Files:**
- `src/engine/engine_startgame.cpp` — add Switch spawn cap after existing logic

## Expected Impact

| Optimization | Draw Calls Saved | CPU Savings |
|---|---|---|
| Particle batching | 50-100 DC → 2 DC | Fewer Renderer::submit() calls, fewer sort entries |
| Projectile pool shrink | — | ~350 KB less memory pressure, better cache |
| Spawn cap | ~60 DC (30 fewer entities × ~2 DC each) | ~40% fewer AI updates, collision checks |
| **Combined** | ~110-160 fewer DC per frame | Significant CPU reduction |

With these changes, the worst-case scenario drops from ~407 DC to ~250 DC (well within the 300 DC budget).

## Verification

1. Build for Switch: `cmake -B build-switch -DCMAKE_TOOLCHAIN_FILE=... && cmake --build build-switch`
2. Deploy to Switch and check FPS counter (bottom-left) — target: stable 60 FPS
3. Test on floor 12+ with full rooms — verify enemy count stays at 3/room
4. Verify particle effects look identical (fireball trails, blood splatter, death debris)
5. Verify projectile-heavy scenarios (orb shards, boss fights) don't hit the 512 cap
6. PC build should be unaffected (no `__SWITCH__` defines active)
