# Autoplay: Stay On The Story & Melee Ranged Sidearm — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On stacked floors the Autoplay bot must stop wandering off its current story to shoot ranged enemies and, above all, must not step off a ledge while fighting; and a melee build must temporarily equip a ranged sidearm from its backpack when the only way to hit an enemy would be to fall or leave the story, switching back to melee when the situation passes.

**Architecture:** Two coordinated pieces sharing one new pure primitive. (1) A `wouldFall` grid test (autoplay_nav.h) is applied as a veto on the FIGHT branch's own movement (which is deliberately un-hazard-vetoed today), so combat kiting/closing can never carry the bot off a balcony or into a drop hole. The FIGHT branch is tagged so the veto never touches an intended TRAVEL/descent step. (2) A driver-side sidearm state machine equips the best ranged weapon already sitting in the backpack (the auto-loot keeper logic keeps one because it upgrades the ranged builds) via `Inventory::equip`, overrides the per-tick doctrine/weapon view to ranged while it is worn, suppresses `autoEquipBackpack` so it can't be re-geared out from under the bot, and switches back to melee when the trigger clears — all in singleplayer lane 0, so there are no netcode concerns.

**Tech Stack:** C++17, doctest (vendored `external/doctest/doctest.h`), the existing Autoplay pure cores (`src/game/autoplay_*`) plus the engine driver (`src/engine/engine_autoplay.cpp`). No new libraries. No save-format or PROTOCOL change (all state is transient driver members).

---

## Background the implementer needs

- **Autoplay is a bot that plays a real singleplayer run through the exact human input path.** The pure decision core lives in `src/game/autoplay_*` (engine-free, unit-tested); the thin engine driver is `src/engine/engine_autoplay.cpp`. Per tick the driver calls `buildBotView()` (snapshots player/weapon/nav/hostiles into a `BotView`), then `Autoplay::decide(view)` returns a `BotIntent`, then `applyBotIntent(intent, ...)` maps it onto held `GameAction`s + a yaw/pitch write. All three are in `engine_autoplay.cpp` and `autoplay_brain.cpp`.
- **The FIGHT branch's movement is NOT hazard-vetoed today, on purpose.** `Autoplay::stepAllowed` (the travel hazard veto) is applied in `buildBotView` to the TRAVEL heading (`v.flowDir`) only. The FIGHT branch's kite/close/strafe movement is short and enemy-derived and runs unvetoed — see the "veto's scope" paragraph in `CLAUDE.md`. That is exactly why a kiting bot can back off a balcony. There is already a precedent for vetoing FIGHT movement narrowly: `applyBotIntent` (engine_autoplay.cpp ~line 715) drops jump-pad steps on FOUR_STORY via `Autoplay::padAhead`. This plan extends that same block.
- **`effectiveFloorHeight(grid, x, z, feetY)`** (`src/world/level_grid.h:144`) returns the walkable surface a body at `feetY` stands on in cell (x,z): the highest slab top at/below `feetY + PLATFORM_STEP_TOLERANCE`, else the base floor. A destination cell whose effective floor is far *below* the feet is a ledge. `PLATFORM_STEP_TOLERANCE` is `0.4f` (`src/world/level_grid.h:60`).
- **`stackedFloor`** is already on `BotView` (true for VERTICAL_HALL and FOUR_STORY). `sameStory(view, target)` already drops cross-story targets from `pickTarget` on stacked floors.
- **The auto-loot scorer** (`src/game/build_score.h`) gates weapons by family: `weaponInFamily(subtype, col)` (col 0 Magic / 1 Melee / 2 Ranged). A melee build (col 1) *equips* only melee, but `worthPickingUp` reasons over all nine build cells, so a ranged weapon is *kept in the backpack* because it upgrades the ranged builds. `buildRow(cell)`/`buildCol(cell)` decompose a cell; `BUILD_COLS == 3`; `cell == row*3 + col`.
- **`Inventory::equip(inv, backpackIndex, itemDefs)`** (`src/game/inventory.cpp:210`) moves the backpack item into its slot and puts the previously-equipped item back into the freed backpack slot, then `recalculateStats`. `Inventory::getEffectiveWeapon(inv, itemDefs, fallbackWeaponDef)` returns the equipped weapon merged with its affixes; `buildBotView` reads it into `v.weaponRange` / `v.weaponProjSpeed` / `v.weaponIsMelee` (engine_autoplay.cpp ~line 924). So once a ranged weapon is equipped, the whole combat policy fires at range for free.
- **`autoEquipBackpack(lane)`** (`src/engine/engine_inventory.cpp`) re-gears the whole bag to the active build cell; it runs on a build-cell change, after every pickup, and on a slow housekeeping pass. It would undo a manual sidearm swap, so it must be suppressed while the sidearm is worn.
- **Build & test commands:**
  - `cmake --build build -j8` (game) / `cmake --build build --target dungeon_tests -j8` (tests)
  - `./build/tests/dungeon_tests` (full suite) / `./build/tests/dungeon_tests -tc="*name*"` (filter)
  - Live dev doors: `./build/src/DungeonEngine --autoplay --vhall --new marksman`, `--fourstory`, plain `--autoplay --new warrior`.
- **Item cell constants used in tests** (mirror `test_build_score.cpp`): `MOD_MELEE = 1*3+1`, `MOD_RANGED = 1*3+2`.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/game/autoplay_nav.h` | modify | Add `wouldFall` (pure ledge test) beside `stepAllowed`/`padAhead`. |
| `tests/world/test_autoplay_nav.cpp` | modify | Pin `wouldFall` on synthetic slab grids. |
| `src/game/autoplay_intent.h` | modify | Add `BotIntent::engaging` (tags a FIGHT tick) and `BotView::meleeWantsSidearm` (driver→driver signal, defaulted false so tests/flat floors are unaffected). |
| `src/game/autoplay_brain.cpp` | modify | Set `out.engaging = true` inside the FIGHT branch. |
| `tests/game/test_autoplay_brain.cpp` | modify | Pin that FIGHT sets `engaging` and TRAVEL/DESCEND do not. |
| `src/game/build_score.h` | modify | Add `bestRangedBackpackIdx` (pure: the backpack slot holding the best ranged-family weapon, or -1). |
| `tests/game/test_build_score.cpp` | modify | Pin the sidearm picker. |
| `src/game/autoplay_doctrine.h` | modify | Add `rangedCellFor(cell)` (same row, Ranged column). |
| `tests/game/test_autoplay_doctrine.cpp` | modify | Pin `rangedCellFor`. |
| `src/engine/engine.h` | modify | Add transient sidearm state members. |
| `src/engine/engine_autoplay.cpp` | modify | Apply the fall veto to FIGHT movement; compute the sidearm trigger; run the sidearm state machine; override the doctrine/weapon view while worn. |
| `src/engine/engine_inventory.cpp` | modify | Guard `autoEquipBackpack` against running while the sidearm is worn. |
| `CLAUDE.md`, `.claude/skills/engine-how-to/SKILL.md` | modify | Document both behaviors and their traps. |

**Shippability:** Tasks 1–2 (the fall veto) are a complete, independently valuable change — the anti-fall guarantee — and can be committed and even released alone. Tasks 3–8 add the sidearm on top.

---

## Task 1: `wouldFall` — the pure ledge test

**Files:**
- Modify: `src/game/autoplay_nav.h` (add after `padAhead`, near the other `stepAllowed` helpers)
- Test: `tests/world/test_autoplay_nav.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/world/test_autoplay_nav.cpp`. It reuses the file's existing `makeFlatGrid`/`setSolid` helpers; it builds a two-cell-high situation by putting a balcony slab (`addPlatform`) on part of the grid so one cell is a 3 m ledge above its neighbour.

```cpp
TEST_CASE("wouldFall: stepping off a balcony edge onto the ground is a fall") {
    // Cells (4,4) and (5,4) carry a 3 m slab (a balcony); (6,4) does not (open ground below).
    // A body standing on the balcony (feetY = 3) that steps +X from (5,4) to (6,4) drops 3 m.
    LevelGrid g = makeFlatGrid(10, 10);
    for (u32 x = 4; x <= 5; x++) LevelGridSystem::addPlatform(g.cells[4 * g.width + x], 12, 0); // 12 q = 3 m
    const Vec3 onBalcony = LevelGridSystem::gridToWorld(g, 5, 4);   // XZ of (5,4)
    CHECK(Autoplay::wouldFall(g, onBalcony, /*feetY=*/3.0f, Vec3{ 1, 0, 0}));  // +X onto bare ground (6,4)
    CHECK_FALSE(Autoplay::wouldFall(g, onBalcony, 3.0f, Vec3{-1, 0, 0}));      // -X onto the slab (4,4)
    LevelGridSystem::shutdown(g);
}

TEST_CASE("wouldFall: no drop on flat ground, and a zero heading never falls") {
    LevelGrid g = makeFlatGrid(10, 10);
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK_FALSE(Autoplay::wouldFall(g, from, /*feetY=*/0.0f, Vec3{1, 0, 0}));  // flat: not a fall
    CHECK_FALSE(Autoplay::wouldFall(g, from, 0.0f, Vec3{0, 0, 0}));            // no heading: never a fall
    LevelGridSystem::shutdown(g);
}

TEST_CASE("wouldFall: a small step-up ledge (<= tolerance) is NOT a fall") {
    // A body on the ground next to a 0.25 m slab is not "falling" onto it — that is a walkable stair.
    LevelGrid g = makeFlatGrid(10, 10);
    LevelGridSystem::addPlatform(g.cells[4 * g.width + 5], 1, 0);   // 1 q = 0.25 m, under the 0.4 tolerance
    const Vec3 from = LevelGridSystem::gridToWorld(g, 4, 4);
    CHECK_FALSE(Autoplay::wouldFall(g, from, 0.0f, Vec3{1, 0, 0}));
    LevelGridSystem::shutdown(g);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target dungeon_tests -j8`
Expected: FAIL to COMPILE with `'wouldFall' is not a member of 'Autoplay'`.

- [ ] **Step 3: Write the implementation**

Add to `src/game/autoplay_nav.h`, immediately after the `padAhead` function:

```cpp
// True when a one-cell XZ step in `dir` from `from` (feet at `feetY`) drops the body more than a
// step below its current footing — i.e. off a LEDGE (a balcony rim, the lip of a drop hole).
//
// This is the "prefer not to fall down" test, and it is DISTINCT from stepAllowed's hazard veto,
// which deliberately does NOT cover balcony-edge drops (those are intentional traversal for the
// TRAVEL/descent layer). Here we want the opposite: the FIGHT branch's kite/close/strafe movement
// must never carry the bot off an edge chasing an enemy. The caller is responsible for applying it
// ONLY to combat movement (see BotIntent::engaging) so it can never veto an intended descent step.
//
// `effectiveFloorHeight` picks the surface the body would stand on in the destination cell for its
// current height; if that surface sits more than PLATFORM_STEP_TOLERANCE below the feet, stepping
// there is a fall rather than a walkable step-down. Off-map is left to stepAllowed (returns false
// here so this test alone never blocks a step the caller didn't ask it to).
inline bool wouldFall(const LevelGrid& g, Vec3 from, f32 feetY, Vec3 dir) {
    Vec3 flat{dir.x, 0.0f, dir.z};
    if (lengthSq(flat) < 1e-6f) return false;                 // no heading: nothing to fall off
    const Vec3 to = from + normalize(flat) * g.cellSize;      // one cell ahead
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, to, gx, gz)) return false;   // off-map: stepAllowed's job
    const f32 dest = LevelGridSystem::effectiveFloorHeight(g, gx, gz, feetY);
    return dest < feetY - PLATFORM_STEP_TOLERANCE;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target dungeon_tests -j8 && ./build/tests/dungeon_tests -tc="*wouldFall*"`
Expected: PASS, 3 test cases.

- [ ] **Step 5: Commit**

```bash
git add src/game/autoplay_nav.h tests/world/test_autoplay_nav.cpp
git commit -m "feat(autoplay): wouldFall — the pure off-a-ledge test"
```

---

## Task 2: Veto FIGHT movement that would fall

**Files:**
- Modify: `src/game/autoplay_intent.h` (add `BotIntent::engaging`)
- Modify: `src/game/autoplay_brain.cpp` (set it in the FIGHT branch)
- Modify: `src/engine/engine_autoplay.cpp` (apply the veto)
- Test: `tests/game/test_autoplay_brain.cpp`

- [ ] **Step 1: Add the `engaging` flag to `BotIntent`**

In `src/game/autoplay_intent.h`, in `struct BotIntent`, add a field after the `dodgeIsGapClose` line:

```cpp
    // True on a tick the FIGHT branch produced this intent (decideCombat). The driver applies the
    // off-a-ledge veto (Autoplay::wouldFall) to combat movement ONLY, so it can never cancel an
    // intended TRAVEL/descent step (walking into a drop hole to descend is a fall the bot WANTS).
    bool engaging = false;
```

- [ ] **Step 2: Write the failing brain test**

Add to `tests/game/test_autoplay_brain.cpp`. It reuses the file's `baseView()` helper.

```cpp
TEST_CASE("engaging flag: set on a FIGHT tick, clear on TRAVEL and DESCEND") {
    // FIGHT: an in-band LOS target => decideCombat => engaging true.
    {
        BotView v = baseView(); v.buildCell = 3*1 + 1;   // Moderate Melee
        v.weaponRange = 2.0f;
        BotTarget t{}; t.pos = {0,1.7f,2}; t.dist = 2.0f; t.hasLOS = true;
        v.targets = &t; v.targetCount = 1;
        CHECK(decide(v).engaging);
    }
    // TRAVEL: no targets, just a flow heading => not engaging.
    {
        BotView v = baseView(); v.flowDir = Vec3{1,0,0};
        CHECK_FALSE(decide(v).engaging);
    }
    // DESCEND: at the door, boss dead => not engaging.
    {
        BotView v = baseView(); v.atExit = true; v.flowDir = Vec3{0,0,0};
        v.doorActive = true; v.distToDoor = 1.0f; v.hasBoss = false; v.bossAlive = false;
        CHECK_FALSE(decide(v).engaging);
    }
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cmake --build build --target dungeon_tests -j8 && ./build/tests/dungeon_tests -tc="*engaging flag*"`
Expected: FAIL — `decide(v).engaging` is false on the FIGHT case (field defaults false, nothing sets it).

- [ ] **Step 4: Set the flag in the FIGHT branch**

In `src/game/autoplay_brain.cpp`, `decide()` calls `decideCombat` for the FIGHT branch. Set the flag at the top of `decideCombat` in `src/game/autoplay_combat.h`. Find `inline BotIntent decideCombat(const BotView& v, const Doctrine& d) {` and its `BotIntent out{};` line; add immediately after it:

```cpp
    out.engaging = true;   // this intent came from FIGHT — the driver may fall-veto its movement
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target dungeon_tests -j8 && ./build/tests/dungeon_tests -tc="*engaging flag*"`
Expected: PASS.

- [ ] **Step 6: Apply the fall veto to combat movement in the driver**

In `src/engine/engine_autoplay.cpp`, find the existing FOUR_STORY jump-pad combat-movement veto in `applyBotIntent` (search for `Autoplay::padAhead`). It currently reads roughly:

```cpp
    if (m_level.layoutStyle == LevelGen::LayoutStyle::FOUR_STORY &&
        !Autoplay::onJumpPad(m_level.grid, m_localPlayer.position) &&
        !m_autoplayDescent.paddedOnly) {   // same carve-out as the travel veto: lifts-only storey
        const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
        const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
        const Vec3 p = m_localPlayer.position;
        if (in.moveFwd   && Autoplay::padAhead(m_level.grid, p, fwd))            in.moveFwd   = false;
        if (in.moveBack  && Autoplay::padAhead(m_level.grid, p, fwd   * -1.0f))  in.moveBack  = false;
        if (in.moveRight && Autoplay::padAhead(m_level.grid, p, right))          in.moveRight = false;
        if (in.moveLeft  && Autoplay::padAhead(m_level.grid, p, right * -1.0f))  in.moveLeft  = false;
    }
```

Leave that FOUR_STORY block **exactly as it is** — the Descent floor's descent-by-falling and its pad veto are carefully tuned and must not change. Add a **separate, new VERTICAL_HALL-only block immediately after it**:

```cpp
    // FALL VETO — VERTICAL_HALL with an UPPER exit ONLY. The FIGHT branch's kite/close/strafe
    // movement is otherwise un-hazard-vetoed (short, reactive, enemy-derived — see the veto-scope
    // note in CLAUDE.md), which is exactly how a kiting bot backs off a balcony rim while chasing an
    // enemy across a gap. Scoped tightly on purpose:
    //   * VHALL only — FOUR_STORY descends BY falling through drop holes (handled by the pad block
    //     above and the descent router) and must stay untouched.
    //   * EXIT UPPER only (`floorDoorPos.y > 1.5f`) — when the exit is on the GROUND the bot spawned
    //     on a balcony and must get DOWN, and dropping off the rim is a valid way down; a fall veto
    //     there would hinder the descent. The protection is for the CLIMB, where a fall undoes it.
    //   * `in.engaging` — only FIGHT movement, never a TRAVEL step.
    // Applied per WASD component so the bot slides along the safe axes instead of freezing.
    if (in.engaging && m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL &&
        m_level.floorDoorPos.y > 1.5f) {
        const f32  cy = cosf(m_localPlayer.yaw), sy = sinf(m_localPlayer.yaw);
        const Vec3 fwd{-sy, 0.0f, -cy}, right{cy, 0.0f, -sy};
        const Vec3 p     = m_localPlayer.position;
        const f32  feetY = p.y;
        if (in.moveFwd   && Autoplay::wouldFall(m_level.grid, p, feetY, fwd))            in.moveFwd   = false;
        if (in.moveBack  && Autoplay::wouldFall(m_level.grid, p, feetY, fwd   * -1.0f))  in.moveBack  = false;
        if (in.moveRight && Autoplay::wouldFall(m_level.grid, p, feetY, right))          in.moveRight = false;
        if (in.moveLeft  && Autoplay::wouldFall(m_level.grid, p, feetY, right * -1.0f))  in.moveLeft  = false;
    }
```

- [ ] **Step 7: Build the game and the suite; run the suite**

Run: `cmake --build build -j8 && cmake --build build --target dungeon_tests -j8 && ./build/tests/dungeon_tests`
Expected: PASS, full suite green (existing count + the new cases).

- [ ] **Step 8: Live smoke-check (no fall off the balcony while fighting)**

Run: `timeout 90 ./build/src/DungeonEngine --autoplay --vhall --new marksman 2>&1 | grep -c "FPS"`
Expected: it runs to timeout with FPS lines and no crash. (Manual watch, if a screen is available: the bot no longer strafes/kites off a balcony edge during a fight.)

- [ ] **Step 9: Commit**

```bash
git add src/game/autoplay_intent.h src/game/autoplay_combat.h src/engine/engine_autoplay.cpp tests/game/test_autoplay_brain.cpp
git commit -m "feat(autoplay): never kite off a ledge — fall-veto FIGHT movement on stacked floors"
```

---

## Task 3: `rangedCellFor` — the doctrine column swap

**Files:**
- Modify: `src/game/autoplay_doctrine.h`
- Test: `tests/game/test_autoplay_doctrine.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/game/test_autoplay_doctrine.cpp`:

```cpp
TEST_CASE("rangedCellFor: keeps the row, forces the Ranged column") {
    using namespace Autoplay;
    // cell = row*3 + col. Ranged column is 2.
    CHECK(rangedCellFor(0*3 + 1) == 0*3 + 2);   // Tanky Melee   -> Tanky Ranged
    CHECK(rangedCellFor(1*3 + 1) == 1*3 + 2);   // Moderate Melee-> Moderate Ranged
    CHECK(rangedCellFor(2*3 + 0) == 2*3 + 2);   // Glass Magic   -> Glass Ranged
    CHECK(rangedCellFor(1*3 + 2) == 1*3 + 2);   // already Ranged: unchanged
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target dungeon_tests -j8`
Expected: FAIL to COMPILE — `'rangedCellFor' is not a member of 'Autoplay'`.

- [ ] **Step 3: Write the implementation**

Add to `src/game/autoplay_doctrine.h` (near `defaultCellForClass`; it needs `BuildScore::buildRow`/`BUILD_COLS`, which are already available through `build_score.h` — check the includes at the top of the file and add `#include "game/build_score.h"` if it is not already included):

```cpp
// The same build cell with its column forced to Ranged (2), keeping the risk ROW. Used when a melee
// build equips a temporary ranged sidearm (engine_autoplay.cpp): the doctrine must then be a ranged
// one — hold ground and shoot — not the melee "close the distance" band, or the bot would try to
// walk a gun into melee reach. The row (potion threshold, dodge/block posture) is preserved.
inline u8 rangedCellFor(u8 cell) {
    return static_cast<u8>(BuildScore::buildRow(cell) * BuildScore::BUILD_COLS + 2);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target dungeon_tests -j8 && ./build/tests/dungeon_tests -tc="*rangedCellFor*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/game/autoplay_doctrine.h tests/game/test_autoplay_doctrine.cpp
git commit -m "feat(autoplay): rangedCellFor — melee cell -> ranged doctrine for the sidearm"
```

---

## Task 4: `bestRangedBackpackIdx` — pick the sidearm

**Files:**
- Modify: `src/game/build_score.h`
- Test: `tests/game/test_build_score.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/game/test_build_score.cpp` (it already has `weaponDef`, `armorDef`, `instance`, and the `MOD_*` cell constants):

```cpp
TEST_CASE("BuildScore: bestRangedBackpackIdx finds the best ranged weapon in the bag") {
    // A defs table indexed by ItemInstance.defId. defId 1 = a bow, 2 = a stronger carbine,
    // 3 = a sword (melee — must be ignored), 4 = armor (not a weapon — must be ignored).
    ItemDef defs[5]{};
    defs[1] = weaponDef(WeaponSubtype::BOW,     20.0f);
    defs[2] = weaponDef(WeaponSubtype::CARBINE, 60.0f);   // higher base damage => higher ranged score
    defs[3] = weaponDef(WeaponSubtype::SWORD,   90.0f);
    defs[4] = armorDef(50.0f);

    PlayerInventory inv{};
    auto put = [&](u8 slot, u8 defId){ inv.backpack[slot].defId = defId; inv.backpack[slot].affixCount = 0; };
    put(0, 1); put(1, 3); put(2, 2); put(3, 4);
    inv.backpackCount = 4;

    // For a MELEE cell we still evaluate ranged candidates against the RANGED column (the sidearm's
    // own archetype), so the carbine (slot 2) wins over the bow (slot 0); the sword and armor are 0.
    const s32 idx = BuildScore::bestRangedBackpackIdx(inv, defs, 5);
    CHECK(idx == 2);
}

TEST_CASE("BuildScore: bestRangedBackpackIdx returns -1 when the bag has no ranged weapon") {
    ItemDef defs[3]{};
    defs[1] = weaponDef(WeaponSubtype::SWORD, 50.0f);
    defs[2] = armorDef(20.0f);
    PlayerInventory inv{};
    inv.backpack[0].defId = 1; inv.backpack[1].defId = 2; inv.backpackCount = 2;
    CHECK(BuildScore::bestRangedBackpackIdx(inv, defs, 3) == -1);
}

TEST_CASE("BuildScore: bestRangedBackpackIdx ignores a pet in the bag") {
    // A pet-summon def claims a slot but is never a weapon; it must never be chosen as a sidearm.
    ItemDef defs[3]{};
    defs[1] = weaponDef(WeaponSubtype::BOW, 20.0f);
    defs[2] = weaponDef(WeaponSubtype::CARBINE, 60.0f); defs[2].petSummon = true;  // (contrived) pet flag
    PlayerInventory inv{};
    inv.backpack[0].defId = 1; inv.backpack[1].defId = 2; inv.backpackCount = 2;
    CHECK(BuildScore::bestRangedBackpackIdx(inv, defs, 3) == 0);   // the bow, not the flagged carbine
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target dungeon_tests -j8`
Expected: FAIL to COMPILE — `'bestRangedBackpackIdx' is not a member of 'BuildScore'`.

- [ ] **Step 3: Write the implementation**

Add to `src/game/build_score.h`, after `worthPickingUp`/`isKeeper` (it reuses `score`, `weaponInFamily`, and `MAX_INVENTORY_ITEMS`). The Ranged column is `2`; evaluating each candidate against a fixed Moderate-Ranged cell (`1*BUILD_COLS + 2`) gives a consistent same-archetype ranking (the row only reweights offense vs defense, which does not change WHICH ranged weapon is strongest for a pure-DPS pick).

```cpp
// The backpack slot holding the strongest RANGED-family weapon, or -1 if the bag has none. Used by
// the Autoplay melee sidearm (engine_autoplay.cpp): a melee build keeps ranged weapons in its bag
// because worthPickingUp reasons over all nine build cells, so the sidearm it needs is usually
// already there. Scored against a Moderate-Ranged reference cell — the family gate makes every
// non-ranged weapon (and armor/rings/pets) score 0, so only ranged weapons can win.
inline s32 bestRangedBackpackIdx(const PlayerInventory& inv, const ItemDef* defs, u32 defCount) {
    constexpr u8 kRangedRefCell = 1 * BUILD_COLS + 2;   // Moderate / Ranged
    s32 best = -1;
    f32 bestScore = 0.0f;
    for (u8 i = 0; i < MAX_INVENTORY_ITEMS; i++) {
        const ItemInstance& it = inv.backpack[i];
        if (it.defId == 0xFFFF || it.defId >= defCount) continue;
        const ItemDef& def = defs[it.defId];
        if (def.slot != ItemSlot::WEAPON) continue;
        const f32 s = score(it, def, kRangedRefCell);   // 0 for non-ranged (family gate) and pets
        if (s > bestScore) { bestScore = s; best = (s32)i; }
    }
    return best;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target dungeon_tests -j8 && ./build/tests/dungeon_tests -tc="*bestRangedBackpackIdx*"`
Expected: PASS, 3 test cases.

- [ ] **Step 5: Commit**

```bash
git add src/game/build_score.h tests/game/test_build_score.cpp
git commit -m "feat(autoplay): bestRangedBackpackIdx — pick the melee build's ranged sidearm"
```

---

## Task 5: Sidearm state members + `autoEquipBackpack` guard

**Files:**
- Modify: `src/engine/engine.h`
- Modify: `src/engine/engine_inventory.cpp`

- [ ] **Step 1: Add the sidearm state members**

In `src/engine/engine.h`, near the other `m_autoplay*` members (e.g. beside `m_autoplayVhCrossed`), add:

```cpp
    // MELEE RANGED SIDEARM (Autoplay, stacked floors). A melee build cannot hit an enemy that is only
    // reachable by falling / leaving the story, so it temporarily equips the best ranged weapon from
    // its backpack (BuildScore::bestRangedBackpackIdx) and fires from where it stands, switching back
    // to melee when the situation clears. All transient — no save/PROTOCOL change.
    bool m_autoplaySidearmActive = false;  // a ranged weapon is currently worn IN PLACE of the melee one
    u32  m_autoplaySidearmMeleeUid = 0;    // uid of the stashed melee weapon, to find it for the switch-back
    f32  m_autoplaySidearmDwell = 0.0f;    // seconds the sidearm has been worn (min-hold, anti-chatter)
    f32  m_autoplaySidearmCooldown = 0.0f; // seconds until another switch is allowed (anti-chatter)
```

- [ ] **Step 2: Confirm the item-uid field name**

Run: `grep -n "uid" src/game/item.h | head`
Expected: `ItemInstance` has a `uid` field (a stable per-instance id). If the field is named differently (e.g. `instanceId`), use that name in every `m_autoplaySidearmMeleeUid` reference in Task 6. Record the exact name now.

- [ ] **Step 3: Guard `autoEquipBackpack`**

In `src/engine/engine_inventory.cpp`, find `void Engine::autoEquipBackpack(` and add, as the first statement of the function body:

```cpp
    // Never re-gear while the Autoplay sidearm is worn: the swap deliberately holds a ranged weapon
    // in a melee build's WEAPON slot, and auto-equip (which runs on every pickup + a housekeeping
    // pass) would immediately put the melee weapon back and undo it. The sidearm state machine
    // (engine_autoplay.cpp) owns the weapon slot for the duration.
    if (m_autoplaySidearmActive) return;
```

- [ ] **Step 4: Build to verify it compiles**

Run: `cmake --build build -j8`
Expected: builds clean (members referenced only here so far).

- [ ] **Step 5: Commit**

```bash
git add src/engine/engine.h src/engine/engine_inventory.cpp
git commit -m "feat(autoplay): sidearm state + guard auto-equip from undoing the swap"
```

---

## Task 6: The sidearm state machine (trigger, equip, switch-back)

**Files:**
- Modify: `src/engine/engine_autoplay.cpp`
- Modify: `src/engine/engine.h` (declare the helper)

This task adds one driver helper, `updateSidearm(const Autoplay::BotView& v, f32 dt)`, called once per tick from `updateAutoplay` AFTER this tick's `BotView` is built (so it sees the current targets), and reworks the doctrine/weapon override in `buildBotView`.

- [ ] **Step 1: Declare the helper**

In `src/engine/engine.h`, near `Autoplay::BotView buildBotView();`, add:

```cpp
    // Runs the melee ranged-sidearm state machine (equip a ranged weapon from the bag when a melee
    // build can only reach a target by falling/leaving the story; switch back when it clears).
    void updateSidearm(const Autoplay::BotView& v, f32 dt);
```

- [ ] **Step 2: Implement the trigger + state machine**

Add the definition in `src/engine/engine_autoplay.cpp` (place it just above `Engine::buildBotView`). It uses `Autoplay::wouldFall`, `BuildScore::bestRangedBackpackIdx`, and `Inventory::equip`. Include headers if not already present: `#include "game/build_score.h"` is likely already included transitively via engine.h; add it if the build complains.

```cpp
// One tick of the melee ranged-sidearm decision. Owns the WEAPON slot while active. Requirements:
// a MELEE build, on a STACKED floor, with a hostile it WANTS to hit but cannot reach by walking —
// out of melee reach AND every approach step toward it would fall off a ledge. Then a ranged weapon
// from the bag is the only way to engage it without leaving the story, which is exactly what the
// player asked the bot NOT to do. When the trigger clears (target gone / now meleeable / no longer
// fighting), it switches back. Hysteresis (dwell + cooldown) stops it chattering when a target
// flickers in and out of the condition.
void Engine::updateSidearm(const Autoplay::BotView& v, f32 dt) {
    constexpr f32 kMinDwell  = 3.0f;   // once switched, hold the sidearm at least this long
    constexpr f32 kCooldown  = 5.0f;   // and wait this long between switches
    if (m_autoplaySidearmCooldown > 0.0f) m_autoplaySidearmCooldown -= dt;
    if (m_autoplaySidearmActive)          m_autoplaySidearmDwell    += dt;

    PlayerInventory& inv = m_inventories[0];

    // Is there a target that a melee build could only reach by falling? Scan this tick's LOS targets.
    // "Wants it" = LOS + within the engagement ceiling (the same gate the brain fights on). "Can't
    // reach" = beyond melee swing AND the step straight toward it would fall. Only meaningful on a
    // stacked floor; on flat ground the bot can always just walk up, so the sidearm never triggers.
    // VHALL with an UPPER exit only — the sidearm exists for the balcony climb, where a melee bot
    // meets a cross-gap enemy it must not fall to reach. FOUR_STORY and ground-exit VHALL are excluded
    // (same reasons as the fall veto), so the sidearm never touches them.
    bool trigger = false;
    if (m_level.layoutStyle == LevelGen::LayoutStyle::VERTICAL_HALL && m_level.floorDoorPos.y > 1.5f) {
        const Autoplay::Doctrine doc = Autoplay::doctrineFor(v.buildCell);
        const f32 ceil = Autoplay::engageCeiling(v, doc);
        const Vec3 eye = v.pos;   // XZ only is used below
        for (u32 i = 0; i < v.targetCount; i++) {
            const Autoplay::BotTarget& t = v.targets[i];
            if (!t.hasLOS || t.dist > ceil) continue;
            if (t.dist <= v.weaponRange * 1.2f) continue;         // already in / near melee reach
            const Vec3 toT{t.pos.x - eye.x, 0.0f, t.pos.z - eye.z};
            if (lengthSq(toT) < 1e-6f) continue;
            if (Autoplay::wouldFall(m_level.grid, v.pos, v.pos.y, toT)) { trigger = true; break; }
        }
    }

    if (!m_autoplaySidearmActive) {
        // --- Consider switching TO the sidearm. ---
        if (trigger && m_autoplaySidearmCooldown <= 0.0f) {
            const s32 idx = BuildScore::bestRangedBackpackIdx(inv, m_itemDefs, m_itemDefCount);
            if (idx >= 0) {
                m_autoplaySidearmMeleeUid = inv.equipped[(u32)ItemSlot::WEAPON].uid;  // stash BEFORE equip
                Inventory::equip(inv, (u8)idx, m_itemDefs);   // ranged weapon -> WEAPON slot; melee -> bag
                m_autoplaySidearmActive   = true;
                m_autoplaySidearmDwell    = 0.0f;
                addChatMessage("", "Autoplay: drew a ranged sidearm", Vec3{0.6f, 0.85f, 1.0f});
            }
        }
        return;
    }

    // --- Active: consider switching BACK to melee. ---
    // Keep it while the trigger holds OR the min dwell has not elapsed. Otherwise put the melee
    // weapon back by finding the stashed uid in the bag (its slot can move as loot comes and goes).
    if (trigger || m_autoplaySidearmDwell < kMinDwell) return;

    s32 meleeIdx = -1;
    for (u8 i = 0; i < MAX_INVENTORY_ITEMS; i++)
        if (inv.backpack[i].defId != 0xFFFF && inv.backpack[i].uid == m_autoplaySidearmMeleeUid) { meleeIdx = i; break; }
    if (meleeIdx >= 0) Inventory::equip(inv, (u8)meleeIdx, m_itemDefs);   // melee -> WEAPON slot
    // If the stashed weapon vanished (should not happen — nothing discards the equipped-then-bagged
    // melee weapon while the sidearm guard blocks auto-equip), fall through: clearing the flag lets
    // the next autoEquipBackpack re-gear the melee build normally.
    m_autoplaySidearmActive   = false;
    m_autoplaySidearmCooldown = kCooldown;
    addChatMessage("", "Autoplay: back to melee", Vec3{0.6f, 0.85f, 1.0f});
}
```

- [ ] **Step 3: Call it from `updateAutoplay`**

In `src/engine/engine_autoplay.cpp`, in `updateAutoplay`, after the `BotView v = buildBotView();` line and its immediate consumers but BEFORE the tick returns, add a call. The safest spot is right after `applyBotIntent(...)` for the primary lane, so it acts on the same-tick view. Find the `applyBotIntent(in, uiOpen, dt, v.weaponIsMelee);` call in `updateAutoplay` and add on the next line:

```cpp
    updateSidearm(v, dt);   // equip/stow the melee build's ranged sidearm (takes effect next tick)
```

- [ ] **Step 4: Override the doctrine/weapon view while the sidearm is worn**

While the ranged weapon is equipped, `getEffectiveWeapon` already makes `v.weaponRange`/`weaponProjSpeed`/`weaponIsMelee` ranged (no change needed there). The one thing that would still be wrong is the DOCTRINE: `doctrineFor(v.buildCell)` would give the melee "close the distance" band. In `src/engine/engine_autoplay.cpp`, `buildBotView`, find where `v.buildCell` is set (search `v.buildCell =`). Immediately after it, add:

```cpp
    // While the ranged SIDEARM is worn, present a RANGED doctrine cell to the brain (same risk row,
    // Ranged column) so it holds ground and shoots instead of trying to walk a gun into melee reach.
    // The equipped weapon is already ranged (getEffectiveWeapon), so weaponRange/isMelee are correct;
    // only the doctrine column needs the swap. The PERSISTED buildCell (m_inventories[0].buildCell)
    // is untouched — this is a per-tick view override.
    if (m_autoplaySidearmActive) v.buildCell = Autoplay::rangedCellFor(v.buildCell);
```

- [ ] **Step 5: Reset the sidearm on run entry and (defensively) on death**

The sidearm must never leak across runs. In `src/engine/engine_menu.cpp`, find `enterAutoplayRun` (it resets the other `m_autoplay*` timers — search `m_autoplayFreePlayTimer   = -1.0f;`) and add beside them:

```cpp
    m_autoplaySidearmActive   = false;   // a fresh run never inherits a mid-fight sidearm swap
    m_autoplaySidearmDwell    = 0.0f;
    m_autoplaySidearmCooldown = 0.0f;
```

Note: the persisted melee weapon is restored by the normal equip/auto-equip flow on a new run, so only the flags need clearing here.

- [ ] **Step 6: Build the game and the suite; run the suite**

Run: `cmake --build build -j8 && cmake --build build --target dungeon_tests -j8 && ./build/tests/dungeon_tests`
Expected: full suite green.

- [ ] **Step 7: Live-verify the sidearm on a stacked floor with a melee build**

Run: `timeout 120 ./build/src/DungeonEngine --autoplay --vhall --new warrior 2>&1 | grep -iE "sidearm|back to melee" | head`
Expected: at least one `drew a ranged sidearm` line when the warrior meets a cross-gap enemy on a balcony, followed later by `back to melee` (a warrior that never happens to carry a ranged weapon in its bag prints nothing — that is correct, not a failure; re-run a few times, or use `--fourstory`, to catch a run where the bag holds a ranged weapon). No crash, runs to timeout.

- [ ] **Step 8: Commit**

```bash
git add src/engine/engine.h src/engine/engine_autoplay.cpp src/engine/engine_menu.cpp
git commit -m "feat(autoplay): melee builds draw a ranged sidearm to hit cross-gap enemies, then stow it"
```

---

## Task 7: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `.claude/skills/engine-how-to/SKILL.md`

- [ ] **Step 1: Add an Autoplay note to CLAUDE.md**

In `CLAUDE.md`, in the Autoplay section (near the "veto's scope" / stacked-floor paragraphs), add a paragraph:

```markdown
**Autoplay stays on its story and draws a sidearm (2026-07-25).** Two coordinated rules keep the bot
from wandering off to shoot ranged enemies, and above all from falling, during a VERTICAL_HALL climb.
Both are scoped to **VHALL floors whose exit is UPPER** (`floorDoorPos.y > 1.5f`): FOUR_STORY descends
BY falling and is untouched, and a ground-exit VHALL wants the bot to drop OFF its balcony to descend,
so the fall protection there would only get in the way. (1) **FALL VETO** — the FIGHT branch's
kite/close/strafe movement, which is deliberately un-hazard-vetoed (see the veto-scope paragraph), is
checked against `Autoplay::wouldFall` (`autoplay_nav.h`: the destination cell's `effectiveFloorHeight`
sits more than a step below the feet) per WASD component, so a fight can never carry the bot off the
balcony rim it just climbed to. It is gated on `BotIntent::engaging` (set only by `decideCombat`) so it
only ever touches FIGHT movement, never a TRAVEL step. (2) **MELEE RANGED SIDEARM** — a melee build on
that same climb that meets a hostile it can only reach by falling (out of melee reach AND the step
toward it would fall) equips the best ranged weapon already in its backpack (`BuildScore::bestRangedBackpackIdx`
— a melee build keeps ranged weapons because `worthPickingUp` reasons over all nine build cells) via
`Inventory::equip`, fires from where it stands, and switches back to melee when the trigger clears
(min 3 s dwell, 5 s between switches). While the sidearm is worn: `getEffectiveWeapon` already makes
the weapon view ranged, `buildBotView` overrides the doctrine cell to the Ranged column
(`Autoplay::rangedCellFor`) so the brain holds ground instead of walking a gun into melee, and
`autoEquipBackpack` is hard-suppressed (`m_autoplaySidearmActive`) so a pickup can't re-gear the
melee weapon back. All transient — SP lane 0, no save/PROTOCOL change.
```

- [ ] **Step 2: Add the traps to engine-how-to**

In `.claude/skills/engine-how-to/SKILL.md`, near the other Autoplay pitfalls, add:

```markdown
- **The Autoplay fall veto must be gated on `BotIntent::engaging`, not applied to all movement.** The
  bot descends a FOUR_STORY floor by walking INTO a drop hole — a fall it wants — and that step comes
  from the TRAVEL branch. `Autoplay::wouldFall` is applied in `applyBotIntent` only when
  `in.engaging` (set by `decideCombat`), so it vetoes a kite off a ledge but never the descent. A new
  movement producer that should respect the fall veto must set `engaging`; one that is an intended
  drop must NOT.
- **The Autoplay sidearm owns the WEAPON slot; `autoEquipBackpack` must stay suppressed while it does.**
  `autoEquipBackpack` runs on every pickup and a housekeeping pass and re-gears to the active build
  cell, which would instantly undo a sidearm swap (put the melee weapon back). The guard
  (`if (m_autoplaySidearmActive) return;` at the top of `autoEquipBackpack`) is load-bearing; the
  switch-back path clears the flag before the next auto-equip can run. The stashed melee weapon is
  found again by `ItemInstance::uid` (its backpack slot can move as loot comes and goes), not by a
  remembered index.
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md .claude/skills/engine-how-to/SKILL.md
git commit -m "docs(autoplay): fall veto + melee ranged sidearm"
```

---

## Task 8: Final verification

- [ ] **Step 1: Full suite**

Run: `cmake --build build -j8 && cmake --build build --target dungeon_tests -j8 && ./build/tests/dungeon_tests`
Expected: `Status: SUCCESS!`, zero failures.

- [ ] **Step 2: No debug scaffolding left**

Run: `grep -rn "TEMP-DIAG\|TEMP-TELEMETRY\|DUNGEON_SEED\|VH_EXIT_UP\|LOG_INFO(\"\[" src/game/autoplay_* src/engine/engine_autoplay.cpp`
Expected: no matches (only intended `addChatMessage` lines, which are gameplay, not debug).

- [ ] **Step 3: Flat-floor regression + all stacked layouts run clean**

Run:
```bash
for f in "" "--vhall" "--fourstory"; do
  timeout 60 ./build/src/DungeonEngine --autoplay $f --new warrior 2>&1 | grep -ciE "error|abort|assert";
done
```
Expected: `0` for each (no crashes/asserts). Flat floors must be unchanged — the fall veto and sidearm only fire on stacked floors (`v.stackedFloor` / `stacked`), and the `engaging` flag changes nothing except when the driver reads it.

- [ ] **Step 4: Confirm the fall veto never freezes a descent**

Run: `timeout 120 ./build/src/DungeonEngine --autoplay --fourstory --new marksman 2>&1 | grep -c "Floor 2 exit portal"`
Expected: the run still descends at least one Descent floor within the window on a good seed (the fall veto's `engaging` gate must not block the drop-hole descent). If it never descends across a few runs, the `engaging` gate is misapplied — revisit Task 2 Step 6 / Task 6 Step 3.

---

## Self-Review

**Spec coverage:**
- Requirement 1 (don't wander off the story to chase ranged enemies; especially don't fall): Task 1 (`wouldFall`) + Task 2 (fall veto on FIGHT movement, gated on `engaging`). The bot physically cannot step off its story while fighting, so it can neither wander off nor fall; it shoots from where it stands. ✓
- Requirement 2 (melee switches to ranged in some situations, switches back): Tasks 3–6 (`rangedCellFor`, `bestRangedBackpackIdx`, sidearm state members + auto-equip guard, the state machine with equip/switch-back + doctrine override). ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"/"similar to Task N". Every code step shows complete code. The one runtime lookup left to the implementer — the exact `ItemInstance` uid field name — is an explicit verification step (Task 5 Step 2) with a fallback instruction, not a placeholder. ✓

**Type consistency:** `wouldFall(grid, from, feetY, dir)` signature matches its call sites (Task 2 Step 6, Task 6 Step 2). `BotIntent::engaging` is defined in Task 2 Step 1 and read in Task 2 Step 6. `rangedCellFor`/`bestRangedBackpackIdx` signatures match their call sites in Task 6. `m_autoplaySidearm*` members are declared in Task 5 and used in Task 6. `ItemSlot::WEAPON`, `Inventory::equip`, `getEffectiveWeapon`, `buildRow`/`BUILD_COLS` are all pre-existing and referenced with their real names. ✓

**Assumptions stated for the reviewer:**
1. **Sidearm is opportunistic, not reserved.** The bot uses a ranged weapon only if one is already in its bag (kept by the all-nine-cells `worthPickingUp` rule). It does not force the loot system to reserve one. If a melee build's bag never holds a ranged weapon, the sidearm simply never triggers and the fall veto alone keeps it safe (it holds position; the existing combat break-off watchdog relocates it if it makes no progress). Reserving a sidearm would be a follow-up touching `isKeeper`.
2. **The switch mechanism is a real `Inventory::equip` swap** (safe because Autoplay is SP lane 0 — no `sendInventorySync`/net concerns), with `autoEquipBackpack` suppressed for the duration. The alternative (a separate "effective combat weapon" that bypasses the slot) was rejected: firing reads the equipped WEAPON slot, so the weapon must actually be equipped.
3. **The doctrine override is a per-tick view change** (`v.buildCell → rangedCellFor`), leaving the persisted build cell untouched, so the player's chosen build is never silently re-geared.
