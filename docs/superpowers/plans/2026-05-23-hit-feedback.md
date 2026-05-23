# Hit Feedback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make landing and taking hits feel impactful via a centralized, tier-based feedback system (shake, particles, knockback, crit/kill emphasis, take-damage vignette/indicator/rumble).

**Architecture:** Every hit is classified into an `ImpactTier` (LIGHT/HEAVY/CRIT/KILL) at the two combat choke points. `Combat::applyDamage` (you hit something) applies authoritative knockback and tier-driven cosmetic FX inline using the `ParticlePool`/`ScreenShake` pointers it already holds via `setFXTargets`. `Combat::applyDamageToPlayer` (you got hit) fires a new player-hit callback that the engine turns into vignette + directional indicator + camera kick + rumble. Per-tier parameters live in one `constexpr` table.

**Tech Stack:** C++17, OpenGL 3.3, SDL2. No unit-test framework — pure logic is verified with `static_assert`, everything else by `cmake --build build` + running `./build/dungeon_game` and observing. Spec: `docs/superpowers/specs/2026-05-23-hit-feedback-design.md`.

**Design refinement vs spec:** The spec described emitting one `HitEvent` to an engine handler for *all* cosmetics. Since `combat.cpp` already owns the particle pool + screen shake (`setFXTargets`, combat.cpp:37), the dealing-damage cosmetics (shake/particles) live directly in `applyDamage`, tier-driven from the table — still centralized in one function, no scattered call-site triggers. Only the **player-hit** side needs a callback to the engine (vignette/indicator/rumble are engine/render/input concerns).

---

## File Structure

- **Create** `src/game/hit_feedback.h` — `ImpactTier` enum, `HitFeedbackTier` struct, `constexpr kHitTiers[]` table, `constexpr classifyTier()` pure fn + its `static_assert` tests. One focused unit; included by combat.
- **Modify** `src/game/combat.h` — add `bool isCrit` param to `applyDamage`; add `PlayerHitCallback` typedef + `setPlayerHitCallback`.
- **Modify** `src/game/combat.cpp` — tier classification, inline tier-driven shake/particles, authoritative knockback in `applyDamage`; fire player-hit callback in `applyDamageToPlayer`.
- **Modify** `src/game/entity.h` — add `f32 knockbackTimer` (drives velocity decay) if entities don't already damp horizontal velocity (verified in Task 3).
- **Modify** `src/engine/engine.h` — `m_damageVignette` (f32), `m_dmgIndicatorTimer` (f32), `m_dmgIndicatorDir` (Vec3).
- **Modify** `src/engine/engine_init.cpp` — register the player-hit callback.
- **Modify** `src/engine/engine_update.cpp` — tick the vignette + indicator timers.
- **Modify** `src/engine/engine_render.cpp` — draw the red vignette overlay (reuse the `m_fadeFromBlack` fullscreen-quad path).
- **Modify** `src/engine/engine_hud.cpp` — draw the directional damage indicator.
- **Modify** `src/platform/input.h` / `src/platform/input.cpp` — `Input::rumble(f32 strength, u32 ms)` wrapping `SDL_GameControllerRumble` (Task 7, after verifying availability).
- **Modify** `CLAUDE.md` — document the hit-feedback system + tier table.

---

## Task 1: Impact-tier model (pure logic, static_assert-tested)

**Files:**
- Create: `src/game/hit_feedback.h`

- [ ] **Step 1: Write the failing test (static_asserts) + the types**

Create `src/game/hit_feedback.h`:

```cpp
#pragma once
// Hit-feedback tiering: classifies every hit into one impact tier and maps each
// tier to a feedback "recipe" (shake, knockback, particle counts). Pure data +
// one pure classify function so it is trivially correct and tunable in one place.
// See docs/superpowers/specs/2026-05-23-hit-feedback-design.md.
#include "core/types.h"

enum struct ImpactTier : u8 { LIGHT = 0, HEAVY = 1, CRIT = 2, KILL = 3, COUNT = 4 };

struct HitFeedbackTier {
    f32 shakeIntensity;   // metres of camera shake (0 = none)
    f32 shakeDuration;    // seconds
    f32 knockback;        // base impulse (m/s) added to victim velocity (0 = none)
    u8  bloodCount;       // blood particles
    u8  sparkCount;       // spark particles
    u8  debrisCount;      // debris particles (kills)
    bool smoke;           // emit a smoke puff (kills)
    f32 hitStopMs;        // RESERVED for deferred freeze-frame; 0 in v1
};

// Tunable recipes, indexed by ImpactTier. Tune game feel here.
static constexpr HitFeedbackTier kHitTiers[static_cast<u32>(ImpactTier::COUNT)] = {
    /* LIGHT */ { 0.015f, 0.10f, 0.0f,  2, 1, 0, false, 0.0f },
    /* HEAVY */ { 0.05f,  0.18f, 4.0f,  5, 2, 0, false, 0.0f },
    /* CRIT  */ { 0.06f,  0.20f, 5.0f,  4, 6, 0, false, 0.0f },
    /* KILL  */ { 0.09f,  0.30f, 0.0f,  6, 3, 5, true,  0.0f },
};

// A hit counts as HEAVY when it removes at least this fraction of the victim's max HP.
static constexpr f32 HEAVY_HIT_HP_FRACTION = 0.20f;

// Pure classification. Precedence: KILL > CRIT > HEAVY > LIGHT.
constexpr ImpactTier classifyTier(f32 damage, f32 victimMaxHp, bool isCrit, bool isKill) {
    if (isKill) return ImpactTier::KILL;
    if (isCrit) return ImpactTier::CRIT;
    if (victimMaxHp > 0.0f && damage >= victimMaxHp * HEAVY_HIT_HP_FRACTION)
        return ImpactTier::HEAVY;
    return ImpactTier::LIGHT;
}

// Compile-time tests (this project has no runtime test framework).
static_assert(classifyTier(5.0f, 100.0f, false, false) == ImpactTier::LIGHT,  "small hit = LIGHT");
static_assert(classifyTier(25.0f, 100.0f, false, false) == ImpactTier::HEAVY, "25% of maxHP = HEAVY");
static_assert(classifyTier(5.0f, 100.0f, true,  false) == ImpactTier::CRIT,   "crit overrides LIGHT");
static_assert(classifyTier(50.0f, 100.0f, false, true) == ImpactTier::KILL,   "kill overrides all");
static_assert(classifyTier(50.0f, 100.0f, true,  true) == ImpactTier::KILL,   "kill beats crit");
static_assert(classifyTier(1.0f, 0.0f,   false, false) == ImpactTier::LIGHT,  "zero maxHP = LIGHT, no div");
```

- [ ] **Step 2: Verify it compiles (static_asserts pass)**

Add a temporary `#include "game/hit_feedback.h"` near the top of `src/game/combat.cpp` (kept permanently in Task 2). Run: `cmake --build build 2>&1 | grep -iE "error|static_assert" | head`
Expected: no output (all static_asserts hold, file compiles).

- [ ] **Step 3: Commit**

```bash
git add src/game/hit_feedback.h src/game/combat.cpp
git commit -m "feat(feedback): impact-tier model + tunable recipe table"
```

---

## Task 2: Tier-driven dealing-damage FX in applyDamage

**Files:**
- Modify: `src/game/combat.h` (add `isCrit` param)
- Modify: `src/game/combat.cpp` (`applyDamage`)

- [ ] **Step 1: Add `isCrit` param to applyDamage declaration**

In `src/game/combat.h`, change the `applyDamage` declaration (currently lines ~30-31) to:

```cpp
    void applyDamage(EntityPool& pool, EntityHandle target, f32 damage,
                     const Vec3* damageOrigin = nullptr, bool isCrit = false);
```

- [ ] **Step 2: Classify tier + emit cosmetic FX in applyDamage**

In `src/game/combat.cpp`, update the `applyDamage` definition signature to match (`..., const Vec3* damageOrigin, bool isCrit`). The function already computes `e->health -= damage;`, sets `e->flashTimer = 0.12f;`, spawns the damage number, and (Task added earlier) calls `killEntity` when health <= 0. Replace the existing damage-number line and add tier FX. Find:

```cpp
    e->health -= damage;
    e->flashTimer = 0.12f;

    // Auto-spawn floating damage number at entity position
    if (s_damageNumberCallback) s_damageNumberCallback(e->position + Vec3{0, 0.5f, 0}, damage);
```

Replace with:

```cpp
    e->health -= damage;
    e->flashTimer = 0.12f;

    // --- Hit feedback: classify impact tier and fire the matching recipe ---
    bool isKill = (e->health <= 0.0f);
    ImpactTier tier = classifyTier(damage, e->maxHealth, isCrit, isKill);
    const HitFeedbackTier& fx = kHitTiers[static_cast<u32>(tier)];
    Vec3 hitPos = e->position + Vec3{0, 0.5f, 0};

    // Camera shake (trigger() keeps the stronger of current/new, so spam is fine).
    if (s_screenShake && fx.shakeIntensity > 0.0f)
        s_screenShake->trigger(fx.shakeIntensity, fx.shakeDuration);

    // Particles. Blood/sparks fly back along the hit direction; debris/smoke on kills.
    if (s_particlePool) {
        Vec3 back = {0, 1, 0};
        if (damageOrigin) {
            Vec3 d = e->position - *damageOrigin; d.y = 0.0f;
            f32 len = sqrtf(d.x*d.x + d.z*d.z);
            if (len > 0.001f) back = Vec3{d.x/len, 0.4f, d.z/len};
        }
        if (fx.bloodCount)  Particles::spawnBlood(*s_particlePool, hitPos, back, fx.bloodCount);
        if (fx.sparkCount)  Particles::spawnSparks(*s_particlePool, hitPos, back, fx.sparkCount);
        if (fx.debrisCount) Particles::spawnDebris(*s_particlePool, hitPos, fx.debrisCount);
        if (fx.smoke)       Particles::spawnSmoke(*s_particlePool, hitPos, 4);
    }

    // Floating damage number (crit styling carried through to the callback in Task 4).
    if (s_damageNumberCallback) s_damageNumberCallback(hitPos, damage);
```

Ensure `#include "game/hit_feedback.h"` is present near the top of combat.cpp (added in Task 1). `Particles::*` come from `renderer/particles.h` which combat.cpp already includes.

- [ ] **Step 3: Build**

Run: `cmake --build build 2>&1 | grep -iE "error" | head`
Expected: no errors. (All existing `applyDamage` calls still compile — the new params default.)

- [ ] **Step 4: In-game verification**

Run `./build/dungeon_game`, start a game, hit enemies:
- Light hits: small blood puff + faint shake.
- Hits that remove ≥20% of an enemy's HP: bigger blood burst + noticeable shake.
- Kills: debris + smoke + strong shake.
Expected: feedback scales with hit size; no crash; framerate steady (press F3 for profiler).

- [ ] **Step 5: Commit**

```bash
git add src/game/combat.h src/game/combat.cpp
git commit -m "feat(feedback): tier-driven shake + particles in applyDamage"
```

---

## Task 3: Authoritative knockback

**Files:**
- Modify: `src/game/combat.cpp` (`applyDamage`)
- Modify: `src/game/entity.h` (add decay field if needed)
- Modify: `src/game/entity.cpp` (`tickTimers` — decay) if needed

> **Note:** KILL-tier knockback is intentionally `0.0f` in `kHitTiers`. `Combat::killEntity` (the death path) zeroes `velocity`, so any impulse on a killing blow would be cancelled anyway — kills convey impact via debris/smoke/shake, not a sliding corpse. Knockback therefore only fires on HEAVY/CRIT non-killing hits (whose victims keep `health > 0` and never reach `killEntity`).

- [ ] **Step 1: Investigate whether entity horizontal velocity already damps**

Run: `grep -n "velocity" src/world/collision.cpp src/game/entity.cpp | grep -iE "0\.9|damp|fric|\\*=|drag" | head`
Decision: if `Collision::moveAndSlide` already multiplies entity horizontal velocity by a damping factor each tick, **skip Step 2** (knockback will decay naturally). If not, add the `knockbackTimer` field + decay in Step 2.

- [ ] **Step 2 (only if no existing damping): Add knockback decay**

In `src/game/entity.h`, add to the `Entity` struct (near other timers like `flashTimer`):

```cpp
    f32  knockbackTimer = 0.0f;  // >0 while a knockback impulse is decaying
```

In `src/game/entity.cpp` `tickTimers`, near the other timer ticks (after `flashTimer`), add:

```cpp
        if (e.knockbackTimer > 0.0f) {
            e.knockbackTimer -= dt;
            // Exponential-ish decay so the push settles in ~0.25s.
            e.velocity.x *= 0.85f;
            e.velocity.z *= 0.85f;
            if (e.knockbackTimer <= 0.0f) { e.velocity.x = 0.0f; e.velocity.z = 0.0f; }
        }
```

- [ ] **Step 3: Apply knockback in applyDamage**

In `src/game/combat.cpp` `applyDamage`, immediately AFTER the FX block from Task 2 and BEFORE the kill check, add:

```cpp
    // --- Knockback (authoritative): push the victim along the hit direction. ---
    // Only when we know where the hit came from (no DoT/environmental knockback).
    if (fx.knockback > 0.0f && damageOrigin) {
        Vec3 push = e->position - *damageOrigin; push.y = 0.0f;
        f32 len = sqrtf(push.x*push.x + push.z*push.z);
        if (len > 0.001f) {
            // Size/mass resistance: larger enemies move less; bosses barely budge.
            f32 sizeResist = 0.5f / (e->halfExtents.x + e->halfExtents.z + 0.001f);
            if (sizeResist > 1.0f) sizeResist = 1.0f;
            if (e->bossDefIdx != 0xFF) sizeResist *= 0.1f; // bosses ~immovable
            f32 impulse = fx.knockback * sizeResist;
            e->velocity.x += (push.x / len) * impulse;
            e->velocity.z += (push.z / len) * impulse;
            e->knockbackTimer = 0.25f; // only set if the field exists (Step 2)
        }
    }
```

If Step 2 was skipped (existing damping), delete the `e->knockbackTimer = 0.25f;` line.

- [ ] **Step 4: Build**

Run: `cmake --build build 2>&1 | grep -iE "error" | head`
Expected: no errors.

- [ ] **Step 5: In-game verification**

Run the game. Hit a regular enemy with a strong/melee hit: it should jolt backward and settle (not slide forever, not teleport). Hit a boss: it should barely move (stagger via flash/shake instead). In a hosted co-op game (if testable), confirm the knocked-back enemy ends up at the same spot on host and client (knockback is authoritative → synced via snapshot).

- [ ] **Step 6: Commit**

```bash
git add src/game/combat.cpp src/game/entity.h src/game/entity.cpp
git commit -m "feat(feedback): authoritative knockback with size/boss resistance"
```

---

## Task 4: Crit & kill number styling + wire crit detection

**Files:**
- Modify: `src/game/combat.cpp` (pass tier/crit to the number callback)
- Modify: `src/game/combat.h` (extend `DamageNumberCallback` signature)
- Modify: `src/engine/engine_init.cpp` and `src/engine/engine_update.cpp` (`spawnDamageNumber`) to honor crit/kill styling
- Modify: weapon-fire crit sites (wire `isCrit` into `applyDamage` calls)

- [ ] **Step 1: Extend the damage-number callback to carry crit + kill**

In `src/game/combat.h`, find the `DamageNumberCallback` typedef (near line 60-66) and change it to:

```cpp
    using DamageNumberCallback = void(*)(Vec3 position, f32 amount, bool isCrit, bool isKill);
```

- [ ] **Step 2: Update combat.cpp number calls**

In `src/game/combat.cpp`, the `spawnDamageNumber` free function and the two call sites must pass the new args. Update the `applyDamage` number line (from Task 2) to:

```cpp
    if (s_damageNumberCallback) s_damageNumberCallback(hitPos, damage, isCrit, isKill);
```

And update `Combat::spawnDamageNumber` (combat.cpp ~21-22) to forward `false, false`:

```cpp
void Combat::spawnDamageNumber(Vec3 position, f32 amount) {
    if (s_damageNumberCallback) s_damageNumberCallback(position, amount, false, false);
}
```

- [ ] **Step 3: Update the engine-side callback to honor styling**

In `src/engine/engine_init.cpp`, find where `Combat::setDamageNumberCallback(...)` is registered (it calls `s_engine->spawnDamageNumber(...)`, ~line 565/1040). Update the lambda to match the new signature and forward crit/kill into the existing `Engine::spawnDamageNumber(pos, amount, isHeal, isCrit)`:

```cpp
    Combat::setDamageNumberCallback([](Vec3 pos, f32 amount, bool isCrit, bool isKill) {
        if (s_engine) s_engine->spawnDamageNumber(pos, amount, /*isHeal*/false, isCrit || isKill);
    });
```

(`Engine::spawnDamageNumber` already styles crits bigger; kills reuse the crit style. No new HUD code needed.)

- [ ] **Step 4: Wire crit detection into applyDamage calls**

Run: `grep -rn "isCrit\|crit\|Crit" src/engine/engine_combat.cpp src/game/combat.cpp | head`
For each weapon-fire path that already computes a crit (e.g. a `bool crit = ...` before calling `applyDamage`), pass it as the new 5th arg: `Combat::applyDamage(pool, h, dmg, &origin, crit);`. If NO crit computation exists yet, leave calls as-is (CRIT tier simply won't trigger) and note in CLAUDE.md that crit wiring is pending — do not invent a crit system here (out of scope).

- [ ] **Step 5: Build**

Run: `cmake --build build 2>&1 | grep -iE "error" | head`
Expected: no errors (all `s_damageNumberCallback` and `setDamageNumberCallback` uses updated).

- [ ] **Step 6: In-game verification**

Run the game. Killing blows and crits (if crit wiring exists) show the larger/styled number; normal hits show the normal number.

- [ ] **Step 7: Commit**

```bash
git add src/game/combat.h src/game/combat.cpp src/engine/engine_init.cpp src/engine/engine_combat.cpp
git commit -m "feat(feedback): crit/kill damage-number styling"
```

---

## Task 5: Player-hit callback + take-damage vignette + camera kick

**Files:**
- Modify: `src/game/combat.h` (player-hit callback typedef + setter)
- Modify: `src/game/combat.cpp` (`applyDamageToPlayer` fires it; setter storage)
- Modify: `src/engine/engine.h` (`m_damageVignette`)
- Modify: `src/engine/engine_init.cpp` (register callback)
- Modify: `src/engine/engine_update.cpp` (tick vignette)
- Modify: `src/engine/engine_render.cpp` (draw red overlay)

- [ ] **Step 1: Add the player-hit callback to combat**

In `src/game/combat.h`, near the other callback typedefs, add:

```cpp
    // Fired when the local-authority player takes damage (engine turns this into
    // vignette / directional indicator / camera kick / rumble). attackerPos may be null.
    using PlayerHitCallback = void(*)(const Vec3* attackerPos, f32 damage, f32 healthFrac);
    void setPlayerHitCallback(PlayerHitCallback cb);
```

In `src/game/combat.cpp`, add storage + setter near the other callbacks (lines ~10-27):

```cpp
static Combat::PlayerHitCallback s_playerHitCallback = nullptr;
void Combat::setPlayerHitCallback(PlayerHitCallback cb) { s_playerHitCallback = cb; }
```

At the END of `Combat::applyDamageToPlayer` (after health is reduced), add:

```cpp
    if (s_playerHitCallback) {
        f32 frac = (player.maxHealth > 0.0f) ? (player.health / player.maxHealth) : 0.0f;
        s_playerHitCallback(attackerPos, damage, frac);
    }
```

- [ ] **Step 2: Add engine vignette state**

In `src/engine/engine.h`, near `m_fadeFromBlack`, add:

```cpp
    f32 m_damageVignette = 0.0f;   // 0..1 red screen pulse, decays each frame
```

- [ ] **Step 3: Register the callback**

In `src/engine/engine_init.cpp`, near the other `Combat::setXxxCallback` registrations, add:

```cpp
    Combat::setPlayerHitCallback([](const Vec3* attackerPos, f32 damage, f32 healthFrac) {
        if (!s_engine) return;
        // Vignette intensity scales with how big the hit was relative to max HP.
        f32 hitFrac = damage / 100.0f; if (hitFrac > 1.0f) hitFrac = 1.0f;
        f32 v = 0.35f + hitFrac * 0.5f;
        if (v > s_engine->m_damageVignette) s_engine->m_damageVignette = v;
        // Light inward camera kick (distinct from the heavier outgoing-hit shake).
        s_engine->m_camera.shake.trigger(0.04f, 0.15f);
        // Directional indicator + rumble wired in Tasks 6 and 7.
        (void)attackerPos; (void)healthFrac;
    });
```

(If `m_camera`/`m_damageVignette` aren't accessible from the lambda, make them public or add an `Engine::onPlayerHit(...)` member method and call that — match how other callbacks reach engine state via `s_engine`.)

- [ ] **Step 4: Tick the vignette down**

In `src/engine/engine_update.cpp`, near where `m_fadeFromBlack` is decayed (or in the main per-frame update), add:

```cpp
    if (m_damageVignette > 0.0f) {
        m_damageVignette -= dt * 2.5f;   // ~0.4s fade
        if (m_damageVignette < 0.0f) m_damageVignette = 0.0f;
    }
```

- [ ] **Step 5: Render the red vignette overlay**

In `src/engine/engine_render.cpp`, find where `m_fadeFromBlack` draws its fullscreen quad (unlit shader). Immediately after that block, add an analogous draw using red with alpha = `m_damageVignette`:

```cpp
    if (m_damageVignette > 0.0f) {
        // Reuse the same fullscreen-quad + unlit-shader path as the fade overlay,
        // but tinted red and semi-transparent. Edge-darkened if a vignette mask
        // exists; otherwise a flat red wash is acceptable for v1.
        drawFullscreenQuad(Vec4{0.6f, 0.0f, 0.0f, m_damageVignette * 0.5f});
    }
```

Use the exact helper/inline GL the fade overlay uses (match its blend state); name it to match the existing code (the snippet's `drawFullscreenQuad` is illustrative — call the real fade-overlay draw with a red color + alpha).

- [ ] **Step 6: Build**

Run: `cmake --build build 2>&1 | grep -iE "error" | head`
Expected: no errors.

- [ ] **Step 7: In-game verification**

Run the game, let an enemy hit you: screen flashes red (stronger for bigger hits, fades in ~0.4s) and the camera gives a small kick.

- [ ] **Step 8: Commit**

```bash
git add src/game/combat.h src/game/combat.cpp src/engine/engine.h src/engine/engine_init.cpp src/engine/engine_update.cpp src/engine/engine_render.cpp
git commit -m "feat(feedback): take-damage red vignette + camera kick"
```

---

## Task 6: Directional damage indicator

**Files:**
- Modify: `src/engine/engine.h` (`m_dmgIndicatorTimer`, `m_dmgIndicatorDir`)
- Modify: `src/engine/engine_init.cpp` (set them in the player-hit callback)
- Modify: `src/engine/engine_update.cpp` (tick the timer)
- Modify: `src/engine/engine_hud.cpp` (draw the wedge)

- [ ] **Step 1: Add indicator state**

In `src/engine/engine.h`, near `m_damageVignette`:

```cpp
    f32 m_dmgIndicatorTimer = 0.0f;  // seconds remaining
    Vec3 m_dmgIndicatorDir  = {0,0,0}; // world dir from player to attacker (xz)
```

- [ ] **Step 2: Populate them in the player-hit callback**

In `src/engine/engine_init.cpp`, inside the `setPlayerHitCallback` lambda from Task 5, replace `(void)attackerPos;` with:

```cpp
        if (attackerPos) {
            Vec3 d = *attackerPos - s_engine->m_localPlayer.position; d.y = 0.0f;
            f32 len = sqrtf(d.x*d.x + d.z*d.z);
            if (len > 0.001f) {
                s_engine->m_dmgIndicatorDir = Vec3{d.x/len, 0.0f, d.z/len};
                s_engine->m_dmgIndicatorTimer = 1.0f;
            }
        }
```

- [ ] **Step 3: Tick it down**

In `src/engine/engine_update.cpp`, next to the vignette tick (Task 5 Step 4):

```cpp
    if (m_dmgIndicatorTimer > 0.0f) {
        m_dmgIndicatorTimer -= dt;
        if (m_dmgIndicatorTimer < 0.0f) m_dmgIndicatorTimer = 0.0f;
    }
```

- [ ] **Step 4: Draw the wedge in the HUD**

In `src/engine/engine_hud.cpp` (where `renderHUD` draws crosshair/hit-marker), add, using the player's yaw to convert the world dir into a screen angle:

```cpp
    if (m_dmgIndicatorTimer > 0.0f) {
        // Angle of the attacker relative to where the player faces.
        f32 worldAng = atan2f(m_dmgIndicatorDir.x, m_dmgIndicatorDir.z);
        f32 rel = worldAng - m_localPlayer.yaw;            // 0 = directly ahead
        f32 alpha = m_dmgIndicatorTimer; if (alpha > 1.0f) alpha = 1.0f;
        // Place a small red arc/triangle at screen-centre offset by `rel`, e.g. on a
        // ring of radius 0.18*sh around the crosshair. Reuse HUD::pushLine / a HUD
        // triangle helper used elsewhere in this file; colour {0.9,0.1,0.1} * alpha.
        HUD::drawDamageWedge(sw, sh, rel, alpha);
    }
```

Implement `HUD::drawDamageWedge(u32 sw, u32 sh, f32 relAngle, f32 alpha)` in `src/renderer/hud.{h,cpp}` using the same batched line/triangle primitives the file already uses for the hit marker (place a filled triangle on a ring at screen-centre, rotated by `relAngle`, pointing inward). Match the existing HUD draw idiom; flush with the HUD batch (do not add a stray `flushHUD` inside a loop — see the audit note).

- [ ] **Step 5: Build + verify**

Run: `cmake --build build 2>&1 | grep -iE "error" | head` → no errors.
Run the game: get hit from the side/behind → a red wedge points toward the attacker and fades over ~1s; rotates correctly as you turn.

- [ ] **Step 6: Commit**

```bash
git add src/engine/engine.h src/engine/engine_init.cpp src/engine/engine_update.cpp src/engine/engine_hud.cpp src/renderer/hud.h src/renderer/hud.cpp
git commit -m "feat(feedback): directional damage indicator"
```

---

## Task 7: Controller rumble

**Files:**
- Modify: `src/platform/input.h`, `src/platform/input.cpp`
- Modify: `src/engine/engine_init.cpp` (call rumble in player-hit callback)

- [ ] **Step 1: Verify SDL game-controller handle availability**

Run: `grep -niE "SDL_GameController|SDL_Joystick|controller|gamepad" src/platform/input.cpp | head`
Expected: the input layer holds an `SDL_GameController*` per player. If only `SDL_Joystick*` is stored, use `SDL_JoystickRumble` instead in Step 2. If neither, add rumble as a no-op stub and note it (do not restructure input).

- [ ] **Step 2: Add a rumble helper**

In `src/platform/input.h`, declare in the `Input` namespace:

```cpp
    // Rumble player `slot`'s controller at `strength` (0..1) for `ms` milliseconds.
    void rumble(u8 slot, f32 strength, u32 ms);
```

In `src/platform/input.cpp`, implement using the stored controller handle (replace `s_controllers[slot]` with the file's actual member name found in Step 1):

```cpp
void Input::rumble(u8 slot, f32 strength, u32 ms) {
    if (slot >= MAX_PLAYERS) return;
    SDL_GameController* gc = s_controllers[slot];
    if (!gc) return;
    if (strength < 0.0f) strength = 0.0f; if (strength > 1.0f) strength = 1.0f;
    Uint16 mag = static_cast<Uint16>(strength * 65535.0f);
    SDL_GameControllerRumble(gc, mag, mag, ms);
}
```

- [ ] **Step 3: Call rumble on player hit**

In `src/engine/engine_init.cpp`, inside the `setPlayerHitCallback` lambda, add:

```cpp
        f32 r = 0.3f + hitFrac * 0.5f;
        Input::rumble(s_engine->m_localPlayerIndex, r, 120 + static_cast<u32>(hitFrac * 180.0f));
```

(Ensure `#include "platform/input.h"` is present in engine_init.cpp — it almost certainly is.)

- [ ] **Step 4: Build + verify**

Run: `cmake --build build 2>&1 | grep -iE "error" | head` → no errors.
With a gamepad connected, take a hit → controller rumbles, harder for bigger hits. (Keyboard-only: no-op, no crash.)

- [ ] **Step 5: Commit**

```bash
git add src/platform/input.h src/platform/input.cpp src/engine/engine_init.cpp
git commit -m "feat(feedback): controller rumble on taking damage"
```

---

## Task 8 (optional): Low-HP danger state

**Files:**
- Modify: `src/engine/engine_update.cpp`, `src/engine/engine_render.cpp`

- [ ] **Step 1: Add a subtle persistent vignette under 25% HP**

In `src/engine/engine_update.cpp`, after the vignette tick, add a floor based on health:

```cpp
    f32 hpFrac = (m_localPlayer.maxHealth > 0.0f) ? (m_localPlayer.health / m_localPlayer.maxHealth) : 1.0f;
    if (hpFrac < 0.25f) {
        // Slow pulse; never lower than this floor while in danger.
        f32 pulse = 0.12f + 0.06f * sinf(static_cast<f32>(Clock::getElapsedSeconds()) * 4.0f);
        if (pulse > m_damageVignette) m_damageVignette = pulse;
    }
```

- [ ] **Step 2: Build + verify**

`cmake --build build` → no errors. Drop below 25% HP in-game → a gentle red pulse persists; clears when healed above 25%.

- [ ] **Step 3: Commit**

```bash
git add src/engine/engine_update.cpp
git commit -m "feat(feedback): low-HP danger vignette pulse"
```

---

## Task 9: Switch tuning + docs

**Files:**
- Modify: `src/game/hit_feedback.h` (Switch particle scaling, if needed)
- Modify: `CLAUDE.md`

- [ ] **Step 1: Profiler check at scale**

Run `./build/dungeon_game`, press F5 (spawn 50 enemies), fight into heavy/kill feedback, press F3 (profiler). Confirm frame time stays within budget. If particle spikes appear, reduce the `*Count` values in `kHitTiers` (single table — that's the tuning point), or guard counts with `#ifdef __SWITCH__` halved values in `hit_feedback.h`.

- [ ] **Step 2: Document in CLAUDE.md**

Add to `CLAUDE.md` (near the Data Lifecycles / combat notes) a short paragraph:

```markdown
**Hit feedback.** `Combat::applyDamage` classifies every hit into an `ImpactTier`
(LIGHT/HEAVY/CRIT/KILL, `game/hit_feedback.h`) and fires the matching recipe from
`kHitTiers`: camera shake + particles inline (combat holds the FX pointers via
`setFXTargets`) and a tier-tagged damage number. Knockback is applied
authoritatively in `applyDamage` (boss/size-resisted) so it syncs over the network.
Taking damage fires `Combat::setPlayerHitCallback` → engine vignette + directional
indicator + camera kick + rumble. Tune all feel from the `kHitTiers` table. Hit-stop
is deferred (the `hitStopMs` field is reserved, 0 in v1).
```

- [ ] **Step 3: Commit**

```bash
git add src/game/hit_feedback.h CLAUDE.md
git commit -m "docs(feedback): Switch tuning notes + CLAUDE.md"
```

---

## Notes for the implementer
- This codebase has **no runtime test framework**; Task 1 uses `static_assert` for the only pure logic. Every other task is verified by `cmake --build build` then running `./build/dungeon_game` and observing the described behavior + F3 profiler.
- Match existing patterns: callbacks reach engine state via the file-scope `s_engine` pointer (see existing `Combat::setDeathCallback`/`setDamageNumberCallback` registrations in `engine_init.cpp`).
- Reuse, don't reinvent: `ScreenShake::trigger`, `Particles::spawn*`, the `m_fadeFromBlack` fullscreen-quad path, `Engine::spawnDamageNumber`, and the HUD batch primitives all already exist.
- Knockback is the only authoritative (gameplay-affecting) change; everything else is client-local cosmetics — keep it that way for co-op correctness.
```
