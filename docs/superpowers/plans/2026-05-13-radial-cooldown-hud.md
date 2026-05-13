# Radial Cooldown HUD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the rectangular cooldown overlays on skill bars with radial pie-sweep timers and scale slots from 32px to 64px for better readability on Switch.

**Architecture:** Modify `drawClassSkillBar()` and `drawEquipSkillBar()` in `hud.cpp` to use 64px slots and a new `drawRadialCooldown()` helper that draws a clockwise pie sweep using `pushLine()` segments. Add ready-flash timer tracking in `engine_hud.cpp`. No new files — changes are confined to the existing HUD rendering code.

**Tech Stack:** C++17, existing HUD line-draw system (`pushLine`/`pushQuad`/`flushHUD`)

**Spec:** `docs/superpowers/specs/2026-05-13-radial-cooldown-hud-design.md`

---

### Task 1: Add radial cooldown helper function

**Files:**
- Modify: `src/renderer/hud.h` (add declaration)
- Modify: `src/renderer/hud.cpp` (add implementation)

- [ ] **Step 1: Add helper declaration to hud.h**

Add after the existing `drawEquipSkillBar` declaration (around line 78):

```cpp
// Draw a radial (pie-sweep) cooldown overlay inside a square region.
// fraction: 0.0 = fully ready (no overlay), 1.0 = fully on cooldown.
// Sweeps clockwise from 12 o'clock.
void drawRadialCooldown(f32 cx, f32 cy, f32 radius, f32 fraction, Vec3 color);
```

- [ ] **Step 2: Implement drawRadialCooldown in hud.cpp**

Add before `drawClassSkillBar()` (around line 558):

```cpp
void HUD::drawRadialCooldown(f32 cx, f32 cy, f32 radius, f32 fraction, Vec3 color) {
    if (fraction <= 0.0f) return;
    if (fraction > 1.0f) fraction = 1.0f;

    // Draw filled pie slice as radial lines from center to edge.
    // 24 steps for smooth circle at 64px. Sweep clockwise from 12 o'clock.
    constexpr u32 STEPS = 24;
    f32 endAngle = fraction * 6.28318f; // fraction of full circle

    for (u32 i = 0; i < STEPS; i++) {
        f32 angle = (static_cast<f32>(i) / STEPS) * 6.28318f;
        if (angle > endAngle) break;

        // Map angle: 0 = up (12 o'clock), clockwise
        // Standard math: up = -PI/2, but we want 0 = up, increasing = clockwise
        f32 a = angle - 1.5708f; // offset so 0 = up
        f32 ex = cx + cosf(a) * radius;
        f32 ey = cy + sinf(a) * radius;

        // Fill with radial lines — draw several lines between this spoke and next
        f32 nextAngle = (static_cast<f32>(i + 1) / STEPS) * 6.28318f;
        if (nextAngle > endAngle) nextAngle = endAngle;
        f32 na = nextAngle - 1.5708f;
        f32 nx = cx + cosf(na) * radius;
        f32 ny = cy + sinf(na) * radius;

        // Fill the triangle (center, edge, next_edge) with horizontal lines
        // Simple approach: draw lines from center to each point along the arc
        constexpr u32 FILL_LINES = 4;
        for (u32 f = 0; f <= FILL_LINES; f++) {
            f32 t = static_cast<f32>(f) / FILL_LINES;
            f32 px = ex + (nx - ex) * t;
            f32 py = ey + (ny - ey) * t;
            pushLine(cx, cy, px, py, color);
        }
    }
}
```

- [ ] **Step 3: Build**

```bash
cmake --build build 2>&1 | tail -5
```

Expected: compiles (function exists but is not called yet).

- [ ] **Step 4: Commit**

```bash
git add src/renderer/hud.h src/renderer/hud.cpp
git commit -m "add drawRadialCooldown helper for pie-sweep cooldown overlay"
```

---

### Task 2: Upgrade class skill bar to 64px + radial cooldown

**Files:**
- Modify: `src/renderer/hud.cpp` (`drawClassSkillBar`, around lines 560-621)

- [ ] **Step 1: Rewrite drawClassSkillBar with 64px slots and radial sweep**

Replace the entire `drawClassSkillBar` function (lines 560-621) with:

```cpp
void HUD::drawClassSkillBar(u32 sw, u32 sh, f32 x, f32 y,
                              u8 activeSlot, u32 currentFloor,
                              const u8* unlockFloors, const u8* upgradeFloors,
                              const f32* cooldownTimers, const f32* maxCooldowns)
{
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    f32 slotW = 64.0f * uiScale, slotH = 64.0f * uiScale, gap = 4.0f * uiScale;

    for (u8 s = 0; s < 4; s++) {
        f32 sx = x + s * (slotW + gap);
        bool unlocked = (currentFloor >= unlockFloors[s]);
        bool selected = (s == activeSlot);
        bool upgraded = (currentFloor >= upgradeFloors[s]);

        // Background fill
        Vec3 bgCol = unlocked ? Vec3{0.12f, 0.12f, 0.18f} : Vec3{0.06f, 0.06f, 0.08f};
        if (selected && unlocked) bgCol = {0.16f, 0.2f, 0.3f};
        for (f32 fy = 0; fy < slotH; fy += 1.0f) {
            pushLine(sx, y + fy, sx + slotW, y + fy, bgCol);
        }

        // Border — bright for selected, gold for upgraded, dim for normal
        Vec3 borderCol = selected ? Vec3{0.4f, 0.9f, 0.5f} : Vec3{0.25f, 0.25f, 0.35f};
        if (!unlocked) borderCol = {0.12f, 0.12f, 0.18f};
        if (upgraded) borderCol = {0.9f, 0.8f, 0.3f};
        pushQuad(sx, y, sx + slotW, y + slotH, borderCol);

        // Radial cooldown sweep (replaces old rectangular top-down fill)
        if (unlocked && cooldownTimers[s] > 0.0f) {
            f32 maxCD = (maxCooldowns && maxCooldowns[s] > 0.0f) ? maxCooldowns[s] : 1.0f;
            f32 cdFrac = cooldownTimers[s] / maxCD;
            if (cdFrac > 1.0f) cdFrac = 1.0f;
            f32 cx = sx + slotW * 0.5f;
            f32 cy = y + slotH * 0.5f;
            f32 radius = slotW * 0.45f;
            drawRadialCooldown(cx, cy, radius, cdFrac, {0.05f, 0.05f, 0.08f});
        }

        flushHUD(sw, sh);

        // Key symbol — larger position to match 64px slot
        const char* skillLabel;
        if (Input::isGamepadConnected(0)) {
            static const char* dpadLabels[] = {"Up", "Rt", "Dn", "Lt"};
            skillLabel = dpadLabels[s];
        } else {
            static char numBuf[4][2] = {{'1',0}, {'2',0}, {'3',0}, {'4',0}};
            skillLabel = numBuf[s];
        }
        drawKeySymbol(sw, sh, sx + 14.0f * uiScale, y + 16.0f * uiScale, skillLabel, selected && unlocked);

        // Locked text
        if (!unlocked) {
            char lockTxt[8];
            std::snprintf(lockTxt, sizeof(lockTxt), "F%u", unlockFloors[s]);
            FontSystem::drawText(sw, sh, sx + 12.0f * uiScale, y + 4.0f * uiScale, lockTxt, {0.35f, 0.25f, 0.25f}, 1);
        }
    }
}
```

- [ ] **Step 2: Update class skill bar position in engine_hud.cpp**

In `engine_hud.cpp`, find where `skillBarX` and `skillBarY` are calculated (around line 330). The bar is now wider (4×64 + 3×4 = 268 scaled px vs old 4×32 + 3×3 = 137). Update the X position to keep it left-aligned with the quickbar. Also update `skillBarY` — the slot is taller so Y may need adjustment.

Read the existing position calculation, then adjust `skillBarX` so the bar's right edge aligns roughly where it was (or stays left of the quickbar). The Y should shift up by the extra height (32px difference).

- [ ] **Step 3: Build and test**

```bash
cmake --build build 2>&1 | tail -5
timeout 3 ./build/src/DungeonEngine 2>&1 | head -5 || true
```

Expected: compiles, game runs. Class skill slots are visibly larger with radial cooldown sweep.

- [ ] **Step 4: Commit**

```bash
git add src/renderer/hud.cpp src/engine/engine_hud.cpp
git commit -m "upgrade class skill bar: 64px slots + radial cooldown sweep"
```

---

### Task 3: Upgrade equipment skill bar to 64px + radial cooldown

**Files:**
- Modify: `src/renderer/hud.cpp` (`drawEquipSkillBar`, around lines 735-802)

- [ ] **Step 1: Rewrite drawEquipSkillBar with 64px slots and radial sweep**

Replace the entire `drawEquipSkillBar` function with:

```cpp
void HUD::drawEquipSkillBar(u32 sw, u32 sh, f32 x, f32 y,
                              const EquipSkillSlot* slots, u32 slotCount)
{
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    f32 slotW = 64.0f * uiScale, slotH = 64.0f * uiScale, gap = 4.0f * uiScale;

    for (u32 s = 0; s < slotCount; s++) {
        const EquipSkillSlot& slot = slots[s];
        f32 sx = x + s * (slotW + gap);
        bool ready = (slot.cooldownTimer <= 0.0f);

        // Background fill
        Vec3 bgCol = ready ? Vec3{0.1f, 0.08f, 0.15f} : Vec3{0.06f, 0.06f, 0.08f};
        for (f32 fy = 0; fy < slotH; fy += 1.0f) {
            pushLine(sx, y + fy, sx + slotW, y + fy, bgCol);
        }

        // Border — gold for legendary, purple for passive
        Vec3 borderCol = ready ? Vec3{0.7f, 0.55f, 0.2f} : Vec3{0.3f, 0.25f, 0.15f};
        if (slot.isPassive) borderCol = ready ? Vec3{0.5f, 0.4f, 0.7f} : Vec3{0.25f, 0.2f, 0.35f};
        pushQuad(sx, y, sx + slotW, y + slotH, borderCol);

        // Radial cooldown sweep (replaces old rectangular top-down fill)
        if (slot.cooldownTimer > 0.0f) {
            f32 maxCD = (slot.maxCooldown > 0.0f) ? slot.maxCooldown : 1.0f;
            f32 cdFrac = slot.cooldownTimer / maxCD;
            if (cdFrac > 1.0f) cdFrac = 1.0f;
            f32 cx = sx + slotW * 0.5f;
            f32 cy = y + slotH * 0.5f;
            f32 radius = slotW * 0.45f;
            drawRadialCooldown(cx, cy, radius, cdFrac, {0.05f, 0.05f, 0.08f});
        }

        // Draw 8x8 skill icon scaled to 32x32 (was 16x16), centered in 64px slot
        const u8* icon = getSkillIcon(slot.skillId);
        if (icon) {
            Vec3 cols[5];
            getSkillIconColors(slot.skillId, cols);
            f32 iconX = sx + 16.0f * uiScale;  // center 32px icon in 64px slot
            f32 iconY = y + 16.0f * uiScale;
            f32 px = 4.0f * uiScale; // pixel scale (was 2.0, now 4.0 for 64px slot)
            for (u32 iy = 0; iy < 8; iy++) {
                for (u32 ix = 0; ix < 8; ix++) {
                    u8 c = icon[iy * 8 + ix];
                    if (c == 0) continue;
                    f32 pxX = iconX + ix * px;
                    f32 pxY = iconY + (7 - iy) * px;
                    for (f32 fy = 0; fy < px; fy += 1.0f) {
                        pushLine(pxX, pxY + fy, pxX + px, pxY + fy,
                                 ready ? cols[c] : cols[c] * 0.4f);
                    }
                }
            }
        }

        flushHUD(sw, sh);

        // Key label or "Auto" for passives — larger position
        if (slot.isPassive) {
            FontSystem::drawText(sw, sh, sx + 8.0f * uiScale, y + 4.0f * uiScale, "auto",
                                 {0.5f, 0.4f, 0.7f}, 1);
        } else {
            drawKeySymbol(sw, sh, sx + 14.0f * uiScale, y - 24.0f * uiScale, slot.keyLabel, ready);
        }
    }
}
```

- [ ] **Step 2: Update equipment bar position in engine_hud.cpp**

Find where `equipBarY` is calculated (it's typically `skillBarY - 56` or similar). Since class slots are now 64px, the equip bar needs to be further above. Update to account for the new class bar height (64px + gap).

- [ ] **Step 3: Build and test**

```bash
cmake --build build 2>&1 | tail -5
```

Expected: compiles. Equipment skill slots are 64px with radial sweep and larger icons.

- [ ] **Step 4: Commit**

```bash
git add src/renderer/hud.cpp src/engine/engine_hud.cpp
git commit -m "upgrade equipment skill bar: 64px slots + radial cooldown + larger icons"
```

---

### Task 4: Add ready-flash effect

**Files:**
- Modify: `src/engine/engine_hud.cpp` (track flash timers, pass to HUD)
- Modify: `src/renderer/hud.h` (add flash parameter)
- Modify: `src/renderer/hud.cpp` (render flash)

- [ ] **Step 1: Add flash timer tracking in engine_hud.cpp**

Near the top of the file or in the Engine class (engine.h), add a static/member array:

```cpp
static f32 s_classSkillFlash[4] = {};
```

In the render code where cooldowns are read (around lines 317-339), before calling `drawClassSkillBar`, add flash detection:

```cpp
for (u8 s = 0; s < 4; s++) {
    // Detect transition from on-cooldown to ready
    if (s_classSkillFlash[s] > 0.0f) {
        s_classSkillFlash[s] -= dt; // dt comes from FIXED_DT or frame time
    }
    if (cooldowns[s] <= 0.0f && prevCooldowns[s] > 0.0f) {
        s_classSkillFlash[s] = 0.15f; // flash for 150ms
    }
    prevCooldowns[s] = cooldowns[s];
}
```

You'll need a `static f32 prevCooldowns[4] = {};` to track the previous frame's values.

- [ ] **Step 2: Pass flash timers to drawClassSkillBar**

Update the declaration and implementation to accept a `const f32* flashTimers` parameter. In the border drawing section, when `flashTimers[s] > 0.0f`, override the border color to bright white `{1.0f, 1.0f, 1.0f}`.

- [ ] **Step 3: Build and test**

```bash
cmake --build build 2>&1 | tail -5
```

Expected: compiles. When a skill comes off cooldown, its slot briefly flashes white.

- [ ] **Step 4: Commit**

```bash
git add src/renderer/hud.h src/renderer/hud.cpp src/engine/engine_hud.cpp
git commit -m "add ready-flash effect when skill comes off cooldown"
```

---

### Task 5: Build and verify on both platforms

- [ ] **Step 1: PC build and smoke test**

```bash
cmake --build build 2>&1 | tail -5
timeout 3 ./build/src/DungeonEngine 2>&1 | head -10 || true
```

- [ ] **Step 2: Switch build**

```bash
docker run --rm -u "$(id -u):$(id -g)" -v /home/aaron/game:/game -w /game devkitpro/devkita64 \
  bash -c "source /opt/devkitpro/switchvars.sh && rm -f build-switch/DungeonEngine.nro build-switch/src/DungeonEngine.elf build-switch/src/CMakeFiles/DungeonEngine.dir/audio/audio.cpp.obj && cmake --build build-switch 2>&1" | tail -5
```

- [ ] **Step 3: Commit any fixes**

Only if builds revealed issues.
