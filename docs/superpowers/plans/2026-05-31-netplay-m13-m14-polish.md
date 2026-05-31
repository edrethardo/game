# Netplay M13 + M14: Smooth Correction (Full) + Debug Tooling Plan

> superpowers:subagent-driven-development.

**Goal:** (M13) Extend the M4 RenderOffset skeleton to cover HP bar visual smoothing and big-divergence screen flash; ensure inventory mirror has no pop on add. (M14) Add net-graph diagnostic logging + fake-latency / fake-loss cvar-style knobs + divergence counter so manual smoke testing can stress the netplay rewrite.

**Scope realism:** UI animation polish (smooth inventory fade-in, status-effect timer lerp) is largely a render-side concern and is too tangled with the HUD code to fully address here. v1 of M13 ships:
- HP bar lerp via a `RenderedHealth` field that lerps toward `m_localPlayer.health`
- Big-divergence screen-flash trigger (≥10 m teleport → 0.5 s screen fade-to-black-and-back)

M14 v1 ships:
- `m_netFakeLatencyMs` + `m_netFakeLossPct` fields on Engine (cvar-style, settable via existing debug-key dispatch or compile-time defaults)
- 1 Hz LOG_INFO summarizing RTT / smoothed loss / prediction divergence count
- Divergence counter that increments in clientNetPost on each mismatch (already logged per-event; counter is the aggregate)

---

## M13 Task: HP lerp + big-divergence screen-flash

**Files:**
- Modify: `src/game/player.h` — add `f32 renderedHealth` field (visible bar value, lerps toward `health`)
- Modify: per-frame tick — lerp renderedHealth toward health at fixed rate
- Modify: HUD — read renderedHealth for the bar instead of health (numeric still reads health for the digit display)
- Modify: prediction reconcile — on `distSq > 100.0f` (>= 10 m), set a brief screen-flash timer

- [ ] **Step 1**: Add to Player struct in src/game/player.h:
```cpp
    f32 renderedHealth = 100.0f;     // visible HP bar value, lerps toward `health` (M13)
    f32 screenFlashTimer = 0.0f;     // 0.5s flash on big prediction divergences (M13)
```

- [ ] **Step 2**: In whatever per-frame tick updates m_localPlayer (likely engine_update_player.cpp), add:
```cpp
    // M13: HP bar lerp. Numeric HP (the digit) jumps with `health`; the visible bar
    // catches up over ~250 ms so a hit doesn't look like a snap.
    const f32 HP_LERP_RATE = 4.0f;   // bar reaches 95% of `health` in ~750 ms; tune.
    f32 hpDelta = m_localPlayer.health - m_localPlayer.renderedHealth;
    m_localPlayer.renderedHealth += hpDelta * dt * HP_LERP_RATE;

    // M13: screen flash decay
    if (m_localPlayer.screenFlashTimer > 0.0f) {
        m_localPlayer.screenFlashTimer -= dt;
        if (m_localPlayer.screenFlashTimer < 0.0f) m_localPlayer.screenFlashTimer = 0.0f;
    }
```

- [ ] **Step 3**: Find the HUD HP bar render call. Look at src/renderer/hud_status.cpp or src/engine/engine_hud.cpp. Wherever it reads `player.health / player.maxHealth` to compute the bar fill ratio, change to `player.renderedHealth / player.maxHealth`. The numeric HP text (e.g., "85/100") keeps using `health`.

- [ ] **Step 4**: In clientNetPost's reconcile block (where divergence is detected), if `distSq > 100.0f` (>= 10 m), set `m_localPlayer.screenFlashTimer = 0.5f;`:
```cpp
    if (distSq > 100.0f) {
        m_localPlayer.screenFlashTimer = 0.5f;
    }
```

- [ ] **Step 5**: Find the screen-render pass (final framebuffer composite or post-process). Add a black overlay whose alpha = `screenFlashTimer / 0.5f`. If there's an existing hurt-vignette / fade overlay shader, reuse its quad with a new alpha source. If not, this can be done with a simple GL clear + viewport blit using the existing 2D HUD pipeline.

For v1, a simple textured quad over the entire screen with `vec4(0,0,0, screenFlashTimer * 2.0f)` blending is enough.

- [ ] **Step 6**: Build, 54/54 tests still pass (no new tests; this is pure visual). Commit: `feat(net): smooth HP bar + big-divergence screen flash — M13`.

---

## M14 Task: Net-graph + fake-latency cvars + divergence counter

**Files:**
- Modify: `src/engine/engine.h` — add diagnostic fields
- Modify: `src/net/net.h` — add fake-latency simulation in send paths
- Modify: client + server packet send paths to apply fake-latency / loss

- [ ] **Step 1**: Add fields to engine.h:
```cpp
    // M14: debug knobs (set via compile-time defaults or interactive keybinds)
    u32 m_netFakeLatencyMs = 0;
    u8  m_netFakeLossPct   = 0;
    u32 m_divergenceCount  = 0;  // bumped in clientNetPost on each reconcile mismatch
    f64 m_lastDebugLogSec  = 0.0;
```

- [ ] **Step 2**: Bump m_divergenceCount in the reconcile block (engine_net.cpp::clientNetPost) when divergence detected:
```cpp
    if (distSq > 0.01f) {
        m_divergenceCount++;
        // ... existing M4 accumulate ...
    }
```

- [ ] **Step 3**: 1 Hz net-graph log. In engine_update.cpp's main tick (or in clientNetPre):
```cpp
    if (m_netRole == NetRole::CLIENT) {
        f64 nowSec = Clock::getElapsedSeconds();
        if (nowSec - m_lastDebugLogSec >= 1.0) {
            LOG_INFO("[NET-GRAPH] rtt=%.1fms serverTickEst=%.1f divergences=%u",
                     m_clockSync.oneWayTripMs * 2.0f, m_clockSync.serverTickEst, m_divergenceCount);
            m_lastDebugLogSec = nowSec;
            m_divergenceCount = 0;
        }
    }
```

- [ ] **Step 4**: Apply fake latency/loss to packet sends. The cleanest place is src/net/net.cpp's `Net::sendToServer` / `Net::sendToPeer` wrappers. Add a small queue of (delivery time, payload) pairs; pop+deliver when current time >= delivery time. For v1, simpler: just `if (rand() % 100 < m_engine->m_netFakeLossPct) drop`. Skip the latency simulation for v1 — the loss simulation alone is useful for testing reliability.

Add `s_engineRef` global pointer (similar to other patterns) so net.cpp can read the cvars without further plumbing.

- [ ] **Step 5**: Add a keybind to toggle fake-loss in increments (e.g., F8 cycles 0% → 5% → 10% → 20% → 0%). Look at how existing debug keys (F-keys) are dispatched in input handling. If too invasive for v1, default to 0% and just expose the field for code-level testing.

For v1, leave the fields as defaults (0%) and add a single LOG_INFO at game start so the user sees they exist.

- [ ] **Step 6**: Build, tests. 54/54 pass. Commit: `feat(net): net-graph diagnostic log + fake-latency/loss cvars — M14`.

---

## Verify

- [ ] Clean tree, 54/54 tests, build clean.

## Definition of Done
- [ ] 54/54 tests pass
- [ ] HP bar uses `renderedHealth` (lerps), not raw `health`
- [ ] Big-divergence screen-flash triggered on >10m corrections
- [ ] 1 Hz [NET-GRAPH] log appears on CLIENT
- [ ] `m_netFakeLossPct` field exists and is read in packet send paths
