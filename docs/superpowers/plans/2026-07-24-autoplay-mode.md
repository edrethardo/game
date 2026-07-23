# Autoplay Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A main-menu "Autoplay" mode where a bot plays a real singleplayer run — navigating, fighting (per the selected 3×3 build's doctrine), looting, descending, dying-and-respawning, and laddering difficulty — while the player can open the inventory to change the build and grab control at any time, with the bot resuming ~2 s after the last input.

**Architecture:** The bot IS synthetic input. A pure decision core (`autoplay_*` modules in `src/game/`) reads a read-only snapshot of the world and emits a semantic `BotIntent`; a thin engine driver translates that intent into synthetic `GameAction`s + direct `player.yaw/pitch` writes, so every existing player system (jump assist, dodge i-frames, CC suppression, the skill-cast preamble, kill credit, interact arbitration, MP prediction) runs unchanged. Nothing persists (no `SAVE_VERSION` change); autoplay is a session mode over ordinary singleplayer.

**Tech Stack:** C++17, doctest (vendored `external/doctest/doctest.h`), CMake. SP-only (`NetRole::NONE`), lane 0 only in v1.

**Spec:** `docs/superpowers/specs/2026-07-23-autoplay-mode-design.md`

---

## Key facts pinned from the codebase (verified at plan time — cite these; do not re-derive)

**Input layer**
- `enum struct GameAction : u8` at `src/platform/input.h:8-33`. Members in order: `MOVE_FORWARD, MOVE_BACKWARD, MOVE_LEFT, MOVE_RIGHT, JUMP, FIRE, BLOCK, CLASS_SKILL, QUICKBAR_USE, POTION, PICKUP, RELOAD, SKILL_1..4, BOOT_SKILL, HELMET_SKILL, INVENTORY, PAUSE, QUICKBAR_SLOT_1, QUICKBAR_SLOT_2, MENU_UP, MENU_DOWN, MENU_CONFIRM, MENU_BACK, DODGE, CHARACTER_SCREEN, QUICKBAR_SLOT_3, QUICKBAR_SLOT_4, COUNT`. **Append-only** (on-disk `controls.json` format) — never insert/reorder; autoplay needs NO new GameAction.
- `Input::isActionDown/isActionPressed` (`input.h:84-85`) are thin wrappers over `static bool checkActionRaw(GameAction, bool pressed)` at `input.cpp:1283`; the two wrappers are at `input.cpp:1342-1348`. **This is the single choke point** for synthetic-action injection.
- `Input::update(f32)` computes per-frame human activity at `input.cpp:561-591`: `kbmActive` (any key / mouse Δ ≥ `DEVICE_MOUSE_MOVE_PX`=6 / any mouse button) and `padActiveByIndex[c]` (buttons / stick ≥ `DEVICE_STICK_DEADZONE`=0.5 / trigger ≥ `DEVICE_TRIGGER_THRESHOLD`=0.5); thresholds at `input.cpp:40-43`. On `__SWITCH__` the block is replaced (`input.cpp:557-559`). **No accessor exposes these booleans today.**
- `Input::consumePressedState()` (`input.cpp:617-644`) rolls previous←current for keys/mouse/pad/axis; called once per render frame after the first fixed substep (`engine.cpp:1159-1164`, guarded by `m_firstTick`). `Input::update` is called once per render frame at `engine.cpp:1152`.
- `--bot-walk` precedent: file statics `s_botWalk`/`s_botFlagsThisTick` (`player.cpp:20-22`), override in `captureLocalInput` (`player.cpp:464`) and re-read in `PlayerController::update` (`player.cpp:189-194`, jump at `:260`). **Dead under `NetRole::NONE`** — `captureLocalInput` only runs on SERVER/CLIENT. Autoplay must inject at the action layer, not here.

**Player tick / aim / movement**
- `PlayerController::update(Player&, f32 dt)` at `player.cpp:119`; WASD+stick merge (threshold 0.3) at `player.cpp:185-188`; stun zeroes WASD at `player.cpp:198-199`; dodge activation at `player.cpp:205-245` (direction via `computeRollDirection(w,s,a,d,yaw)` — no keys ⇒ rolls forward); `applyMovement` at `player.cpp:62-113` sets `velocity.xz` from the WASD move vector rotated by yaw and does `yaw -= lookDX*sensitivity` (bot writes yaw; idle mouse Δ=0 preserves it); `player.forward` cached at end of update (`player.cpp:305`), formula at `player.cpp:80`.
- Called from `gameUpdate` (`engine_update.cpp:1575`) at `engine_update.cpp:1780`, inside `if (!gameplayInputFrozen())` (`:1769`); `Collision::moveAndSlide` at `:1806-1807`; fire/skills also `!gameplayInputFrozen()`-gated (`:1821-1824`, `:1898-1899`).
- `gameplayInputFrozen()` at `engine.h:300-303`: `m_inventoryOpen || m_characterScreenOpen || m_menu.confirmQuit || m_menu.optionsFromPause || m_menagerieOpen`.
- Player struct (`player.h`): `position` (feet, `:32`), `velocity` (`:33`), `eyeHeight` (`:34`, 1.7), `yaw` (`:35`), `pitch` (`:36`, clamp ±89° enforced in applyMovement), `moveSpeed` (`:37`, 6), `forward` (`:174-175`), `health`/`maxHealth` (`:47-48`), `invulnTimer` (`:65`), `stunTimer` (`:134`), `blocking`/`blockTimer` (`:128-130`), `dodgeState` (`:155`, DodgeState `:14-29` with `rolling`/`rollTimer`/`cooldownTimer`), `onGround`, `ccDodgeImmune`. **No `energy`/`spawnPosition` on Player** — energy is `m_skillStates[m_localPlayerIndex].energy` (`item.h:495-496`); spawn is `m_players[activeNetSlot()].spawnPosition` (`net_player.h:98`).
- Fire origin/dir: `eyePos = position + {0,eyeHeight,0}`; `forward = m_localPlayer.forward` (`engine_combat.cpp:414-415`). Setting yaw/pitch is sufficient — forward is recomputed each update.

**Navigation / descent / death**
- `LevelGridSystem::buildFlowField(LevelGrid&, Vec3 exitWorldPos)` (`level_grid.h:149`, impl `level_grid.cpp:190`) built in `startGame` toward `m_level.floorDoorPos` at `engine_startgame.cpp:885` (only when `dungeon.roomCount > 1`). `flowDirection(const LevelGrid&, Vec3)` (`level_grid.h:161`, impl `level_grid.cpp:316`) returns a **unit XZ** vector toward the next cell, or `{0,0,0}` for **both** at-exit (byte `0xFE`) and unreachable (`0xFF`) — read the raw byte to disambiguate (`grid.flowDir[gz*width+gx]`, `level_grid.h:95`).
- `LevelState` (`engine.h:541`, member `m_level` `:585`): `grid` (`:542`), `currentFloor` (`:546`), `lavaFloor` (`:552`), `dungeon` (`:553`), `layoutStyle` (`:558`), `floorDoorPos`/`floorDoorActive` (`:560-561`), `floorHasBoss` (`:562`), `inSourceChamber`/`inTown`/`inArena` (`:567/:570/:574`), `sourcePortalActive`/`Pos` (`:577-578`), `townPortalActive`/`Pos` (`:575-576`), `exitPortalActive`/`Pos` (`:582-583`).
- `DungeonResult` (`level_gen.h:43`): `spawnPos`, `rooms[]`, `roomCount`, `spawnRoomIdx`/`exitRoomIdx`, `portals[16]`/`portalCount`, `spawnOnUpper`, `spawnBalconyPos`/`exitBalconyPos`, `dropHoles[64]` (`DropHole{Vec3 pos; f32 surfaceY;}` `:33`)/`dropHoleCount`, `jumpPads[32]`/`jumpPadCount`. `LayoutStyle` enum `:74-82` (`BSP_ROOMS=0, CAVERN, GAUNTLET, HUB, VERTICAL_HALL, FOUR_STORY, COUNT`).
- `StoryNav` (pure, header-only `story_nav.h`): `onUpperStory(g,xz,feetY)` (`:17`), `nearestPortalGoal(d,from,fromUpper,toUpper)` (`:28`), `nearestPadGoal(d,from)` (`:50`), `targetIsAbove(myY,tgtY)` (`:64`, >1.5), `planVault(g,from,feetY,dirXZ)→VaultPlan{viable,gapAhead,landing}` (`:80/:91`, `VAULT_MAX_CELLS`=3).
- Descend: `m_descendRequested` (`engine.h:1014`, reset `engine_update.cpp:2205`, set `:2244`); `updateFloorDoor()` (`engine_update.cpp:2760`) consumes it — gate: `floorDoorActive && lengthSq(floorDoorPos-pos)<4.0 && m_descendRequested && !(floorHasBoss && floorBossAlive())`; CLIENT branch sends `sendDescendRequest()` (`:2774`); success `triggerFloorDescent()` (`:2796`); called `if (updateFloorDoor()) return;` (`:1932-1933`). `floorBossAlive()` at `:2740`.
- SP death: `m_localPlayer.health <= 0` → true-SP branch `engine_update.cpp:1685-1693` sets `GameState::GAME_OVER`. Respawn (option 0) at `:186-217` on JUMP/SPACE/RETURN/KP_ENTER/click: `health=maxHealth`, `position=m_players[activeNetSlot()].spawnPosition`, `invulnTimer=1.5`, `m_gameState=IN_GAME`. GAME_OVER dispatch top at `:148`.
- `triggerFloorDescent` auto-ladders difficulty at floor 50 and autosaves; Hell 50 → credits/VICTORY/town.

**Combat / build / test harness**
- `CombatQuery` (`combat_query.h`): `raycast(grid,pool,origin,dir,maxDist)→CombatHit` (`:21`), `rayNearestEntity(pool,origin,dir,maxDist,outHandle,outT)→bool` (`:36`, entities only, no world occlusion), `queryConeSorted(...)` (`:50`). All three skip `ENT_DEAD|ENT_FRIENDLY|PROP|ENT_BURROWED` (`combat_query.cpp:81-87` etc.). `CombatHit{bool hit; Vec3 position,normal; f32 distance; HitType type; EntityHandle entityHandle;}` (`:9-16`).
- `LeadAssist::interceptTime(Vec3 rel, Vec3 vel, f32 speed, f32& tOut)→bool` (`lead_assist.h:32`, pure, engine-free, `MAX_LEAD_SEC`=1.5).
- `LevelGridQuery::findCoverCell(grid,from,threatPos,outPos,maxRadius=8)→bool` (`level_grid.h:168`), `findFlankCell(grid,entityPos,targetPos,attackRange,preferRight,outPos)→bool` (`:173`).
- `BuildScore` (`build_score.h`): `buildRow(cell)` (`:29`), `buildCol(cell)` (`:30`), `DEFAULT_BUILD_CELL`=4 (`:27`), `rowName`/`colName` (`:227/:230`). cell=row*3+col; rows 0 Tanky/1 Moderate/2 Glass; cols 0 Magic/1 Melee/2 Ranged.
- `PlayerInventory.autoMode`/`buildCell` at `item.h:567-568` (v4 persisted).
- Entity (`entity.h:133`): `position` (`:139`, AABB centre), `velocity` (`:140`), `halfExtents` (`:144`), `health` (`:147`), `attackRange` (`:149`), `attackTimer` (`:151`), `detectionRange` (`:155`), `aiState` (`:158`), flags enum `EntityFlags` (`:17-36`). `EntityPool{entities[MAX_ENTITIES]; activeList[]; activeCount;}` (`:338`); `MAX_ENTITIES`=192.
- Tests: add a `tests/<dir>/test_*.cpp` to the `add_executable(dungeon_tests ...)` list in `tests/CMakeLists.txt:9`; header-only tests link no extra `.cpp`; tests touching production `.cpp` append `${CMAKE_SOURCE_DIR}/src/...` to the same list. `DUNGEON_REPO_ROOT` compile def at `:123-128`. Build: `cmake --build build --target dungeon_tests`; run `./build/tests/dungeon_tests -tc="<filter>"`; full `--no-version`.

**Menu / mode flag**
- `MenuState` struct `engine.h:136-204`, member `m_menu` `:204`; `bool arena=false;` at `:161`. Main-menu render arrays `engine_render_menus.cpp:1156-1172` (`fullLabels`/`fullColors`/`demoLabels`/`demoColors`, count `7u`/`4u`, row-Y `sh*0.26 + (count-1-i)*50*uiScale`). Menu sites in `engine_menu.cpp`: `menuMouseForState` case 0 count `:132`; `MENU_DOWN` maxSel `:1802`; session-flag reset (`m_menu.arena=false`) `:1819`; `demoActionMap` `:1820-1827`; `switch(menuAction)` `:1828-1903` (`case 0` SP → subState 1 `:1829-1833`). SP chain: `1→6` (`:401-444`/`:743-754`), `6→2` (`:751`), `2→23` (`:760-788`, `applyClassToLane0` at `:779`), `23` confirm sets `autoMode`/`buildCell` `:809-810` then routes (CLIENT/SERVER/NONE) `:812-839`, solo-start `:846-918` (NONE branch `:899-906`). `m_menu.arena` lifecycle: decl `engine.h:161`; write `engine_menu.cpp:1233` (set) / `:1819` (reset) / `engine_arena.cpp:529`; read `engine_menu.cpp:832`/`:887`, `engine.cpp:1285`, render `engine_render_menus.cpp:428/532/959`.
- `--autoloot` launch door: `LaunchOptions.autoLoot` (`launch_options.h:56`), parse `launch_options.cpp:157-159`, apply `engine_launch.cpp:87-90` (`m_inventories[0].autoMode=1; buildCell=DEFAULT_BUILD_CELL;`). `--bot-walk` at `:94-97`. Force flags `m_forceVerticalHall/FourStory/Lava` near `engine_launch.cpp:74-82`.

---

## File structure

New files (all pure/header-only decision core in `src/game/`, unit-tested; one engine glue `.cpp`):

| File | Responsibility |
|---|---|
| `src/game/autoplay_intent.h` | `BotIntent` (semantic output) + `BotView` (read-only input snapshot) plain structs — the interface between brain and engine. |
| `src/game/bot_input.h` | `BotInput` pure overlay: per-`GameAction` held/prev bitsets, `down/pressed/setHeld/clear/rollEdges`. Consumed by `Input::`. |
| `src/game/autoplay_control.h` | `AutoplayControl` pure takeover/resume latch: `tick(humanActive, uiOpen, dt)→inControl`. |
| `src/game/autoplay_doctrine.h` | `Doctrine` pure table: `doctrineFor(u8 cell)→Doctrine` (engagement band, potion %, dodge/block/cover policy per row×col). |
| `src/game/autoplay_nav.h` | Pure navigation: hazard veto (`stepAllowed`), per-style steering goal (`travelGoal`), descend-eligibility. |
| `src/game/autoplay_combat.h` | Pure combat policy: target pick, aim (with lead), fire/skill/defense decisions from `BotView` + `Doctrine`. |
| `src/game/autoplay_brain.h` / `.cpp` | The state machine: `decide(const BotView&)→BotIntent` (SURVIVE→FIGHT→LOOT-SETTLE→TRAVEL→DESCEND + RESPAWN/STUCK). Pure. |
| `src/engine/engine_autoplay.cpp` | Engine glue: build `BotView` from live state each tick, call the brain, apply the `BotIntent` (yaw/pitch + synthetic actions), run the control latch, freeze carve-out, descend/respawn, HUD badge. |

Modified: `src/platform/input.{h,cpp}` (overlay + activity accessor), `src/game/player.cpp` (nothing — action layer covers it), `src/engine/engine.h` (bot state fields + `botMayAct()`), `src/engine/engine_update.cpp` (driver call + freeze carve-out + respawn), `src/engine/engine_menu.cpp` + `engine_render_menus.cpp` (menu row + flag), `src/engine/launch_options.{h,cpp}` + `engine_launch.cpp` (`--autoplay`), `src/engine/engine_hud.cpp` (AUTO badge), `tests/CMakeLists.txt`.

Each task below is TDD where a pure unit exists; the two integration tasks (menu wiring, engine glue) are build-and-verify with a live acceptance step.

---

### Task 1: `BotInput` synthetic-action overlay + human-activity accessor

**Files:**
- Create: `src/game/bot_input.h`
- Create: `tests/game/test_bot_input.cpp`
- Modify: `src/platform/input.h`, `src/platform/input.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test** — `tests/game/test_bot_input.cpp`:

```cpp
// tests/game/test_bot_input.cpp — the pure synthetic-input overlay the Autoplay bot drives.
// Verifies held-vs-pressed edge semantics mirror the engine's once-per-render-frame model.
#include "doctest/doctest.h"
#include "game/bot_input.h"
#include "platform/input.h"   // GameAction

TEST_CASE("BotInput: held reports down every frame; pressed fires once per edge") {
    BotInput b;
    b.setHeld(GameAction::FIRE, true);
    CHECK(b.down(GameAction::FIRE));
    CHECK(b.pressed(GameAction::FIRE));      // first frame the bit is up: an edge
    b.rollEdges();                            // end-of-render-frame roll (mirrors consumePressedState)
    CHECK(b.down(GameAction::FIRE));          // still held
    CHECK_FALSE(b.pressed(GameAction::FIRE)); // no longer an edge
}

TEST_CASE("BotInput: releasing clears down and pressed") {
    BotInput b;
    b.setHeld(GameAction::JUMP, true);
    b.rollEdges();
    b.setHeld(GameAction::JUMP, false);
    CHECK_FALSE(b.down(GameAction::JUMP));
    CHECK_FALSE(b.pressed(GameAction::JUMP));
}

TEST_CASE("BotInput: a fresh press after release is a new edge") {
    BotInput b;
    b.setHeld(GameAction::DODGE, true); b.rollEdges();
    b.setHeld(GameAction::DODGE, false); b.rollEdges();
    b.setHeld(GameAction::DODGE, true);
    CHECK(b.pressed(GameAction::DODGE));      // up->down again = edge
}

TEST_CASE("BotInput: clear() drops all held bits (used when the bot yields control)") {
    BotInput b;
    b.setHeld(GameAction::MOVE_FORWARD, true);
    b.setHeld(GameAction::FIRE, true);
    b.clear();
    CHECK_FALSE(b.down(GameAction::MOVE_FORWARD));
    CHECK_FALSE(b.down(GameAction::FIRE));
}

TEST_CASE("BotInput: an out-of-range action never reports down (bounds safety)") {
    BotInput b;
    CHECK_FALSE(b.down(GameAction::COUNT));
    CHECK_FALSE(b.pressed(GameAction::COUNT));
}
```

Add to `tests/CMakeLists.txt` after `game/test_lead_assist.cpp` line:
```cmake
    game/test_bot_input.cpp          # synthetic-action overlay: held/pressed edge semantics (header-only)
```
(Header-only — no production `.cpp` to link; `input.h` provides `GameAction` with no `.cpp` dependency for the enum.)

- [ ] **Step 2: Run to verify it fails** — `cmake --build build --target dungeon_tests 2>&1 | tail -5` → compile FAILURE `game/bot_input.h: No such file or directory`.

- [ ] **Step 3: Create `src/game/bot_input.h`**:

```cpp
// bot_input.h — the Autoplay bot's synthetic-action overlay.
//
// A per-GameAction held bitset with edge tracking, consulted by Input::isActionDown /
// isActionPressed so the bot drives EVERY existing input consumer (movement, fire, dodge,
// skills, potion, interact) through the exact code paths a human's keypresses take. Pure and
// header-only so it unit-tests without SDL. `pressed` mirrors the engine's once-per-render-frame
// edge model: rollEdges() is called from Input::consumePressedState(), exactly like the real
// device previous<-current roll, so a held synthetic action fires isActionPressed on one frame only.
#pragma once
#include "core/types.h"
#include "platform/input.h"   // GameAction

struct BotInput {
    static constexpr u32 N = static_cast<u32>(GameAction::COUNT);
    bool held[N] = {};
    bool prev[N] = {};

    void setHeld(GameAction a, bool on) { const u32 i = idx(a); if (i < N) held[i] = on; }
    bool down(GameAction a)   const { const u32 i = idx(a); return i < N && held[i]; }
    bool pressed(GameAction a) const { const u32 i = idx(a); return i < N && held[i] && !prev[i]; }
    void rollEdges() { for (u32 i = 0; i < N; i++) prev[i] = held[i]; }   // end-of-render-frame
    void clear() { for (u32 i = 0; i < N; i++) held[i] = false; }         // bot yields control

private:
    static u32 idx(GameAction a) { return static_cast<u32>(a); }
};
```

- [ ] **Step 4: Wire it into `Input::`** — add the overlay accessors + the human-activity accessor to `src/platform/input.h` (in the `namespace Input` block, near `isActionDown` at `:84-85`):

```cpp
    // --- Autoplay synthetic-input overlay ---------------------------------------------------
    // The bot's held/pressed action state, OR'd into isActionDown/isActionPressed below the
    // real-device read so a bot press is indistinguishable from a human one. Off unless armed.
    void setBotOverlayActive(bool on);
    bool botOverlayActive();
    void setBotHeld(GameAction action, bool on);   // bot arms/clears one action for this tick
    void clearBotHeld();                            // drop all bot-held actions
    // True if a human touched any gameplay device THIS render frame (the kbmActive/padActive
    // computation already done in update(), threshold-filtered). Autoplay's takeover trigger.
    bool humanActivityThisFrame();
```

In `src/platform/input.cpp`, add file statics near `s_activeDevice` (`:40`):
```cpp
#include "game/bot_input.h"
static BotInput s_botInput;
static bool     s_botOverlayActive = false;
static bool     s_humanActiveThisFrame = false;
```

Implement the accessors (near the `isActionDown` wrappers at `:1342`):
```cpp
void Input::setBotOverlayActive(bool on) { s_botOverlayActive = on; if (!on) s_botInput.clear(); }
bool Input::botOverlayActive() { return s_botOverlayActive; }
void Input::setBotHeld(GameAction action, bool on) { s_botInput.setHeld(action, on); }
void Input::clearBotHeld() { s_botInput.clear(); }
bool Input::humanActivityThisFrame() { return s_humanActiveThisFrame; }
```

In `checkActionRaw` (`:1283`), immediately after the `idx >= COUNT` guard and BEFORE the device reads, consult the overlay:
```cpp
    // Autoplay overlay: a bot-held action reports down/pressed exactly like a human's, so every
    // downstream consumer (movement, fire, skills, dodge, interact) is driven with zero call-site
    // changes. OR with the real device so a human can always override on the same frame.
    if (s_botOverlayActive) {
        if (pressed ? s_botInput.pressed(action) : s_botInput.down(action)) return true;
        // fall through to real-device read (human override wins instantly)
    }
```

In `Input::update`, at the END of the activity block (`input.cpp:591`, right after `if (kbmActive != padActive) ...`), latch the human-activity boolean:
```cpp
    s_humanActiveThisFrame = kbmActive || padActive;
```
On the `__SWITCH__` path (`:557-559`) set it from the pad computation too — move the `padActive` computation out of the non-Switch guard, or add `s_humanActiveThisFrame = padActive;` in that branch. (Verify the exact brace structure and keep both paths setting the flag.)

In `Input::consumePressedState` (`:617`), add the overlay edge roll at the end:
```cpp
    s_botInput.rollEdges();   // bot pressed-edges are once-per-render-frame, like device edges
```

- [ ] **Step 5: Run tests + build** — `cmake --build build && ./build/tests/dungeon_tests -tc="*BotInput*"` → PASS; full suite `--no-version | tail -3` green; game target links.

- [ ] **Step 6: Commit**
```bash
git add src/game/bot_input.h tests/game/test_bot_input.cpp src/platform/input.h src/platform/input.cpp tests/CMakeLists.txt
git commit -m "feat(input): BotInput synthetic-action overlay + human-activity accessor

Foundation for Autoplay: the bot's held/pressed actions OR into isActionDown/
isActionPressed below the device read, so a bot drives every existing input
consumer through the human code path; Input::humanActivityThisFrame() exposes the
already-computed threshold-filtered device activity as the takeover trigger.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: `AutoplayControl` takeover / resume latch

**Files:**
- Create: `src/game/autoplay_control.h`
- Create: `tests/game/test_autoplay_control.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test** — `tests/game/test_autoplay_control.cpp`:

```cpp
// tests/game/test_autoplay_control.cpp — the takeover/resume latch: the bot yields to a human
// instantly on real device activity and resumes after RESUME_SECONDS of inactivity; UI screens
// never count as takeover (so the player can browse the build while the bot keeps fighting).
#include "doctest/doctest.h"
#include "game/autoplay_control.h"

TEST_CASE("starts in bot control") {
    AutoplayControl c;
    CHECK(c.botInControl());
}

TEST_CASE("human activity grabs control instantly, same tick") {
    AutoplayControl c;
    c.tick(/*humanActive=*/true, /*uiOpen=*/false, 1.0f/60.0f);
    CHECK_FALSE(c.botInControl());
}

TEST_CASE("bot resumes only after the full RESUME_SECONDS of inactivity") {
    AutoplayControl c;
    c.tick(true, false, 1.0f/60.0f);              // human takes over
    REQUIRE_FALSE(c.botInControl());
    const f32 dt = 0.1f;
    for (f32 t = 0.0f; t < AutoplayControl::RESUME_SECONDS - 0.05f; t += dt)
        c.tick(false, false, dt);
    CHECK_FALSE(c.botInControl());                // just under the threshold: still manual
    c.tick(false, false, 0.1f);
    CHECK(c.botInControl());                      // crossed RESUME_SECONDS: bot resumes
}

TEST_CASE("activity mid-countdown re-arms the full timer") {
    AutoplayControl c;
    c.tick(true, false, 0.016f);
    for (f32 t = 0; t < 1.0f; t += 0.1f) c.tick(false, false, 0.1f);
    c.tick(true, false, 0.016f);                  // touched again
    for (f32 t = 0; t < AutoplayControl::RESUME_SECONDS - 0.2f; t += 0.1f) c.tick(false, false, 0.1f);
    CHECK_FALSE(c.botInControl());                // timer restarted, not resumed yet
}

TEST_CASE("input while a UI screen is open never grabs control") {
    AutoplayControl c;                            // bot in control
    c.tick(/*humanActive=*/true, /*uiOpen=*/true, 0.016f);   // Tab/nav keys while inventory open
    CHECK(c.botInControl());                      // bot keeps playing underneath the inventory
}

TEST_CASE("closing the UI and then acting DOES grab control") {
    AutoplayControl c;
    c.tick(true, true, 0.016f);                   // activity ignored (UI open)
    CHECK(c.botInControl());
    c.tick(true, false, 0.016f);                  // now acting with no UI open
    CHECK_FALSE(c.botInControl());
}
```

Add to `tests/CMakeLists.txt`:
```cmake
    game/test_autoplay_control.cpp   # takeover/resume latch (header-only)
```

- [ ] **Step 2: Run to verify it fails** — compile FAILURE (`autoplay_control.h` missing).

- [ ] **Step 3: Create `src/game/autoplay_control.h`**:

```cpp
// autoplay_control.h — who is driving: the bot or the human.
//
// Pure latch. Real gameplay-device activity (Input::humanActivityThisFrame) hands control to the
// human instantly; after RESUME_SECONDS with no activity the bot resumes. Activity while a blocking
// UI screen is open is IGNORED — opening the inventory to change the build must not count as taking
// over (that is the whole point of "browse the build while it keeps fighting"). Only the caller
// knows whether a takeover-exempt screen is open; it passes uiOpen in.
#pragma once
#include "core/types.h"

struct AutoplayControl {
    static constexpr f32 RESUME_SECONDS = 2.0f;   // idle time before the bot resumes

    bool botInControl() const { return m_botControl; }
    f32  resumeCountdown() const { return m_resumeTimer; }   // for the HUD "MANUAL · Ns" readout

    void tick(bool humanActive, bool uiOpen, f32 dt) {
        if (uiOpen) return;                       // UI navigation is never a gameplay takeover
        if (humanActive) { m_botControl = false; m_resumeTimer = RESUME_SECONDS; return; }
        if (!m_botControl) {
            m_resumeTimer -= dt;
            if (m_resumeTimer <= 0.0f) { m_resumeTimer = 0.0f; m_botControl = true; }
        }
    }
    void forceBot() { m_botControl = true; m_resumeTimer = 0.0f; }   // mode entry / floor start

private:
    bool m_botControl = true;
    f32  m_resumeTimer = 0.0f;
};
```

- [ ] **Step 4: Run** — `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*AutoplayControl*,*takeover*,*resume*,*control*"` → PASS; full suite green.

- [ ] **Step 5: Commit**
```bash
git add src/game/autoplay_control.h tests/game/test_autoplay_control.cpp tests/CMakeLists.txt
git commit -m "feat(autoplay): AutoplayControl takeover/resume latch

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: `Doctrine` — the build cell's combat playstyle

**Files:**
- Create: `src/game/autoplay_doctrine.h`
- Create: `tests/game/test_autoplay_doctrine.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test** — `tests/game/test_autoplay_doctrine.cpp`:

```cpp
// tests/game/test_autoplay_doctrine.cpp — the 3x3 build cell -> combat doctrine mapping. The ROW
// (posture) sets risk (potion %, dodge/block bias); the COLUMN (archetype) sets the engagement
// band. These are the "play the build, not just wear it" parameters Aaron asked for.
#include "doctest/doctest.h"
#include "game/autoplay_doctrine.h"
#include "game/build_score.h"   // DEFAULT_BUILD_CELL, buildRow/buildCol

TEST_CASE("column sets engagement band: melee hugs, ranged holds, magic mid") {
    using namespace Autoplay;
    const Doctrine melee  = doctrineFor(3*1 + 1);   // Moderate/Melee
    const Doctrine ranged = doctrineFor(3*1 + 2);   // Moderate/Ranged
    const Doctrine magic  = doctrineFor(3*1 + 0);   // Moderate/Magic
    CHECK(melee.engageMin  == doctest::Approx(0.0f));   // close all the way in
    CHECK(melee.engageMax  <  ranged.engageMax);        // melee fights closer than ranged
    CHECK(ranged.engageMin >  0.0f);                    // ranged keeps a gap (kite band)
    CHECK(ranged.engageMin <  ranged.engageMax);
    CHECK(magic.engageMax  <  ranged.engageMax);        // magic mid-range, not max-range
    CHECK(magic.engageMax  >  melee.engageMax);
}

TEST_CASE("row sets risk: tanky drinks late & blocks, glass drinks early & dodges") {
    using namespace Autoplay;
    const Doctrine tankyMelee = doctrineFor(3*0 + 1);
    const Doctrine glassMelee = doctrineFor(3*2 + 1);
    CHECK(tankyMelee.potionHpFrac <  glassMelee.potionHpFrac);   // 0.35 vs 0.60
    CHECK(tankyMelee.blocks);
    CHECK(glassMelee.dodgesProactively);
    CHECK_FALSE(tankyMelee.dodgesProactively);
    CHECK(glassMelee.usesCover);                                 // glass breaks LOS
    CHECK_FALSE(tankyMelee.usesCover);
}

TEST_CASE("engagement band is expressed as a fraction of the weapon's attackRange") {
    using namespace Autoplay;
    const Doctrine d = doctrineFor(BuildScore::DEFAULT_BUILD_CELL);  // Moderate/Melee
    CHECK(d.engageMin >= 0.0f);
    CHECK(d.engageMax <= 2.0f);            // never more than 2x attackRange
    CHECK(d.disengageCount >= 3);          // "surrounded" threshold, in enemies
}

TEST_CASE("every cell 0..8 yields a valid doctrine (no gaps)") {
    using namespace Autoplay;
    for (u8 cell = 0; cell < 9; cell++) {
        const Doctrine d = doctrineFor(cell);
        CHECK(d.potionHpFrac > 0.0f);
        CHECK(d.potionHpFrac < 1.0f);
        CHECK(d.engageMax > d.engageMin);
    }
}
```

Add to `tests/CMakeLists.txt`:
```cmake
    game/test_autoplay_doctrine.cpp  # build-cell -> combat doctrine table (header-only)
```

- [ ] **Step 2: Run to verify it fails** — compile FAILURE (`autoplay_doctrine.h` missing).

- [ ] **Step 3: Create `src/game/autoplay_doctrine.h`**:

```cpp
// autoplay_doctrine.h — how the Autoplay bot PLAYS a given 3x3 build cell.
//
// The build cell already decides what gear the bot wears (Auto Loot & Equip / BuildScore). This
// table decides how it FIGHTS with that gear: the column (0 Magic / 1 Melee / 2 Ranged) sets the
// engagement band as a fraction of the weapon's attackRange; the row (0 Tanky / 1 Moderate /
// 2 Glass Cannon) sets risk posture (when to drink, whether to hold-block or proactively dodge,
// whether to fight from cover). Pure — engagement is unitless (x attackRange) so the same table
// serves a 2 m sword and a 40 m wand. Consumed by autoplay_combat.h / autoplay_brain.
#pragma once
#include "core/types.h"
#include "game/build_score.h"

namespace Autoplay {

struct Doctrine {
    f32  engageMin = 0.0f;   // hold no closer than this * attackRange (kite floor; 0 = commit)
    f32  engageMax = 1.0f;   // close to at least this * attackRange to fire
    f32  potionHpFrac = 0.5f;// drink when hp/maxHp drops below this
    u8   disengageCount = 3; // this many enemies inside melee arc => break off (Moderate/Melee)
    bool blocks = false;             // hold-block between actions / during reloads
    bool dodgesProactively = false;  // roll away from closing enemies before they hit
    bool usesCover = false;          // reload / recast from findCoverCell, break LOS on cooldown
    bool preferHighGround = false;    // seek balconies/ramps on stacked floors
};

inline Doctrine doctrineFor(u8 cell) {
    const u8 row = BuildScore::buildRow(cell);   // 0 Tanky / 1 Moderate / 2 Glass
    const u8 col = BuildScore::buildCol(cell);   // 0 Magic / 1 Melee / 2 Ranged
    Doctrine d;

    // Column: engagement band as x attackRange.
    switch (col) {
        case 1: d.engageMin = 0.00f; d.engageMax = 0.90f; break;  // Melee: hug
        case 2: d.engageMin = 0.55f; d.engageMax = 1.00f; break;  // Ranged: kite band
        default: d.engageMin = 0.30f; d.engageMax = 0.70f; break;  // Magic: mid
    }

    // Row: risk posture.
    switch (row) {
        case 0: // Tanky
            d.potionHpFrac = 0.35f; d.blocks = true;  d.dodgesProactively = false;
            d.usesCover = false; d.disengageCount = 6; break;
        case 2: // Glass Cannon
            d.potionHpFrac = 0.60f; d.blocks = false; d.dodgesProactively = true;
            d.usesCover = true;  d.disengageCount = 2;
            d.engageMax = (col == 1) ? d.engageMax : 1.00f;      // ranged/magic go max-range
            d.preferHighGround = (col == 2); break;
        default: // Moderate
            d.potionHpFrac = 0.50f; d.blocks = (col != 2); d.dodgesProactively = (col == 2);
            d.usesCover = false; d.disengageCount = 3; break;
    }
    return d;
}

} // namespace Autoplay
```

- [ ] **Step 4: Run** — `./build/tests/dungeon_tests -tc="*doctrine*,*engagement*,*Doctrine*"` → PASS. (If the "melee engageMax < ranged engageMax" check fails, the Glass-Cannon `engageMax` override changed a melee value — verify melee never enters the `col==2` branch.)

- [ ] **Step 5: Commit**
```bash
git add src/game/autoplay_doctrine.h tests/game/test_autoplay_doctrine.cpp tests/CMakeLists.txt
git commit -m "feat(autoplay): build-cell combat doctrine table (play the build, not just wear it)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: `autoplay_nav.h` — hazard veto + per-style steering + descend eligibility

**Files:**
- Create: `src/game/autoplay_nav.h`
- Create: `tests/world/test_autoplay_nav.cpp`
- Modify: `tests/CMakeLists.txt`

This is pure navigation logic over `LevelGrid` / `DungeonResult` / `StoryNav`. It does NOT move the player; it answers "given where I am and where I want to go, what XZ heading is safe, and may I descend?".

- [ ] **Step 1: Write the failing test** — `tests/world/test_autoplay_nav.cpp`:

```cpp
// tests/world/test_autoplay_nav.cpp — pure Autoplay navigation policy: the hazard veto (never
// steer into lava / off a ledge / into an un-landable gap) and descend eligibility (never while
// the boss lives; never onto the Source portal). Built on synthetic LevelGrids so it needs no engine.
#include "doctest/doctest.h"
#include "game/autoplay_nav.h"
#include "world/level_grid.h"

// Minimal 1-story flat grid helper: all floor, optional lava cells.
static LevelGrid makeFlatGrid(u32 w, u32 d);        // impl below the tests
static void setLava(LevelGrid& g, u32 x, u32 z);
static void setSolid(LevelGrid& g, u32 x, u32 z);

TEST_CASE("hazard veto: a heading into a lava cell one step ahead is rejected") {
    LevelGrid g = makeFlatGrid(8, 8);
    setLava(g, 5, 4);
    // Standing at cell (4,4), a +X heading steps onto the lava cell (5,4): vetoed.
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK_FALSE(Autoplay::stepAllowed(g, from, /*feetY=*/0.0f, Vec3{1,0,0}, /*lavaFloor=*/true));
    CHECK(Autoplay::stepAllowed(g, from, 0.0f, Vec3{-1,0,0}, true));   // away from lava: fine
}

TEST_CASE("hazard veto: airborne over lava is allowed (feet above the surface)") {
    LevelGrid g = makeFlatGrid(8, 8);
    setLava(g, 5, 4);
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK(Autoplay::stepAllowed(g, from, /*feetY=*/1.2f, Vec3{1,0,0}, true));  // jumping the vein
}

TEST_CASE("hazard veto: a solid wall one step ahead is rejected (don't walk into walls)") {
    LevelGrid g = makeFlatGrid(8, 8);
    setSolid(g, 5, 4);
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK_FALSE(Autoplay::stepAllowed(g, from, 0.0f, Vec3{1,0,0}, false));
}

TEST_CASE("descend eligibility: never while a boss is alive") {
    Autoplay::DescendCtx ctx;
    ctx.doorActive = true; ctx.distToDoor = 1.0f; ctx.hasBoss = true; ctx.bossAlive = true;
    CHECK_FALSE(Autoplay::mayDescend(ctx));
    ctx.bossAlive = false;
    CHECK(Autoplay::mayDescend(ctx));
}

TEST_CASE("descend eligibility: only inside the 2 m door radius, only when active") {
    Autoplay::DescendCtx ctx;
    ctx.doorActive = true; ctx.hasBoss = false; ctx.bossAlive = false;
    ctx.distToDoor = 3.0f; CHECK_FALSE(Autoplay::mayDescend(ctx));   // too far (>2 m)
    ctx.distToDoor = 1.5f; CHECK(Autoplay::mayDescend(ctx));
    ctx.doorActive = false; CHECK_FALSE(Autoplay::mayDescend(ctx));  // no door (town/arena)
}

// --- helpers (implement using the real LevelGrid API; allocate flowDir/cells as the grid needs) ---
// ... makeFlatGrid / setLava / setSolid bodies here, using CELL_FLOOR / CELL_LAVA / CELL_SOLID
//     from world/cell.h and LevelGridSystem::gridToWorld/worldToGrid.
```

**Note to implementer:** write `makeFlatGrid`/`setLava`/`setSolid` against the real `LevelGrid` struct (see `test_platform.cpp` / `test_lava.cpp` for how those tests allocate a grid and set `cells[].flags`). `stepAllowed` must use `LevelGridSystem::worldToGrid` + `effectiveFloorHeight`/`feetInLava` semantics; mirror the lava check `world/level_grid.h:117-122` (cell is `CELL_LAVA` AND feet at/below surface). Link `world/level_grid.cpp` (already in the test CMake list) — this test is NOT header-only.

Add to `tests/CMakeLists.txt`:
```cmake
    world/test_autoplay_nav.cpp      # Autoplay nav policy: hazard veto + descend gate
```
(`level_grid.cpp` is already linked at `tests/CMakeLists.txt:105`.)

- [ ] **Step 2: Run to verify it fails** — compile FAILURE (`autoplay_nav.h` missing).

- [ ] **Step 3: Create `src/game/autoplay_nav.h`**:

```cpp
// autoplay_nav.h — pure navigation POLICY for the Autoplay bot (no player mutation here).
//
// Two jobs: (1) the hazard veto — is a one-cell XZ step in `dir` safe given the bot's feet
// height, so combat kiting and travel can never back it into lava, a wall, or an un-landable
// gap; (2) descend eligibility — the exact gate updateFloorDoor enforces, mirrored so the brain
// only asks to descend when it actually can. The per-style TRAVEL goal (ramp climb / drop-hole
// pick / causeway) is computed in the brain from StoryNav + DungeonResult; this file holds the
// safety check every steering intent passes through. Built on LevelGrid so it links level_grid.cpp.
#pragma once
#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"

namespace Autoplay {

// True if stepping one cell along `dir` (unit XZ) from `from` at feet height `feetY` is safe:
// destination cell is in-bounds, not solid, and not lava-at-feet-height (airborne over lava is
// free). `lavaFloor` short-circuits the lava test on non-Hellforge floors.
inline bool stepAllowed(const LevelGrid& g, Vec3 from, f32 feetY, Vec3 dir, bool lavaFloor) {
    Vec3 to = from + normalize(Vec3{dir.x, 0.0f, dir.z}) * g.cellSize;   // one cell ahead
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, to, gx, gz)) return false;      // off the map
    const GridCell& c = g.cells[gz * g.width + gx];
    if (c.flags & CELL_SOLID) return false;
    if (lavaFloor && (c.flags & CELL_LAVA)) {
        const f32 surf = LevelGridSystem::getFloorHeight(g, gx, gz);     // lava surface = floor Y
        if (feetY <= surf + 0.05f) return false;                        // grounded in lava: veto
    }
    return true;
}

struct DescendCtx {
    bool doorActive = false;
    f32  distToDoor = 1e9f;
    bool hasBoss = false;
    bool bossAlive = false;
};
// Mirror of updateFloorDoor's gate (engine_update.cpp:2760-2796): within 2 m of an active door and
// not blocked by a live boss. (The Source portal is a SEPARATE trigger the brain simply never
// requests — this function is only ever asked about the ordinary floor door.)
inline bool mayDescend(const DescendCtx& c) {
    if (!c.doorActive) return false;
    if (c.distToDoor > 2.0f) return false;
    if (c.hasBoss && c.bossAlive) return false;
    return true;
}

} // namespace Autoplay
```

**Implementer note:** verify the exact field/method names against `world/level_grid.h` (`g.cellSize`, `g.width`, `g.cells`, `GridCell.flags`, `CELL_SOLID/CELL_LAVA` from `world/cell.h`, `worldToGrid`, `getFloorHeight`). Adjust the snippet to the real names if any differ (e.g. cell size accessor) — the test pins behavior, the field names must compile.

- [ ] **Step 4: Run** — `./build/tests/dungeon_tests -tc="*autoplay_nav*,*hazard*,*descend*"` → PASS; full suite green.

- [ ] **Step 5: Commit**
```bash
git add src/game/autoplay_nav.h tests/world/test_autoplay_nav.cpp tests/CMakeLists.txt
git commit -m "feat(autoplay): pure nav policy — hazard veto + descend-eligibility gate

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: `autoplay_combat.h` — target pick, aim (with lead), fire/defense decisions

**Files:**
- Create: `src/game/autoplay_intent.h` (the shared `BotView`/`BotIntent` structs — first used here)
- Create: `src/game/autoplay_combat.h`
- Create: `tests/game/test_autoplay_combat.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create the shared types** `src/game/autoplay_intent.h`:

```cpp
// autoplay_intent.h — the interface between the Autoplay brain (pure) and the engine driver.
//
// BotView is a read-only snapshot the driver fills from live engine state each tick; BotIntent is
// the semantic output the driver translates into synthetic GameActions + a yaw/pitch write. Keeping
// them plain structs (no engine types beyond math) is what lets the brain unit-test without the Engine.
#pragma once
#include "core/types.h"
#include "core/math.h"

namespace Autoplay {

// One hostile the driver has already resolved from the entity pool (dead/friendly/prop/burrowed
// pre-filtered, matching CombatQuery). Positions are world-space; vel is XZ for lead.
struct BotTarget {
    Vec3 pos;         // AABB centre (aim point)
    Vec3 vel;         // for projectile lead
    f32  dist;        // to the bot's eye
    f32  hp;
    bool isBoss;
    bool hasLOS;      // width-aware LOS from the bot's eye already computed by the driver
};

struct BotView {
    // self
    Vec3 pos;             // feet
    f32  yaw, pitch;      // current aim
    f32  eyeHeight;
    f32  hp, maxHp;
    f32  energy, maxEnergy;
    bool stunned, rolling, onGround;
    f32  dodgeCooldown;   // 0 = dodge ready
    bool potionReady;
    f32  weaponRange;     // effective weapon attackRange (melee small, ranged large)
    f32  weaponProjSpeed; // 0 for hitscan/melee (no lead)
    bool weaponIsMelee;
    u8   buildCell;
    // world
    bool  onNormalFloor;  // false in town/arena/source chamber => bot idles
    // nav
    Vec3  flowDir;        // unit XZ toward exit, or {0,0,0}
    bool  flowValid;      // false when the flow byte is 0xFF (unreachable) — disambiguates zero
    bool  atExit;         // flow byte 0xFE
    // targets (nearest-first, driver-capped)
    const BotTarget* targets;
    u32   targetCount;
    // globes/pickups the driver found in reach (low-hp detour goals), nearest first
    const Vec3* globes;
    u32   globeCount;
};

// One tick of decision. Semantic — the driver maps these onto GameActions.
struct BotIntent {
    f32  aimYaw, aimPitch;                    // desired absolute aim (driver writes to player)
    bool moveFwd, moveBack, moveLeft, moveRight;  // WASD to hold (movement rides yaw + forward)
    bool jump = false, fire = false, block = false, dodge = false;
    bool potion = false, reload = false, descend = false, interact = false;
    s8   classSkillSlot = -1;                 // 0..3 => select SKILL_n + press CLASS_SKILL; -1 none
    bool bootSkill = false, helmetSkill = false;
};

} // namespace Autoplay
```

- [ ] **Step 2: Write the failing test** — `tests/game/test_autoplay_combat.cpp`:

```cpp
// tests/game/test_autoplay_combat.cpp — pure combat policy: pick a target, aim (leading for
// projectiles), and decide fire/move per the doctrine's engagement band. No engine — BotView is
// hand-built.
#include "doctest/doctest.h"
#include "game/autoplay_combat.h"
#include "game/autoplay_doctrine.h"

using namespace Autoplay;

static BotView selfAt(Vec3 p) {
    BotView v{}; v.pos = p; v.eyeHeight = 1.7f; v.hp = 100; v.maxHp = 100;
    v.energy = 100; v.maxEnergy = 100; v.onGround = true; v.weaponRange = 20.0f;
    v.weaponProjSpeed = 40.0f; v.weaponIsMelee = false; v.buildCell = 3*1+2; // Moderate/Ranged
    v.onNormalFloor = true; return v;
}

TEST_CASE("aims at the only target and fires when inside the engagement band") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, 15.0f}; t.dist = 15.0f; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    const Doctrine d = doctrineFor(v.buildCell);
    BotIntent out = decideCombat(v, d);
    // Ranged band 0.55-1.0 x 20 = 11..20 m; 15 m is inside => fire, aim toward +Z.
    CHECK(out.fire);
    CHECK(out.aimYaw == doctest::Approx(0.0f).epsilon(0.02));  // +Z is yaw 0 in the engine convention
}

TEST_CASE("holds fire and advances when the target is beyond engageMax") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, 40.0f}; t.dist = 40.0f; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK_FALSE(out.fire);        // 40 m > 20 m max range
    CHECK(out.moveFwd);           // close the distance
}

TEST_CASE("kites: backs off when the target is inside engageMin") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, 5.0f}; t.dist = 5.0f; t.hasLOS = true;  // 5 m < 11 m floor
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.moveBack);          // Ranged kite floor: retreat
}

TEST_CASE("does not fire without line of sight") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, 15.0f}; t.dist = 15.0f; t.hasLOS = false;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK_FALSE(out.fire);
}

TEST_CASE("leads a crossing target: aim yaw is offset ahead of its position") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, 15.0f}; t.vel = {8.0f, 0, 0};  // moving +X fast
    t.dist = 15.0f; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.fire);
    CHECK(out.aimYaw < 0.0f);     // lead point is +X of the target => aim rotates toward +X (yaw<0)
}

TEST_CASE("picks the nearest LOS target when several exist") {
    BotView v = selfAt({0,0,0});
    BotTarget ts[2];
    ts[0] = {}; ts[0].pos = {0,1.7f,30}; ts[0].dist = 30; ts[0].hasLOS = true;
    ts[1] = {}; ts[1].pos = {0,1.7f,14}; ts[1].dist = 14; ts[1].hasLOS = true;
    v.targets = ts; v.targetCount = 2;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.fire);              // the 14 m one is in band
}
```

Add to `tests/CMakeLists.txt`:
```cmake
    game/test_autoplay_combat.cpp    # pure combat policy: target/aim/fire/kite (header-only)
```
(Header-only: `autoplay_combat.h` includes `lead_assist.h` (engine-free) + math; no `.cpp` to link.)

- [ ] **Step 3: Run to verify it fails** — compile FAILURE (`autoplay_combat.h` missing).

- [ ] **Step 4: Create `src/game/autoplay_combat.h`**:

```cpp
// autoplay_combat.h — pure combat decision for one tick: pick a target, aim (leading projectiles
// with LeadAssist::interceptTime), and translate the doctrine's engagement band into fire + move.
// Aim is returned as absolute yaw/pitch; the engine convention is forward = {-sinYaw*cosPitch,
// sinPitch, -cosYaw*cosPitch} (player.cpp:80), so +Z is yaw 0. Header-only, engine-free.
#pragma once
#include "core/types.h"
#include "core/math.h"
#include "game/autoplay_intent.h"
#include "game/autoplay_doctrine.h"
#include "game/lead_assist.h"

namespace Autoplay {

// dir (unit) -> (yaw,pitch) in the engine convention. Inverse of the forward formula.
inline void dirToAim(Vec3 dir, f32& yaw, f32& pitch) {
    Vec3 d = normalize(dir);
    yaw   = atan2f(-d.x, -d.z);   // forward.x=-sinYaw, forward.z=-cosYaw
    pitch = asinf(d.y);
}

// Returns the index of the nearest target with LOS, or -1 if none has LOS.
inline s32 pickTarget(const BotView& v) {
    s32 best = -1; f32 bestD = 1e9f;
    for (u32 i = 0; i < v.targetCount; i++) {
        if (!v.targets[i].hasLOS) continue;
        if (v.targets[i].dist < bestD) { bestD = v.targets[i].dist; best = (s32)i; }
    }
    return best;
}

inline BotIntent decideCombat(const BotView& v, const Doctrine& d) {
    BotIntent out{};
    out.aimYaw = v.yaw; out.aimPitch = v.pitch;
    const s32 ti = pickTarget(v);
    if (ti < 0) return out;                       // no LOS target: caller falls through to TRAVEL
    const BotTarget& t = v.targets[(u32)ti];
    const Vec3 eye = v.pos + Vec3{0, v.eyeHeight, 0};

    // Aim: lead projectile weapons; hitscan/melee aim straight at the centre.
    Vec3 aimPt = t.pos;
    if (v.weaponProjSpeed > 0.1f) {
        f32 tHit;
        if (LeadAssist::interceptTime(t.pos - eye, t.vel, v.weaponProjSpeed, tHit))
            aimPt = t.pos + t.vel * tHit;
    }
    dirToAim(aimPt - eye, out.aimYaw, out.aimPitch);

    // Engagement band (x weaponRange).
    const f32 lo = d.engageMin * v.weaponRange;
    const f32 hi = d.engageMax * v.weaponRange;
    const bool inBand = t.dist >= lo && t.dist <= hi;

    if (t.dist < lo)      out.moveBack = true;    // too close: kite out (ranged/magic)
    else if (t.dist > hi) out.moveFwd  = true;    // too far: close in

    // Fire when in band with LOS (melee: also require facing, which the aim above provides).
    out.fire = inBand && t.hasLOS && !v.stunned && !v.rolling;

    // Defense posture from the row.
    if (d.dodgesProactively && v.dodgeCooldown <= 0.0f && t.dist < lo * 0.6f && !v.stunned)
        out.dodge = true;                         // glass cannon rolls away from a closer
    if (d.blocks && !out.fire && t.dist <= v.weaponRange && !v.stunned)
        out.block = true;                          // tank blocks between swings

    return out;
}

} // namespace Autoplay
```

- [ ] **Step 5: Run** — `./build/tests/dungeon_tests -tc="*autoplay_combat*,*kite*,*leads*,*engagement band*"` → PASS. (If the lead test's yaw sign fails, re-derive `dirToAim` against the `player.cpp:80` forward formula — the sign convention is load-bearing and pinned by that test.)

- [ ] **Step 6: Commit**
```bash
git add src/game/autoplay_intent.h src/game/autoplay_combat.h tests/game/test_autoplay_combat.cpp tests/CMakeLists.txt
git commit -m "feat(autoplay): pure combat policy — target/aim/lead/fire/kite by doctrine

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: `autoplay_brain.{h,cpp}` — the priority state machine

**Files:**
- Create: `src/game/autoplay_brain.h`, `src/game/autoplay_brain.cpp`
- Create: `tests/game/test_autoplay_brain.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test** — `tests/game/test_autoplay_brain.cpp`:

```cpp
// tests/game/test_autoplay_brain.cpp — the priority state machine that composes survive/fight/
// travel/descend into one BotIntent per tick. Pure: BotView in, BotIntent out.
#include "doctest/doctest.h"
#include "game/autoplay_brain.h"

using namespace Autoplay;

static BotView baseView() {
    BotView v{}; v.eyeHeight = 1.7f; v.hp = 100; v.maxHp = 100; v.energy = 100; v.maxEnergy = 100;
    v.onGround = true; v.weaponRange = 20.0f; v.weaponProjSpeed = 40.0f; v.buildCell = 3*1+2;
    v.onNormalFloor = true; v.flowValid = true; v.flowDir = Vec3{0,0,1}; return v;
}

TEST_CASE("SURVIVE wins: low HP with potion ready => drink") {
    BotView v = baseView(); v.hp = 20; v.potionReady = true;   // 20% < Moderate 50% threshold
    BotIntent out = decide(v);
    CHECK(out.potion);
}

TEST_CASE("FIGHT over TRAVEL: an in-band LOS target => fire, not walk to exit") {
    BotView v = baseView();
    BotTarget t{}; t.pos = {0,1.7f,15}; t.dist = 15; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decide(v);
    CHECK(out.fire);
}

TEST_CASE("TRAVEL when no targets: face the flow direction and walk forward") {
    BotView v = baseView(); v.flowDir = Vec3{1,0,0};   // exit is +X
    BotIntent out = decide(v);
    CHECK(out.moveFwd);
    CHECK(out.aimYaw == doctest::Approx(-1.5708f).epsilon(0.03));  // yaw facing +X
}

TEST_CASE("DESCEND: at the door with the boss dead => request descent") {
    BotView v = baseView(); v.atExit = true; v.flowDir = Vec3{0,0,0};
    v.doorActive = true; v.distToDoor = 1.0f; v.hasBoss = false; v.bossAlive = false;
    BotIntent out = decide(v);
    CHECK(out.descend);
}

TEST_CASE("no descend while the boss lives (walk into the fight instead)") {
    BotView v = baseView(); v.doorActive = true; v.distToDoor = 1.0f;
    v.hasBoss = true; v.bossAlive = true;
    BotTarget boss{}; boss.pos = {0,1.7f,12}; boss.dist = 12; boss.hasLOS = true; boss.isBoss = true;
    v.targets = &boss; v.targetCount = 1;
    BotIntent out = decide(v);
    CHECK_FALSE(out.descend);
    CHECK(out.fire);               // fight the boss
}

TEST_CASE("idle in a non-normal world (town/arena): no movement, no fire") {
    BotView v = baseView(); v.onNormalFloor = false;
    v.flowDir = Vec3{1,0,0};
    BotIntent out = decide(v);
    CHECK_FALSE(out.moveFwd);
    CHECK_FALSE(out.fire);
    CHECK_FALSE(out.descend);
}

TEST_CASE("stunned: emits no move/fire/dodge (CC correctness even in the brain)") {
    BotView v = baseView(); v.stunned = true;
    BotTarget t{}; t.pos={0,1.7f,15}; t.dist=15; t.hasLOS=true; v.targets=&t; v.targetCount=1;
    BotIntent out = decide(v);
    CHECK_FALSE(out.fire);
    CHECK_FALSE(out.moveFwd);
    CHECK_FALSE(out.dodge);
}
```

`autoplay_brain.h` needs the extra `BotView` fields the descend gate uses — extend `autoplay_intent.h`'s `BotView` with: `bool doorActive; f32 distToDoor; bool hasBoss; bool bossAlive;` (add them in this task; they were forward-referenced above). Update `autoplay_intent.h` accordingly and note it in the commit.

Add to `tests/CMakeLists.txt`:
```cmake
    game/test_autoplay_brain.cpp     # the state machine: survive/fight/travel/descend priority
    ${CMAKE_SOURCE_DIR}/src/game/autoplay_brain.cpp
```

- [ ] **Step 2: Run to verify it fails** — compile FAILURE (`autoplay_brain.h` missing).

- [ ] **Step 3: Create `src/game/autoplay_brain.h`**:

```cpp
// autoplay_brain.h — the Autoplay decision core. One pure call per tick: survive > fight >
// loot-settle > travel > descend, plus the non-normal-world idle. Composes autoplay_combat
// (fight) and the flow-field heading (travel) under the doctrine chosen by the build cell.
// Engine-free so it unit-tests on hand-built BotViews.
#pragma once
#include "game/autoplay_intent.h"

namespace Autoplay {
BotIntent decide(const BotView& v);
}
```

Create `src/game/autoplay_brain.cpp`:

```cpp
// autoplay_brain.cpp — see autoplay_brain.h. Priority state machine; each branch returns a full
// BotIntent. TRAVEL faces the flow direction and walks; the per-style vertical goal (ramp/hole/
// pad) is folded into flowDir by the engine driver before the view is built, so the brain stays
// 2D here and the driver owns story routing (it has StoryNav + DungeonResult).
#include "game/autoplay_brain.h"
#include "game/autoplay_doctrine.h"
#include "game/autoplay_combat.h"
#include "game/autoplay_nav.h"
#include "core/math.h"

namespace Autoplay {

static void faceAndGo(const BotView& v, BotIntent& out) {
    if (lengthSq(v.flowDir) < 0.0001f) return;    // at exit or unreachable: no heading
    f32 yaw, pitch; dirToAim(Vec3{v.flowDir.x, 0.0f, v.flowDir.z}, yaw, pitch);
    out.aimYaw = yaw; out.aimPitch = 0.0f;
    // Only advance if the step is safe (hazard veto). The driver passes a lava flag via weaponRange?
    // No — lava/grid live in the driver; TRAVEL safety is enforced driver-side after decide(). Here
    // we simply request forward; the driver vetoes the actual move if stepAllowed() is false.
    out.moveFwd = true;
}

BotIntent decide(const BotView& v) {
    BotIntent out{};
    out.aimYaw = v.yaw; out.aimPitch = v.pitch;

    // Non-normal worlds (town, arena, source chamber): the bot does nothing.
    if (!v.onNormalFloor) return out;

    // Stun: CC correctness — emit nothing actionable (the input layer would suppress it anyway,
    // but keeping the brain honest means the HUD/telemetry never shows a "stunned but acting" bot).
    if (v.stunned) return out;

    const Doctrine d = doctrineFor(v.buildCell);

    // SURVIVE: drink at the doctrine threshold. (Globe detours are a driver concern — it steers
    // toward v.globes when low; here we just handle the potion press.)
    if (v.hp <= v.maxHp * d.potionHpFrac && v.potionReady) { out.potion = true; return out; }

    // FIGHT: any LOS target in reach takes priority over travelling.
    if (pickTarget(v) >= 0) return decideCombat(v, d);

    // DESCEND: at an eligible door, ask to descend.
    DescendCtx dc; dc.doorActive = v.doorActive; dc.distToDoor = v.distToDoor;
    dc.hasBoss = v.hasBoss; dc.bossAlive = v.bossAlive;
    if (mayDescend(dc)) { out.descend = true; return out; }

    // TRAVEL: walk the flow field toward the exit.
    faceAndGo(v, out);
    return out;
}

} // namespace Autoplay
```

- [ ] **Step 4: Run** — `./build/tests/dungeon_tests -tc="*autoplay_brain*,*SURVIVE*,*FIGHT*,*TRAVEL*,*DESCEND*,*idle in a non*"` → PASS; full suite green.

- [ ] **Step 5: Commit**
```bash
git add src/game/autoplay_brain.h src/game/autoplay_brain.cpp src/game/autoplay_intent.h tests/game/test_autoplay_brain.cpp tests/CMakeLists.txt
git commit -m "feat(autoplay): the brain — survive/fight/travel/descend priority state machine

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Menu entry, `m_menu.autoplay` flag, `--autoplay` launch door

Pure wiring — no new testable unit; verified by build + a manual launch. This adds the mode-selection surface and arms the bot state (defined next task) at start.

**Files:**
- Modify: `src/engine/engine.h` (add `m_menu.autoplay` bool + the bot-state fields the driver will use, and `botMayAct()`)
- Modify: `src/engine/engine_render_menus.cpp` (8th menu row)
- Modify: `src/engine/engine_menu.cpp` (5 mirrored sites + flag set/reset + subState-2 skip-chooser + solo-start arm)
- Modify: `src/engine/launch_options.h`, `src/engine/launch_options.cpp`, `src/engine/engine_launch.cpp` (`--autoplay`)

- [ ] **Step 1: Add state to `engine.h`.** In `MenuState` after `bool arena = false;` (`:161`):
```cpp
        bool autoplay = false;   // Autoplay mode intent (main-menu row); armed at solo-start,
                                 // cleared by the main-menu confirm reset. Mirrors `arena`.
```
Add Engine members (near other per-session flags; these are the driver's home — the driver task uses them):
```cpp
    bool             m_autoplayActive = false;   // this run is an Autoplay session (lane 0 bot)
    AutoplayControl  m_autoplayControl;           // takeover/resume latch (game/autoplay_control.h)
```
Add `#include "game/autoplay_control.h"` to engine.h's includes. Add the freeze carve-out helper next to `gameplayInputFrozen()` (`:300`):
```cpp
    // The bot may act (drive movement/fire/skills) when it holds control and only the inventory is
    // open — NOT while a hard-freeze UI (pause / character inspect / options / menagerie) is up.
    bool botMayAct() const {
        if (!m_autoplayActive || !m_autoplayControl.botInControl()) return false;
        return !(m_characterScreenOpen || m_menu.confirmQuit || m_menu.optionsFromPause || m_menagerieOpen);
    }
```

- [ ] **Step 2: 8th main-menu row** in `engine_render_menus.cpp:1156-1172`. Add `"Autoplay"` after `"Single Player"` in `fullLabels`, a color in `fullColors`, bump `count` full to `8u`. **Leave `demoLabels`/`demoColors`/demo count untouched** (Autoplay is hidden in the demo, like Host/Join/Arena):
```cpp
        static const char* fullLabels[] = {"Single Player", "Autoplay", "Host Game", "Join Game", "Arena Mode", "Options", "Credits", "Exit Game"};
        static const Vec3 fullColors[] = {
            {0.2f, 0.9f, 0.2f},
            {0.4f, 0.85f, 0.95f},   // cyan for Autoplay
            {0.2f, 0.5f, 1.0f}, {1.0f, 0.7f, 0.2f}, {0.9f, 0.25f, 0.4f},
            {0.6f, 0.6f, 0.8f}, {1.0f, 0.6f, 0.1f}, {0.7f, 0.2f, 0.2f},
        };
        ...
        const u32 count = GameConst::kDemoBuild ? 4u : 8u;   // was 7u
```
**Because Autoplay is inserted at index 1, every full-build action index below shifts by one.** The `demoActionMap` maps demo rows to full action indices — since demo hides Autoplay, its map must now point at the shifted full indices.

- [ ] **Step 3: Menu sites in `engine_menu.cpp`.**
  - `menuMouseForState` case 0 (`:132`): `const u8 mainCount = GameConst::kDemoBuild ? 4 : 8;`
  - `MENU_DOWN` maxSel (`:1802`): `const u8 maxSel = GameConst::kDemoBuild ? 3 : 7;`
  - Session reset block (`:1819`): add `m_menu.autoplay = false;` beside `m_menu.arena = false;`
  - `demoActionMap` (`:1825`): the demo's 4 rows {Single Player, Options, Credits, Exit} now map to full indices {0, 5, 6, 7} (Options/Credits/Exit each shifted +1): `static const u8 demoActionMap[4] = {0, 5, 6, 7};`
  - `switch(menuAction)` (`:1828-1903`): renumber the existing cases (Host 1→2, Join 2→3, Arena 3→4, Options 4→5, Credits 5→6, Exit 6→7) and insert:
```cpp
        case 1: // Autoplay — same New/Continue → slot → class flow as Single Player, bot armed at start
            m_menu.autoplay = true;
            scanSaveSlots();
            m_menu.subState = 1;
            m_menu.subSelection = 0;
            break;
```
  **This case-renumbering is the drift trap the row-Y comment warns about — do it as one edit and recount.**

- [ ] **Step 4: Skip the play-style chooser for autoplay.** In the subState-2 (class select) confirm at `engine_menu.cpp:774-786`, after `applyClassToLane0(...)`, branch:
```cpp
            applyClassToLane0(static_cast<PlayerClass>(m_menu.subSelection));
            if (m_menu.autoplay) {
                // Autoplay forces Auto Loot & Equip (the bot's gear brain) and skips the Classic/Auto
                // chooser — go straight to the solo-start lobby. Keep the character's persisted build.
                m_inventories[0].autoMode  = 1;
                if (m_inventories[0].buildCell >= 9) m_inventories[0].buildCell = BuildScore::DEFAULT_BUILD_CELL;
                m_difficulty = 0;
                m_menu.subState = 4;      // couch-lobby solo-start (NONE branch)
                m_menu.subSelection = 0;
                return;
            }
            m_menu.subState = 23;   // normal: play-style chooser
            m_menu.subSelection = 0;
```

- [ ] **Step 5: Arm the bot at solo-start.** In the couch-lobby NONE branch (`engine_menu.cpp:899-906`), where `startGame(...)` / `enterTown()` run, set `m_autoplayActive` and force bot control AFTER the world is up. Since a fresh autoplay hero on a cleared account calls `enterTown()` (which is a non-normal world — the bot idles there until the player portals in), arm the flag regardless and let `botMayAct()`/`onNormalFloor` gate actual driving:
```cpp
                } else {
                    startGame(m_menu.p1Continue ? GameStart::CONTINUE : GameStart::NEW_GAME);
                    if (!m_menu.p1Continue && m_townUnlocked) enterTown();
                    if (m_menu.autoplay) {
                        m_autoplayActive = true;
                        m_autoplayControl.forceBot();
                        Input::setBotOverlayActive(true);
                    }
                }
```
Also clear it when a normal (non-autoplay) run starts anywhere, and on any teardown to menu — simplest: set `m_autoplayActive = false; Input::setBotOverlayActive(false);` in the same session-reset block that clears `m_menu.arena` (`engine_menu.cpp:1811-1819` area) and in `arenaLeaveToMenu`/quit-to-menu paths (grep `m_gameState = GameState::MENU` and add the two lines where a run ends).

- [ ] **Step 6: `--autoplay` launch door.** `launch_options.h` after `autoLoot` (`:56`): `bool autoplay = false;  // --autoplay: bot plays the run (dev door; needs --new/--load)`. `launch_options.cpp` mirror the `--autoloot` branch (`:157-159`): `} else if (ieq(a, "--autoplay")) { opt.autoplay = true; opt.autoLoot = true; opt.active = true; }` (autoplay implies autoloot). `engine_launch.cpp` after the autoloot block (`:90`):
```cpp
    if (opt.autoplay) {
        m_autoplayActive = true;
        m_autoplayControl.forceBot();
        Input::setBotOverlayActive(true);
    }
```

- [ ] **Step 7: Build + manual check.** `cmake --build build 2>&1 | tail -3` (game + tests link). Run `./build/dungeon_game --autoplay --new 0` (or the repo's dev-jump syntax — check `launch_options.cpp` for the `--new`/`--load` argument shape) and confirm the game reaches a floor without the bot doing anything yet (the driver lands next task); confirm the "Autoplay" menu row renders and is selectable (mouse + keyboard) with the other rows correctly aligned. Full suite still green.

- [ ] **Step 8: Commit**
```bash
git add src/engine/engine.h src/engine/engine_render_menus.cpp src/engine/engine_menu.cpp src/engine/launch_options.h src/engine/launch_options.cpp src/engine/engine_launch.cpp
git commit -m "feat(autoplay): main-menu Autoplay row, m_menu.autoplay flag, --autoplay dev door

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: Engine glue — the per-tick bot driver

The integration task: each sim tick, build a `BotView` from live state, call `Autoplay::decide`, and apply the `BotIntent` as synthetic actions + a yaw/pitch write; run the control latch; enforce the freeze carve-out; drive respawn; draw the HUD badge. Verified live (the pure logic it composes is already unit-tested).

**Files:**
- Create: `src/engine/engine_autoplay.cpp` (the driver: `Engine::updateAutoplay(f32 dt)`, `Engine::buildBotView(...)`, `Engine::applyBotIntent(...)`)
- Modify: `src/engine/engine.h` (declare the three methods)
- Modify: `src/engine/engine_update.cpp` (call the driver; freeze carve-out; auto-respawn)
- Modify: `src/engine/engine_hud.cpp` (AUTO/MANUAL badge)
- Modify: `src/CMakeLists.txt` (add `engine/engine_autoplay.cpp`)

- [ ] **Step 1: Declare + register.** In `engine.h` near other update helpers: `void updateAutoplay(f32 dt); Autoplay::BotView buildBotView(); void applyBotIntent(const Autoplay::BotIntent& in);` and `#include "game/autoplay_intent.h"`, `#include "game/autoplay_brain.h"`. Add `src/engine/engine_autoplay.cpp` to `src/CMakeLists.txt` (near the other `engine/` sources).

- [ ] **Step 2: The driver.** Create `src/engine/engine_autoplay.cpp`. It must, once per sim tick while `m_autoplayActive` and `m_gameState == IN_GAME`:
  1. Update the control latch: `m_autoplayControl.tick(Input::humanActivityThisFrame(), gameplayInputFrozen(), dt);` — **but** pass `uiOpen = (m_inventoryOpen || hardFreeze)` so activity during any UI never grabs control (the latch already ignores it; use the same set as `botMayAct`'s exemptions plus the inventory).
  2. If the bot does not hold control, `Input::clearBotHeld()` and return (human drives).
  3. If it holds control but `!botMayAct()` (hard-freeze up), `Input::clearBotHeld()` and return (paused/inspecting — bot waits).
  4. Otherwise: `BotView v = buildBotView(); BotIntent in = Autoplay::decide(v); applyBotIntent(in);`

`buildBotView()` fills the snapshot from live state:
  - self from `m_localPlayer` (pos, yaw, pitch, eyeHeight, health/maxHealth, stunTimer>0, dodgeState.rolling, onGround, dodgeState.cooldownTimer), energy from `m_skillStates[m_localPlayerIndex]`, `potionReady` from the potion cooldown check (`engine_update.cpp:1741` logic), `weaponRange`/`weaponProjSpeed`/`weaponIsMelee` from the effective weapon (`Inventory::getEffectiveWeapon`), `buildCell` from `m_inventories[0].buildCell`.
  - `onNormalFloor = !(m_level.inTown || m_level.inArena || m_level.inSourceChamber) && m_level.floorDoorActive` (a normal dungeon floor has a door; sentinels don't route to an exit).
  - nav: read the flow byte at the bot's cell (`m_level.grid.flowDir[...]`) to set `flowValid`/`atExit`, and `flowDir = LevelGridSystem::flowDirection(m_level.grid, m_localPlayer.position)`. **Then fold in the story layer:** if `layoutStyle` is VERTICAL_HALL/FOUR_STORY, override `flowDir` toward the `StoryNav` goal when the exit is on another story (`nearestPortalGoal` for ramps; `dropHoles[]` filtered by `surfaceY` for descent; `nearestPadGoal` for going up) — this is where the per-style routing lives (the brain stays 2D). Apply `Autoplay::stepAllowed` as a veto: if the chosen `flowDir` step is unsafe (lava/gap), try the two 45°-rotated headings and pick the first safe one; if none, leave `flowDir` zero (STUCK — the driver's stuck timer re-paths).
  - descend ctx: `doorActive`, `distToDoor = length(m_level.floorDoorPos - m_localPlayer.position)`, `hasBoss = m_level.floorHasBoss`, `bossAlive = floorBossAlive()`.
  - **LOOT-SETTLE + cover/high-ground positioning (driver-side, because they need the grid the pure brain lacks):**
    - **LOOT-SETTLE:** keep an `f32 m_autoplayLootDwell` on Engine, set to ~1.5 s whenever the target count drops to zero having been non-zero last tick (a fight just ended). While it is counting down AND a `worthPickingUp` item is within the auto-loot radius, suppress the TRAVEL forward-move (let the vacuum collect before moving on); cap the dwell at ~3 s. This is the spec's LOOT-SETTLE state, implemented as a driver dwell rather than a brain state (the vacuum/equip/prune are all existing systems).
    - **Cover (`doctrine.usesCover`):** when the chosen combat intent is not firing (reloading / on cooldown) and there is a live threat, steer toward `LevelGridQuery::findCoverCell(m_level.grid, pos, threatPos, out)` instead of the raw combat move — fold that XZ heading into the applied move. **High ground (`doctrine.preferHighGround`)** on VERTICAL_HALL/FOUR_STORY: bias the fight position toward the nearest `dungeon.portals[].highPos` / balcony when one is reachable. These read the doctrine flags Task 3 set and Task 5 deliberately left for the driver.
  - targets: iterate `m_entities.activeList[0..activeCount)`, skip `ENT_DEAD|ENT_FRIENDLY|PROP|ENT_BURROWED` (mirror `combat_query.cpp:81-87`), fill a small fixed `BotTarget[16]` nearest-first (sort by dist), `hasLOS` via `CombatQuery::raycast(m_level.grid, m_entities, eye, dirToTarget, dist)` returning that entity (or the existing `hasLOSToPoint` helper). Cap at 16.
  - globes: scan `m_worldItems` for `isGlobe()` within a detour radius when `hp` is low; fill `globes[]`.

`applyBotIntent(in)`:
  - `m_localPlayer.yaw = in.aimYaw; m_localPlayer.pitch = in.aimPitch;` (clamp pitch ±89° to match applyMovement).
  - `Input::setBotHeld(GameAction::MOVE_FORWARD, in.moveFwd);` and same for BACK/LEFT/RIGHT, `JUMP` (in.jump), `FIRE` (in.fire), `BLOCK` (in.block), `DODGE` (in.dodge), `POTION` (in.potion), `RELOAD` (in.reload). For skills: if `in.classSkillSlot >= 0`, `setBotHeld(SKILL_1 + slot, true)` and `setBotHeld(CLASS_SKILL, true)`; `BOOT_SKILL`/`HELMET_SKILL` from the bools.
  - Descent: `if (in.descend) m_descendRequested = true;` (set the flag directly — `updateFloorDoor` owns the boss gate + autosave; never call `triggerFloorDescent`).
  - **Clear last tick's held actions first** each tick (`Input::clearBotHeld()` at the top of `applyBotIntent`) so a released action doesn't stick.

- [ ] **Step 3: Wire the driver + freeze carve-out + respawn into `engine_update.cpp`.**
  - Call `updateAutoplay(dt)` inside the IN_GAME tick BEFORE `gameUpdate` runs `PlayerController::update` — so the bot's yaw write and synthetic actions are in place when the player controller and fire/skill handlers read them. Place it in `gameUpdate` just before the `if (!gameplayInputFrozen())` block at `:1769`.
  - Freeze carve-out: change the three gates from `if (!gameplayInputFrozen())` to `if (!gameplayInputFrozen() || botMayAct())` at: movement/moveAndSlide (`:1769`), fire/target-lock (`:1821`), and the skill activations (`:1898-1899` — check the exact guard site). This lets the bot drive under an open inventory while keeping pause/inspect fully frozen. **Verify** the potion (`:1744`) and block (`:1905`) guards similarly — add `|| botMayAct()` where the bot needs them.
  - Auto-respawn: in the `GAME_OVER` dispatch (`:148`), when `m_autoplayActive`, run a short countdown (reuse a new `f32 m_autoplayRespawnTimer` on Engine, set to ~1.5 s when GAME_OVER is entered), then synthesize the Respawn (drive `Input::setBotHeld(GameAction::JUMP, true)` for one frame, OR directly execute the option-0 body at `:191-216`). Directly executing is cleaner and avoids the overlay running while GAME_OVER's own input read expects JUMP — set `m_localPlayer.health = m_localPlayer.maxHealth; m_localPlayer.position = m_players[activeNetSlot()].spawnPosition; m_localPlayer.invulnTimer = 1.5f; m_gameState = IN_GAME;` (copy the exact respawn body).

- [ ] **Step 4: HUD badge.** In `engine_hud.cpp`, in the floor-indicator block (`:569-578`, which already draws "Arena"/"The Town"/"Floor %u"), when `m_autoplayActive` draw an "AUTO" strap; when `!m_autoplayControl.botInControl()` show "MANUAL · %.0fs" using `m_autoplayControl.resumeCountdown()`. It sits in the normal HUD branch, so F10-hide and pause-hide apply for free. Announce bot milestones (respawn, descend) via `addChatMessage(...)` like auto-equip does.

- [ ] **Step 5: Build + live smoke.** `cmake --build build 2>&1 | tail -3`. Run `./build/dungeon_game --autoplay --new 0`: the bot should walk toward the exit, fight enemies it meets, pick up loot (auto-loot), and descend. Grab the mouse/WASD → control transfers instantly; stop → "MANUAL · Ns" counts down and the bot resumes at 0. Open the inventory → the bot keeps fighting; change the build cell → doctrine changes live. Full suite green.

- [ ] **Step 6: Commit**
```bash
git add src/engine/engine_autoplay.cpp src/engine/engine.h src/engine/engine_update.cpp src/engine/engine_hud.cpp src/CMakeLists.txt
git commit -m "feat(autoplay): engine driver — BotView build, intent apply, freeze carve-out, HUD badge

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: Live acceptance across layout styles + docs

**Files:**
- Modify: `CLAUDE.md`, `.claude/skills/engine-reference/SKILL.md`, `.claude/skills/engine-how-to/SKILL.md`
- (No production code unless acceptance reveals a bug — if it does, fix it under the relevant task's file with a test where a pure unit is involved.)

- [ ] **Step 1: Acceptance runs.** Build release-ish (`cmake --build build`). Run each and observe unattended for the stated goal (the bot must not need input):
  - `./build/dungeon_game --autoplay --new 0` — completes floors 1–10 including the floor-5 and floor-10 bosses (bosses gate the exit; the bot fights until the door unseals).
  - `./build/dungeon_game --autoplay --fourstory --new 0` — descends one "Descent" maze floor (drop-hole routing; must not stall over a wide hole or fall past its target without recovering via a pad).
  - `./build/dungeon_game --autoplay --lava --new 0` — crosses one Hellforge floor without walking into a lake (hazard veto) and jumps a 1-cell vein.
  - `./build/dungeon_game --autoplay --vhall --new 0` — climbs the diagonal-corner ramp to the exit balcony (never attempts the broken catwalk).
  Record pass/fail per style. Any stall → investigate (flow-byte ambiguity, story-goal fold-in, or hazard veto over-vetoing) and fix in Task 8's driver / Task 4's nav; add a nav unit test if the bug is in pure logic.

- [ ] **Step 2: CLAUDE.md.** Add an "Autoplay mode" paragraph to the architecture section: the input-overlay seam (`Input::setBotHeld` OR'd into `checkActionRaw`), the pure brain (`src/game/autoplay_*`), the freeze carve-out (`botMayAct()` — bot drives under an open inventory, pause/inspect still freeze it), the per-lane note (v1 lane-0-only; the overlay/control are ready for per-lane), SP-only (`NetRole::NONE`), no save-format change, and the `--autoplay` dev door. Note that this bot is the empirical rig the balance-lab spec deferred (follow-up: emit per-floor metrics).

- [ ] **Step 3: engine-reference.** Add the new `Input` API (`setBotHeld`/`humanActivityThisFrame`/overlay) and the `AutoplayControl::RESUME_SECONDS` constant to the cheat sheet.

- [ ] **Step 4: engine-how-to.** Add a "How autoplay drives the player" recipe + pitfalls: the `--bot-walk` seam is dead in SP (inject at the action layer); interact arbitration means set `m_descendRequested`, never synthesize a PICKUP tap with loot in reach; the flow byte's zero-vector ambiguity (0xFE vs 0xFF); lava/gaps are invisible to the flow field (the hazard veto is mandatory); the menu five-site mirror + case-renumber trap when adding a row.

- [ ] **Step 5: Full verification.** `cmake --build build 2>&1 | tail -3`; `./build/tests/dungeon_tests --no-version | tail -3` — all green; game builds.

- [ ] **Step 6: Commit**
```bash
git add CLAUDE.md .claude/skills/engine-reference/ .claude/skills/engine-how-to/
git commit -m "docs: Autoplay mode — CLAUDE.md paragraph, engine-reference API, engine-how-to recipe

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Post-plan notes for the executor

- **Order matters:** Tasks 1–6 are pure and independently testable; Task 7 (menu) and Task 8 (driver) are the integration spine and depend on 1–6. Do them in order.
- **Do not** add a new `GameAction` — the overlay drives the existing ones. Do not touch `PlayerController`'s `s_botWalk` path (it's the dead MP-only rig).
- **The freeze carve-out (`botMayAct`) is the single subtlest change** — verify by opening the inventory mid-run and confirming the bot keeps moving/fighting while pause still freezes it.
- **If a live acceptance run stalls on a stacked/lava floor**, that's the story-goal fold-in or hazard veto in Task 8's `buildBotView` — fix there; only add code to the pure `autoplay_nav.h` if the bug is in the pure veto/step logic (and pin it with a `test_autoplay_nav.cpp` case).
- Keep everything SP/lane-0. The overlay, control latch, and view are per-invocation, not per-lane yet; couch/MP bot lanes are explicitly out of scope (spec).
