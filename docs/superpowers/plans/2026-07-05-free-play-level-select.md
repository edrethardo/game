# Free-Play Level Select (Post-Clear) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a character that has cleared the game (beaten Hell floor 50) pick a difficulty + floor (1–50) when they Continue, for non-destructive boss/floor farming.

**Architecture:** A pure `FreePlay::saveCleared()` predicate detects a cleared save (`difficulty == Hell && floor > 50`) with no save-format change. On single-player Continue, the couch-lobby solo-start hook routes a cleared hero to a new menu screen (sub-state 14) that picks difficulty + floor; confirming overrides `m_difficulty` / `m_level.currentFloor` / `m_level.savedFloor` and calls `startGame(CONTINUE)`. The existing no-downgrade save guard keeps the cleared slot pinned automatically.

**Tech Stack:** C++17, existing menu state machine (`engine_menu.cpp` input + `engine_render_menus.cpp` render), `FontSystem` / `HUD::drawMenuOption`, doctest.

**⚠️ Commit policy:** This project requires **explicit user authorization before any commit**. Each task's commit step is part of the plan, but the executor MUST get the user's go-ahead before running it; otherwise complete the task and leave it staged/unstaged.

**Scope guardrails (do NOT touch):** save format / `SAVE_VERSION`; the descent/victory mechanics; non-cleared Continue; New Game; couch co-op continue (P2 joined); the Hell 51+ endless climb (range is strictly 1–50). The `--load` launch flag stays as-is (bypasses the menu; Free-Play is menu-only).

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/game/free_play.h` (new) | Pure predicates: `saveCleared`, `clampFloor`, `clampDifficulty`, constants | 1 |
| `tests/game/test_free_play.cpp` (new) | doctest for the predicates | 1 |
| `tests/CMakeLists.txt` | Register the test | 1 |
| `src/engine/engine.h` | `MenuState` gains `freePlayDifficulty` + `freePlayFloor`; sub-state comment | 2 |
| `src/engine/engine_render_menus.cpp` | Render the sub-state-14 Free-Play screen | 2 |
| `src/engine/engine_menu.cpp` | Sub-state-14 input (kbd/gamepad/mouse) + apply; the entry hook | 3, 4 |

---

## Task 1: Pure `FreePlay` helpers + doctest

**Files:**
- Create: `src/game/free_play.h`
- Create: `tests/game/test_free_play.cpp`
- Modify: `tests/CMakeLists.txt` (add to the `add_executable(dungeon_tests ...)` list)

- [ ] **Step 1: Write the failing test**

Create `tests/game/test_free_play.cpp`:

```cpp
// test_free_play.cpp — unit tests for the post-clear Free-Play predicates (game/free_play.h).
#include "doctest/doctest.h"
#include "game/free_play.h"

using namespace FreePlay;

TEST_CASE("saveCleared: true only for Hell past floor 50") {
    CHECK(saveCleared(57, 2));        // the wanderer (save_01)
    CHECK(saveCleared(51, 2));        // just beat Hell 50
    CHECK_FALSE(saveCleared(50, 2));  // on Hell 50, not yet beaten
    CHECK_FALSE(saveCleared(57, 1));  // Nightmare (difficulty 1), not Hell
    CHECK_FALSE(saveCleared(30, 2));  // mid-Hell
    CHECK_FALSE(saveCleared(20, 0));  // Normal
}

TEST_CASE("clampFloor keeps floor in [1,50]") {
    CHECK(clampFloor(1) == 1);
    CHECK(clampFloor(50) == 50);
    CHECK(clampFloor(25) == 25);
    CHECK(clampFloor(0) == 1);
    CHECK(clampFloor(51) == 50);
    CHECK(clampFloor(-5) == 1);
    CHECK(clampFloor(999) == 50);
}

TEST_CASE("clampDifficulty keeps difficulty in [0,2]") {
    CHECK(clampDifficulty(0) == 0);
    CHECK(clampDifficulty(2) == 2);
    CHECK(clampDifficulty(-1) == 0);
    CHECK(clampDifficulty(3) == 2);
}
```

Register it — in `tests/CMakeLists.txt`, add this line to the `add_executable(dungeon_tests ...)` source list, right after `game/test_difficulty_scaling.cpp`:

```cmake
    game/test_free_play.cpp
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target dungeon_tests`
Expected: FAIL — `game/free_play.h: No such file or directory`.

- [ ] **Step 3: Create the header**

Create `src/game/free_play.h`:

```cpp
// free_play.h — pure predicates for the post-clear Free-Play level select.
//
// A character has "cleared the game" once it beats Hell floor 50. Because triggerFloorDescent()
// saves the incremented floor BEFORE the floor-50 victory check (engine_update.cpp:1741 vs :1769),
// a cleared Hell save sits at difficulty 2 (Hell), floor 51+ (e.g. save_01 = floor 57). Header-only
// and engine-free so it unit-tests without a GL/engine context.
#pragma once

#include "core/types.h"

namespace FreePlay {

inline constexpr u8 DIFFICULTY_COUNT = 3;   // Normal(0), Nightmare(1), Hell(2)
inline constexpr u8 MIN_FLOOR = 1;
inline constexpr u8 MAX_FLOOR = 50;         // final designed floor of each difficulty tier

// True once the save has beaten Hell floor 50 (Hell == difficulty 2, floor climbed past 50).
inline bool saveCleared(u8 floor, u8 difficulty) {
    return difficulty >= 2u && floor > 50u;
}

// Clamp a (possibly stepped) floor into [MIN_FLOOR, MAX_FLOOR].
inline u8 clampFloor(s32 floor) {
    if (floor < static_cast<s32>(MIN_FLOOR)) return MIN_FLOOR;
    if (floor > static_cast<s32>(MAX_FLOOR)) return MAX_FLOOR;
    return static_cast<u8>(floor);
}

// Clamp a difficulty index into [0, DIFFICULTY_COUNT-1].
inline u8 clampDifficulty(s32 d) {
    if (d < 0) return 0;
    if (d > static_cast<s32>(DIFFICULTY_COUNT) - 1) return DIFFICULTY_COUNT - 1;
    return static_cast<u8>(d);
}

} // namespace FreePlay
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*saveCleared*,*clampFloor*,*clampDifficulty*"`
Expected: PASS — 3 cases green.

- [ ] **Step 5: Verify full suite**

Run: `cmake --build build && ./build/tests/dungeon_tests --no-version`
Expected: build succeeds; all tests pass.

- [ ] **Step 6: Commit** (only with user authorization)

```bash
git add src/game/free_play.h tests/game/test_free_play.cpp tests/CMakeLists.txt
git commit -m "feat(freeplay): saveCleared predicate + floor/difficulty clamps"
```

---

## Task 2: MenuState fields + Free-Play render (sub-state 14)

**Files:**
- Modify: `src/engine/engine.h` (MenuState struct, ~line 99-137)
- Modify: `src/engine/engine_render_menus.cpp` (add the sub-state-14 render branch)

- [ ] **Step 1: Add MenuState fields**

In `src/engine/engine.h`, inside `struct MenuState`, after the `subSelection` line (line 104), add:

```cpp
        // Free-Play (post-clear level select, sub-state 14): a cleared character's chosen difficulty
        // (0-2) and floor (1-50) for a non-destructive farming session. subSelection picks the active
        // row (0 = difficulty, 1 = floor).
        u8   freePlayDifficulty = 2;   // default Hell
        u8   freePlayFloor      = 1;   // default floor 1
```

Update the `subState` comment (lines 101-103) to append `, 14=free-play level select (post-clear)`.

- [ ] **Step 2: Ensure includes in engine_render_menus.cpp**

At the top of `src/engine/engine_render_menus.cpp`, ensure `<cstdio>` is included (for `std::snprintf`); add it if missing.

- [ ] **Step 3: Add the render branch**

In `src/engine/engine_render_menus.cpp`, in `renderMenu()`, add a new branch in the `if/else if` chain — insert it right after the `else if (m_menu.subState == 13) { ... }` block (which ends near line 542, before the `else if (m_menu.subState == 7)` credits block):

```cpp
    } else if (m_menu.subState == 14) {
        // Free-Play level select — a cleared hero (Hell, floor > 50) picks difficulty + floor 1-50
        // to farm. Non-destructive: the no-downgrade save guard keeps the cleared slot pinned.
        const char* title = "Free Play";
        f32 tW = FontSystem::textWidth(title, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tW) * 0.5f, sh * 0.68f, title,
                             {0.3f, 1.0f, 0.5f}, 3);

        static const char* diffNames[3] = {"Normal", "Nightmare", "Hell"};

        // Row 0 — difficulty (y = sh*0.50), Row 1 — floor (y = sh*0.50 - 46px). Matches the mouse
        // hit-test in engine_menu.cpp's sub-state-14 handler — keep both in sync.
        for (u32 row = 0; row < 2; row++) {
            bool sel = (m_menu.subSelection == row);
            f32 y = sh * 0.50f - static_cast<f32>(row) * 46.0f * uiScale;
            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.4f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 360.0f * uiScale, 35.0f * uiScale, col, sel);
            char buf[48];
            if (row == 0)
                std::snprintf(buf, sizeof(buf), "Difficulty:  < %s >", diffNames[m_menu.freePlayDifficulty]);
            else
                std::snprintf(buf, sizeof(buf), "Floor:  < %u >", static_cast<u32>(m_menu.freePlayFloor));
            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.6f, 0.6f, 0.6f};
            f32 lw = FontSystem::textWidth(buf, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - lw) * 0.5f, y + 10.0f * uiScale, buf, tc, 2);
        }

        const char* hint = "Up/Down select   Left/Right change   Enter/A Descend   B/ESC Back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.12f, hint,
                             {0.4f, 0.4f, 0.5f}, 1);
    }
```

(`uiScale`, `sw`, `sh` are already in scope in `renderMenu` — confirm against the sibling sub-state-13 block which uses them the same way.)

- [ ] **Step 4: Verify build**

Run: `cmake --build build && ./build/tests/dungeon_tests --no-version`
Expected: build succeeds; tests pass. (The screen isn't reachable yet — Task 4 wires the entry — but it compiles.)

- [ ] **Step 5: Commit** (only with user authorization)

```bash
git add src/engine/engine.h src/engine/engine_render_menus.cpp
git commit -m "feat(freeplay): MenuState fields + Free-Play level-select render (sub-state 14)"
```

---

## Task 3: Free-Play input + apply (sub-state 14)

**Files:**
- Modify: `src/engine/engine_menu.cpp` (add the sub-state-14 input block; include `free_play.h`)

- [ ] **Step 1: Include the header**

At the top of `src/engine/engine_menu.cpp`, add:

```cpp
#include "game/free_play.h"
```

- [ ] **Step 2: Add the sub-state-14 input block**

In `src/engine/engine_menu.cpp`, in the menu input function, add this block right after the `if (m_menu.subState == 4) { ... }` block closes (after its trailing `return;` near line 457). It handles keyboard + gamepad + mouse, then applies on confirm:

```cpp
    // Free-Play level select (post-clear). subSelection: 0 = difficulty row, 1 = floor row.
    if (m_menu.subState == 14) {
        // --- keyboard + gamepad navigation (P1 == controller 0) ---
        bool up   = Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_DPAD_UP)   ||
                    Input::isMenuStickPressed(Input::StickNav::Up, 0)   || Input::isKeyPressed(SDL_SCANCODE_UP);
        bool down = Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
                    Input::isMenuStickPressed(Input::StickNav::Down, 0) || Input::isKeyPressed(SDL_SCANCODE_DOWN);
        bool left = Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
                    Input::isMenuStickPressed(Input::StickNav::Left, 0) || Input::isKeyPressed(SDL_SCANCODE_LEFT);
        bool right= Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)||
                    Input::isMenuStickPressed(Input::StickNav::Right, 0)|| Input::isKeyPressed(SDL_SCANCODE_RIGHT);

        if (up   && m_menu.subSelection > 0) { m_menu.subSelection--; AudioSystem::play(SfxId::MENU_HOVER); }
        if (down && m_menu.subSelection < 1) { m_menu.subSelection++; AudioSystem::play(SfxId::MENU_HOVER); }

        // Left/Right adjusts the active row's value (difficulty or floor), clamped.
        if (left || right) {
            s32 delta = right ? 1 : -1;
            if (m_menu.subSelection == 0)
                m_menu.freePlayDifficulty = FreePlay::clampDifficulty(static_cast<s32>(m_menu.freePlayDifficulty) + delta);
            else
                m_menu.freePlayFloor = FreePlay::clampFloor(static_cast<s32>(m_menu.freePlayFloor) + delta);
            AudioSystem::play(SfxId::MENU_HOVER);
        }

        // --- mouse: hover selects a row; click a row's left/right third steps its value ---
        {
            s32 mx, my; Input::getMousePosition(mx, my);
            f32 uiScale = static_cast<f32>(Window::getHeight()) / 720.0f;
            f32 sw = static_cast<f32>(Window::getWidth());
            f32 sh = static_cast<f32>(Window::getHeight());
            // HUD coords are y-up; drawMenuOption boxes are 360x35 (scaled), centered, at the render y's.
            f32 boxW = 360.0f * uiScale, boxH = 35.0f * uiScale;
            f32 hudY = sh - static_cast<f32>(my);
            f32 cxL = (sw - boxW) * 0.5f, cxR = cxL + boxW;
            for (u32 row = 0; row < 2; row++) {
                f32 y = sh * 0.50f - static_cast<f32>(row) * 46.0f * uiScale;
                if (mx >= cxL && mx <= cxR && hudY >= y && hudY <= y + boxH) {
                    if (m_menu.subSelection != row) { m_menu.subSelection = static_cast<u8>(row); AudioSystem::play(SfxId::MENU_HOVER); }
                    if (Input::isMouseButtonPressed(MOUSE_LEFT)) {
                        s32 delta = (mx > cxL + boxW * 0.5f) ? 1 : -1; // right half = +, left half = -
                        if (row == 0)
                            m_menu.freePlayDifficulty = FreePlay::clampDifficulty(static_cast<s32>(m_menu.freePlayDifficulty) + delta);
                        else
                            m_menu.freePlayFloor = FreePlay::clampFloor(static_cast<s32>(m_menu.freePlayFloor) + delta);
                        AudioSystem::play(SfxId::MENU_HOVER);
                    }
                }
            }
        }

        // --- confirm (Descend): apply the pick and start ---
        if (Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_A) ||
            Input::isKeyPressed(SDL_SCANCODE_RETURN) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            AudioSystem::play(SfxId::UI_CONFIRM);
            m_difficulty         = m_menu.freePlayDifficulty;      // override the loaded save's difficulty
            m_level.currentFloor = m_menu.freePlayFloor;           // startGame(CONTINUE) reads this
            m_level.savedFloor   = m_menu.freePlayFloor;           // so death-respawn (currentFloor=savedFloor,
                                                                   // engine_update.cpp:195) stays on the chosen
                                                                   // floor, not the cleared 57. Guarded save keeps disk at Hell 57.
            m_menu.subState      = 0;
            m_menu.subSelection  = 0;
            startGame(GameStart::CONTINUE);
            return;
        }

        // --- back: return to the slot list ---
        if (Input::isButtonPressed(0, SDL_CONTROLLER_BUTTON_B) || Input::isActionPressed(GameAction::MENU_BACK)) {
            AudioSystem::play(SfxId::UI_BACK);
            m_menu.subState     = 6;          // save-slot list
            m_menu.subSelection = 0;
            m_menu.msg          = "continue";
        }
        return;
    }
```

Notes for the implementer:
- Verify the exact input helpers exist with these signatures (they're used verbatim by sibling sub-states): `Input::isButtonPressed(u8 pad, SDL_GameControllerButton)`, `Input::isMenuStickPressed(Input::StickNav, u8 pad)`, `Input::isKeyPressed(SDL_Scancode)`, `Input::isActionPressed(GameAction)`, `Input::isMouseButtonPressed(MOUSE_LEFT)`, `Input::getMousePosition(s32&, s32&)`, `Window::getWidth/getHeight()`. If a name differs, match the sibling sub-state-4 / sub-state-11 code in this file.
- `SfxId::MENU_HOVER / UI_CONFIRM / UI_BACK` are used elsewhere in this file — reuse as-is.

- [ ] **Step 3: Verify build**

Run: `cmake --build build && ./build/tests/dungeon_tests --no-version`
Expected: build succeeds; tests pass. (Still not reachable until Task 4.)

- [ ] **Step 4: Commit** (only with user authorization)

```bash
git add src/engine/engine_menu.cpp
git commit -m "feat(freeplay): Free-Play input + apply (override floor/difficulty, guarded start)"
```

---

## Task 4: Entry hook — route cleared solo-continue to Free-Play

**Files:**
- Modify: `src/engine/engine_menu.cpp` (the couch-lobby solo-start branch, ~line 443-446)

- [ ] **Step 1: Intercept the solo continue**

In `src/engine/engine_menu.cpp`, in the sub-state-4 (couch lobby) handler, the local-solo branch currently reads:

```cpp
            } else {
                // Local solo: CONTINUE keeps the loaded hero, NEW_GAME wipes+grants P1.
                startGame(m_menu.p1Continue ? GameStart::CONTINUE : GameStart::NEW_GAME);
            }
```

Replace it with:

```cpp
            } else {
                // Local solo. A CLEARED hero (Hell, floor > 50) opens the Free-Play level select to
                // farm any difficulty + floor 1-50 (non-destructive); everyone else starts now.
                // m_level.savedFloor / m_difficulty were set by loadGame() for the continued hero.
                if (m_menu.p1Continue &&
                    FreePlay::saveCleared(static_cast<u8>(m_level.savedFloor), m_difficulty)) {
                    m_menu.freePlayDifficulty = 2;   // default Hell
                    m_menu.freePlayFloor      = 1;   // default floor 1
                    m_menu.subState           = 14;  // Free-Play level select
                    m_menu.subSelection       = 0;
                } else {
                    startGame(m_menu.p1Continue ? GameStart::CONTINUE : GameStart::NEW_GAME);
                }
            }
```

(The enclosing branch already set `m_splitPlayerCount = 1`, `Input::setSplitScreen(false)`, and `m_menu.subState = 0` earlier in the block; setting `subState = 14` here overrides the 0 so the Free-Play screen shows next frame. `free_play.h` is already included from Task 3.)

- [ ] **Step 2: Verify build + suite**

Run: `cmake --build build && ./build/tests/dungeon_tests --no-version`
Expected: build succeeds; all tests pass.

- [ ] **Step 3: Manual end-to-end check (the wanderer)**

`save_01.dat` is the Wanderer at Hell floor 57 (cleared). Run `./build/src/DungeonEngine`, then: Main Menu → Single Player → **Continue** → slot 1 (Wanderer) → couch lobby → press **Start/Enter** (start solo). Expected: the **Free Play** screen appears (Difficulty ‹ Hell ›, Floor ‹ 1 ›). Change difficulty/floor with Left/Right, switch rows with Up/Down, press **Enter** to descend into that floor. Play it, descend once, quit — then re-scan saves and confirm **slot 1 still reads Hell 57** (decode: `python3 -c "import struct;b=open('save_01.dat','rb').read();print('floor',b[4],'diff',b[16])"` → `floor 57 diff 2`), proving the run was non-destructive.

  - Selecting a **non-cleared** save (e.g. slot 2, Warrior Normal 17) must Continue exactly as before (no Free-Play screen).

- [ ] **Step 4: Commit** (only with user authorization)

```bash
git add src/engine/engine_menu.cpp
git commit -m "feat(freeplay): open Free-Play level select for cleared solo continue"
```

---

## Final review (after all tasks)

- [ ] **Full build + suite:** `cmake --build build && ./build/tests/dungeon_tests --no-version` → green.
- [ ] **Non-destructive proof:** after farming a low floor with the wanderer, `save_01.dat` header still decodes to `floor 57, difficulty 2` (the no-downgrade guard held).
- [ ] **Regression:** non-cleared Continue, New Game, and couch co-op continue behave exactly as before (the hook only fires for `p1Continue && saveCleared`).
- [ ] **Dispatch a final code review** over the whole change (predicate correctness, the `savedFloor` set preventing a floor-57 respawn, no stray sub-state collisions, menu input parity with siblings).

## Notes / decisions carried from the spec

- **Detection uses the loaded `m_level.savedFloor` + `m_difficulty`** (set by `loadGame`), which equal the save's on-disk floor/difficulty at the moment of the hook. No `SaveSlotInfo` re-read needed.
- **`savedFloor` is overridden along with `currentFloor`** so an in-run death respawns on the chosen floor (SP respawn does `currentFloor = savedFloor`, engine_update.cpp:195) — not the cleared 57. Persistence stays safe because the no-downgrade guard compares against the on-disk `SaveSlotInfo` (still 57), not `savedFloor`.
- **Menu-only:** the `--load` launch flag continues to boot straight into the saved floor (bypasses the menu); Free-Play is intentionally a menu feature.
- **Sub-state 14** is the first unused menu sub-state (0–13 and 9 are taken; 7=credits, 13=couch-start).
