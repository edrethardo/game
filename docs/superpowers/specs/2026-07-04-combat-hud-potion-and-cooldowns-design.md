# Combat HUD: Potion Discoverability + Skill Cooldown Readability — Design Spec

**Date:** 2026-07-04
**Status:** Approved (design); pending spec review → implementation plan
**Scope owner:** DungeonEngine HUD/renderer

## Goal

Make the in-game HUD read at combat speed on two specific pain points players hit today:
1. **They don't find the potion.** The heal prompt is stranded in the top-left corner, far from where combat attention lives.
2. **Skill cooldowns are unreadable.** A near-black pie over a dark slot with no countdown and a one-frame "ready" flash.

Fix both with **one shared "ready / urgent" visual language**, keeping the change to pure HUD rendering — no gameplay, networking, save, or input-binding changes.

## Architecture (approach in 3 sentences)

The potion moves from a top-left text label to a **belt flask welded to the health/energy bars** (bottom-left), rendered with the *same cooldown language as the skill slots* (radial sweep + centered seconds), plus a low-HP red "drink now" pulse. The skill slots (class + equipment bars) gain a **bright sweep leading-edge, a centered seconds countdown, and a green "ready pop"** (expanding ring + border flash) when an ability returns. A single shared helper `HUD::drawReadyPop(...)` and a single low-HP threshold constant unify the "it's back" (green pop) and "act now" (red pulse) cues everywhere they appear.

## Tech Stack / Constraints (inherited, non-negotiable)

- **Rendering:** line-batched HUD only (`pushLine`/`pushQuad`/`flushHUD` in `hud_internal.h`) + `FontSystem::drawText` + existing radial helper. **No new textures, shaders, or GPU resources.**
- **Budget:** stays within the 300–500 draw-call / 16.6 ms frame budget. New cost is a handful of extra lines + one font number per cooling slot — negligible.
- **Split-screen:** every element is drawn per-viewport, sized by `uiScale = sh / 720.0f`, reading the **active-alias** per-player state (`m_localPlayer`, `m_potionCooldown`, per-player skill states). Both local players get their own flask + bars.
- **Switch + low-end PC:** legible at handheld resolution; no per-pixel work beyond the existing icon rasterization.
- **Photosensitivity:** all pulses are steady sinusoids or single expand-and-fade — **no strobe** (matches the existing steady low-HP vignette).
- **Standing project rules:** no unprompted commits; no save-format changes; no hand-authored asset files (the flask is drawn from primitives, not an asset).

## Background: current state (as rendered today)

- **Potion indicator** — `engine_hud.cpp:388-405` (`Engine::renderMinimapAndFloor`): a `Q`/`B` key symbol + green "Potion" / red "Potion: X.Xs" text at `x=20*hs, y=sh-60*hs` — i.e. **top-left, under "Floor N"**. The health bar it should relate to is at the **bottom-left** (`hud.cpp:157` `drawHealthBar`, 200×16; energy `hud.cpp:327` `drawEnergyBar`).
- **Potion model** — `engine_update.cpp:959-989`: an **infinite, cooldown-gated** heal. `POTION_COOLDOWN = 5.0s` (`game_constants.h:120`), reduced by `bonusCooldownReduction`; restores `POTION_HEAL_PCT = 60%` HP + `POTION_ENERGY_PCT = 30%` energy. **No count / no charges.** `m_potionCooldown` (seconds remaining) is derived each tick for the HUD.
- **Skill cooldowns** — `hud_skill_bar.cpp`: `drawRadialCooldown` (`:19`) fills a wedge in near-black `{0.05,0.05,0.08}` (class, `:330`) / `{0.15,0.12,0.2}` (equip, `:388`) over slot backgrounds `{0.12,0.12,0.18}`; icon dims to `×0.4` while cooling; "ready" = a one-frame white **border** flash driven by `flashTimers` (class bar only — `drawClassSkillBar` `:291`). `drawEquipSkillBar` (`:356`) has **no** flash timer.
- **Dead code** — `drawSkillCooldown` (`hud_skill_bar.cpp:430`, decl `hud.h:103`): a 16px cyan square. **Zero callers** — not drawn at all.
- **Low-HP threshold** — the red edge vignette uses a hardcoded `0.25` HP fraction in `engine_render.cpp:~476`.

## Detailed Design

### Part 1 — Potion belt flask

**Placement.** Bottom-left, immediately to the **right of the health/energy bar stack**, vertically spanning both bars. Anchored off the same origin/scale the health bar uses so it tracks in split-screen. Drawn in the normal-HUD path right after `drawHealthBar`/`drawEnergyBar`. **Remove** the top-left potion text block at `engine_hud.cpp:388-405`.

**Rendering.** A new `HUD::drawPotionFlask(sw, sh, x, y, PotionHudState st)` draws the flask from **primitives** (no asset): a rounded body (stacked horizontal fill lines like `drawFilledBar`), a narrow neck + cork, and a liquid-red fill. Sizing ~44×48 px at 720p baseline × `uiScale`. `PotionHudState` carries: `cooldownRemaining`, `maxCooldown`, `healthFrac`, `readyFlash` (0..POP_DURATION), `keyLabel` ("Q"/"B"), `pulsePhase` (from `m_statsTimer`).

**Base states** (mutually exclusive — one is always the flask's resting look):
1. **Cooling** (`cooldownRemaining > 0`): flask dimmed; `drawRadialCooldown` sweep over it; **centered seconds number** (same formatter as skills). No red pulse (can't drink yet).
2. **Low HP + ready** (`healthFrac ≤ LOW_HP_FRACTION` && `cooldownRemaining == 0`): **steady red pulse + glow ring** = "drink NOW", phase-locked to `m_statsTimer` so it breathes in sync with the red screen-edge vignette.
3. **Ready + healthy:** calm flask + `Q`/`B` key label, understated (low visual weight so it doesn't nag).

**Overlay** (draws on top of whatever base state is active): **just became ready** (`readyFlash > 0`) → `drawReadyPop(...)` green expanding ring + brief green border, shared with skills. It fires at the cooling→ready transition and plays for `POP_DURATION`; if the player is still low-HP when it finishes, the base state is #2 and the red pulse continues seamlessly. So a low-HP heal-return reads as "green pop → settles into red pulse."

**Data source.** Per-player active aliases: `m_potionCooldown`, `m_localPlayer.health / maxHealth`. New per-local-player `m_potionReadyFlash[MAX_LOCAL_PLAYERS]` set to `POP_DURATION` on the tick `m_potionCooldown` transitions `>0 → 0`, decremented each frame.

### Part 2 — Skill cooldown treatment (class + equipment bars)

Applies to both `drawClassSkillBar` and `drawEquipSkillBar`.

1. **Bright leading edge.** In `drawRadialCooldown`, also draw the **boundary radius** (the sweep's trailing edge at `endAngle`) as a bright line (class-tinted or gold) so progress is visible against the dark cover. Keep the receding-dark-cover convention.
2. **Seconds countdown.** Centered integer seconds via `FontSystem::drawText`, white with a 1px dark outline (draw offset black behind), scaled to the slot. Uses shared formatter `hudCooldownSeconds(remaining)` → integer `ceil`, blank below `~0.2s` (the pop takes over). **Default: no decimals** (chosen for speed-readability).
3. **Green "ready pop."** On the `>0 → 0` cooldown transition, set the slot's flash timer to `POP_DURATION (~0.4s)`. Drive: (a) `drawReadyPop` expanding ring from the slot edge, (b) border lerp bright-green → resting color over the timer. Icon brightens from `×0.4` back to full as the pop plays.
   - Class bar: **reuse** the existing `flashTimers` plumbing (extend its meaning from one-frame to `POP_DURATION`).
   - Equip bar: **add** `f32 readyFlash` to `EquipSkillSlot` and set it on the transition (new plumbing, parallels the class bar).
4. **Cleanup.** Delete `drawSkillCooldown` (definition `hud_skill_bar.cpp:430` + declaration `hud.h:103`) — dead code.

### Part 3 — Shared "ready / urgent" language

- **`HUD::drawReadyPop(f32 cx, f32 cy, f32 halfSize, f32 t01, Vec3 color)`** — one expand-and-fade ring: outward radius `= halfSize + (1 - t01) * GROW`, alpha `∝ t01`. `t01 = readyFlash / POP_DURATION`. Used by skill slots (green) and the potion flask (green). Single source of the "it's back" motion.
- **Red urgent pulse** — a steady `0.5 + 0.5*sin(phase*SPEED)` glow/border in red, used by the flask when **low HP + ready**. Matches the red edge vignette; steady, no strobe.
- **`GameConst::LOW_HP_FRACTION = 0.25f`** — new shared constant. Replace the hardcoded `0.25` in `engine_render.cpp` (low-HP vignette) *and* use it for the flask pulse, so screen-edge red and flask red fire together by construction.
- **`POP_DURATION`** — a HUD-side `constexpr f32` (~0.4s) in a shared HUD header, used by skills + potion so every pop has identical duration.

## Files touched

| File | Change |
|---|---|
| `src/renderer/hud.h` | Declare `drawPotionFlask`, `drawReadyPop`; add `PotionHudState`; add `readyFlash` to `EquipSkillSlot`; remove `drawSkillCooldown` decl. |
| `src/renderer/hud_skill_bar.cpp` | Bright sweep edge in `drawRadialCooldown`; seconds number + ready-pop in both skill bars; implement `drawReadyPop`; delete `drawSkillCooldown`. |
| `src/renderer/hud.cpp` (or new `hud_potion.cpp`) | Implement `drawPotionFlask` from primitives. |
| `src/renderer/hud_cooldown_util.h` (new) | Pure testable helpers: `hudCooldownSeconds`, `potionUrgent`, `readyPopRadius`/alpha, `POP_DURATION`. |
| `src/engine/engine_hud.cpp` | Remove top-left potion text; call `drawPotionFlask` after the health/energy bars; pass flash/urgency state. |
| `src/engine/engine_update_player.cpp` (or where skill/potion cooldowns tick) | Detect `>0 → 0` transitions → set class `flashTimers`, equip `readyFlash`, `m_potionReadyFlash`. |
| `src/engine/engine.h` | Add `m_potionReadyFlash[MAX_LOCAL_PLAYERS]`. |
| `src/engine/engine_render.cpp` | Use `GameConst::LOW_HP_FRACTION` for the low-HP vignette. |
| `src/game/game_constants.h` | Add `LOW_HP_FRACTION = 0.25f`. |
| `tests/renderer/test_hud_cooldown.cpp` (new) | Unit tests for the pure helpers. |
| `tests/CMakeLists.txt` | Add the new test + any production `.cpp` it links. |

## Testing

Forward-only per project convention. Extract the pure logic into `hud_cooldown_util.h` free functions and unit-test them with doctest (no GL needed):
- `hudCooldownSeconds(remaining)` — `ceil` behavior, blank threshold, boundaries (5.0→"5", 0.9→"1", 0.1→"").
- `potionUrgent(healthFrac, cooldownRemaining)` — true only when `healthFrac ≤ LOW_HP_FRACTION` **and** `cooldownRemaining == 0`; false while cooling even at low HP.
- `readyPopRadius(t01, halfSize)` / alpha — monotonic expand as `t01→0`, alpha fades to 0.
Rendering itself (line output) is validated visually in-engine + on Switch, consistent with the codebase's forward-only stance on render code.

## Non-goals

- No potion **count/charges** system (potion stays an infinite 5s cooldown heal).
- No new input bindings (potion remains `Q` / gamepad `B`).
- No changes to networking, snapshots, save format, or combat balance.
- No changes to other HUD elements (minimap, weapon indicator, ammo, status icons, damage numbers) beyond the shared `LOW_HP_FRACTION` constant.
- No kill/crit/loot "juice" (explicitly out of scope — tight pass).

## Open decisions (defaulted; flag on review to change)

- **Flask visual:** drawn from primitives (no asset). Alternative: generate a pixel-art flask via `tools/gen_skill_icons.py`. Default = primitives.
- **Low-HP threshold:** `0.25` (matches existing vignette). 
- **Cooldown number precision:** integer seconds, no decimals.
- **Pop color:** green for "ready"; red reserved for "urgent."
- **Pop duration:** ~0.4s.
