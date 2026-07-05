# Combat HUD: Potion Discoverability + Skill Cooldown Readability — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the potion impossible to miss (belt flask welded to the health bar) and make skill cooldowns readable at combat speed (bright sweep edge + seconds countdown + a shared green "ready" pop), as pure HUD/render changes.

**Architecture:** A header-only pure-helper unit (`hud_cooldown_util.h`) holds the testable math (seconds formatting, low-HP urgency predicate, pop timing); one shared `HUD::drawReadyPop` renders the green "it's back" ring for both skills and the potion; `HUD::drawPotionFlask` draws a primitive flask beside the health/energy bars using the same cooldown language as the skill slots plus a steady red low-HP pulse. A single `GameConst::LOW_HP_FRACTION` ties the flask's red pulse to the existing red screen-edge vignette.

**Tech Stack:** C++17, line-batched HUD (`pushLine`/`pushQuad`/`flushHUD` from `renderer/hud_internal.h`), `FontSystem::drawText`, doctest unit tests. No new textures/shaders. Per-viewport split-screen safe.

**⚠️ Commit policy:** This project requires **explicit user authorization before any commit** (per project rule). The commit step in each task is part of the plan, but the executor MUST get the user's go-ahead before running it. If not authorized, complete the task and leave it staged/unstaged for the user.

**Scope guardrails (do NOT touch):** networking, snapshots, save format, combat balance, input bindings, or any HUD element other than the potion + skill cooldowns (plus the shared `LOW_HP_FRACTION` constant swap in the low-HP vignette). No potion count/charges (it's an infinite 5s-cooldown heal).

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/renderer/hud_cooldown_util.h` (new) | Pure, header-only cooldown/urgency/pop math (unit-tested) | 1 |
| `tests/renderer/test_hud_cooldown.cpp` (new) | doctest cases for the pure helpers | 1 |
| `tests/CMakeLists.txt` | Register the new test file | 1 |
| `src/game/game_constants.h` | Add `LOW_HP_FRACTION` constant | 1 |
| `src/engine/engine_render.cpp` | Use `LOW_HP_FRACTION` in the low-HP vignette | 1 |
| `src/renderer/hud.h` | Declare `drawReadyPop`, `drawPotionFlask` + `PotionHudState`; add `edgeColor` to `drawRadialCooldown`; add `readyFlash` to `EquipSkillSlot`; remove `drawSkillCooldown` decl | 2,3,4 |
| `src/renderer/hud.cpp` | Implement `drawReadyPop` + `drawPotionFlask` | 2,4 |
| `src/renderer/hud_skill_bar.cpp` | Bright sweep edge; seconds number + ready-pop in both skill bars; delete `drawSkillCooldown` | 3 |
| `src/engine/engine_hud.cpp` | Flash-timer plumbing (class/equip/potion); flask draw; remove top-left potion text | 3,4 |

---

## Task 1: Pure cooldown/urgency helpers + shared constant + tests

**Files:**
- Create: `src/renderer/hud_cooldown_util.h`
- Create: `tests/renderer/test_hud_cooldown.cpp`
- Modify: `tests/CMakeLists.txt` (add the test to the `add_executable` list)
- Modify: `src/game/game_constants.h:120` (add constant after `POTION_COOLDOWN`)
- Modify: `src/engine/engine_render.cpp:476` (use the constant)

- [ ] **Step 1: Write the failing test**

Create `tests/renderer/test_hud_cooldown.cpp`:

```cpp
// test_hud_cooldown.cpp — unit tests for the pure HUD cooldown/urgency/pop helpers
// (renderer/hud_cooldown_util.h). No GL context needed; the header is inline-only.
#include "doctest/doctest.h"
#include "renderer/hud_cooldown_util.h"

using namespace HudCooldown;

TEST_CASE("cooldownSeconds ceils to whole seconds, min 1 while visible") {
    CHECK(cooldownSeconds(5.0f) == 5);
    CHECK(cooldownSeconds(4.1f) == 5);
    CHECK(cooldownSeconds(0.9f) == 1);
    CHECK(cooldownSeconds(0.05f) == 1);
}

TEST_CASE("showCooldownNumber hides the final sliver") {
    CHECK(showCooldownNumber(1.0f));
    CHECK(showCooldownNumber(0.21f));
    CHECK_FALSE(showCooldownNumber(0.2f));
    CHECK_FALSE(showCooldownNumber(0.0f));
}

TEST_CASE("potionUrgent fires only when ready AND low HP") {
    const f32 LOW = 0.25f;
    CHECK(potionUrgent(0.20f, 0.0f, LOW));        // low + ready
    CHECK(potionUrgent(0.25f, 0.0f, LOW));        // exactly at threshold
    CHECK_FALSE(potionUrgent(0.20f, 1.0f, LOW));  // low but still cooling
    CHECK_FALSE(potionUrgent(0.50f, 0.0f, LOW));  // ready but healthy
}

TEST_CASE("readyPopT maps and clamps a POP_DURATION..0 flash timer to 1..0") {
    CHECK(readyPopT(POP_DURATION) == doctest::Approx(1.0f));
    CHECK(readyPopT(POP_DURATION * 0.5f) == doctest::Approx(0.5f));
    CHECK(readyPopT(0.0f) == doctest::Approx(0.0f));
    CHECK(readyPopT(POP_DURATION * 2.0f) == doctest::Approx(1.0f)); // clamp high
}

TEST_CASE("readyPop ring expands as it fades") {
    f32 rStart = readyPopRadius(1.0f, 20.0f, 1.0f); // just fired
    f32 rEnd   = readyPopRadius(0.0f, 20.0f, 1.0f); // finished
    CHECK(rStart == doctest::Approx(20.0f));
    CHECK(rEnd   == doctest::Approx(20.0f + POP_GROW));
    CHECK(rEnd > rStart);
    CHECK(readyPopAlpha(1.0f) == doctest::Approx(1.0f));
    CHECK(readyPopAlpha(0.0f) == doctest::Approx(0.0f));
}
```

Then register it — in `tests/CMakeLists.txt`, add this line to the `add_executable(dungeon_tests ...)` source list (after line 33, `world/test_raycast.cpp`):

```cmake
    renderer/test_hud_cooldown.cpp
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target dungeon_tests`
Expected: FAIL — compile error, `renderer/hud_cooldown_util.h: No such file or directory` (header not created yet).

- [ ] **Step 3: Create the header**

Create `src/renderer/hud_cooldown_util.h`:

```cpp
// hud_cooldown_util.h — pure, header-only helpers for HUD cooldown/urgency visuals.
// Extracted so the seconds formatting, low-HP urgency predicate, and "ready pop"
// timing math can be unit-tested without a GL context. Consumed by the skill bars
// (hud_skill_bar.cpp), the potion flask (hud.cpp / engine_hud.cpp), and drawReadyPop.
#pragma once

#include "core/types.h"
#include <cmath>

namespace HudCooldown {

// Seconds the green "ability is back" pop lasts — shared by skills AND the potion so
// every "it's back" reads identically.
inline constexpr f32 POP_DURATION = 0.4f;
// How far (px at the 720p baseline) the pop ring expands past the slot half-size.
inline constexpr f32 POP_GROW = 7.0f;

// Whole-seconds countdown shown centered on a cooling slot. Ceil so a 4.1s timer
// reads 5..1 and only disappears when truly ready. Minimum 1 while still visible.
inline int cooldownSeconds(f32 remaining) {
    int s = static_cast<int>(std::ceil(remaining));
    return s < 1 ? 1 : s;
}

// Suppress the number in the last sliver so the pop — not a flickering "1" — carries
// the final fraction of a second.
inline bool showCooldownNumber(f32 remaining) {
    return remaining > 0.2f;
}

// "Drink NOW": the heal is ready AND HP is at/below the low-HP threshold. The threshold
// is a parameter so this header carries no game-layer dependency (the caller passes
// GameConst::LOW_HP_FRACTION).
inline bool potionUrgent(f32 healthFrac, f32 cooldownRemaining, f32 lowHpFrac) {
    return cooldownRemaining <= 0.0f && healthFrac <= lowHpFrac;
}

// Pop progress 1..0 from a flash timer counting POP_DURATION -> 0. Clamped.
inline f32 readyPopT(f32 flashTimer) {
    f32 t = flashTimer / POP_DURATION;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

// Ring radius: starts at the slot half-size (t=1, just fired) and grows outward as it
// fades (t->0). `scale` converts the baseline POP_GROW to the caller's uiScale.
inline f32 readyPopRadius(f32 t01, f32 baseHalf, f32 scale) {
    return baseHalf + (1.0f - t01) * POP_GROW * scale;
}

// Ring opacity proxy 1..0. The HUD line batch has no alpha, so callers lerp the ring
// color toward black by this factor.
inline f32 readyPopAlpha(f32 t01) {
    return t01;
}

} // namespace HudCooldown
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*cooldownSeconds*,*showCooldownNumber*,*potionUrgent*,*readyPop*"`
Expected: PASS — all 5 cases green (`===============================================================================
[doctest] test cases: 5 | 5 passed`).

- [ ] **Step 5: Add the shared low-HP constant and use it in the vignette**

In `src/game/game_constants.h`, add after line 120 (`POTION_COOLDOWN`):

```cpp
    // Shared low-HP threshold: HP fraction at/below which the screen-edge red vignette
    // ramps up AND the potion flask pulses red. One constant so both fire together.
    static constexpr f32 LOW_HP_FRACTION     = 0.25f;
```

In `src/engine/engine_render.cpp`, replace the hardcoded `0.25f` at line 476-477:

```cpp
    if (hpFrac > 0.0f && hpFrac < GameConst::LOW_HP_FRACTION)
        lowHp = (GameConst::LOW_HP_FRACTION - hpFrac) / GameConst::LOW_HP_FRACTION * 0.40f;   // 0 at threshold -> 0.40 near death, constant per frame
```

(`game_constants.h` is already included by `engine_render.cpp`; if the build says `GameConst` is undeclared, add `#include "game/game_constants.h"`.)

- [ ] **Step 6: Verify the full build + suite still pass**

Run: `cmake --build build && ./build/tests/dungeon_tests --no-version`
Expected: build succeeds; all tests pass (existing + the 5 new).

- [ ] **Step 7: Commit** (only with user authorization — see Commit policy)

```bash
git add src/renderer/hud_cooldown_util.h tests/renderer/test_hud_cooldown.cpp tests/CMakeLists.txt src/game/game_constants.h src/engine/engine_render.cpp
git commit -m "feat(hud): pure cooldown/urgency helpers + shared LOW_HP_FRACTION"
```

---

## Task 2: Shared `drawReadyPop` helper

**Files:**
- Modify: `src/renderer/hud.h` (declare `drawReadyPop`)
- Modify: `src/renderer/hud.cpp` (implement it; add includes)

No unit test — this is pure rendering (line output). Its math is already covered by Task 1. The gate is a clean build.

- [ ] **Step 1: Declare the helper**

In `src/renderer/hud.h`, inside the `namespace HUD { ... }` block, add after the `drawRadialCooldown` declaration (line 65):

```cpp
    // Shared "ability is back" pop — an expanding, fading square ring drawn around a
    // slot/flask. t01: 1 at the instant of readiness, 0 when the pop ends
    // (see renderer/hud_cooldown_util.h). scale = caller uiScale.
    void drawReadyPop(f32 cx, f32 cy, f32 baseHalf, f32 t01, f32 scale, Vec3 color);
```

- [ ] **Step 2: Ensure includes in `hud.cpp`**

At the top of `src/renderer/hud.cpp`, add these includes if not already present:

```cpp
#include "renderer/hud_cooldown_util.h"
```

- [ ] **Step 3: Implement the helper**

Add to `src/renderer/hud.cpp` (near the other simple primitives, e.g. after `drawFilledBar`):

```cpp
// Expanding, fading ring for the "ability is back" pop. The HUD batch has no alpha,
// so the ring color is lerped toward black by the pop's remaining life (readyPopAlpha).
// Caller is responsible for flushHUD() afterward.
void HUD::drawReadyPop(f32 cx, f32 cy, f32 baseHalf, f32 t01, f32 scale, Vec3 color) {
    f32 r = HudCooldown::readyPopRadius(t01, baseHalf, scale);
    f32 a = HudCooldown::readyPopAlpha(t01);
    Vec3 c = color * a;                       // fade toward black as it grows
    pushQuad(cx - r, cy - r, cx + r, cy + r, c);   // pushQuad draws a hollow outline = ring
}
```

- [ ] **Step 4: Verify build**

Run: `cmake --build build`
Expected: build succeeds (helper compiles; not yet called — that's Tasks 3 & 4).

- [ ] **Step 5: Commit** (only with user authorization)

```bash
git add src/renderer/hud.h src/renderer/hud.cpp
git commit -m "feat(hud): shared drawReadyPop ability-ready pop helper"
```

---

## Task 3: Skill cooldown treatment (class + equipment bars) + delete dead code

**Files:**
- Modify: `src/renderer/hud.h` (add `edgeColor` to `drawRadialCooldown`; add `readyFlash` to `EquipSkillSlot`; remove `drawSkillCooldown` decl)
- Modify: `src/renderer/hud_skill_bar.cpp` (bright edge; seconds + pop in both bars; delete `drawSkillCooldown`; add includes)
- Modify: `src/engine/engine_hud.cpp` (flash plumbing for class + equip bars; add include)

- [ ] **Step 1: Update `drawRadialCooldown` signature + `EquipSkillSlot`; remove `drawSkillCooldown` decl (hud.h)**

In `src/renderer/hud.h`:

Change the `drawRadialCooldown` declaration (line 63-65) to add a bright leading-edge color:

```cpp
    // Radial pie-sweep cooldown overlay — draws clockwise from 12 o'clock.
    // fraction=1.0 fills entire circle, fraction=0.0 draws nothing.
    // edgeColor draws a bright line along the sweep boundary so progress is legible
    // against the dark cover.
    void drawRadialCooldown(f32 cx, f32 cy, f32 radius, f32 fraction,
                            Vec3 color, Vec3 edgeColor);
```

Add a `readyFlash` field to `EquipSkillSlot` (after `isPassive`, line 85) — a default member initializer keeps existing aggregate initializers valid:

```cpp
        bool isPassive;    // armor aura / weapon proc (no key activation)
        f32  readyFlash = 0.0f; // 0..POP_DURATION; drives the green "ready" pop (set by caller)
```

Delete the dead `drawSkillCooldown` declaration (lines 102-103):

```cpp
    // Skill cooldown indicator (small square near weapon indicator)
    void drawSkillCooldown(u32 sw, u32 sh, f32 cooldownPct);
```

- [ ] **Step 2: Add includes to `hud_skill_bar.cpp`**

At the top of `src/renderer/hud_skill_bar.cpp`, add:

```cpp
#include "renderer/hud_cooldown_util.h"
```

(`font.h`, `hud_internal.h`, `<cmath>`, `<cstdio>` are already included there.)

- [ ] **Step 3: Add the bright leading edge to `drawRadialCooldown`**

In `src/renderer/hud_skill_bar.cpp`, update the signature (line 19) and append the edge line before the closing brace (after the wedge-fill loop, line 40):

```cpp
void HUD::drawRadialCooldown(f32 cx, f32 cy, f32 radius, f32 fraction,
                             Vec3 color, Vec3 edgeColor) {
    if (fraction <= 0.0f) return;
    if (fraction > 1.0f) fraction = 1.0f;
    constexpr u32 STEPS = 24;
    f32 endAngle = fraction * 6.28318f;
    for (u32 i = 0; i < STEPS; i++) {
        f32 a0 = (static_cast<f32>(i) / STEPS) * 6.28318f;
        if (a0 >= endAngle) break;
        f32 a1 = (static_cast<f32>(i + 1) / STEPS) * 6.28318f;
        if (a1 > endAngle) a1 = endAngle;
        f32 r0 = a0 - 1.5708f, r1 = a1 - 1.5708f;
        f32 ex0 = cx + cosf(r0) * radius, ey0 = cy + sinf(r0) * radius;
        f32 ex1 = cx + cosf(r1) * radius, ey1 = cy + sinf(r1) * radius;
        for (u32 f = 0; f <= 4; f++) {
            f32 t  = static_cast<f32>(f) / 4.0f;
            f32 px = ex0 + (ex1 - ex0) * t;
            f32 py = ey0 + (ey1 - ey0) * t;
            pushLine(cx, cy, px, py, color);
        }
    }
    // Bright leading edge along the sweep boundary — the visible "hand" of the cooldown.
    f32 re = endAngle - 1.5708f;
    pushLine(cx, cy, cx + cosf(re) * radius, cy + sinf(re) * radius, edgeColor);
}
```

- [ ] **Step 4: Class bar — pass edge color, add seconds number + ready pop**

In `drawClassSkillBar` (`hud_skill_bar.cpp`):

(a) The radial call (line 330) gains a gold edge color:

```cpp
            drawRadialCooldown(cx, cy, radius, cdFrac, {0.05f, 0.05f, 0.08f}, {1.0f, 0.8f, 0.3f});
```

(b) The border flash (line 291) becomes a green "ready" tint driven by the pop timer:

```cpp
        if (flashTimers && flashTimers[s] > 0.0f) {
            f32 t = HudCooldown::readyPopT(flashTimers[s]);
            borderCol = {0.4f + 0.6f * t, 0.9f, 0.5f + 0.3f * t};   // green ready flash, fades in over the pop
        }
```

(c) After the existing `flushHUD();` (line 333), add the pop ring + centered seconds number:

```cpp
        flushHUD();

        // Green "ready" pop when the skill just came off cooldown (shared with the potion).
        if (flashTimers && flashTimers[s] > 0.0f) {
            f32 cxp = sx + slotW * 0.5f, cyp = y + slotH * 0.5f;
            drawReadyPop(cxp, cyp, slotW * 0.5f, HudCooldown::readyPopT(flashTimers[s]), uiScale,
                         {0.42f, 0.88f, 0.54f});
            flushHUD();
        }

        // Seconds countdown centered in the slot while cooling.
        if (unlocked && cooldownTimers[s] > 0.0f &&
            HudCooldown::showCooldownNumber(cooldownTimers[s])) {
            f32 cxp = sx + slotW * 0.5f, cyp = y + slotH * 0.5f;
            char cdBuf[8];
            std::snprintf(cdBuf, sizeof(cdBuf), "%d", HudCooldown::cooldownSeconds(cooldownTimers[s]));
            f32 tw = FontSystem::textWidth(cdBuf, 2);
            FontSystem::drawText(sw, sh, cxp - tw * 0.5f, cyp - 7.0f * uiScale, cdBuf,
                                 {1.0f, 1.0f, 1.0f}, 2);
        }
```

- [ ] **Step 5: Equipment bar — pass edge color, add readyFlash pop + seconds number**

In `drawEquipSkillBar` (`hud_skill_bar.cpp`):

(a) The border (line 375-377) gains a green ready tint when `readyFlash > 0`:

```cpp
        Vec3 borderCol = ready ? Vec3{0.7f, 0.55f, 0.2f} : Vec3{0.3f, 0.25f, 0.15f};
        if (slot.isPassive) borderCol = ready ? Vec3{0.5f, 0.4f, 0.7f} : Vec3{0.25f, 0.2f, 0.35f};
        if (slot.readyFlash > 0.0f) {
            f32 t = HudCooldown::readyPopT(slot.readyFlash);
            borderCol = {0.4f + 0.6f * t, 0.9f, 0.5f + 0.3f * t};
        }
```

(b) The radial call (line 388) gains a gold edge color:

```cpp
            drawRadialCooldown(cx, cy, radius, cdFrac, {0.15f, 0.12f, 0.2f}, {1.0f, 0.85f, 0.4f});
```

(c) After the existing `flushHUD();` (line 418), add the pop ring + seconds number:

```cpp
        flushHUD();

        // Green "ready" pop when a legendary skill just came off cooldown.
        if (slot.readyFlash > 0.0f) {
            f32 cxp = sx + slotW * 0.5f, cyp = y + slotH * 0.5f;
            drawReadyPop(cxp, cyp, slotW * 0.5f, HudCooldown::readyPopT(slot.readyFlash), uiScale,
                         {0.42f, 0.88f, 0.54f});
            flushHUD();
        }

        // Seconds countdown centered while cooling (passives have cooldownTimer 0 → skipped).
        if (slot.cooldownTimer > 0.0f && HudCooldown::showCooldownNumber(slot.cooldownTimer)) {
            f32 cxp = sx + slotW * 0.5f, cyp = y + slotH * 0.5f;
            char cdBuf[8];
            std::snprintf(cdBuf, sizeof(cdBuf), "%d", HudCooldown::cooldownSeconds(slot.cooldownTimer));
            f32 tw = FontSystem::textWidth(cdBuf, 2);
            FontSystem::drawText(sw, sh, cxp - tw * 0.5f, cyp - 7.0f * uiScale, cdBuf,
                                 {1.0f, 1.0f, 1.0f}, 2);
        }
```

- [ ] **Step 6: Delete the dead `drawSkillCooldown` definition**

In `src/renderer/hud_skill_bar.cpp`, delete the entire `drawSkillCooldown` function (lines 430-454, from `void HUD::drawSkillCooldown(...)` through its closing `}`). (Confirmed zero callers via `grep -rn "drawSkillCooldown" src/`.)

- [ ] **Step 7: Class flash plumbing — per-player, POP_DURATION (engine_hud.cpp)**

In `src/engine/engine_hud.cpp`, add the include near the top:

```cpp
#include "renderer/hud_cooldown_util.h"
```

Replace the class-skill flash block (lines 183-193) with a per-player version at `POP_DURATION`:

```cpp
        // Flash effect: green "ready" pop when a skill comes off cooldown. Per-player
        // (split-screen renders each local player's HUD in turn) and POP_DURATION long
        // so the pop is felt, not a one-frame blink.
        static f32 s_classSkillFlash[MAX_LOCAL_PLAYERS][4] = {};
        static f32 s_prevCooldowns[MAX_LOCAL_PLAYERS][4]   = {};
        u32 clp = m_localPlayerIndex;
        for (u8 s = 0; s < 4; s++) {
            if (s_classSkillFlash[clp][s] > 0.0f) s_classSkillFlash[clp][s] -= 1.0f / 60.0f;
            if (cooldowns[s] <= 0.0f && s_prevCooldowns[clp][s] > 0.0f) {
                s_classSkillFlash[clp][s] = HudCooldown::POP_DURATION;
            }
            s_prevCooldowns[clp][s] = cooldowns[s];
        }
```

Update the `drawClassSkillBar` call (line 200-203) to pass the per-player flash row:

```cpp
        HUD::drawClassSkillBar(sw, sh, skillBarX, skillBarY,
                                m_activeClassSkill, effectiveFloor,
                                cls.skillUnlockFloor, cls.skillUpgradeFloor,
                                cooldowns, maxCooldowns, s_classSkillFlash[clp], skillIdBytes);
```

- [ ] **Step 8: Equip flash plumbing (engine_hud.cpp)**

In `src/engine/engine_hud.cpp`, immediately before the `if (equipCount > 0) {` block (line 270), insert per-slot flash tracking that writes into each slot's `readyFlash`:

```cpp
            // Green "ready" pop tracking for equip skills — parallels the class bar.
            // Index-keyed (equipped set is stable during combat); passives stay at 0.
            static f32 s_equipFlash[MAX_LOCAL_PLAYERS][6]  = {};
            static f32 s_prevEquipCd[MAX_LOCAL_PLAYERS][6] = {};
            u32 eqp = m_localPlayerIndex;
            for (u32 i = 0; i < equipCount; i++) {
                if (s_equipFlash[eqp][i] > 0.0f) s_equipFlash[eqp][i] -= 1.0f / 60.0f;
                if (equipSlots[i].cooldownTimer <= 0.0f && s_prevEquipCd[eqp][i] > 0.0f) {
                    s_equipFlash[eqp][i] = HudCooldown::POP_DURATION;
                }
                s_prevEquipCd[eqp][i] = equipSlots[i].cooldownTimer;
                equipSlots[i].readyFlash = s_equipFlash[eqp][i];
            }
```

- [ ] **Step 9: Verify build + suite**

Run: `cmake --build build && ./build/tests/dungeon_tests --no-version`
Expected: build succeeds; all tests pass (no test covers rendering — this gate confirms the render code compiles and nothing else broke).

- [ ] **Step 10: Manual visual check**

Run: `./build/dungeon_game` — start a run, use a class skill. Expected: while cooling, the slot shows a **bright gold sweep edge** and a **centered white seconds number** ticking down; when it comes back, a **green ring pops** outward and the border flashes green for ~0.4s. Legendary (equipment) skills behave the same.

- [ ] **Step 11: Commit** (only with user authorization)

```bash
git add src/renderer/hud.h src/renderer/hud_skill_bar.cpp src/engine/engine_hud.cpp
git commit -m "feat(hud): readable skill cooldowns — bright edge, seconds, ready pop; drop dead drawSkillCooldown"
```

---

## Task 4: Potion belt flask + remove the stranded top-left prompt

**Files:**
- Modify: `src/renderer/hud.h` (declare `PotionHudState` + `drawPotionFlask`)
- Modify: `src/renderer/hud.cpp` (implement `drawPotionFlask`; add includes)
- Modify: `src/engine/engine_hud.cpp` (remove top-left potion text; draw the flask after the energy bar)

- [ ] **Step 1: Declare `PotionHudState` + `drawPotionFlask` (hud.h)**

In `src/renderer/hud.h`, inside `namespace HUD`, add after the `drawEnergyBar` declaration (line 51):

```cpp
    // Potion belt flask — drawn beside the health/energy bars. Shows cooldown state
    // (same language as the skill slots) plus a low-HP red "drink now" pulse. The
    // potion is an infinite 5s-cooldown heal, so there is no count.
    struct PotionHudState {
        f32 cooldownRemaining; // seconds; >0 = cooling
        f32 maxCooldown;       // denominator for the radial sweep
        f32 healthFrac;        // 0..1 current HP fraction
        f32 readyFlash;        // 0..POP_DURATION; >0 = just came ready (green pop)
        bool urgent;           // low HP + ready -> steady red pulse
        f32 pulsePhase;        // seconds accumulator that drives the red pulse sine
        const char* keyLabel;  // "Q" / "B"
    };
    void drawPotionFlask(u32 sw, u32 sh, f32 x, f32 y, const PotionHudState& st);
```

- [ ] **Step 2: Ensure includes in `hud.cpp`**

At the top of `src/renderer/hud.cpp`, ensure these are present (add any missing):

```cpp
#include "renderer/font.h"
#include "renderer/hud_cooldown_util.h"
#include <cmath>
#include <cstdio>
```

- [ ] **Step 3: Implement `drawPotionFlask` (hud.cpp)**

Add to `src/renderer/hud.cpp` (near `drawHealthBar`, so the health/energy/flask trio lives together):

```cpp
// Potion belt flask — a primitive-drawn flask (no asset) welded beside the health bar.
// States: cooling (radial sweep + seconds number, dimmed) | urgent (steady red pulse when
// low HP + ready) | ready. A green "ready" pop plays on the cooling->ready transition.
void HUD::drawPotionFlask(u32 sw, u32 sh, f32 x, f32 y, const PotionHudState& st) {
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    f32 w = 44.0f * uiScale;
    f32 h = 46.0f * uiScale;
    f32 cx = x + w * 0.5f;
    f32 cy = y + h * 0.5f;
    bool cooling = st.cooldownRemaining > 0.0f;

    // Steady red pulse (no strobe) when urgent — sine of the seconds accumulator.
    f32 pulse = st.urgent ? (0.55f + 0.45f * std::sin(st.pulsePhase * 6.0f)) : 0.0f;

    // Rim / border color per state.
    Vec3 rim = cooling ? Vec3{0.45f, 0.18f, 0.15f}
             : st.urgent ? Vec3{0.7f + 0.3f * pulse, 0.22f, 0.16f}
                         : Vec3{0.75f, 0.28f, 0.22f};

    // Glass body: rounded rect over the lower ~72% of the cell.
    f32 bx0 = x + 10.0f * uiScale, bx1 = x + w - 10.0f * uiScale;
    f32 by0 = y + 2.0f * uiScale,  by1 = y + h * 0.72f;
    pushQuad(bx0, by0, bx1, by1, rim);

    // Red liquid fill (horizontal lines), dim while cooling, brighter/pulsing when urgent.
    f32 lum = cooling ? 0.42f : (st.urgent ? 0.7f + 0.3f * pulse : 0.9f);
    Vec3 liquid = {0.85f * lum, 0.16f * lum + 0.04f, 0.12f * lum + 0.03f};
    for (f32 ly = by0 + 2.0f * uiScale; ly < by1 - 1.0f * uiScale; ly += 1.0f * uiScale) {
        pushLine(bx0 + 2.0f * uiScale, ly, bx1 - 2.0f * uiScale, ly, liquid);
    }

    // Neck + cork.
    f32 nk = 6.0f * uiScale;
    pushLine(cx - nk, by1, cx - nk, y + h - 8.0f * uiScale, rim);
    pushLine(cx + nk, by1, cx + nk, y + h - 8.0f * uiScale, rim);
    pushQuad(cx - nk - 1.0f * uiScale, y + h - 8.0f * uiScale,
             cx + nk + 1.0f * uiScale, y + h - 3.0f * uiScale, {0.55f, 0.4f, 0.25f});

    // Urgent glow: a steady red ring around the whole cell (pulsing, no strobe).
    if (st.urgent) {
        Vec3 glow = Vec3{0.9f, 0.25f, 0.2f} * (0.4f + 0.6f * pulse);
        pushQuad(x + 1.0f, y + 1.0f, x + w - 1.0f, y + h - 1.0f, glow);
    }
    flushHUD();

    // Cooling: radial sweep + centered seconds number (same language as skills).
    if (cooling) {
        f32 frac = (st.maxCooldown > 0.0f) ? st.cooldownRemaining / st.maxCooldown : 0.0f;
        drawRadialCooldown(cx, cy, w * 0.42f, frac, {0.05f, 0.04f, 0.05f}, {1.0f, 0.5f, 0.4f});
        flushHUD();
        if (HudCooldown::showCooldownNumber(st.cooldownRemaining)) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", HudCooldown::cooldownSeconds(st.cooldownRemaining));
            f32 tw = FontSystem::textWidth(buf, 2);
            FontSystem::drawText(sw, sh, cx - tw * 0.5f, cy - 7.0f * uiScale, buf, {1.0f, 1.0f, 1.0f}, 2);
        }
    }

    // Key label above the flask (highlighted when ready to drink).
    drawKeySymbol(sw, sh, x + 4.0f * uiScale, y + h + 2.0f * uiScale, st.keyLabel, !cooling);

    // Green "ready" pop (shared with skills) on the cooling->ready transition.
    if (st.readyFlash > 0.0f) {
        drawReadyPop(cx, cy, w * 0.5f, HudCooldown::readyPopT(st.readyFlash), uiScale,
                     {0.42f, 0.88f, 0.54f});
        flushHUD();
    }
}
```

- [ ] **Step 4: Remove the stranded top-left potion prompt (engine_hud.cpp)**

In `src/engine/engine_hud.cpp`, delete the entire "Potion cooldown indicator" block (lines 388-405, from the `// Potion cooldown indicator...` comment through its closing `}`). The flask replaces it.

- [ ] **Step 5: Draw the flask beside the energy bar (engine_hud.cpp)**

In `src/engine/engine_hud.cpp`, immediately after the energy-bar draw (line 669, `HUD::drawEnergyBar(...)`), insert:

```cpp
        // Potion belt flask — welded to the right of the health/energy bars, where the
        // eye already sits when hurt. Per-player ready-pop tracking mirrors the skill bars.
        {
            f32 phs = static_cast<f32>(sh) / 720.0f;
            static f32 s_potionFlash[MAX_LOCAL_PLAYERS]  = {};
            static f32 s_prevPotionCd[MAX_LOCAL_PLAYERS] = {};
            u32 pp = m_localPlayerIndex;
            if (s_potionFlash[pp] > 0.0f) s_potionFlash[pp] -= 1.0f / 60.0f;
            if (m_potionCooldown <= 0.0f && s_prevPotionCd[pp] > 0.0f) {
                s_potionFlash[pp] = HudCooldown::POP_DURATION;
            }
            s_prevPotionCd[pp] = m_potionCooldown;

            f32 hpFrac = (m_localPlayer.maxHealth > 0.0f)
                       ? m_localPlayer.health / m_localPlayer.maxHealth : 1.0f;
            HUD::PotionHudState ps;
            ps.cooldownRemaining = m_potionCooldown;
            ps.maxCooldown       = GameConst::POTION_COOLDOWN;
            ps.healthFrac        = hpFrac;
            ps.readyFlash        = s_potionFlash[pp];
            ps.urgent            = HudCooldown::potionUrgent(hpFrac, m_potionCooldown,
                                                             GameConst::LOW_HP_FRACTION);
            ps.pulsePhase        = m_statsTimer;
            ps.keyLabel          = Input::isGamepadConnected(0) ? "B" : "Q";
            // x = health bar x0(20) + barW(200) + 8px gap; y = 18px (spans both bars).
            HUD::drawPotionFlask(sw, sh, 228.0f * phs, 18.0f * phs, ps);
        }
```

If the build reports `GameConst` undeclared, add `#include "game/game_constants.h"` to `engine_hud.cpp`. (`Input` and `renderer/hud_cooldown_util.h` are already included — the latter from Task 3 Step 7.)

- [ ] **Step 6: Verify build + suite**

Run: `cmake --build build && ./build/tests/dungeon_tests --no-version`
Expected: build succeeds; all tests pass.

- [ ] **Step 7: Manual visual check**

Run: `./build/dungeon_game`. Expected:
- A **red flask sits just right of the health bar** with a `Q` (or `B` on gamepad) label.
- Drink it → flask **dims, shows a radial sweep + seconds countdown**; when it returns, a **green ring pops**.
- Take damage to ≤25% HP → the flask **pulses red in sync with the red screen-edge vignette** (steady, no strobe). Drink → red pulse stops.
- The old top-left "Potion" text is **gone**.

- [ ] **Step 8: Commit** (only with user authorization)

```bash
git add src/renderer/hud.h src/renderer/hud.cpp src/engine/engine_hud.cpp
git commit -m "feat(hud): potion belt flask welded to the health bar with low-HP pulse"
```

---

## Final review (after all tasks)

- [ ] **Full build, all targets:** `cmake --build build && ./build/tests/dungeon_tests --no-version` → green.
- [ ] **Grep for leftovers:** `grep -rn "drawSkillCooldown" src/` → no results (dead code fully removed).
- [ ] **Split-screen check:** start a 2-player couch game; confirm each viewport has its own flask + skill cooldown visuals, and one player's cooldowns/pops don't leak into the other's HUD.
- [ ] **Switch build (delivery parity):** build the NRO and confirm the flask + cooldown numbers are legible at handheld resolution.
- [ ] **Dispatch a final code review** over the whole change (correctness of the flash static indexing, no per-frame heap, no NaN when `maxHealth`/`maxCooldown` is 0).

## Notes / decisions carried from the spec

- **Flash tracking lives in the HUD render path** (statics keyed by `m_localPlayerIndex`), mirroring the existing class-skill flash — so no `engine.h` field or update-loop changes are needed (a refinement over the spec's original "add `m_potionReadyFlash` + detect in `engine_update_player.cpp`"; same behavior, smaller blast radius).
- **`drawPotionFlask` lives in `hud.cpp`** (not a new file) so the health/energy/flask trio is cohesive and no `src/CMakeLists.txt` change is needed.
- **Flask radial denominator** is `POTION_COOLDOWN` (5.0s). With cooldown-reduction the effective cooldown is shorter, so the sweep may start slightly under full — cosmetic only. If exactness is wanted later, pass the effective (CDR-adjusted) max instead.
- **Equip ready-pop is index-keyed** (stable during combat). A gear swap mid-fight could mistime one pop — cosmetic, acceptable.
