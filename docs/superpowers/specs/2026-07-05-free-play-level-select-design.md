# Free-Play Level Select (Post-Clear) — Design Spec

**Date:** 2026-07-05
**Status:** Approved (design); pending spec review → implementation plan
**Scope owner:** DungeonEngine — menu / continue flow

## Goal

Once a character has **cleared the game** (beaten Hell floor 50), let them **choose a difficulty (Normal/Nightmare/Hell) and a floor (1–50)** when they Continue, so they can farm specific bosses / floors (and a home for future free-play options). The pick is a **non-destructive replay** — the cleared save is never overwritten with a lower floor.

## Background (verified against code + save data)

- Floors advance in `triggerFloorDescent()` (`engine_update.cpp:1716`). The **save is written (line 1741) *before* the floor-50 victory/reset check (line 1769)**. So a **Hell** character who beats floor 50 saves `floor=51`, then hits the "Hell complete → VICTORY" branch (no reset). Continuing + descending again climbs 52, 53, … — this is how the real save `save_01` (**Wanderer, floor byte = 57, difficulty = Hell**) exists. Normal/Nightmare instead bump difficulty and reset to floor 1.
- Therefore **"cleared the game" is detectable with zero save-format change:** `difficulty == Hell (2) && floor > 50`.
- The save header stores `floor (u8)` and `difficulty (u8)` in separate fields (`engine_persist.cpp` reader at :150-156, writer at :207-236); `SaveSlotInfo` already surfaces both (`m_saveSlots[]`).
- **No-downgrade guard** (`engine_persist.cpp:210-224`): for a lane `m_laneLoadedFromSave[lane]`, a save is never written with a lower effective floor (`floor + difficulty*50`) than the slot already holds. So farming a lower floor with a cleared hero keeps `save_01` pinned at Hell 57 automatically — **the non-destructive property is free.**
- Level generation is deterministic from `(levelSeed, currentFloor, difficulty)` (`engine_startgame.cpp:418-433`); the `--load` launch path already demonstrates "set `m_level.currentFloor` then `startGame(CONTINUE)`" (`engine_launch.cpp:72-84`).

## Requirements / decisions

1. **Trigger:** cleared = `difficulty == 2 && floor > 50`. Applies to **single-player Continue** only (couch co-op continue is unchanged / out of scope for v1).
2. **Choice:** difficulty ∈ {Normal, Nightmare, Hell} (all three — clearing Hell implies all tiers unlocked) + floor ∈ [1, 50].
3. **Defaults:** difficulty = **Hell**, floor = **1**.
4. **Non-destructive:** the character's slot keeps its cleared state (Hell 57); farming a lower floor does not overwrite it (relies on the existing guard — no new logic needed to protect it).
5. **No save-format change**, **no change to non-cleared Continue**, **no persistent floor jump**, **Hell 51+ endless climb not exposed** (range capped at 50).

## Architecture / components

### A. Cleared-save predicate (pure, testable)
A free function usable without the engine, e.g. in a small header `src/game/free_play.h`:

```cpp
// A save has "cleared the game" once it has beaten Hell floor 50. The final floor of each
// difficulty tier is 50 (matches the >50 check in triggerFloorDescent); the save-before-
// victory ordering leaves such saves at Hell, floor 51+.
inline bool saveCleared(u8 floor, u8 difficulty) {
    return difficulty >= 2u && floor > 50u;   // 2 == Hell
}
```
(Use a named constant for `50`/`2` if the codebase gains one; today both are literals in `triggerFloorDescent`.)

### B. Menu state — new "Free Play" sub-state
- Add a new unused `MenuState::subState` value (the plan picks the exact number after auditing existing ones: 0 main, 1 SP New/Continue, 2 Class, 4 Couch Lobby, 6 Save Slot, 11 P2 New/Continue, 12 P2 Slot).
- Add two `MenuState` fields: `u8 freePlayDifficulty` (0–2) and `u8 freePlayFloor` (1–50).

### C. Flow hook (single-player Continue)
In the slot-select handler (`engine_menu.cpp`, subState 6), on the SP (non-couch) Continue branch, **after `loadGame(slot)` succeeds and before `startGame(GameStart::CONTINUE)`**:
- If `saveCleared(m_saveSlots[slot-1].floor, m_saveSlots[slot-1].difficulty)` → initialize `freePlayDifficulty = 2`, `freePlayFloor = 1`, set `subState = FREE_PLAY`, and **return** (do not start the game yet).
- Else → `startGame(CONTINUE)` unchanged.

### D. Free-Play screen (render + input)
Follows the existing menu patterns (`engine_render_menus.cpp` render; `engine_menu.cpp` input + `menuMouseHit`):
- **Render:** title ("Free Play"); a **Difficulty** row showing Normal / Nightmare / Hell with the current one highlighted; a **Floor** row showing `‹  N  ›` (1–50); a **Descend** confirm; a **Back** hint. Reuse `FontSystem::drawText` / `HUD::drawMenuOption`.
- **Input:** navigate/change difficulty (Left/Right or click a tier) and floor (Left/Right or Up/Down, clamped 1–50, optional hold-to-repeat); **Confirm** (Enter / gamepad A / click Descend); **Back** (Esc / gamepad B / MENU_BACK) → return to the slot list (subState 6). Keyboard + gamepad + mouse, consistent with sibling screens.

### E. Applying the pick
On Confirm:
```cpp
m_difficulty          = m_menu.freePlayDifficulty;   // override the loaded save's difficulty
m_level.currentFloor  = m_menu.freePlayFloor;        // override the loaded save's floor
startGame(GameStart::CONTINUE);
```
`startGame(CONTINUE)` regenerates the level from the save's `levelSeed` + the chosen floor/difficulty; the character's inventory/stats are already loaded. Plan must verify `startGame(CONTINUE)` consumes `m_level.currentFloor` (as the `--load` path implies) and does not re-derive it from `savedFloor`.

## Data flow

pick slot → `loadGame()` (sets savedFloor=57, difficulty=Hell, levelSeed) → **cleared? →** Free-Play screen → user picks (diff, floor) → override `m_difficulty` + `m_level.currentFloor` → `startGame(CONTINUE)` → deterministic level for that floor → play. On the first in-run descent, the no-downgrade guard compares on-disk `57 (Hell, eff 157)` vs the new lower floor and **keeps 57** → save protected.

## Error handling / edge cases

- **Effective-floor safety:** any pick maxes at Hell 50 (eff 150) < a cleared save's ≥151, so every pick is a downgrade → always non-destructive. (No pick can accidentally advance/overwrite the cleared save.)
- **Back out:** Esc/B from the Free-Play screen returns to the slot list with the character still loaded but no game started (matches how other sub-states unwind); re-picking a slot re-runs the check.
- **Non-cleared saves:** never see the screen — identical behavior to today.
- **Demo build:** `kDemoBuild` caps at floor 20 and never reaches Hell, so no cleared saves exist there — the screen is effectively full-game-only, no special-casing required.

## Testing

Pure logic gets doctest coverage (no GL/engine needed):
- `saveCleared(floor, difficulty)`: true for `(57, 2)` and `(51, 2)`; false for `(50, 2)` (on the final floor, not yet beaten), `(57, 1)` (Nightmare), `(30, 2)` (mid-Hell).
- Floor-stepper clamp helper (`clampFloor(n) ∈ [1,50]`, and the +/- step logic): boundaries at 1 and 50.
The screen's rendering/input is validated in-engine (forward-only on UI, per project convention). A natural manual check: Continue `save_01` (Wanderer) → Free-Play screen appears → pick Hell/floor 1 → farm → confirm the slot still reads Hell 57 afterward.

## Non-goals

- No save-format / `SAVE_VERSION` change.
- No change to non-cleared Continue, to New Game, or to the descent/victory mechanics.
- No persistent "move my save to floor N" (non-destructive only).
- No exposure of the Hell 51+ endless climb (range is strictly 1–50).
- Couch co-op continue unchanged (v1 is single-player Continue).

## Extensibility

The Free-Play screen + `MenuState` free-play fields are the intended home for later additions the user mentioned ("or something else I add later") — e.g. run modifiers, target-boss shortcuts, loot/difficulty toggles — without touching the normal Continue path.
