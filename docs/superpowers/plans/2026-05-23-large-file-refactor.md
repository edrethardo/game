# Large-File Refactor Plan

**Date:** 2026-05-23
**Scope:** Maintainability only — split oversized files + break up god functions. **Behavior-preserving** (no gameplay changes). Functional/correctness bugs were handled in a separate audit + fix sweep; this plan is purely structural.

## Context
`src/` is ~35.7k lines across 125 files, but **9 hand-maintained files hold ~16k of them** and contain functions thousands of lines long — hard to read, hold in context, or edit safely.

| File | Lines | Worst function(s) |
|---|---|---|
| `engine/engine_render.cpp` | 2801 | `render` 622, `renderProjectilesAndEffects` 650, `renderEntities` 524, `renderViewmodel` 440 |
| `game/skill.cpp` | 2287 | `tryActivate` 370 (+ ~40 self-contained `fire*` helpers) |
| `game/enemy_ai.cpp` | 2189 | `EnemyAI::update` **~2025** (one function ≈ whole file) |
| `renderer/hud.cpp` | 2081 | many small fns; `drawInventoryScreen` 246; 3× duplicated icon loop |
| `engine/engine_update.cpp` | 1744 | `gameUpdate` 1056, `update` 289 |
| `engine/engine_startgame.cpp` | 1410 | `startGame` 1070 |
| `engine/engine_init.cpp` | 1336 | `init` 1158 |
| `engine/engine_hud.cpp` | 1212 | `renderHUD` 701, `renderMenu` 441 |
| `game/item.cpp` | 1196 | none >90; 4 unrelated concerns in one file |

(`renderer/skill_icons_data.h`, 1268 lines, is **generated** by `gen_skill_icons.py` — excluded.)

Two work types: **(A) file splits** — move groups of functions into new focused `.cpp` files; near-mechanical because the engine already spreads `Engine::` methods across files (`engine_render.cpp`, `engine_combat.cpp`, …). **(B) god-function breakup** — extract named helpers; ranges from trivial to risky.

## Guardrails (this codebase has no test framework)
- **One file/extraction per commit.** `cmake --build build` must be clean (zero warnings) after each.
- **Pure moves only** — copy a function verbatim to the new file, delete from the old, add any needed `#include` and (for methods) the declaration already exists in `engine.h`. No logic edits in the same commit as a move.
- **Namespace/static functions** that move and are used elsewhere need a shared internal header (e.g. `hud_internal.h`, `enemy_ai_internal.h`) — don't widen public headers.
- For the risky `EnemyAI::update` and `startGame` extractions, **build the headless combat self-test first** (proposed earlier) as a safety net, and smoke-test in-game after.
- Optional: add a `dungeon_tests` target so the pure logic gets CI coverage before the risky phases.

---

## Phase 1 — Pure file splits (Small risk, high reward)
Mechanical moves; do these first to shrink the files and make later god-function work tractable. Each bullet = one commit, build between.

### 1a. `game/item.cpp` (1196) → 5 files
| New file | Moves |
|---|---|
| `item_loader.cpp` | `ItemLoader::*` (loadItemDefs/loadAffixDefs/loadSkillDefs/resolveVisuals), the 5 `*FromString` parsers, file-read helper |
| `item_gen.cpp` | `ItemGen::*` + LCG statics (`rollRarity/rollAffixes/rollItem`) |
| `inventory.cpp` | `Inventory::*` + static `buildWeaponDef` + `s_statsChangedCb` |
| `world_item.cpp` | `WorldItemSystem::*` |
| `quickbar.cpp` | `Quickbar::*` |
Side win: removes `nlohmann/json` from `inventory.cpp`'s TU (faster incremental builds). All Small.

### 1b. `game/skill.cpp` (2287) → per-class families + residual
New files, each holding the self-contained `static fire*` helpers (each already takes explicit params, no shared state): `skill_legendary.cpp`, `skill_warrior.cpp`, `skill_ranger.cpp`, `skill_sorcerer.cpp`, `skill_rogue.cpp`, `skill_paladin.cpp`, `skill_engineer.cpp`, `skill_marksman.cpp`, `skill_tinkerer.cpp`. Module-level statics (`s_bombardmentTimer`, `s_overcharged*`, …) move with the family that owns them. `skill_system.cpp` keeps the public `SkillSystem::` API (`tryActivate`, `update`, `updateOrbProjectiles`, `updateMeteors`). Add `skill_internal.h` with the `fire*` signatures. All Small.

### 1c. `renderer/hud.cpp` (2081) → focused HUD files
First add `hud_internal.h` exposing `pushLine/pushQuad/flushHUD`. Then:
| New file | Moves |
|---|---|
| `hud_input_glyphs.cpp` | `drawControllerButton/drawKeySymbol/drawMouseButton` + glyph statics |
| `hud_status.cpp` | `drawStatusIcons` (+ new `drawStatusIcon8` helper), `drawSpeechBubble`, `drawDamageVignette`, `drawDamageDirection`, status bitmaps |
| `hud_skill_bar.cpp` | `drawClassSkillBar/drawEquipSkillBar/drawSkillCooldown/drawRadialCooldown`, skill icon getters/bitmaps + new `drawSkillIcon32` helper (kills the 3× duplicated icon loop; move `flushHUD` out of the per-slot loops) |
| `hud_inventory.cpp` | `drawInventoryScreen` (→ `drawEquipmentPanel`/`drawBackpackPanel`), `drawItemTooltip`, `drawLootNotification`, name helpers |
| `hud_portraits.cpp` | `drawSummonPortrait`, `drawQuickbar` |
`hud.cpp` keeps the GL batch core + crosshair/bars/profiler/netstats. Glyphs/status Small; skill_bar/inventory Medium (they fold in the duplicate-loop fix + panel splits).

### 1d. `engine/engine_render.cpp` (2801) → per-concern render files
Each target is **already a standalone `Engine::` method** — moving it to a new `.cpp` is a verbatim cut (declaration already in `engine.h`):
`engine_render_viewmodel.cpp` (`renderViewmodel`), `engine_render_entities.cpp` (`renderEntities`), `engine_render_effects.cpp` (`renderProjectilesAndEffects`), `engine_render_world.cpp` (`renderWorldItems/renderSpeechBubbles/renderDamageNumbers`). `engine_render.cpp` keeps `render()`. All Small.

### 1e. `engine/engine_hud.cpp` (1212) → `engine_render_menus.cpp`
Move `renderMenu` + `renderLobby` out (verbatim methods). Small. (`renderHUD` god-function breakup is Phase 2.)

### 1f. `engine/engine_init.cpp` (1336) → assets + callbacks
Extract two private methods called from `init()`:
- `engine_init_assets.cpp` ← `Engine::initAssets()`: hand-mesh builder, `kMeshes` table + OBJ load loop, LimbSystem/mesh-id setup, weapon/item/affix/skill/boss/enemy JSON load + visual resolution. Move `kClassDefs` + `kMeshes` to file-scope statics here.
- `engine_init_callbacks.cpp` ← `Engine::initCallbacks()`: all `set*Callback` wiring (SkillSystem FX, death, splash, hit, perfect-block, dodge-through, drone-spawn).
`init()` shrinks to ~160 lines of orchestration. Adds 2 private decls to `engine.h`. Small/Low risk (lambdas already capture the file-scope `s_engine`, so no local state crosses the cut).

---

## Phase 2 — God-function breakup (Medium)
Extract named helpers from the giant functions. Declarations go in `engine.h` (Engine methods) or the relevant `*_internal.h`.

### 2a. `Engine::init` (1158) — done structurally by 1f; verify the body is just orchestration.

### 2b. `gameUpdate` (1056) → extract methods (group into `engine_update_player.cpp` / `engine_update_skills.cpp`)
`tickWandererTimers`, `tickPlayerStatusEffects`, `tickVisualFeedback` (flash/vignette/rumble/camera), `tickMiscTimers`, `tickFXDecay`, `tickSkillCooldowns`, `tickPassiveEquipment`, `handleClassSkillActivation`, `handleEquipmentSkillActivation`, `tickArmorRingPassives`, `handleDebugKeys`. **Order matters:** `tickPassiveEquipment` (sets `m_armorAura`/`m_ringPassive`) must run before `tickArmorRingPassives`; carry `eyePos` into skill-activation helpers. Removes the duplicated Wanderer-timer block. Medium.

### 2c. `startGame` (1070) → `engine_spawn.cpp`
`spawnFloorEnemies`, `spawnFloorBoss(&bossRoomOut)`, `spawnFloorDecorations`, `spawnFloorChests`, `spawnFloorNpcs`. Promote the nested `kTier1..5` / `kBosses` static tables to file scope. **Watch:** `bossRoomForExit` must become an out-param (consumed by exit-portal placement); `spawnFloorBoss` mutates `dungeon.rooms`, grid cells, and rebuilds the level mesh — keep those side effects in the helper and call it before exit-portal/flow-field. Medium→Large.

### 2d. `renderHUD` (701) + `renderMenu` (441)
`renderHUD` → `renderInventoryHUD`, `renderSkillsHUD`, `renderMinimapAndFloor`, `renderTutorials`. `renderMenu` → per-`subState` sub-renderers. Medium (cutting live code from `renderHUD`).

### 2e. `render` (622) / `renderViewmodel` (440) / `renderEntities` (524) / `renderProjectilesAndEffects` (650)
Within each (now-isolated) file, extract helpers: `render` → `renderTransitionScreens`/`selectPointLights`/`renderAuraDiscs`/`renderPostOverlays` (+ a shared `drawFullscreenQuad(Vec4)` reused by fade + hurt vignette); viewmodel → `computeAttackAnim`/`submitViewmodelMesh`; entities → `renderEntityLimbs`/`computeEntityAnim`; effects → `renderOrbProjectile`/`renderSplashEffect`/`renderMeteorEffect`. Small–Medium.

### 2f. `SkillSystem::tryActivate` (370) → `playActivationSound` + `spawnActivationParticles` + `activateSkillEffect`. Small.

### 2g. `Combat::setDeathCallback` lambda (316) → `handleFirstKillDrop`/`handleBossLootDrop`/`handleNormalLootDrop`/`handleOnKillRingPassives`. **Watch** the two early `return`s (first-kill, boss) — convert to conditional orchestration so each helper has no `return`-skips-rest. Medium.

---

## Phase 3 — `EnemyAI::update` (~2025) — the hard one (do last, with the self-test in place)
1. Extract the file-scope helpers (`entityMoveAndSlide`, `hasLOS`, `hasLOSToPoint`, `snapEntityToFloor`, `entityOverlapsGrid`) into `enemy_ai_internal.h`/`.cpp`. (Small)
2. **Establish a loop-flow convention first:** the per-entity body uses `continue`/`break` heavily. Define `enum class AIStep { Done, NextEntity };` (or `bool keepProcessing`) returned by each extracted block so the driving loop reproduces the exact early-exits. Pin every early-exit before cutting.
3. Extract in increasing risk:
   - `updateLegacyBossAbilities` (the `switch(e.level)` block, ~260) — most self-contained. **Small.**
   - `applyRoleModifiers` (SUMMONER/HEALER/CHARGER/RANGED/BOMBER/SHIELD, ~195) — has a lethal-`break` in charger+bomber → must signal NextEntity. **Medium.**
   - `updateFriendlyNPC` (~585) — `continue` paths + static locals (`s_frameTick`, `s_friendlyGroupCenter/Count`) lifted to file scope and passed in. **Medium.**
   - `updateHostileStates` (the `switch(e.aiState)`, ~650) — thread `targetPlayer`/`targetPos`/`targetVel`/flags through the signature; `switch`-internal `break`s stay inside. **Medium.**
4. `enemy_ai.cpp` retains the aura pass + loop spine + post-loop separation/stuck-detection (~500).

Each step: build + smoke-test (spawn enemies with F4/F5, confirm friendly NPCs, bosses, and each role still behave). This is the one place a behavior regression is most likely — go slow, one block per commit.

---

## Suggested overall order (risk/reward)
1. **Phase 1a (item.cpp)** — cleanest seams, instant win.
2. **Phase 1d + 1e (engine_render/hud method moves)** — verbatim Engine-method cuts, ~1.5k lines relocated with near-zero risk.
3. **Phase 1b (skill.cpp families)** + **2f (tryActivate)**.
4. **Phase 1c (hud.cpp)** — folds in the duplicate-icon-loop + draw-call fixes.
5. **Phase 1f + 2a (engine_init)** + **2g (death callback)**.
6. **Phase 2b (gameUpdate)**.
7. **Phase 2d/2e (HUD + render god-functions)**.
8. **Phase 2c (startGame spawn extraction)** — build the self-test first.
9. **Phase 3 (EnemyAI::update)** — last, slowest, with the self-test + in-game smoke after each block.

## Verification
- After every commit: `cmake --build build` clean (zero warnings).
- After each phase that touches runtime behavior (2b/2c/3): launch the game, F4/F5 spawn enemies, fight, descend a floor, open inventory, cast skills — confirm no behavior change.
- Outcome target: no hand-maintained file > ~600 lines; no function > ~250 lines; ~9 god functions eliminated. The split is purely structural — gameplay must be identical before/after.
