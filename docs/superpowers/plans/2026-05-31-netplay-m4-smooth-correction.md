# Netplay M4: Smooth Correction Layer (Skeleton) Plan

> superpowers:subagent-driven-development. Checkbox `- [ ]`.

**Goal:** Replace M3's hard-snap on prediction divergence with a smoothed correction. Introduce `m_renderOffset` (Vec3) on Engine: when divergence is detected, store the delta `serverPos - simPos` in `m_renderOffset` and decay it to zero over ~150 ms. Renderer reads `m_localPlayer.position - m_renderOffset` so the visible player smoothly slides toward the corrected sim position. Sim state is server-canonical immediately (kept consistent for replay correctness); only the rendered position is interpolated.

**Architecture:** New Engine member `Vec3 m_renderOffset = {0,0,0}` and `f32 m_renderOffsetDecay = 0.0f` (the per-second rate). On reconcile divergence: compute `delta = serverPos - m_localPlayer.position` (BEFORE we snap m_localPlayer.position to server), set `m_renderOffset += delta` (or replace; if existing offset exists, accumulate), `m_renderOffset *= exp(-dt * decayRate)` each frame. Camera and viewmodel read `m_localPlayer.position - m_renderOffset` for rendering. HUD reads m_localPlayer directly (HUD jumps are fine; position is the visual smoothing target).

**Reference:** §8 (Smooth Correction Layer) of the design.

---

## Task 1: render-offset math (TDD)

**Files:**
- Create: `src/net/render_offset.h`, `src/net/render_offset.cpp`
- Create: `tests/net/test_render_offset.cpp`
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1**: Write `tests/net/test_render_offset.cpp`:

```cpp
#include <doctest/doctest.h>
#include "net/render_offset.h"

TEST_CASE("RenderOffset: decay reaches near zero over expected timeframe") {
    RenderOffset r;
    r.offset = {1.0f, 0.0f, 0.0f};
    // Decay rate tuned so a 1 m correction is ~10% remaining after 150 ms.
    RenderOffsetOps::tick(r, 0.150f);  // simulate one 150 ms tick (one-shot)
    CHECK(r.offset.x < 0.2f);
    CHECK(r.offset.x > 0.0f);  // not fully zero
}

TEST_CASE("RenderOffset: accumulating two corrections sums into offset") {
    RenderOffset r;
    RenderOffsetOps::accumulate(r, {0.5f, 0.0f, 0.0f});
    RenderOffsetOps::accumulate(r, {0.5f, 0.0f, 0.0f});
    CHECK(r.offset.x == doctest::Approx(1.0f));
}

TEST_CASE("RenderOffset: tick with dt=0 is identity") {
    RenderOffset r;
    r.offset = {1.0f, 2.0f, 3.0f};
    RenderOffsetOps::tick(r, 0.0f);
    CHECK(r.offset.x == doctest::Approx(1.0f));
    CHECK(r.offset.y == doctest::Approx(2.0f));
    CHECK(r.offset.z == doctest::Approx(3.0f));
}

TEST_CASE("RenderOffset: many small ticks converge to near zero") {
    RenderOffset r;
    r.offset = {2.0f, 0.0f, 0.0f};
    for (u32 i = 0; i < 60; i++) RenderOffsetOps::tick(r, 1.0f / 60.0f); // 1 second of 60 Hz
    CHECK(r.offset.x < 0.01f);  // <1 cm residual after 1 s
}
```

- [ ] **Step 2**: Create `src/net/render_offset.h`:

```cpp
#pragma once
#include "core/types.h"
#include "core/math.h"

struct RenderOffset {
    Vec3 offset = {0,0,0};
};

namespace RenderOffsetOps {
    // Tunable: how quickly the offset decays toward zero.
    // exp(-DECAY_RATE * dt) per tick. DECAY_RATE = ~13 produces a "1 m correction
    // visually mostly closed in ~150 ms" feel (10% remaining at t = 0.150 s).
    static constexpr f32 DECAY_RATE = 13.0f;

    void accumulate(RenderOffset& r, Vec3 delta);
    void tick(RenderOffset& r, f32 dt);
    Vec3 apply(const RenderOffset& r, Vec3 simPos);
}
```

- [ ] **Step 3**: Create `src/net/render_offset.cpp`:

```cpp
#include "net/render_offset.h"
#include <math.h>

void RenderOffsetOps::accumulate(RenderOffset& r, Vec3 delta) {
    r.offset = r.offset + delta;
}

void RenderOffsetOps::tick(RenderOffset& r, f32 dt) {
    if (dt <= 0.0f) return;
    f32 k = expf(-DECAY_RATE * dt);
    r.offset = r.offset * k;
}

Vec3 RenderOffsetOps::apply(const RenderOffset& r, Vec3 simPos) {
    return simPos - r.offset;
}
```

- [ ] **Step 4**: Add `net/render_offset.cpp` to `src/CMakeLists.txt` + `tests/CMakeLists.txt`. Add `tests/net/test_render_offset.cpp` to tests.

- [ ] **Step 5**: Build, run. 27/27 should pass (23 + 4 new). Commit: `feat(net): RenderOffset for smooth corrections (TDD) — M4.1`.

---

## Task 2: Wire into prediction reconcile + camera

**Files:**
- Modify: `src/engine/engine.h` — add `RenderOffset m_renderOffset;`
- Modify: `src/engine/engine_net.cpp::clientNetPost` — accumulate offset on divergence
- Modify: `src/engine/engine_update.cpp` (or wherever the per-frame tick happens) — call `RenderOffsetOps::tick(m_renderOffset, dt)` each frame on CLIENT
- Modify: camera/render path — apply offset when computing eye/camera pos for rendering only

- [ ] **Step 1**: Add member declaration in engine.h:
```cpp
    RenderOffset m_renderOffset;   // accumulates prediction corrections; decays each frame
```
Add `#include "net/render_offset.h"` to engine.h.

- [ ] **Step 2**: Change reconcile in clientNetPost. Find the M3 block that does `m_localPlayer.position = serverPos;` on divergence. Change to:

```cpp
    if (distSq > 0.01f) {
        LOG_INFO("net: prediction divergence at tick %u: %.2f m", ackedTick, sqrtf(distSq));
        // Smooth correction (M4): accumulate visible delta, decay over ~150 ms.
        // Sim state snaps to server immediately for replay correctness; visible
        // position interpolates via m_renderOffset.
        Vec3 visibleDelta = m_localPlayer.position - serverPos;
        RenderOffsetOps::accumulate(m_renderOffset, visibleDelta);
        m_localPlayer.position = serverPos;
    }
```

The accumulate stores the visible "stayed where I thought I was" amount; on render we subtract it from the sim position so the visible player starts at the old spot and slides toward the corrected sim.

- [ ] **Step 3**: Tick the offset each frame on CLIENT. In `Engine::update` (or wherever the per-frame loop runs for CLIENT), call:
```cpp
    if (m_netRole == NetRole::CLIENT) {
        RenderOffsetOps::tick(m_renderOffset, dt);
    }
```

- [ ] **Step 4**: Apply offset to camera. Find `PlayerController::applyToCamera` or wherever the camera reads `m_localPlayer.position`. The cleanest spot is in engine_update_player.cpp where `applyToCamera` is called. Right BEFORE `applyToCamera`, on CLIENT only, temporarily adjust m_localPlayer's position by subtracting the offset, OR pass an adjusted Vec3 to a new overload. The simplest robust path is to apply the offset just before sending the camera transform:

Option A (cleanest): change `applyToCamera` to take an explicit eyePos parameter:
```cpp
void PlayerController::applyToCamera(const Player& player, Camera& cam, Vec3 eyePosOverride);
```
On CLIENT, callers compute `eyePos = RenderOffsetOps::apply(m_renderOffset, m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0})`.

Option B (minimal change): briefly mutate then revert:
```cpp
    if (m_netRole == NetRole::CLIENT) {
        Vec3 saved = m_localPlayer.position;
        m_localPlayer.position = RenderOffsetOps::apply(m_renderOffset, saved);
        PlayerController::applyToCamera(m_localPlayer, m_camera);
        m_localPlayer.position = saved;
    } else {
        PlayerController::applyToCamera(m_localPlayer, m_camera);
    }
```

Pick the option that produces the smallest diff to existing code. If the camera-apply call site is a one-liner already inside engine_update_player.cpp, Option B is fine.

- [ ] **Step 5**: Build, tests (27/27). Commit: `feat(net): wire RenderOffset into reconcile + camera — M4.2`.

---

## Task 3: Reset on connect + verify

- [ ] In the same spot ClockSync and PredictionRing are reset on connect, add:
```cpp
    m_renderOffset.offset = {0,0,0};
```

- [ ] Build, tests, ctest. Commit count for M4 = 2 (M4.1 + M4.2). Optionally include the reset in M4.2's commit if you do it together.

## Definition of Done
- [ ] `grep -c 'RenderOffset m_renderOffset' src/engine/engine.h` returns 1
- [ ] 27/27 tests pass
- [ ] `grep -c 'RenderOffsetOps::accumulate' src/engine/engine_net.cpp` returns 1
- [ ] `grep -c 'RenderOffsetOps::tick' src/engine/` returns 1
- [ ] Game builds clean
