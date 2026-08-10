# Quest Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the ten-entry static quest table and its fading chat line with a real quest engine — multi-stage objectives, a Journal panel inside the inventory, and NPC quest givers in the act hubs.

**Architecture:** A pure, engine-free state machine (`src/game/quest_state.h`) owns all quest progress and is unit-tested with no GL or engine context, following the `zone_route.h` / `free_play.h` pattern already in the tree. Every existing consumer — `ZoneRoute`, the gate refusals, the autoplay act branch — keeps taking a plain `u64`, which is now **derived** from that state rather than stored beside it. The UI is a new panel in the inventory's existing shoulder cycle, with its layout single-sourced so draw and hit-test cannot drift.

**Tech Stack:** C++17, header-only pure logic, doctest (`external/doctest/doctest.h`), CMake, Python asset generators (`tools/gen_mesh.py`, `tools/gen_skin.py`).

**Spec:** `docs/superpowers/specs/2026-08-09-quest-engine-design.md`

---

## Conventions for every task

- **Build:** `cmake --build build`
- **The binary is `build/src/DungeonEngine`** (not `build/dungeon_game`).
- **The live save directory is `/home/aaron/snap/code/254/.local/share/EdRethardo/DungeonEngine/`.**
  The game runs under the VS Code snap, so `SDL_GetPrefPath` resolves inside the snap's confinement
  rather than to `~/.local/share/...`. The `save_NN.dat` files in the REPO ROOT are stale pre-migration
  leftovers (all v2/v3) and are not what the game reads — testing a save path against them silently
  measures nothing. Confirm the path from a run's own log line: `Saved character (lane 0) to slot N (<path>)`.
- **Back up that directory before any run that loads a slot.** `--endgame` re-gears the character it
  loads and the bot then plays and saves, so a "read-only" verification run is not read-only.
- A live run needs a display: prefix `DISPLAY=:1 __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia`
  and always wrap in `timeout`. Never kill another process on this machine.
- **Test:** `./build/tests/dungeon_tests -tc="<name>"`, full suite `./build/tests/dungeon_tests`
- **Do not trust a `-tc` filter to run what you think it runs.** doctest matches on the CASE NAME, and
  several cases in this work are named for their behaviour rather than for "quest" — `-tc="*quest*"`
  silently skips the v6 migration case, which is the single most load-bearing test in the plan. The
  filters below are widened accordingly; when adding a case, either name it so an existing filter
  catches it or widen the filter in the same commit. **Always finish a task on the FULL suite.**
- **A new test file must be added to `tests/CMakeLists.txt`'s `add_executable(dungeon_tests ...)` list**, along with any production `.cpp` it links. Header-only logic needs no `.cpp` entry (see the `# header-only` comments in that list).
- **Never insert into an existing enum or array whose ordinal is serialized** — append only. This applies to `Quest::QUESTS[]` (table position is the save bit), `GameAction`, `SkillId`, and `items.json`.
- Inline-comment the *why* of non-obvious logic only. Don't restate what the names already say.
- **Do not commit unless the plan step says to.** The user has a standing rule against unprompted
  commits; each task below ends with an explicit commit step, which is that prompt. **Commit only —
  never `--amend`, never rebase, never `git add -A`.** Amending is not authorized even to tidy your
  own previous commit: a review fix is a NEW commit, so the history shows what was found and what it
  cost. The working tree carries unrelated uncommitted work — stage only the files the step lists.
- **The `obj` canary in the migration test discriminates only while `MAX_QUESTS < 64`.** Its whole
  mechanism is that bits 32-63 overflow `state[]` into `obj[]`. Raising `MAX_QUESTS` to exactly 64
  closes that window and the test silently stops guarding — still correct, no longer a guard.

---

## File Structure

**New files**

| File | Responsibility |
|---|---|
| `src/game/quest_state.h` | The whole state machine. `Progress`, `State`, all mutators, `completionMask`, `migrateFromMask`. Pure — no engine types, no GL. |
| `src/renderer/hud_journal.cpp` | Draws the Journal panel. Word-wrapping helper lives here. |
| `tests/game/test_quest_state.cpp` | Unit tests for the state machine, the derived mask, the v6 migration, and the never-strand invariant. |

**Modified files**

| File | Change |
|---|---|
| `src/game/quest_def.h` | `ObjectiveDef`, `GiverDef`, `narration`, `MAX_QUESTS`/`MAX_OBJ`; `bitFor` → `indexForZone`. |
| `src/game/inventory_ui.h` / `.cpp` | `JournalRects`, `journalLayout()`, `hitTestJournal()`, two new `SlotHit::Panel` values. |
| `src/renderer/hud.h` | Declares `drawJournalPanel`. |
| `src/engine/engine.h` | Panel constants, `m_questProgress`, `m_invCursorQuest`, `m_invJournalAct`, `SavedChar` tail, `InteractState::npcIdx`. |
| `src/engine/engine_inventory.cpp` | Journal panel navigation + mouse. |
| `src/engine/engine_hud.cpp` | Calls `drawJournalPanel`. |
| `src/engine/engine_zone.cpp` | The three quest hooks route through `quest_state`. |
| `src/engine/engine_persist.cpp` | `SAVE_VERSION` 7 tail + migration. |
| `src/engine/engine.cpp` | `addChatMessage` repair. |
| `src/engine/engine_town.cpp` | Named givers at plaza posts. |
| `src/engine/engine_update.cpp` | `NPC` interact target + talk handling. |
| `src/game/interact.h` | `Target::NPC`. |
| `src/game/item.h`, `src/game/world_item.cpp` | `CAIRN_STONE_ID`. |
| `src/engine/asset_manifest.h`, `tools/build_assets.py` | The `cairn_stone` mesh — **both**, or the asset build hard-fails. |
| `tests/CMakeLists.txt` | The new test file. |

---

# PHASE 1 — Data model, state machine, persistence

Nothing is visible to the player after this phase. The point is that the derived mask keeps every existing consumer working byte-identically, so the acts play exactly as they do today.

## Task 1: Quest state container and the derived mask

**Files:**
- Create: `src/game/quest_state.h`
- Create: `tests/game/test_quest_state.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/game/test_quest_state.cpp`:

```cpp
// test_quest_state.cpp — the pure quest state machine.
//
// Every test here builds a Quest::Progress by hand. Nothing in this file may touch the engine,
// GL, or a live level: the whole point of quest_state.h being header-only and engine-free is
// that the act's rules are testable without booting anything.
#include "../../external/doctest/doctest.h"
#include "game/quest_state.h"

TEST_CASE("quest progress starts empty and reports nothing complete") {
    Quest::Progress p{};
    for (u32 i = 0; i < Quest::MAX_QUESTS; i++)
        REQUIRE(p.state[i] == static_cast<u8>(Quest::State::LOCKED));
    REQUIRE(Quest::completionMask(p) == 0ull);
}

TEST_CASE("completionMask sets exactly the bits of COMPLETE quests") {
    Quest::Progress p{};
    p.state[0] = static_cast<u8>(Quest::State::COMPLETE);
    p.state[3] = static_cast<u8>(Quest::State::COMPLETE);
    p.state[1] = static_cast<u8>(Quest::State::ACTIVE);    // must NOT appear in the mask
    p.state[2] = static_cast<u8>(Quest::State::OFFERED);   // must NOT appear in the mask

    const u64 m = Quest::completionMask(p);
    REQUIRE((m & (1ull << 0)) != 0);
    REQUIRE((m & (1ull << 3)) != 0);
    REQUIRE((m & (1ull << 1)) == 0);
    REQUIRE((m & (1ull << 2)) == 0);
}

// THE migration test. A v6 save carries a u64 questMask with real completions; loading one and
// reading the new v7 tail as zeros would silently un-complete both acts for every existing hero.
TEST_CASE("v6 mask migrates to COMPLETE states, bit for bit") {
    const u64 legacy = (1ull << 0) | (1ull << 2) | (1ull << 9);
    Quest::Progress p{};
    Quest::migrateFromMask(p, legacy);

    for (u32 i = 0; i < Quest::MAX_QUESTS; i++) {
        const bool wasDone = (legacy & (1ull << i)) != 0;
        REQUIRE(p.state[i] == static_cast<u8>(wasDone ? Quest::State::COMPLETE
                                                      : Quest::State::LOCKED));
    }
    // Round trip: the derived mask must reproduce the file it came from.
    REQUIRE(Quest::completionMask(p) == legacy);
}

TEST_CASE("migration ignores bits above the quest table") {
    Quest::Progress p{};
    Quest::migrateFromMask(p, ~0ull);           // every bit set, including 32..63
    REQUIRE(Quest::completionMask(p) == ((1ull << Quest::MAX_QUESTS) - 1ull));
}
```

- [ ] **Step 2: Add the test file to the build**

In `tests/CMakeLists.txt`, add this line to the `add_executable(dungeon_tests ...)` source list, directly after the `game/test_zone_route.cpp` line:

```cmake
    game/test_quest_state.cpp        # quest state machine: objectives, derived mask, v6 migration (header-only)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build build --target dungeon_tests`
Expected: FAIL to compile — `fatal error: game/quest_state.h: No such file or directory`

- [ ] **Step 4: Write the state container**

Create `src/game/quest_state.h`:

```cpp
// quest_state.h — every mutable fact about a character's quest progress, and the rules that move it.
//
// Header-only and engine-free (the zone_route.h / free_play.h pattern) so the act's rules unit-test
// with no GL and no live level. `Progress` is exactly what the save file holds; nothing else in the
// game stores quest progress.
//
// THE DERIVED MASK is the compatibility hinge. ZoneRoute, the gate refusals and the whole autoplay
// act branch consume a plain u64 of completed quests. They keep doing so — completionMask() builds
// it from `state` on demand, so the mask is a CACHE with one home rather than a second copy of a
// fact that can drift from the first. That drift is the single most common bug shape in this
// codebase's history; see CLAUDE.md.
#pragma once

#include "core/types.h"
#include "game/quest_def.h"

namespace Quest {

// How far along one quest is.
//   LOCKED   — the character has not met it yet (not entered its zone, not spoken to its giver)
//   OFFERED  — known, no objective progress yet
//   ACTIVE   — at least one objective advanced
//   COMPLETE — every objective satisfied
enum struct State : u8 { LOCKED, OFFERED, ACTIVE, COMPLETE, COUNT };

// Per character. This IS the save payload (see engine_persist.cpp, SAVE_VERSION 7).
//
// `obj` holds STORED progress only. A CLEAR_ZONE objective deliberately has no stored counterpart:
// "how many hostiles are left" is a property of the entity pool, which is already polled every
// frame, and keeping a copy here would be a second home for a fact that already has one.
struct Progress {
    u8 state[MAX_QUESTS] = {};
    u8 obj[MAX_QUESTS][MAX_OBJ] = {};
};

// Bit i set iff quest i is COMPLETE. The u64 every existing consumer already takes.
inline u64 completionMask(const Progress& p) {
    u64 m = 0;
    for (u32 i = 0; i < MAX_QUESTS && i < 64; i++)
        if (p.state[i] == static_cast<u8>(State::COMPLETE)) m |= (1ull << i);
    return m;
}

// v6 -> v7. A pre-v7 save stores completions as one u64 bit per quest and nothing else, so every
// set bit becomes a COMPLETE quest with no objective detail. Objectives of an already-finished
// quest are never read, so leaving `obj` zeroed loses nothing a player can see.
//
// Called on exactly one path (the legacy load). Getting it wrong un-completes both acts for every
// hero who already played them, and the next autosave writes that loss back permanently.
inline void migrateFromMask(Progress& p, u64 legacyMask) {
    for (u32 i = 0; i < MAX_QUESTS && i < 64; i++)
        if (legacyMask & (1ull << i)) p.state[i] = static_cast<u8>(State::COMPLETE);
}

} // namespace Quest
```

- [ ] **Step 5: Add the capacity constants the header depends on**

`src/game/quest_def.h` does not yet define `MAX_QUESTS` or `MAX_OBJ`. Add them inside `namespace Quest {`, immediately after the opening brace (before the `Trigger` enum):

```cpp
// Capacity, sized for growth rather than for today. `Progress` is serialized at these sizes, so
// enlarging them later costs a SAVE_VERSION bump and another pair of legacy readers — do the
// widening once, now, while v7 is unreleased and it is free. Ten quests are authored today.
inline constexpr u32 MAX_QUESTS = 32;   // per-character state arrays are sized to this
inline constexpr u32 MAX_OBJ    = 4;    // objectives per quest
```

And at the very end of the file, immediately before the closing `} // namespace Quest`, add the guard that pins the table against the array:

```cpp
// The table must fit the state arrays it is indexed against. Appending a 33rd quest without
// raising MAX_QUESTS would write past `Progress::state` — caught here at compile time instead.
static_assert(COUNT <= MAX_QUESTS, "QUESTS[] outgrew Progress::state — raise MAX_QUESTS (save bump)");
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*quest*,*migrat*,*giver*,*TALK*,*ACTIVATE*,*objective*"`
Expected: PASS — 4 test cases, 0 failures.

- [ ] **Step 7: Commit**

```bash
git add src/game/quest_state.h src/game/quest_def.h tests/game/test_quest_state.cpp tests/CMakeLists.txt
git commit -m "feat(quest): per-character quest state container + derived completion mask

The u64 completion mask becomes DERIVED from a per-quest state byte rather than
stored beside it, so ZoneRoute, the gate refusals and the autoplay act branch keep
consuming exactly what they consume today while the underlying state grows room for
objectives. migrateFromMask is the v6 -> v7 path: read the new tail as zeros instead
and every existing hero silently loses both acts.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 2: Objectives on the quest table

**Files:**
- Modify: `src/game/quest_def.h`
- Modify: `tests/game/test_quest_state.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/game/test_quest_state.cpp`:

```cpp
// Every quest must carry at least one objective, or the Journal has an empty body and the quest
// can never complete. A data lint, not a logic test — the kind that catches an authoring slip.
TEST_CASE("every authored quest has at least one objective and a narration") {
    for (u32 i = 0; i < Quest::COUNT; i++) {
        const Quest::QuestDef& q = Quest::QUESTS[i];
        CAPTURE(q.name);
        REQUIRE(q.objectiveCount >= 1);
        REQUIRE(q.objectiveCount <= Quest::MAX_OBJ);
        REQUIRE(q.narration != nullptr);
        REQUIRE(q.narration[0] != '\0');
        REQUIRE(q.giverIdx < Quest::GIVER_COUNT);
    }
}

// Every quest keeps a TALK objective, and it is always objective 0 — the Journal draws them in
// order and "speak to the giver" is the first beat of every D2 quest.
TEST_CASE("every quest opens with a TALK objective") {
    for (u32 i = 0; i < Quest::COUNT; i++) {
        CAPTURE(Quest::QUESTS[i].name);
        REQUIRE(Quest::QUESTS[i].objectives[0].trigger == Quest::Trigger::TALK);
    }
}

// A SLAY objective's target must be a non-empty name, or the kill hook can never match it.
TEST_CASE("SLAY objectives name a target") {
    for (u32 i = 0; i < Quest::COUNT; i++) {
        for (u32 o = 0; o < Quest::QUESTS[i].objectiveCount; o++) {
            const Quest::ObjectiveDef& od = Quest::QUESTS[i].objectives[o];
            if (od.trigger != Quest::Trigger::SLAY) continue;
            CAPTURE(Quest::QUESTS[i].name);
            REQUIRE(od.target != nullptr);
            REQUIRE(od.target[0] != '\0');
        }
    }
}

// Each giver must actually have quests, or an NPC stands in the hub with nothing to say.
TEST_CASE("every giver hands out at least one quest") {
    for (u32 g = 0; g < Quest::GIVER_COUNT; g++) {
        bool found = false;
        for (u32 i = 0; i < Quest::COUNT && !found; i++)
            if (Quest::QUESTS[i].giverIdx == g) found = true;
        CAPTURE(Quest::GIVERS[g].name);
        REQUIRE(found);
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target dungeon_tests`
Expected: FAIL to compile — `'ObjectiveDef' is not a member of 'Quest'`, `'GIVER_COUNT' was not declared`.

- [ ] **Step 3: Extend the quest table structures**

In `src/game/quest_def.h`, replace the `Trigger` enum and the `QuestDef` struct with:

```cpp
// How one OBJECTIVE is satisfied. These are exactly what the engine can already observe without
// new bookkeeping, plus ACTIVATE for the Cairn Stones.
enum struct Trigger : u8 {
    CLEAR_ZONE,   // every hostile in the quest's zone is dead  (live count, never stored)
    SLAY,         // the named enemy died                        (boolean)
    REACH,        // the quest's zone was entered                (boolean)
    TALK,         // the giver was spoken to                     (boolean, NEVER a prerequisite)
    ACTIVATE,     // N world fixtures interacted with            (bitmask, stored)
    COUNT
};

struct ObjectiveDef {
    Trigger     trigger;
    const char* text;      // journal row label: "Hostiles remaining", "Stones aligned"
    const char* target;    // enemy name for SLAY, fixture tag for ACTIVATE, "" otherwise
    u8          required;  // 1 for a boolean step; 5 for the Cairn Stones
};

// A quest giver: a named NPC standing in an act hub. GiverDef exists rather than deriving the
// giver straight from the act so an act can field two or three the way D2's Rogue Encampment
// fields Akara, Kashya and Charsi — one authored byte, no duplicated fact (the ACT is still
// derived, from the giver's own hubFloor).
struct GiverDef {
    u8          hubFloor;  // 98 = the town (Act 1), 61 = Null Terminus (Act 2)
    const char* name;      // interact prompt, nameplate, journal attribution
    const char* greeting;  // the one short line spoken to chat on talk
};

inline constexpr GiverDef GIVERS[] = {
    { 98, "Akara, the Allocator",   "You came back. Good. Something here still will not free." },
    { 98, "Charsi, the Forgemaid",  "Steel I can fix. What is out there, I cannot." },
    { 61, "The Signalman",          "Mind the gap. Mind everything, really." },
    { 61, "Kashya of the Platform", "We hold this platform. Nothing else down here is held." },
};
inline constexpr u32 GIVER_COUNT = sizeof(GIVERS) / sizeof(GIVERS[0]);

struct QuestDef {
    u8           zoneFloor;
    const char*  name;
    const char*  blurb;         // the one-line offer; still goes to chat
    const char*  narration;     // the journal body. No length limit — the Journal wraps it.
    u8           giverIdx;      // index into GIVERS[]
    u8           objectiveCount;
    ObjectiveDef objectives[MAX_OBJ];
};
```

- [ ] **Step 4: Re-author the ten quests with objectives**

Replace the whole `QUESTS[]` initializer in `src/game/quest_def.h` with the following. **Order is unchanged** — table position is the save bit, so these rows are append-only and must not be resorted.

```cpp
inline constexpr QuestDef QUESTS[] = {
    { 53, "Free the Allocation",
          "Something is still holding the Den. Clear it.",
          "The Den was freed once and never released. Whatever holds it now has held it since "
          "before anyone here kept records. Go down, and let it go.",
          /*giver*/ 0, /*objCount*/ 2,
          { { Trigger::TALK,       "Speak to Akara",        "", 1 },
            { Trigger::CLEAR_ZONE, "Hostiles remaining",    "", 1 } } },

    { 55, "The Rebaser",
          "The graveyard keeps bringing its history back. Stop whatever is rewriting it.",
          "The graves do not stay written. Every night the history is replayed onto them and "
          "whatever was buried comes back with it. Find what is doing the rewriting, and stop it.",
          /*giver*/ 0, /*objCount*/ 2,
          { { Trigger::TALK, "Speak to Akara",          "",                      1 },
            { Trigger::SLAY, "Slay The Garbage Collector", "The Garbage Collector", 1 } } },

    // D2's Cairn Stones beat. This row keeps CLEAR_ZONE for now; Task 14 flips it to ACTIVATE at
    // the moment the five stone fixtures actually exist.
    //
    // The ORDER matters and is not fussiness: ZoneRoute::linkOpen gates the onward road on
    // zoneSettled(56), so a quest 56 whose trigger has no implementation SEALS the way to TristRAM
    // and strands anyone playing an intermediate commit. Author a trigger and the thing that
    // satisfies it in the SAME commit, always.
    { 56, "Align the Standing Stones",
          "Monuments to abandoned features, and none of them agree. Clear the field and they will.",
          "Five stones, each raised for something that was going to be finished. They disagree "
          "about what the field was for, and while they disagree the way to TristRAM stays shut. "
          "Settle the field and they will settle with it.",
          /*giver*/ 1, /*objCount*/ 2,
          { { Trigger::TALK,       "Speak to Charsi",    "", 1 },
            { Trigger::CLEAR_ZONE, "Hostiles remaining", "", 1 } } },

    { 57, "The Search for Deckard Cache",
          "The village was restored from backup once too often. Something in the forge came back wrong.",
          "TristRAM has been restored from backup more times than anyone kept count of. Each "
          "restore came back a little further from the village that was saved. The smith came "
          "back worst of all.",
          /*giver*/ 1, /*objCount*/ 2,
          { { Trigger::TALK, "Speak to Charsi",                    "",                              1 },
            { Trigger::SLAY, "Slay Griswald, the Unfinished Build", "Griswald, the Unfinished Build", 1 } } },

    { 59, "Terminal Access",
          "The road ends at a boarded station. Find the way down.",
          "The road out of the fields ends at a station nobody has boarded a train from in a very "
          "long time. It is boarded, not locked. There is a difference, and it matters.",
          /*giver*/ 1, /*objCount*/ 2,
          { { Trigger::TALK,  "Speak to Charsi",              "", 1 },
            { Trigger::REACH, "Reach Whitechapel Terminal",   "", 1 } } },

    // --- ACT 2: "Hellgate: Localhost" ---
    { 61, "Signal Restored",
          "Somebody down here still has the lights on. Find them.",
          "London fell and the survivors went underground. One platform still has power, which "
          "means somebody down there is still running it. Find them before whatever else is in "
          "the tunnels does.",
          /*giver*/ 2, /*objCount*/ 2,
          { { Trigger::TALK,  "Speak to the Signalman", "", 1 },
            { Trigger::REACH, "Reach Null Terminus",    "", 1 } } },

    { 62, "Break the Loop",
          "The Circle Line is running, and it is not carrying passengers. Clear it.",
          "The Circle Line never stopped running. It has no passengers, no drivers and no "
          "timetable, and it has been going round since the gate opened. Break it.",
          /*giver*/ 2, /*objCount*/ 2,
          { { Trigger::TALK,       "Speak to the Signalman", "", 1 },
            { Trigger::CLEAR_ZONE, "Hostiles remaining",     "", 1 } } },

    { 64, "Insufficient Funds",
          "Something has been drawing on Bank for a long time. Settle it.",
          "Something has been drawing on Bank Station since before the gate, and the balance has "
          "never once been questioned. Go and question it.",
          /*giver*/ 3, /*objCount*/ 2,
          { { Trigger::TALK, "Speak to Kashya",              "",                       1 },
            { Trigger::SLAY, "Slay The Perpetual Commuter",  "The Perpetual Commuter", 1 } } },

    { 65, "Privilege Escalation",
          "The gate will not open while the circus is this crowded. Make room, then force it.",
          "The rift at Piccadilly is held shut by everything crowded around it. Clear the circus "
          "and it can be forced. It should not be possible to force it. It is.",
          /*giver*/ 3, /*objCount*/ 2,
          { { Trigger::TALK,       "Speak to Kashya",    "", 1 },
            { Trigger::CLEAR_ZONE, "Hostiles remaining", "", 1 } } },

    { 66, "Kill -9",
          "The gate is running on this machine. Terminate it.",
          "The gate is not a door. It is a process, and it is running on this machine. It will "
          "not close politely. Terminate it.",
          /*giver*/ 3, /*objCount*/ 2,
          { { Trigger::TALK, "Speak to Kashya",       "",               1 },
            { Trigger::SLAY, "Slay Signal Failure",   "Signal Failure", 1 } } },
};
```

- [ ] **Step 5: Rename `bitFor` to `indexForZone`**

`bitFor` now returns a quest INDEX used for both the state array and the derived mask bit, so the old name misdescribes it. In `src/game/quest_def.h` rename the function and update its comment:

```cpp
// The quest's row in QUESTS[], which is ALSO its slot in Progress::state and its bit in the derived
// mask. That makes QUESTS[] effectively append-only: reordering it silently reassigns every saved
// hero's progress. 0xFF = this zone hosts no quest.
inline u8 indexForZone(u8 zoneFloor) {
    for (u32 i = 0; i < COUNT; i++)
        if (QUESTS[i].zoneFloor == zoneFloor) return static_cast<u8>(i);
    return 0xFF;
}
```

Then update the two callers inside the same file — `isComplete` uses it:

```cpp
inline bool isComplete(u64 mask, u8 zoneFloor) {
    const u8 b = indexForZone(zoneFloor);
    return b != 0xFF && (mask & (1ull << b)) != 0;
}
```

- [ ] **Step 6: Fix the remaining `bitFor` call site**

Run: `grep -rn "bitFor" src/`
Expected: one hit, `src/engine/engine_zone.cpp` inside `Engine::questComplete`. Change `Quest::bitFor(zoneFloor)` to `Quest::indexForZone(zoneFloor)`.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*quest*,*migrat*,*giver*,*TALK*,*ACTIVATE*,*objective*"`
Expected: PASS — 8 test cases, 0 failures.

- [ ] **Step 8: Verify nothing else regressed**

Run: `cmake --build build && ./build/tests/dungeon_tests`
Expected: the full suite passes. `test_zone_route.cpp` in particular still passes — it consumes the `u64` mask, which is unchanged in shape.

- [ ] **Step 9: Commit**

```bash
git add src/game/quest_def.h src/engine/engine_zone.cpp tests/game/test_quest_state.cpp
git commit -m "feat(quest): objectives, narration and named givers on the quest table

Each quest becomes a TALK step plus its existing trigger, and gains an unbounded
narration string for the Journal (the chat line truncates at 48 bytes, which was
cutting nine of the ten blurbs mid-sentence). GIVERS[] lets one act field several
NPCs the way D2's Rogue Encampment does, while the ACT stays derived from the
giver's hub floor. bitFor -> indexForZone: it now indexes state, not just a bit.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 3: Objective evaluation — the mutators

**Files:**
- Modify: `src/game/quest_state.h`
- Modify: `tests/game/test_quest_state.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/game/test_quest_state.cpp`:

```cpp
// Quest 0 (zone 53, Free the Allocation) is TALK + CLEAR_ZONE. Quest 1 (zone 55) is TALK + SLAY.
TEST_CASE("offer moves a locked quest to OFFERED and is idempotent") {
    Quest::Progress p{};
    Quest::offer(p, 0);
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::OFFERED));
    Quest::offer(p, 0);
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::OFFERED));
}

TEST_CASE("offer never demotes a quest that is already further along") {
    Quest::Progress p{};
    p.state[0] = static_cast<u8>(Quest::State::COMPLETE);
    Quest::offer(p, 0);
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::COMPLETE));
}

TEST_CASE("talking ticks the TALK objective and advances OFFERED to ACTIVE") {
    Quest::Progress p{};
    Quest::offer(p, 0);
    Quest::noteTalk(p, 0);
    REQUIRE(p.obj[0][0] == 1);
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::ACTIVE));
}

TEST_CASE("clearing the zone completes a TALK+CLEAR_ZONE quest") {
    Quest::Progress p{};
    Quest::offer(p, 0);
    Quest::noteTalk(p, 0);
    Quest::noteCleared(p, 0);
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::COMPLETE));
    REQUIRE((Quest::completionMask(p) & 1ull) != 0);
}

// THE non-blocking rule, stated as a test. A quest whose deed is done in the field completes even
// though its TALK objective was never ticked. Sabotage check: making TALK a prerequisite fails
// this by name.
TEST_CASE("TALK is never a prerequisite - field completion works unspoken") {
    Quest::Progress p{};
    Quest::offer(p, 0);
    Quest::noteCleared(p, 0);              // never talked to anyone
    REQUIRE(p.obj[0][0] == 0);             // TALK still unticked
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::COMPLETE));
}

TEST_CASE("SLAY matches only its named target") {
    Quest::Progress p{};
    Quest::offer(p, 1);
    Quest::noteKill(p, 1, "Bit Rat");
    REQUIRE(p.state[1] != static_cast<u8>(Quest::State::COMPLETE));
    Quest::noteKill(p, 1, "The Garbage Collector");
    REQUIRE(p.state[1] == static_cast<u8>(Quest::State::COMPLETE));
}

TEST_CASE("SLAY tolerates a null enemy name") {
    Quest::Progress p{};
    Quest::offer(p, 1);
    Quest::noteKill(p, 1, nullptr);
    REQUIRE(p.state[1] != static_cast<u8>(Quest::State::COMPLETE));
}

// ACTIVATE stores WHICH fixtures were used, not how many. A zone is rebuilt from its seed on every
// entry, so a bare count could not tell which stones were already lit and re-entry would re-light
// the wrong ones.
TEST_CASE("ACTIVATE stores a bitmask and reports popcount progress") {
    Quest::Progress p{};
    Quest::offer(p, 2);                        // quest 2 = Align the Standing Stones, 5 stones
    Quest::noteActivate(p, 2, 0);
    Quest::noteActivate(p, 2, 3);
    Quest::noteActivate(p, 2, 3);              // repeat must not double-count
    REQUIRE(Quest::objectiveProgress(p, 2, 1) == 2);
    REQUIRE(p.state[2] != static_cast<u8>(Quest::State::COMPLETE));

    Quest::noteActivate(p, 2, 1);
    Quest::noteActivate(p, 2, 2);
    Quest::noteActivate(p, 2, 4);
    REQUIRE(Quest::objectiveProgress(p, 2, 1) == 5);
    REQUIRE(p.state[2] == static_cast<u8>(Quest::State::COMPLETE));
}

TEST_CASE("out-of-range quest and fixture indices are ignored, not written") {
    Quest::Progress p{};
    Quest::offer(p, 200);
    Quest::noteTalk(p, 200);
    Quest::noteActivate(p, 2, 99);
    REQUIRE(Quest::completionMask(p) == 0ull);
    REQUIRE(Quest::objectiveProgress(p, 2, 1) == 0);
}

// The never-strand invariant, walked over the whole table: doing every quest's deed in authored
// order, with nobody ever spoken to, must finish both acts. If any quest could not complete this
// way, a player who never found its giver would be permanently blocked on the road.
TEST_CASE("every quest completes without ever talking to a giver") {
    Quest::Progress p{};
    for (u32 i = 0; i < Quest::COUNT; i++) {
        Quest::offer(p, static_cast<u8>(i));
        const Quest::QuestDef& q = Quest::QUESTS[i];
        for (u32 o = 0; o < q.objectiveCount; o++) {
            switch (q.objectives[o].trigger) {
                case Quest::Trigger::CLEAR_ZONE: Quest::noteCleared(p, static_cast<u8>(i)); break;
                case Quest::Trigger::REACH:      Quest::noteReached(p, static_cast<u8>(i)); break;
                case Quest::Trigger::SLAY:
                    Quest::noteKill(p, static_cast<u8>(i), q.objectives[o].target); break;
                case Quest::Trigger::ACTIVATE:
                    for (u8 f = 0; f < q.objectives[o].required; f++)
                        Quest::noteActivate(p, static_cast<u8>(i), f);
                    break;
                case Quest::Trigger::TALK:  break;   // deliberately never fired
                default: break;
            }
        }
        CAPTURE(q.name);
        REQUIRE(p.state[i] == static_cast<u8>(Quest::State::COMPLETE));
    }
    REQUIRE(Quest::actComplete(Quest::completionMask(p), 1));
    REQUIRE(Quest::actComplete(Quest::completionMask(p), 2));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target dungeon_tests`
Expected: FAIL to compile — `'offer' is not a member of 'Quest'`.

- [ ] **Step 3: Implement the mutators**

Append to `src/game/quest_state.h`, inside `namespace Quest`, before the closing brace:

```cpp
// ---- Reading progress -------------------------------------------------------------------------

// Stored progress for one objective.
//
// A CLEAR_ZONE objective returns 0 here by design: it has no stored counterpart, and the Journal
// supplies the live remaining-hostile count itself. An ACTIVATE objective returns the POPCOUNT of
// its bitmask, since the byte records which fixtures were used rather than how many.
inline u8 objectiveProgress(const Progress& p, u8 questIdx, u8 objIdx) {
    if (questIdx >= COUNT || objIdx >= QUESTS[questIdx].objectiveCount) return 0;
    const u8 raw = p.obj[questIdx][objIdx];
    if (QUESTS[questIdx].objectives[objIdx].trigger != Trigger::ACTIVATE) return raw;
    u8 n = 0;
    for (u8 b = 0; b < 8; b++) if (raw & (1u << b)) n++;
    return n;
}

inline bool objectiveDone(const Progress& p, u8 questIdx, u8 objIdx) {
    if (questIdx >= COUNT || objIdx >= QUESTS[questIdx].objectiveCount) return false;
    return objectiveProgress(p, questIdx, objIdx) >= QUESTS[questIdx].objectives[objIdx].required;
}

// ---- Advancing --------------------------------------------------------------------------------

// Re-derive a quest's state from its objectives. Called after every mutation so `state` can never
// disagree with `obj` — the same single-source discipline the derived mask follows.
//
// TALK is EXCLUDED from the completion test on purpose. A quest completes on its deed alone; the
// conversation is narration, not permission. See the design doc's "TALK is never a prerequisite":
// this is where that rule actually lives, and test_quest_state pins it by name.
inline void reevaluate(Progress& p, u8 questIdx) {
    if (questIdx >= COUNT) return;
    if (p.state[questIdx] == static_cast<u8>(State::LOCKED)) return;
    // COMPLETE is terminal, and that is not tidiness. A v6 hero migrates in COMPLETE with `obj`
    // zeroed — migrateFromMask has no detail to restore — so re-deriving would un-complete every
    // quest they had already finished the first time they greeted its giver, re-sealing a road
    // they had already walked.
    if (p.state[questIdx] == static_cast<u8>(State::COMPLETE)) return;

    const QuestDef& q = QUESTS[questIdx];
    bool allDeedsDone = true;
    bool anyProgress  = false;
    for (u32 o = 0; o < q.objectiveCount; o++) {
        const bool done = objectiveDone(p, questIdx, static_cast<u8>(o));
        if (done) anyProgress = true;
        if (q.objectives[o].trigger == Trigger::TALK) continue;
        if (!done) allDeedsDone = false;
    }

    if (allDeedsDone)      p.state[questIdx] = static_cast<u8>(State::COMPLETE);
    else if (anyProgress)  p.state[questIdx] = static_cast<u8>(State::ACTIVE);
}

// Make a quest known. Never demotes: a COMPLETE quest re-offered by walking back into its zone
// must stay complete.
inline void offer(Progress& p, u8 questIdx) {
    if (questIdx >= COUNT) return;
    if (p.state[questIdx] == static_cast<u8>(State::LOCKED))
        p.state[questIdx] = static_cast<u8>(State::OFFERED);
}

// Set the first objective of `trigger` kind whose target matches, then re-derive the state.
// Internal; the named hooks below are what callers use.
inline void satisfy(Progress& p, u8 questIdx, Trigger trigger, const char* target) {
    if (questIdx >= COUNT) return;
    offer(p, questIdx);                                   // reaching a deed implies knowing of it
    const QuestDef& q = QUESTS[questIdx];
    for (u32 o = 0; o < q.objectiveCount; o++) {
        if (q.objectives[o].trigger != trigger) continue;
        if (trigger == Trigger::SLAY) {
            if (!target || !q.objectives[o].target) continue;
            // Hand-rolled compare: <cstring> would be the only include this header needs, and the
            // point of it being engine-free is that it drags nothing in.
            const char* a = target; const char* b = q.objectives[o].target;
            while (*a && *a == *b) { a++; b++; }
            if (*a != *b) continue;
        }
        p.obj[questIdx][o] = q.objectives[o].required;
        break;
    }
    reevaluate(p, questIdx);
}

inline void noteTalk   (Progress& p, u8 questIdx)                     { satisfy(p, questIdx, Trigger::TALK,       nullptr); }
inline void noteReached(Progress& p, u8 questIdx)                     { satisfy(p, questIdx, Trigger::REACH,      nullptr); }
inline void noteCleared(Progress& p, u8 questIdx)                     { satisfy(p, questIdx, Trigger::CLEAR_ZONE, nullptr); }
inline void noteKill   (Progress& p, u8 questIdx, const char* enemy)  { satisfy(p, questIdx, Trigger::SLAY,       enemy);   }

// One fixture of an ACTIVATE objective. `fixtureIdx` is the fixture's ordinal within the quest
// (0..required-1) and becomes a bit, so re-touching a stone cannot double-count it.
inline void noteActivate(Progress& p, u8 questIdx, u8 fixtureIdx) {
    if (questIdx >= COUNT || fixtureIdx >= 8) return;
    offer(p, questIdx);
    const QuestDef& q = QUESTS[questIdx];
    for (u32 o = 0; o < q.objectiveCount; o++) {
        if (q.objectives[o].trigger != Trigger::ACTIVATE) continue;
        if (fixtureIdx >= q.objectives[o].required) return;   // not a fixture this quest owns
        p.obj[questIdx][o] |= static_cast<u8>(1u << fixtureIdx);
        break;
    }
    reevaluate(p, questIdx);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*quest*,*migrat*,*giver*,*TALK*,*ACTIVATE*,*objective*"`
Expected: PASS — 18 test cases, 0 failures.

- [ ] **Step 5: Sabotage-verify the non-blocking rule**

The "TALK is never a prerequisite" test is only worth having if it actually fails when the rule is broken. Temporarily delete this line from `reevaluate`:

```cpp
        if (q.objectives[o].trigger == Trigger::TALK) continue;
```

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*TALK is never*"`
Expected: **FAIL**, naming `TALK is never a prerequisite - field completion works unspoken`.

Then restore the line and re-run to confirm PASS. A sabotage that does not fail is evidence about the test, not permission to move on.

- [ ] **Step 6: Commit**

```bash
git add src/game/quest_state.h tests/game/test_quest_state.cpp
git commit -m "feat(quest): objective mutators and the non-blocking TALK rule

reevaluate() re-derives a quest's state from its objectives after every mutation, so
state can never disagree with the objectives it summarises. TALK is excluded from the
completion test: the conversation is narration, not permission. That keeps the
never-strand invariant intact and keeps the autoplay act soak passing, and it is
pinned by a sabotage-verified test.

ACTIVATE stores a bitmask rather than a count because a zone is rebuilt from its seed
on every entry — a count could not say WHICH stones were already lit.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 4: Wire the state machine into the engine

The engine currently owns `u64 m_questMask[MAX_LOCAL_PLAYERS]` as the authority. It becomes a cache recomputed from `Quest::Progress`, which is the new authority. Every consumer of the mask is untouched.

**Files:**
- Modify: `src/engine/engine.h:2042` (the mask declaration), `src/engine/engine.h:1532-1535` (hook declarations)
- Modify: `src/engine/engine_zone.cpp:565-626` (the three hooks)

- [ ] **Step 1: Add the progress array and the refresh helper declaration**

In `src/engine/engine.h`, immediately after the existing `m_questMask` declaration at line 2042, add:

```cpp
    // The AUTHORITY for quest progress; m_questMask above is a cache derived from it. Kept as a
    // cache rather than deleted because ~a dozen consumers (ZoneRoute, the gate refusals, the
    // autoplay act branch) take a plain u64 and have no reason to learn a new type.
    Quest::Progress m_questProgress[MAX_LOCAL_PLAYERS] = {};
```

In the same file, beside the four quest hook declarations at lines 1532-1535, add:

```cpp
    // Recompute m_questMask[lane] from m_questProgress[lane]. Call after ANY progress mutation —
    // the two must never be allowed to disagree.
    void refreshQuestMask(u8 lane);
```

Add the include beside the existing quest_def.h include at `src/engine/engine.h:23`:

```cpp
#include "game/quest_state.h"   // Quest::Progress — the per-character authority
```

- [ ] **Step 2: Rewrite the three hooks against the state machine**

In `src/engine/engine_zone.cpp`, replace the whole block from `void Engine::questComplete(u8 zoneFloor) {` through the end of `Engine::questCheckZoneCleared()` with:

```cpp
// The mask is a CACHE of m_questProgress. Every mutation goes through here, so the two cannot
// drift — the failure shape this codebase keeps rediscovering (see CLAUDE.md's "one fact stored
// twice" thread).
void Engine::refreshQuestMask(u8 lane) {
    if (lane >= MAX_LOCAL_PLAYERS) return;
    m_questMask[lane] = Quest::completionMask(m_questProgress[lane]);
}

// Announce a quest's completion once, when it crosses into COMPLETE. Callers mutate progress and
// then call this; it is a no-op if the quest did not just finish.
void Engine::questAnnounce(u8 questIdx, bool wasComplete) {
    if (questIdx >= Quest::COUNT) return;
    const u8 lane = m_localPlayerIndex;
    const bool nowComplete = Quest::isComplete(m_questMask[lane], Quest::QUESTS[questIdx].zoneFloor);
    if (!nowComplete || wasComplete) return;

    const Quest::QuestDef& q = Quest::QUESTS[questIdx];
    addChatMessage("", q.name, Vec3{1.0f, 0.85f, 0.35f});
    LOG_INFO("[QUEST] complete: %s", q.name);
    AudioSystem::play(SfxId::LEVEL_UP);

    const u8 act = Quest::actOf(q.zoneFloor);
    if (Quest::actComplete(m_questMask[lane], act)) {
        addChatMessage("", act == 1 ? "Act 1 complete - the platform is open."
                                    : "Act 2 complete - the gate is closed.",
                       Vec3{1.0f, 0.95f, 0.6f});
        LOG_INFO("[QUEST] ACT %u COMPLETE", static_cast<u32>(act));
    }
}

// Offered on arrival. REACH quests take their objective from the same event — finding the place
// WAS the task — so arrival both offers and advances.
void Engine::questOnZoneEnter(u8 zoneFloor) {
    const u8 idx = Quest::indexForZone(zoneFloor);
    if (idx == 0xFF) return;
    const u8 lane = m_localPlayerIndex;
    const bool wasComplete = Quest::isComplete(m_questMask[lane], zoneFloor);

    Quest::offer(m_questProgress[lane], idx);
    Quest::noteReached(m_questProgress[lane], idx);
    refreshQuestMask(lane);

    if (!wasComplete && !Quest::isComplete(m_questMask[lane], zoneFloor)) {
        const Quest::QuestDef& q = Quest::QUESTS[idx];
        addChatMessage("", q.blurb, Vec3{0.75f, 0.8f, 0.9f});
        // Logged as well as shown. A chat-only offer is invisible to a soak and to any
        // after-the-fact check of whether the chain actually armed — the blind spot that hid the
        // credits park and the dead legendaries until a log line was added.
        LOG_INFO("[QUEST] offered: %s (giver %s)", q.name, Quest::GIVERS[q.giverIdx].name);
    }
    questAnnounce(idx, wasComplete);
}

// SLAY. Called from handleDeathPreamble — the ONE choke every enemy death funnels through — so no
// kill route (a proc, a pet, a thorns reflect) can miss an objective.
void Engine::questOnEnemyKilled(const char* enemyName) {
    if (!m_level.inZone || !enemyName) return;
    const u8 idx = Quest::indexForZone(m_level.zoneFloor);
    if (idx == 0xFF) return;
    const u8 lane = m_localPlayerIndex;
    const bool wasComplete = Quest::isComplete(m_questMask[lane], m_level.zoneFloor);

    Quest::noteKill(m_questProgress[lane], idx, enemyName);
    refreshQuestMask(lane);
    questAnnounce(idx, wasComplete);
}

// CLEAR_ZONE. Polled rather than event-driven because "no hostiles left" is a property of the
// pool, not of any one death — a summoner's last minion and the summoner itself can die on the
// same tick, and an event-per-death would have to re-scan anyway.
void Engine::questCheckZoneCleared() {
    if (!m_level.inZone) return;
    const u8 idx = Quest::indexForZone(m_level.zoneFloor);
    if (idx == 0xFF) return;
    const u8 lane = m_localPlayerIndex;
    if (Quest::isComplete(m_questMask[lane], m_level.zoneFloor)) return;
    if (zoneHostilesAlive() > 0) return;

    Quest::noteCleared(m_questProgress[lane], idx);
    refreshQuestMask(lane);
    questAnnounce(idx, /*wasComplete*/ false);
}
```

- [ ] **Step 3: Extract the live hostile count**

The old `questCheckZoneCleared` walked the entity pool inline. The Journal needs the same number for its `Hostiles remaining n/m` row, so it becomes a small named helper rather than being counted in two places. Add to `src/engine/engine_zone.cpp`, above `questCheckZoneCleared`:

```cpp
// Live hostile count in the current zone. Used by BOTH the CLEAR_ZONE completion poll and the
// Journal's progress row, so the number the player reads and the number that completes the quest
// are the same number.
u16 Engine::zoneHostilesAlive() const {
    u16 n = 0;
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        const Entity& e = m_entities.entities[m_entities.activeList[a]];
        if (e.flags & (ENT_DEAD | ENT_FRIENDLY)) continue;
        if (e.npcClass != NpcClass::NONE) continue;   // friendly class NPCs are not hostiles
        n++;
    }
    return n;
}
```

Declare it in `src/engine/engine.h` beside the quest hooks:

```cpp
    u16  zoneHostilesAlive() const;   // live hostiles in the current zone (quest poll + journal row)
    void questAnnounce(u8 questIdx, bool wasComplete);
```

- [ ] **Step 4: Build and run the full suite**

Run: `cmake --build build && ./build/tests/dungeon_tests`
Expected: PASS. `test_zone_route.cpp` in particular — it consumes the derived mask and must be unaffected.

- [ ] **Step 5: Verify the acts still play**

Run: `DISPLAY=:1 ./build/src/DungeonEngine --load 4 --endgame --zone 52 --autoplay --quests-done 2>&1 | grep -m5 "QUEST\|ZONEX"`
Expected: the run reaches a zone and logs `[ZONEX]` world changes. `--quests-done` sets every mask bit, so no quest offers should print.

Then the discriminating control, a hero with NO quests done:

Run: `timeout 120 DISPLAY=:1 ./build/src/DungeonEngine --load 4 --endgame --zone 52 --autoplay 2>&1 | grep -m5 "\[QUEST\]"`
Expected: at least one `[QUEST] offered:` line naming a quest and its giver.

- [ ] **Step 6: Commit**

```bash
git add src/engine/engine.h src/engine/engine_zone.cpp
git commit -m "refactor(quest): route the three quest hooks through the state machine

m_questProgress becomes the authority and m_questMask a cache refreshed from it, so
every existing consumer (ZoneRoute, the gate refusals, the autoplay act branch) is
byte-identical while the underlying state gains objectives. zoneHostilesAlive() is
extracted because the CLEAR_ZONE poll and the Journal's progress row must report the
same number.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 5: Persist it — `SAVE_VERSION` 7

**Files:**
- Modify: `src/engine/engine_persist.cpp:88-98` (versions), `:341-342` (write), `:566-571` and `:679+` (readers), `src/engine/engine.h:1989-2001` (`SavedChar`)

- [ ] **Step 1: The v6 fixture is already in place — verify it, do not re-make it**

`tests/fixtures/save_v6_fixture.dat` is a genuine **v6** save copied from the live save directory,
carrying `questMask = 1023` (`0x3FF`) — all ten quests complete. It is the single most important
artifact in this task: it is the only thing that can prove a hero who has finished both acts still
has them after the v7 bump.

Confirm it before relying on it:

```bash
head -c4 tests/fixtures/save_v6_fixture.dat | od -An -tu4     # must be 6
tail -c8 tests/fixtures/save_v6_fixture.dat | od -An -tu8     # must be 1023
```

Note for context: the twenty `save_NN.dat` files in the REPO ROOT are stale v2/v3 leftovers and are
NOT what the game reads. An earlier draft of this plan pointed the fixture step at them, which would
have "tested" the v6 migration against a v2 file and reported green while testing nothing.

- [ ] **Step 2: Bump the version constants**

In `src/engine/engine_persist.cpp`, replace lines 88-98 with:

```cpp
static constexpr u32 SAVE_VERSION           = 7;
static constexpr u32 SAVE_VERSION_LEGACY_V6 = 6;   // same layout minus the quest-progress tail
static constexpr u32 SAVE_VERSION_LEGACY_V5 = 5;   // ...minus the waypoint/quest masks too
static constexpr u32 SAVE_VERSION_LEGACY_V4 = 4;   // same layout — read as-is, migrates on next save
static constexpr u32 SAVE_VERSION_LEGACY_V3 = 3;
static constexpr u32 SAVE_VERSION_LEGACY_V2 = 2;

static bool saveVersionSupported(u32 ver) {
    return ver == SAVE_VERSION || ver == SAVE_VERSION_LEGACY_V6 ||
           ver == SAVE_VERSION_LEGACY_V5 || ver == SAVE_VERSION_LEGACY_V4 ||
           ver == SAVE_VERSION_LEGACY_V3 || ver == SAVE_VERSION_LEGACY_V2;
}
```

- [ ] **Step 3: Fix the direct-read gate**

Line 145 gates the direct struct read on an explicit version list. v7 adds no field to `PlayerInventory`, so v6 must be added to that list or every v6 save fails to load:

```cpp
    if (ver == SAVE_VERSION || ver == SAVE_VERSION_LEGACY_V6 ||
        ver == SAVE_VERSION_LEGACY_V5 || ver == SAVE_VERSION_LEGACY_V4)
```

This is the trap the file's own comment already warns about: keying the direct path on `== SAVE_VERSION` alone breaks every prior save the day a bump adds no fields.

- [ ] **Step 4: Extend `SavedChar`**

In `src/engine/engine.h`, replace the v6 tail of `SavedChar` (lines 1996-2000) with:

```cpp
        // v6 tail: discovered overworld waypoints + the legacy completion mask. A pre-v6 save
        // leaves both 0, which reads as "this hero has found/done none" — correct, since those
        // characters predate the overworld entirely.
        u64 waypointMask = 0;
        u64 questMask    = 0;
        // v7 tail: full quest progress. On a v6 file this is reconstructed from questMask by
        // Quest::migrateFromMask — read as zeros instead and every hero who already played the
        // acts silently loses both of them, permanently, on their next autosave.
        Quest::Progress questProgress{};
```

- [ ] **Step 5: Write the new tail**

In `src/engine/engine_persist.cpp`, after the two `fwrite` calls at lines 341-342, add:

```cpp
    // v7 tail: the per-quest state + objective bytes. Appended AFTER questMask, which therefore
    // stays in the file by construction — removing it would shift the v6 portion's layout and
    // break the legacy reader it exists to serve.
    std::fwrite(m_questProgress[lane].state, sizeof(u8), Quest::MAX_QUESTS, f);
    std::fwrite(m_questProgress[lane].obj,   sizeof(u8), Quest::MAX_QUESTS * Quest::MAX_OBJ, f);
```

- [ ] **Step 6: Read it in `loadGame`**

In `src/engine/engine_persist.cpp`, replace the v6 tail read at lines 566-571 with:

```cpp
        // v6 tail. Version-conditional rather than unconditional: a v5 file simply ends here, and
        // reading past it would fail the load outright for every save written before the overworld.
        if (pok && ver >= SAVE_VERSION_LEGACY_V6) {
            pok = std::fread(&ps.waypointMask, sizeof(u64), 1, f) == 1;
            pok = pok && std::fread(&ps.questMask, sizeof(u64), 1, f) == 1;
        }
        // v7 tail. On a v6 file there is nothing to read: reconstruct the progress from the mask
        // instead, or the hero loses both acts.
        if (pok && ver >= SAVE_VERSION) {
            pok = pok && std::fread(ps.questProgress.state, sizeof(u8),
                                    Quest::MAX_QUESTS, f) == Quest::MAX_QUESTS;
            pok = pok && std::fread(ps.questProgress.obj, sizeof(u8),
                                    Quest::MAX_QUESTS * Quest::MAX_OBJ, f)
                         == Quest::MAX_QUESTS * Quest::MAX_OBJ;
        } else if (pok) {
            Quest::migrateFromMask(ps.questProgress, ps.questMask);
        }
```

- [ ] **Step 7: Mirror the same read in `loadCharacterInto`**

Find the second reader (around line 679, the one that reads `ps.activeSkill`) and apply the identical version-conditional pair after its waypoint/quest mask reads. Both readers must agree; a fix applied to one is exactly how a save bug survives a review.

- [ ] **Step 8: Apply the progress on load**

In `Engine::applySavedCharToLane` (`src/engine/engine_persist.cpp:435`), after the two existing mask assignments at lines 437-438, add:

```cpp
    m_questProgress[lane] = ps.questProgress;   // v7; reconstructed from questMask on any older save
    refreshQuestMask(lane);                     // the mask is a cache — never trust the file's copy
```

Note the second line: the file's `questMask` is read and then **discarded**. `questState` is the authority, and recomputing prevents a hand-edited or half-migrated file from presenting a mask that disagrees with the state behind it.

- [ ] **Step 9: Write the round-trip test**

Append to `tests/game/test_quest_state.cpp`:

```cpp
// The v7 payload must survive a byte-level round trip at exactly the size the writer emits. This
// is the shape check; the real-file check is the manual fixture load in the plan's next step.
TEST_CASE("quest progress round-trips through a byte buffer at the serialized size") {
    Quest::Progress out{};
    Quest::offer(out, 0);
    Quest::noteTalk(out, 0);
    Quest::offer(out, 5);
    out.state[9] = static_cast<u8>(Quest::State::COMPLETE);
    // Objective bytes written DIRECTLY rather than through noteActivate: no quest owns an ACTIVATE
    // objective until Task 14 flips quest 56, so the mutator would be a silent no-op here and the
    // round trip would prove nothing about obj[] at all. What this test pins is the SERIALIZED
    // SHAPE — that every byte of both arrays survives — so it wants arbitrary bytes, not a
    // realistic play state.
    out.obj[2][1] = 0b00010001;   // the Cairn bitmask shape: stones 0 and 4
    out.obj[7][0] = 1;

    u8 buf[Quest::MAX_QUESTS + Quest::MAX_QUESTS * Quest::MAX_OBJ] = {};
    u32 w = 0;
    for (u32 i = 0; i < Quest::MAX_QUESTS; i++) buf[w++] = out.state[i];
    for (u32 i = 0; i < Quest::MAX_QUESTS; i++)
        for (u32 o = 0; o < Quest::MAX_OBJ; o++) buf[w++] = out.obj[i][o];
    REQUIRE(w == sizeof(buf));

    Quest::Progress in{};
    u32 r = 0;
    for (u32 i = 0; i < Quest::MAX_QUESTS; i++) in.state[i] = buf[r++];
    for (u32 i = 0; i < Quest::MAX_QUESTS; i++)
        for (u32 o = 0; o < Quest::MAX_OBJ; o++) in.obj[i][o] = buf[r++];

    for (u32 i = 0; i < Quest::MAX_QUESTS; i++) {
        REQUIRE(in.state[i] == out.state[i]);
        for (u32 o = 0; o < Quest::MAX_OBJ; o++) REQUIRE(in.obj[i][o] == out.obj[i][o]);
    }
    REQUIRE(Quest::completionMask(in) == Quest::completionMask(out));
    REQUIRE(in.obj[2][1] == 0b00010001);   // the raw byte, not objectiveProgress — see above
}
```

- [ ] **Step 10: Run the tests**

Run: `cmake --build build && ./build/tests/dungeon_tests -tc="*quest*,*migrat*,*giver*,*TALK*,*ACTIVATE*,*objective*"`
Expected: PASS — 19 test cases.

- [ ] **Step 11: Verify the v6 fixture still loads with its acts intact**

This is the step the whole task exists for. Restore the fixture into the live save directory under an unused slot and load it:

```bash
cp tests/fixtures/save_v6_fixture.dat save_09.dat
timeout 60 DISPLAY=:1 ./build/src/DungeonEngine --load 9 --town 2>&1 | grep -iE "load|quest|version" | head -20
```

Expected: the save loads (no `version` rejection), and the hero's quest completions survive. Confirm by checking the mask is non-zero if the fixture hero had done quests — add a temporary `LOG_INFO("[QUEST] loaded mask %llu", m_questMask[lane])` in `applySavedCharToLane` if the log is not otherwise conclusive, then remove it.

**If the mask comes back 0 for a hero who had completed quests, STOP.** That is the migration failing, and it is unrecoverable once the game autosaves over the file.

- [ ] **Step 12: Verify a fresh v7 save round-trips**

```bash
timeout 90 DISPLAY=:1 ./build/src/DungeonEngine --load 9 --endgame --zone 52 --autoplay 2>&1 | grep -m3 "\[QUEST\]"
```

Let it complete a quest, quit, reload, and confirm the Journal state persists (readable via the mask log until Phase 2 lands).

- [ ] **Step 13: Commit**

```bash
git add src/engine/engine_persist.cpp src/engine/engine.h tests/game/test_quest_state.cpp tests/fixtures/save_v6_fixture.dat
git commit -m "feat(save): SAVE_VERSION 7 - per-character quest progress

Appends questState[32] + objProgress[32][4] to the v6 per-player tail: one writer,
two version-conditional readers, no existing struct changes size so every layout
static_assert stays green. Sized to 32 quests now, while the bump is free.

The migration is the load-bearing half: a v6 file carries completions as a u64 mask,
and reading the new tail as zeros instead would silently un-complete both acts for
every existing hero and write that loss back on the next autosave. The mask itself is
now read and DISCARDED on v7 — questState is the authority, so a file whose mask
disagrees with its state cannot mislead the game.

Also fixes the direct-read gate, which keyed on == SAVE_VERSION and would have
rejected every v6 save: the trap that file's own comment warns about.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

# PHASE 2 — The Journal panel

After this phase the player can read their quests. This is the smallest point at which the reported complaint is actually fixed.

## Task 6: Journal layout and hit-test

**Files:**
- Modify: `src/game/inventory_ui.h`, `src/game/inventory_ui.cpp`
- Create: `tests/game/test_journal_layout.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/game/test_journal_layout.cpp`:

```cpp
// test_journal_layout.cpp — the Journal panel's geometry.
//
// Draw and hit-test both derive from journalLayout(). This file pins that they agree: every other
// inventory panel in this codebase drifted apart at some point because each side re-derived its
// own rects (see the quickbar note in inventory_ui.h).
#include "../../external/doctest/doctest.h"
#include "game/inventory_ui.h"

TEST_CASE("journal layout scales with screen height and stays on screen") {
    for (u32 sh : {720u, 1080u, 480u}) {
        const u32 sw = sh * 16 / 9;
        const InventoryUI::JournalRects r = InventoryUI::journalLayout(sw, sh);
        CAPTURE(sh);
        REQUIRE(r.rowH > 0.0f);
        REQUIRE(r.listX >= 0.0f);
        REQUIRE(r.listX + r.listW <= static_cast<f32>(sw));
        REQUIRE(r.detailX > r.listX + r.listW);          // detail sits right of the list
        REQUIRE(r.detailX + r.detailW <= static_cast<f32>(sw));
        REQUIRE(r.listTopY <= static_cast<f32>(sh));
    }
}

TEST_CASE("clicking a quest row hit-tests to that row index") {
    const u32 sw = 1280, sh = 720;
    const InventoryUI::JournalRects r = InventoryUI::journalLayout(sw, sh);

    for (u8 row = 0; row < 4; row++) {
        // Row 0 is drawn at the TOP and rows descend, matching the build grid's convention.
        const s32 mx = static_cast<s32>(r.listX + r.listW * 0.5f);
        const s32 my = static_cast<s32>(r.listTopY - r.rowH * (static_cast<f32>(row) + 0.5f));
        const InventoryUI::SlotHit h = InventoryUI::hitTestJournal(sw, sh, mx, my);
        CAPTURE(row);
        REQUIRE(h.panel == InventoryUI::SlotHit::JOURNAL_ROW);
        REQUIRE(h.index == row);
    }
}

TEST_CASE("clicking an act tab hit-tests to that tab") {
    const u32 sw = 1280, sh = 720;
    const InventoryUI::JournalRects r = InventoryUI::journalLayout(sw, sh);
    for (u8 t = 0; t < 2; t++) {
        const s32 mx = static_cast<s32>(r.tabX + (r.tabW + r.tabGap) * t + r.tabW * 0.5f);
        const s32 my = static_cast<s32>(r.tabY + r.tabH * 0.5f);
        const InventoryUI::SlotHit h = InventoryUI::hitTestJournal(sw, sh, mx, my);
        CAPTURE(t);
        REQUIRE(h.panel == InventoryUI::SlotHit::JOURNAL_TAB);
        REQUIRE(h.index == t);
    }
}

TEST_CASE("a click outside the panel hits nothing") {
    const InventoryUI::SlotHit h = InventoryUI::hitTestJournal(1280, 720, 5, 5);
    REQUIRE(h.panel == InventoryUI::SlotHit::NONE);
}
```

- [ ] **Step 2: Add it to the build**

In `tests/CMakeLists.txt`, after the `game/test_quest_state.cpp` line:

```cmake
    game/test_journal_layout.cpp     # journal panel geometry: draw and hit-test derive from one place
    ${CMAKE_SOURCE_DIR}/src/game/inventory_ui.cpp
```

Check first whether `src/game/inventory_ui.cpp` is already in the list (`test_skill_bar_hit.cpp` and `test_quickbar.cpp` may already link it) — CMake errors on a duplicate source. If it is already there, add only the test line.

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build build --target dungeon_tests`
Expected: FAIL to compile — `'JournalRects' is not a member of 'InventoryUI'`.

- [ ] **Step 4: Declare the layout**

In `src/game/inventory_ui.h`, extend the `SlotHit::Panel` enum by **appending** (its values are not serialized, but appending keeps every existing comparison stable):

```cpp
        enum Panel : u8 { NONE, BACKPACK, EQUIPMENT, QUICKBAR, STASH, STASH_TAB,
                          BUILD_CELL, BUILD_TOGGLE, JOURNAL_ROW, JOURNAL_TAB };
```

Then add, after the `buildGridLayout` block:

```cpp
    // ---- Quest journal panel (drawn in the inventory's right column) ----
    // Two columns: a quest list on the left, the selected quest's narration + objectives on the
    // right. Single-sourced like every panel here — HUD::drawJournalPanel and hitTestJournal both
    // derive from journalLayout(), or the click rects drift off the drawn thing.
    static constexpr u32 JOURNAL_TABS = 2;      // ACT I, ACT II
    static constexpr u32 JOURNAL_ROWS = 12;     // visible quest rows per act (10 authored today)
    struct JournalRects {
        f32 listX = 0.0f, listTopY = 0.0f, listW = 0.0f;   // row 0's left / TOP edge, column width
        f32 rowH = 0.0f;                                    // one quest row's height
        f32 detailX = 0.0f, detailTopY = 0.0f, detailW = 0.0f;
        f32 tabX = 0.0f, tabY = 0.0f, tabW = 0.0f, tabH = 0.0f, tabGap = 0.0f;
        f32 uiScale = 1.0f;
    };
    JournalRects journalLayout(u32 sw, u32 sh);
    // Hit-test the journal: JOURNAL_ROW with index = visible row, or JOURNAL_TAB with index = act.
    SlotHit hitTestJournal(u32 sw, u32 sh, s32 mx, s32 my);
```

- [ ] **Step 5: Implement it**

Append to `src/game/inventory_ui.cpp`:

```cpp
// The journal occupies the same right-hand column the build grid uses, so the two never overlap:
// only one panel is ever the active one. Geometry is expressed in the same 720p-relative uiScale
// every other panel here uses.
InventoryUI::JournalRects InventoryUI::journalLayout(u32 sw, u32 sh) {
    JournalRects r;
    r.uiScale = static_cast<f32>(sh) / 720.0f;

    const f32 panelW = 520.0f * r.uiScale;
    const f32 panelX = static_cast<f32>(sw) * 0.5f - panelW * 0.5f;
    const f32 topY   = static_cast<f32>(sh) * 0.82f;

    r.tabW   = 110.0f * r.uiScale;
    r.tabH   = 24.0f  * r.uiScale;
    r.tabGap = 8.0f   * r.uiScale;
    r.tabX   = panelX;
    r.tabY   = topY;

    r.rowH     = 22.0f * r.uiScale;
    r.listX    = panelX;
    r.listW    = 200.0f * r.uiScale;
    r.listTopY = topY - r.tabH - 10.0f * r.uiScale;

    r.detailX    = r.listX + r.listW + 16.0f * r.uiScale;
    r.detailW    = panelW - r.listW - 16.0f * r.uiScale;
    r.detailTopY = r.listTopY;
    return r;
}

InventoryUI::SlotHit InventoryUI::hitTestJournal(u32 sw, u32 sh, s32 mx, s32 my) {
    SlotHit result;
    const JournalRects r = journalLayout(sw, sh);
    const f32 fx = static_cast<f32>(mx), fy = static_cast<f32>(my);

    for (u32 t = 0; t < JOURNAL_TABS; t++) {
        const f32 x0 = r.tabX + (r.tabW + r.tabGap) * static_cast<f32>(t);
        if (fx >= x0 && fx < x0 + r.tabW && fy >= r.tabY && fy < r.tabY + r.tabH) {
            result.panel = SlotHit::JOURNAL_TAB;
            result.index = static_cast<u8>(t);
            return result;
        }
    }

    if (fx >= r.listX && fx < r.listX + r.listW) {
        // Rows descend from listTopY, matching the build grid's row-0-at-top convention.
        const f32 dy = r.listTopY - fy;
        if (dy >= 0.0f) {
            const u32 row = static_cast<u32>(dy / r.rowH);
            if (row < JOURNAL_ROWS) {
                result.panel = SlotHit::JOURNAL_ROW;
                result.index = static_cast<u8>(row);
                return result;
            }
        }
    }
    return result;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*journal*,*quest row*,*act tab*,*outside the panel*"`
Expected: PASS — 4 test cases, 0 failures.

- [ ] **Step 7: Commit**

```bash
git add src/game/inventory_ui.h src/game/inventory_ui.cpp tests/game/test_journal_layout.cpp tests/CMakeLists.txt
git commit -m "feat(ui): journal panel layout, single-sourced for draw and hit-test

journalLayout() is the one place the panel's geometry exists, so the drawn rows and
the clickable rows cannot drift apart — the failure every other inventory panel here
hit before being converted to this discipline.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 7: Draw the Journal

**Files:**
- Create: `src/renderer/hud_journal.cpp`
- Modify: `src/renderer/hud.h`, `CMakeLists.txt` (root, if it globs explicitly), `src/engine/engine_hud.cpp:148-151`

- [ ] **Step 1: Declare the draw entry point**

In `src/renderer/hud.h`, after the `drawBuildGrid` declaration (line 80):

```cpp
    // Quest journal panel (right column of the inventory screen): act tabs, the act's quest list,
    // and the selected quest's narration + objective rows. Geometry from InventoryUI::journalLayout
    // (single-sourced with the hit-test).
    //
    // `liveQuestIdx` / `liveRemaining` / `liveTotal` carry the CLEAR_ZONE count, which is a property
    // of the entity pool and is deliberately not stored in Quest::Progress. 0xFF = the player is not
    // standing in a quest zone, so no live row is drawn.
    void drawJournalPanel(u32 sw, u32 sh, const Quest::Progress& prog,
                          u8 selectedRow, u8 actTab,
                          u8 liveQuestIdx, u16 liveRemaining, u16 liveTotal,
                          s32 mouseX, s32 mouseY);
```

Add the include at the top of `src/renderer/hud.h`, beside the other game includes:

```cpp
#include "game/quest_state.h"
```

- [ ] **Step 2: Implement the panel**

Create `src/renderer/hud_journal.cpp`:

```cpp
// hud_journal.cpp — the quest Journal, drawn as a panel of the inventory screen.
//
// The Journal replaced a chat line as the game's quest record. That matters for one concrete
// reason beyond legibility: Engine::addChatMessage formats into a fixed buffer, so nine of the ten
// quest blurbs were being truncated mid-sentence and several lost the part that says what to DO.
// Text here is word-WRAPPED against the panel width instead, so a narration can be any length.
//
// Geometry comes entirely from InventoryUI::journalLayout — this file never re-derives a rect.
#include "renderer/hud.h"
#include "renderer/font.h"
#include "game/inventory_ui.h"
#include "game/quest_def.h"
#include "game/quest_state.h"

namespace {

// Colour by state. Shape carries WHAT, colour carries WHICH — the same rule the overworld's
// minimap icons follow.
Vec3 stateColour(Quest::State s) {
    switch (s) {
        case Quest::State::COMPLETE: return Vec3{1.00f, 0.85f, 0.35f};   // gold
        case Quest::State::ACTIVE:   return Vec3{0.95f, 0.95f, 0.95f};   // white
        case Quest::State::OFFERED:  return Vec3{0.70f, 0.76f, 0.88f};   // grey-blue
        default:                     return Vec3{0.38f, 0.38f, 0.42f};   // dim: not yet known
    }
}

// Draw `text` wrapped to `maxW`, starting at (x, topY) and descending. Returns the Y below the
// last line so the caller can keep stacking. Breaks on spaces only; a single word longer than the
// column overflows rather than being split, which never happens with authored prose and keeps this
// simple enough to be obviously correct.
f32 drawWrapped(u32 sw, u32 sh, f32 x, f32 topY, f32 maxW,
                const char* text, Vec3 col, f32 scale, f32 lineH) {
    if (!text || !text[0]) return topY;

    char line[256];
    u32  len = 0;
    f32  y   = topY;

    const char* word = text;
    while (*word) {
        const char* end = word;
        while (*end && *end != ' ') end++;
        const u32 wlen = static_cast<u32>(end - word);

        // Would appending this word overflow the column?
        char probe[256];
        u32  plen = 0;
        for (u32 i = 0; i < len && plen < sizeof(probe) - 1; i++) probe[plen++] = line[i];
        if (len && plen < sizeof(probe) - 1) probe[plen++] = ' ';
        for (u32 i = 0; i < wlen && plen < sizeof(probe) - 1; i++) probe[plen++] = word[i];
        probe[plen] = '\0';

        if (len && FontSystem::textWidth(probe, scale) > maxW) {
            line[len] = '\0';
            FontSystem::drawText(sw, sh, x, y, line, col, scale);
            y -= lineH;
            len = 0;
            for (u32 i = 0; i < wlen && len < sizeof(line) - 1; i++) line[len++] = word[i];
        } else {
            len = 0;
            for (u32 i = 0; i < plen && len < sizeof(line) - 1; i++) line[len++] = probe[i];
        }

        word = end;
        while (*word == ' ') word++;
    }
    if (len) {
        line[len] = '\0';
        FontSystem::drawText(sw, sh, x, y, line, col, scale);
        y -= lineH;
    }
    return y;
}

} // namespace

void HUD::drawJournalPanel(u32 sw, u32 sh, const Quest::Progress& prog,
                           u8 selectedRow, u8 actTab,
                           u8 liveQuestIdx, u16 liveRemaining, u16 liveTotal,
                           s32 mouseX, s32 mouseY) {
    const InventoryUI::JournalRects r = InventoryUI::journalLayout(sw, sh);
    const InventoryUI::SlotHit hover  = InventoryUI::hitTestJournal(sw, sh, mouseX, mouseY);
    const f32 s  = r.uiScale;
    const f32 tx = 1.0f * s;   // text scale

    // --- Act tabs ---
    for (u32 t = 0; t < InventoryUI::JOURNAL_TABS; t++) {
        const f32 x0  = r.tabX + (r.tabW + r.tabGap) * static_cast<f32>(t);
        const bool on = (t == actTab);
        const bool hov = hover.panel == InventoryUI::SlotHit::JOURNAL_TAB && hover.index == t;
        const Vec3 col = on  ? Vec3{1.0f, 0.9f, 0.5f}
                        : hov ? Vec3{0.85f, 0.85f, 0.9f}
                              : Vec3{0.55f, 0.55f, 0.6f};
        FontSystem::drawText(sw, sh, x0 + 8.0f * s, r.tabY + 6.0f * s,
                             t == 0 ? "ACT I" : "ACT II", col, tx);
    }

    // --- Quest list for the visible act ---
    // The rows are the act's quests IN TABLE ORDER, which is the road's walking order.
    u8 rowQuest[InventoryUI::JOURNAL_ROWS];
    u8 rowCount = 0;
    for (u32 i = 0; i < Quest::COUNT && rowCount < InventoryUI::JOURNAL_ROWS; i++) {
        if (Quest::actOf(Quest::QUESTS[i].zoneFloor) != actTab + 1) continue;
        rowQuest[rowCount++] = static_cast<u8>(i);
    }

    for (u8 row = 0; row < rowCount; row++) {
        const u8 q = rowQuest[row];
        const Quest::State st = static_cast<Quest::State>(prog.state[q]);
        const f32 y = r.listTopY - r.rowH * (static_cast<f32>(row) + 1.0f) + 5.0f * s;

        const bool sel = (row == selectedRow);
        const bool hov = hover.panel == InventoryUI::SlotHit::JOURNAL_ROW && hover.index == row;
        Vec3 col = stateColour(st);
        if (sel || hov) col = Vec3{col.x + 0.15f, col.y + 0.15f, col.z + 0.15f};

        // A locked quest shows that it EXISTS without naming it. The player can see the act has
        // more to give; they just have not found it yet — the minimap's "dim while unexplored"
        // rule, applied to text.
        const char* label = (st == Quest::State::LOCKED) ? "? ? ?" : Quest::QUESTS[q].name;
        FontSystem::drawText(sw, sh, r.listX + (sel ? 10.0f * s : 4.0f * s), y, label, col, tx);
    }

    if (selectedRow >= rowCount) return;   // empty act, or a stale cursor after a tab flip

    // --- Detail pane: title, narration, objectives ---
    const u8 q = rowQuest[selectedRow];
    const Quest::QuestDef& qd = Quest::QUESTS[q];
    const Quest::State st = static_cast<Quest::State>(prog.state[q]);
    const f32 lineH = 16.0f * s;
    f32 y = r.detailTopY - lineH;

    if (st == Quest::State::LOCKED) {
        FontSystem::drawText(sw, sh, r.detailX, y, "Not yet known.",
                             Vec3{0.45f, 0.45f, 0.5f}, tx);
        return;
    }

    FontSystem::drawText(sw, sh, r.detailX, y, qd.name, stateColour(st), tx);
    y -= lineH * 1.6f;

    y = drawWrapped(sw, sh, r.detailX, y, r.detailW, qd.narration,
                    Vec3{0.78f, 0.78f, 0.82f}, tx, lineH);
    y -= lineH * 0.6f;

    FontSystem::drawText(sw, sh, r.detailX, y, Quest::GIVERS[qd.giverIdx].name,
                         Vec3{0.55f, 0.60f, 0.70f}, tx * 0.9f);
    y -= lineH * 1.4f;

    for (u32 o = 0; o < qd.objectiveCount; o++) {
        const Quest::ObjectiveDef& od = qd.objectives[o];
        const bool done = Quest::objectiveDone(prog, q, static_cast<u8>(o));

        char row[128];
        if (od.trigger == Quest::Trigger::CLEAR_ZONE && q == liveQuestIdx && !done) {
            // The live pool count — the same number the completion poll reads, so what the player
            // sees and what finishes the quest can never be two different numbers.
            std::snprintf(row, sizeof(row), "%s %s  %u/%u",
                          done ? "[x]" : "[ ]", od.text,
                          static_cast<u32>(liveTotal - liveRemaining), static_cast<u32>(liveTotal));
        } else if (od.required > 1) {
            std::snprintf(row, sizeof(row), "%s %s  %u/%u",
                          done ? "[x]" : "[ ]", od.text,
                          static_cast<u32>(Quest::objectiveProgress(prog, q, static_cast<u8>(o))),
                          static_cast<u32>(od.required));
        } else {
            std::snprintf(row, sizeof(row), "%s %s", done ? "[x]" : "[ ]", od.text);
        }

        FontSystem::drawText(sw, sh, r.detailX, y, row,
                             done ? Vec3{0.55f, 0.85f, 0.55f} : Vec3{0.80f, 0.80f, 0.84f}, tx);
        y -= lineH;
    }
}
```

Add `#include <cstdio>` at the top of the file for `snprintf`.

- [ ] **Step 3: Add the file to the build**

Run: `grep -n "hud_inventory.cpp" CMakeLists.txt src/CMakeLists.txt 2>/dev/null`

If the sources are listed explicitly, add `src/renderer/hud_journal.cpp` beside `src/renderer/hud_inventory.cpp`. If they are globbed, no edit is needed — confirm with a build.

- [ ] **Step 4: Call it from the inventory screen**

In `src/engine/engine_hud.cpp`, replace the build-grid call at lines 148-151 with:

```cpp
    // The right column hosts EITHER the build grid or the journal — one panel at a time, chosen by
    // which the cursor is on. The stash panel would overlap both, so neither draws in stash mode.
    if (!m_stashOpen) {
        if (m_invCursorPanel == INV_PANEL_JOURNAL) {
            const u8 liveIdx = m_level.inZone ? Quest::indexForZone(m_level.zoneFloor) : 0xFF;
            HUD::drawJournalPanel(sw, sh, m_questProgress[m_localPlayerIndex],
                                  m_invCursorQuest, m_invJournalAct,
                                  liveIdx, zoneHostilesAlive(), m_zoneHostilesAtEntry,
                                  invMX, invMY);
        } else {
            HUD::drawBuildGrid(sw, sh, m_inventories[m_localPlayerIndex].autoMode,
                               m_inventories[m_localPlayerIndex].buildCell, invMX, invMY);
        }
    }
```

- [ ] **Step 5: Record the zone's starting hostile count**

`m_zoneHostilesAtEntry` is the denominator of the `n/m` row and does not exist yet. Add to `src/engine/engine.h` beside the other zone members:

```cpp
    // Hostiles present when this zone was built — the denominator of the Journal's
    // "Hostiles remaining" row. Recorded at spawn time rather than derived, because the roster is
    // culled by zoneApplyRemembered on re-entry and a live re-count would show a shrinking total.
    u16 m_zoneHostilesAtEntry = 0;
```

Set it at the end of `Engine::spawnZoneContents` in `src/engine/engine_zone.cpp`:

```cpp
    m_zoneHostilesAtEntry = zoneHostilesAlive();
```

- [ ] **Step 6: Build**

Run: `cmake --build build`
Expected: compiles clean.

- [ ] **Step 7: Commit**

```bash
git add src/renderer/hud_journal.cpp src/renderer/hud.h src/engine/engine_hud.cpp src/engine/engine.h src/engine/engine_zone.cpp CMakeLists.txt
git commit -m "feat(ui): draw the quest journal

Act tabs, the act's quest list, and the selected quest's narration + objective rows.
Narration is word-wrapped against the panel, which is the actual fix for the reported
problem: the chat line truncates at a fixed buffer and was cutting nine of ten quest
blurbs mid-sentence, several of them losing the part that says what to do.

A LOCKED quest renders as '? ? ?' rather than being hidden, so the act visibly has
more to give without spoiling what. The CLEAR_ZONE row reads the same live pool count
the completion poll reads, so the number shown and the number that completes the quest
cannot diverge.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 8: Reach the Journal — panel navigation

**Files:**
- Modify: `src/engine/engine.h:1681-1694` (panel constants), `src/engine/engine_inventory.cpp:398-410` (shoulder cycle), `:562-580` (mouse)
- Modify: `src/engine/engine_hud.cpp:148-151` — **the draw call site, moved here from Task 7.** It
  cannot compile until the constants below exist, and wiring it alone would put a panel in the
  shoulder cycle that `inventoryCursorToMouse` falls through to its skill-bar branch — reachable but
  unnavigable. `HUD::drawJournalPanel` and `m_zoneHostilesAtEntry` already exist and are currently
  UNUSED; if this step is skipped the Journal is built and never drawn.

- [ ] **Step 1: Add the panel constants**

In `src/engine/engine.h`, replace the panel constant block at lines 1681-1694 with:

```cpp
    static constexpr u8 INV_PANEL_BACKPACK    = 0;
    static constexpr u8 INV_PANEL_EQUIPMENT   = 1;
    static constexpr u8 INV_PANEL_CLASS_SKILL = 2;
    static constexpr u8 INV_PANEL_EQUIP_SKILL = 3;
    static constexpr u8 INV_PANEL_BUILD       = 4;
    // The quest journal joins the shoulder cycle so controller and Switch reach it exactly as they
    // reach the build grid. Appended, and STASH moves up with it — STASH sits OUTSIDE the cycle
    // (it is entered from the town stash chest, not by cycling), so its value is just "the next
    // free panel id" and moving it costs nothing.
    static constexpr u8 INV_PANEL_JOURNAL     = 5;
    static constexpr u8 INV_PANEL_COUNT       = 6;   // main-inventory cycle length
    static constexpr u8 INV_PANEL_STASH       = 6;
```

Add the two cursor members beside `m_invCursorBuild`:

```cpp
    u8 m_invCursorQuest = 0;    // selected row within the visible act's quest list
    u8 m_invJournalAct  = 0;    // 0 = ACT I, 1 = ACT II
```

- [ ] **Step 2: Verify nothing hard-coded the old STASH value**

Run: `grep -rn "INV_PANEL_STASH\|== 5\b" src/engine/engine_inventory.cpp src/engine/engine_hud.cpp | head`
Expected: every reference is via the named constant. If any literal `5` compares against a panel, replace it with the constant — that is exactly how the shift would silently break the stash.

- [ ] **Step 3: Add journal navigation**

In `src/engine/engine_inventory.cpp`, after the `INV_PANEL_BUILD` navigation block (which ends around line 430), add:

```cpp
        // Journal: D-pad up/down walks the visible act's quest list, left/right flips the act tab.
        // Read-only by design — there is nothing to activate, so nothing can be mis-pressed.
        if (m_invCursorPanel == INV_PANEL_JOURNAL) {
            // How many quests the visible act actually has; the cursor must never point past it.
            u8 rows = 0;
            for (u32 i = 0; i < Quest::COUNT; i++)
                if (Quest::actOf(Quest::QUESTS[i].zoneFloor) == m_invJournalAct + 1) rows++;

            if (navU && m_invCursorQuest > 0)            m_invCursorQuest--;
            if (navD && m_invCursorQuest + 1 < rows)     m_invCursorQuest++;
            if (navL && m_invJournalAct > 0)  { m_invJournalAct--; m_invCursorQuest = 0; }
            if (navR && m_invJournalAct < 1)  { m_invJournalAct++; m_invCursorQuest = 0; }
            // Flipping the act resets the row: a cursor left at row 4 on an act with 2 quests
            // would draw no detail pane and read as the journal being broken.
            if (m_invCursorQuest >= rows) m_invCursorQuest = 0;
        }
```

- [ ] **Step 4: Add mouse handling**

In `src/engine/engine_inventory.cpp`, after the build-grid mouse block (around line 580), add:

```cpp
                const InventoryUI::SlotHit jr = InventoryUI::hitTestJournal(sw, sh, mx, my);
                if (jr.panel == InventoryUI::SlotHit::JOURNAL_TAB) {
                    m_invJournalAct  = jr.index;
                    m_invCursorQuest = 0;
                    m_invCursorPanel = INV_PANEL_JOURNAL;
                    AudioSystem::play(SfxId::UI_MOVE);
                }
                if (jr.panel == InventoryUI::SlotHit::JOURNAL_ROW) {
                    m_invCursorQuest = jr.index;
                    m_invCursorPanel = INV_PANEL_JOURNAL;
                    AudioSystem::play(SfxId::UI_MOVE);
                }
```

Gate both on `m_invCursorPanel == INV_PANEL_JOURNAL || <the click landed in the journal>` so a click in the build grid's column while the build panel is up is not stolen by the journal — the two panels share the right column and only one draws at a time, so route the hit-test only when the journal is the active panel:

```cpp
            if (m_invCursorPanel == INV_PANEL_JOURNAL) {
                // ... the two blocks above ...
            }
```

- [ ] **Step 4b: Wire the draw call site (carried over from Task 7)**

In `src/engine/engine_hud.cpp`, replace the unconditional `drawBuildGrid` call at lines 148-151 with:

```cpp
    // The right column hosts EITHER the build grid or the journal — one panel at a time, chosen by
    // which the cursor is on. The stash panel would overlap both, so neither draws in stash mode.
    if (!m_stashOpen) {
        if (m_invCursorPanel == INV_PANEL_JOURNAL) {
            const u8 liveIdx = m_level.inZone ? Quest::indexForZone(m_level.zoneFloor) : 0xFF;
            HUD::drawJournalPanel(sw, sh, m_questProgress[m_localPlayerIndex],
                                  m_invCursorQuest, m_invJournalAct,
                                  liveIdx, zoneHostilesAlive(), m_zoneHostilesAtEntry,
                                  invMX, invMY);
        } else {
            HUD::drawBuildGrid(sw, sh, m_inventories[m_localPlayerIndex].autoMode,
                               m_inventories[m_localPlayerIndex].buildCell, invMX, invMY);
        }
    }
```

- [ ] **Step 5: Build and check it live**

Run: `cmake --build build && timeout 90 DISPLAY=:1 ./build/src/DungeonEngine --load 4 --endgame --zone 52`

**This is the first time the Journal is rendered by anything.** Task 7 verified its arithmetic and
its word-wrapping by execution, but nothing about how it LOOKS has ever been checked — not legibility,
not collision with the backpack or the build column, not vertical fit away from 720p, not hover.
Take a screenshot (**F8**, `F10` hides the HUD) and actually look at it.

Manual check: open the inventory (`I` / `Tab`), cycle with the shoulder buttons or click the journal column. Confirm:
- the Journal appears in the cycle after Build,
- ACT I / ACT II tabs flip with left/right,
- up/down walks the quest list and the detail pane follows,
- a completed quest is gold, an unknown one reads `? ? ?`,
- the stash chest still opens the stash (the panel id shift did not break it).

- [ ] **Step 6: Commit**

```bash
git add src/engine/engine.h src/engine/engine_inventory.cpp
git commit -m "feat(ui): reach the journal from the inventory shoulder cycle

INV_PANEL_JOURNAL joins the cycle so controller and Switch reach it the same way they
reach the build grid; STASH shifts to 6 (it sits outside the cycle, so its id is just
the next free number). The act flip resets the row cursor — left on row 4 of a
two-quest act would draw no detail pane and read as a broken journal.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 9: Repair the chat line

Narration now lives in the Journal, so quest chat lines are short by design. The truncation bug is still real for every other caller and dies here.

**Files:**
- Modify: `src/engine/engine.h:1086-1090`, `src/engine/engine.cpp:1117-1126`

- [ ] **Step 1: Widen the buffer and drop the empty separator**

In `src/engine/engine.h`, change line 1087:

```cpp
    static constexpr u32 CHAT_LINE_LEN = 128;   // was 48 — see addChatMessage
```

In `src/engine/engine.cpp`, replace `addChatMessage`'s formatting line (1123) with:

```cpp
    // A speakerless line is a SYSTEM line (quest offers, act completions, gate refusals) and must
    // not be prefixed: "%s: %s" with an empty speaker rendered every one of them with a stray
    // leading ": ". The buffer was also 48 bytes, which truncated nine of the ten quest blurbs
    // mid-sentence — several losing the clause that said what to do.
    if (speaker && speaker[0])
        std::snprintf(m_chatLog[0].text, CHAT_LINE_LEN, "%s: %s", speaker, msg);
    else
        std::snprintf(m_chatLog[0].text, CHAT_LINE_LEN, "%s", msg);
```

- [ ] **Step 2: Check the HUD can render the longer line**

Run: `grep -n "chatX\|drawText(sw, sh, chatX" src/engine/engine_hud.cpp | head`

Read the surrounding draw. If the chat column has a fixed pixel width that a 128-char line would overrun, clamp the DRAW rather than the buffer — truncating at storage time is what caused this bug. Note the finding in the commit message either way.

- [ ] **Step 3: Verify live**

Run: `timeout 60 DISPLAY=:1 ./build/src/DungeonEngine --load 4 --endgame --zone 55 2>&1 | grep -m2 "QUEST"`

Manual check: the on-screen offer line for *The Rebaser* now reads to the end — "…Stop whatever is rewriting it." — with no leading colon.

- [ ] **Step 4: Commit**

```bash
git add src/engine/engine.h src/engine/engine.cpp
git commit -m "fix(chat): stop truncating messages and prefixing systems lines with ': '

addChatMessage formatted '%s: %s' into a 48-byte buffer. Nine of the ten quest blurbs
were cut mid-sentence — The Rebaser lost 'Stop whatever is rewriting it', Kill -9 ended
at 'Terminat' — and every speakerless line rendered with a stray leading colon.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

# PHASE 3 — NPC quest givers

## Task 10: Resolve which quest a giver is holding

**Files:**
- Modify: `src/game/quest_state.h`
- Modify: `tests/game/test_quest_state.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/game/test_quest_state.cpp`:

```cpp
// Giver 0 (Akara) holds quests 0 and 1; giver 1 (Charsi) holds 2, 3 and 4.
TEST_CASE("a giver offers its first unfinished quest, in table order") {
    Quest::Progress p{};
    REQUIRE(Quest::giverOutstanding(p, 0) == 0);

    p.state[0] = static_cast<u8>(Quest::State::COMPLETE);
    REQUIRE(Quest::giverOutstanding(p, 0) == 1);

    p.state[1] = static_cast<u8>(Quest::State::COMPLETE);
    REQUIRE(Quest::giverOutstanding(p, 0) == 0xFF);   // nothing left to give
}

TEST_CASE("a giver with an in-progress quest keeps offering that one") {
    Quest::Progress p{};
    Quest::offer(p, 0);
    Quest::noteTalk(p, 0);
    REQUIRE(Quest::giverOutstanding(p, 0) == 0);      // ACTIVE, not COMPLETE — still theirs
}

TEST_CASE("an unknown giver index yields nothing rather than reading out of bounds") {
    Quest::Progress p{};
    REQUIRE(Quest::giverOutstanding(p, 200) == 0xFF);
}

TEST_CASE("each giver's quests all belong to that giver's own act") {
    for (u32 i = 0; i < Quest::COUNT; i++) {
        const Quest::QuestDef& q = Quest::QUESTS[i];
        const Quest::GiverDef& g = Quest::GIVERS[q.giverIdx];
        CAPTURE(q.name);
        CAPTURE(g.name);
        // Act 1's hub is the town (98), Act 2's is Null Terminus (61). A giver standing in the
        // wrong hub can never be reached at the point its quest is relevant.
        REQUIRE(Quest::actOf(q.zoneFloor) == (g.hubFloor == 98 ? 1 : 2));
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target dungeon_tests`
Expected: FAIL to compile — `'giverOutstanding' is not a member of 'Quest'`.

- [ ] **Step 3: Implement it**

Append to `src/game/quest_state.h`, inside `namespace Quest`:

```cpp
// The quest this giver currently has to hand out: their first non-COMPLETE quest in table order,
// which is the road's walking order. 0xFF = they have nothing left.
//
// One quest at a time on purpose. A giver who dumps their whole act at once turns the Journal into
// a wall of text on the first conversation and removes any sense of the chain advancing.
inline u8 giverOutstanding(const Progress& p, u8 giverIdx) {
    if (giverIdx >= GIVER_COUNT) return 0xFF;
    for (u32 i = 0; i < COUNT; i++) {
        if (QUESTS[i].giverIdx != giverIdx) continue;
        if (p.state[i] != static_cast<u8>(State::COMPLETE)) return static_cast<u8>(i);
    }
    return 0xFF;
}
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*quest*,*migrat*,*giver*,*TALK*,*ACTIVATE*,*objective*"`
Expected: PASS — 24 test cases.

- [ ] **Step 5: Commit**

```bash
git add src/game/quest_state.h tests/game/test_quest_state.cpp
git commit -m "feat(quest): resolve a giver's outstanding quest

One quest at a time, in table order. A giver who hands over their whole act at once
turns the Journal into a wall of text on the first conversation and removes any sense
of the chain advancing. Pinned: every giver's quests belong to that giver's own act,
or the NPC stands in a hub the player has no reason to visit when it matters.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 11: Make givers interactable

**Files:**
- Modify: `src/game/entity.h` (a giver field), `src/game/interact.h`, `src/engine/engine.h` (`InteractState`), `src/engine/engine_update.cpp:2292+`

- [ ] **Step 1: Mark an entity as a giver**

In `src/game/entity.h`, beside the existing `nameTag` member (line 230), add:

```cpp
    // Quest-giver index into Quest::GIVERS, or 0xFF for every other entity. Transient pool state —
    // Entity is never serialized, so this costs static memory and nothing on the wire.
    u8 questGiver = 0xFF;
```

- [ ] **Step 2: Add the interact target**

In `src/game/interact.h`, **append** to the target enum:

```cpp
enum struct Target : u8 { NONE, ITEM, SHRINE, EXIT, NPC };
```

This enum is a per-frame resolve and is not serialized, so appending is free — but append anyway, so a future serialization cannot inherit a reordered set.

In `src/engine/engine.h`, add to `InteractState` (after `zoneGateIdx`):

```cpp
        s32 npcIdx = -1;   // entity-pool index of the nearest quest giver in reach (-1 = none)
```

- [ ] **Step 3: Resolve givers in `resolveInteractTargets`**

In `src/engine/engine_update.cpp`, add `st.npcIdx = -1;` to the reset block at line 2292, then add this loop **after** the world-item loop and before the function returns:

```cpp
    // Quest givers are ENTITIES, not world items, so they need their own pass. Scored the same way
    // (aim first, then distance) so the prompt the player sees and the target the button acts on
    // are chosen by one rule.
    f32 bestGiver = -2.0f;
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        const u32 i = m_entities.activeList[a];
        const Entity& e = m_entities.entities[i];
        if (e.questGiver == 0xFF || (e.flags & ENT_DEAD)) continue;

        const Vec3 to = e.position - m_localPlayer.position;
        const f32 hDist = sqrtf(to.x * to.x + to.z * to.z);
        const f32 hLen  = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
        const f32 dot   = (hDist > 0.01f && hLen > 0.01f)
                        ? (fwd.x * to.x + fwd.z * to.z) / (hDist * hLen) : 1.0f;
        if (!Interact::inReach(hDist, dot, GameConst::INTERACT_RANGE,
                               GameConst::INTERACT_GRAB_RADIUS, GameConst::INTERACT_MIN_DOT,
                               fabsf(to.y)))
            continue;

        const f32 score = dot - hDist * 0.1f;
        if (score > bestGiver) { bestGiver = score; st.npcIdx = static_cast<s32>(i); }
    }
```

- [ ] **Step 4: Include the giver in the "something to interact with" test**

Line 2429 builds the flag that arms the interact button. Add `|| (st.npcIdx >= 0)`:

```cpp
                               (st.stashIdx >= 0) || (st.waypointIdx >= 0) ||
                               (st.zoneGateIdx >= 0) || (st.npcIdx >= 0);
```

- [ ] **Step 5: Act on it**

In the same file, extend the local-fixture block at line 2458. A giver is resolved locally on every role for the same reason a waypoint is — it grants nothing, changes no world state, and only writes per-character progress:

```cpp
    // Talking is LOCAL on every role. It grants nothing and mutates no world state: it records a
    // per-character objective and opens a UI. Routing it through the server would be a packet that
    // could only ever tell the host something about a client's own journal.
    if (wantItem && st.npcIdx >= 0 && st.itemIdx < 0) {
        talkToGiver(st.npcIdx);
        return;
    }

    if (wantItem && (st.waypointIdx >= 0 || st.zoneGateIdx >= 0)) {
```

Note the `st.itemIdx < 0` guard: real loot at your feet must still win the button, matching the tier rule the item scoring already applies.

- [ ] **Step 6: Show the prompt**

In `src/engine/engine_render_world.cpp`, beside the stash prompt at line 1031, add:

```cpp
    // "Speak to <name>" — the giver's own name, so the player knows who they are walking up to
    // before they press anything. Same shape and screen height as the stash prompt directly above:
    // both are town fixtures the player walks between.
    if (st.npcIdx >= 0 && st.itemIdx < 0 && m_gameState == GameState::IN_GAME) {
        const Entity& g = m_entities.entities[st.npcIdx];
        if (g.questGiver < Quest::GIVER_COUNT) {
            char prompt[96];
            std::snprintf(prompt, sizeof(prompt), "Speak to %s", Quest::GIVERS[g.questGiver].name);
            drawPrompt(prompt, {0.75f, 0.85f, 1.0f}, static_cast<f32>(sh) * 0.45f);
        }
    }
```

`drawPrompt(text, colour, y)` is the helper every other prompt in this function uses (`"Open Stash"`,
`"Enter the Dungeon"`, the shrine name). It takes no button glyph — do not invent one. **It is a
local LAMBDA declared at `engine_render_world.cpp:998`, not a member function**, so this block must
sit inside that same function after that line; calling it from anywhere else will not compile.
`GameConst::INTERACT_RANGE` / `INTERACT_GRAB_RADIUS` / `INTERACT_MIN_DOT` live in
`src/game/game_constants.h` and are all real.

- [ ] **Step 7: Build**

Run: `cmake --build build`
Expected: compiles clean. If `talkToGiver` is undefined, that is expected — it lands in Task 12. Stub it for this step:

```cpp
void Engine::talkToGiver(s32 /*entityIdx*/) {}
```

declared in `src/engine/engine.h` beside the quest hooks.

- [ ] **Step 8: Commit**

```bash
git add src/game/entity.h src/game/interact.h src/engine/engine.h src/engine/engine_update.cpp src/engine/engine_render_world.cpp
git commit -m "feat(quest): quest givers are interactable entities

Entity::questGiver + an InteractState::npcIdx pass. Givers are entities rather than
world items, so they need their own scoring loop — scored by the same aim-then-distance
rule the item pass uses, so the prompt shown and the target acted on are chosen once.

Resolved LOCALLY on every role, like the waypoint and the zone gate: talking grants
nothing and changes no world state, so there is nothing for the server to arbitrate.
Real loot at your feet still wins the button.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 12: Talk — offer the quest and open the Journal

**Files:**
- Modify: `src/engine/engine_zone.cpp` (add `talkToGiver`), `src/engine/engine_town.cpp:117-131`, `src/engine/engine_zone.cpp` (`spawnZoneContents`)

- [ ] **Step 1: Implement `talkToGiver`**

Replace the stub in `src/engine/engine_zone.cpp`:

```cpp
// Talking to a giver: their line goes to chat for flavour, their outstanding quest is offered and
// its TALK objective ticked, and the Journal opens on that quest.
//
// The Journal opening IS the dialogue. Quest text lives in exactly one place, so it can never
// truncate or disagree with itself — and the first conversation teaches the player where the
// Journal is, which a separate dialogue box would not.
void Engine::talkToGiver(s32 entityIdx) {
    if (entityIdx < 0 || static_cast<u32>(entityIdx) >= MAX_ENTITIES) return;
    const Entity& g = m_entities.entities[entityIdx];
    if (g.questGiver >= Quest::GIVER_COUNT) return;

    const u8 lane = m_localPlayerIndex;
    const u8 q    = Quest::giverOutstanding(m_questProgress[lane], g.questGiver);
    const Quest::GiverDef& gd = Quest::GIVERS[g.questGiver];

    addChatMessage(gd.name, gd.greeting, Vec3{0.80f, 0.85f, 0.95f});

    if (q == 0xFF) {
        addChatMessage(gd.name, "Nothing more from me. Not yet.", Vec3{0.65f, 0.68f, 0.75f});
        LOG_INFO("[QUEST] talked to %s - nothing outstanding", gd.name);
        return;
    }

    const bool wasComplete = Quest::isComplete(m_questMask[lane], Quest::QUESTS[q].zoneFloor);
    Quest::offer(m_questProgress[lane], q);
    Quest::noteTalk(m_questProgress[lane], q);
    refreshQuestMask(lane);
    LOG_INFO("[QUEST] talked to %s -> %s", gd.name, Quest::QUESTS[q].name);
    questAnnounce(q, wasComplete);

    // Open the inventory straight onto this quest.
    m_inventoryOpen   = true;
    m_invCursorPanel  = INV_PANEL_JOURNAL;
    m_invJournalAct   = static_cast<u8>(Quest::actOf(Quest::QUESTS[q].zoneFloor) - 1);
    // The cursor is a ROW in the visible act's list, not a quest index — count this quest's
    // position within its own act.
    u8 row = 0;
    for (u32 i = 0; i < q; i++)
        if (Quest::actOf(Quest::QUESTS[i].zoneFloor) == m_invJournalAct + 1) row++;
    m_invCursorQuest = row;
    AudioSystem::play(SfxId::UI_CONFIRM);
}
```

- [ ] **Step 2: Name the town's givers**

In `src/engine/engine_town.cpp`, replace the townsfolk block (lines 117-131) with:

```cpp
    // --- Townsfolk: the companion cast at plaza posts ---
    // Two of the six are Act 1's QUEST GIVERS. They are ordinary friendly NPCs with a giver index
    // rather than a new entity kind — the town already spawns and seats these.
    //
    // NOT gated on role, unlike the rest of this function. `Entity::questGiver` is a POOL field and
    // is NOT in SnapEntity, so a host-only spawn replicated by snapshot reaches a guest with
    // questGiver == 0xFF — no prompt, no conversation, ever. The town is deterministic geometry
    // every peer rebuilds from the sentinel seed, so spawning givers locally on each peer is both
    // safe and the only thing that works. Talking mutates nothing shared (see talkToGiver), so
    // there is nothing for the host to arbitrate.
    {
        const u8 floor = 1;   // town NPCs use base-floor stats; they never fight anyway
        const Vec3 posts[6] = {
            center + Vec3{-4.0f, 0.0f, -1.0f}, center + Vec3{ 4.0f, 0.0f, -1.0f},
            center + Vec3{ 0.0f, 0.0f,  4.0f}, center + Vec3{-9.0f, 0.0f,  7.0f},
            center + Vec3{ 9.0f, 0.0f,  7.0f}, center + Vec3{ 0.0f, 0.0f, -8.0f},
        };
        const NpcClass kinds[6] = {NpcClass::CLERIC, NpcClass::ROGUE, NpcClass::ARCHER,
                                   NpcClass::CLERIC, NpcClass::ROGUE, NpcClass::ARCHER};
        // Posts 0 and 1 flank the stash, which is where the player already walks — a giver nobody
        // passes is a giver nobody talks to. But they must not stand INSIDE the stash's own interact
        // reach: the posts sit 4.47 m from the chest against a 3.5 m INTERACT_RANGE, and both
        // prompts render at screen height 0.45, so a player between them would see two prompts on
        // one line. Push these two posts outward before use and re-measure — the constraint is
        // `dist(post, stashPos) > GameConst::INTERACT_RANGE + player reach`, not a fixed number.
        const u8 givers[6] = {0, 1, 0xFF, 0xFF, 0xFF, 0xFF};
        for (u32 n = 0; n < 6; n++) {
            EntityHandle h = spawnFriendlyNpc(posts[n], kinds[n], floor);
            Entity* npc = handleGet(m_entities, h);
            if (npc) {
                npc->homePosition = posts[n];   // the post the town-mode AI holds
                npc->questGiver   = givers[n];
                if (givers[n] != 0xFF) npc->nameTag = Quest::GIVERS[givers[n]].name;
            }
        }
    }
```

- [ ] **Step 3: Spawn Act 2's givers in Null Terminus**

In `src/engine/engine_zone.cpp`, inside `spawnZoneContents`, after the existing content spawns and before the `m_zoneHostilesAtEntry` line added in Task 7:

```cpp
    // Act 2's quest givers stand on Null Terminus, the act's one peaceful platform — its hub, the
    // way the town is Act 1's. Placed relative to the zone centre, which spawnZoneContents has
    // already cleared for the boss/return-gate pads.
    // Not role-gated, for the reason the town givers are not: questGiver is a pool field, absent
    // from SnapEntity, so a guest would never see them.
    if (def.floor == 61) {
        // `center` is spawnZoneContents' own parameter — there is no zoneCenterPos() accessor.
        const Vec3 posts[2] = { center + Vec3{-3.0f, 0.0f, 2.0f},
                                center + Vec3{ 3.0f, 0.0f, 2.0f} };
        const u8   givers[2] = { 2, 3 };
        for (u32 n = 0; n < 2; n++) {
            EntityHandle h = spawnFriendlyNpc(posts[n], NpcClass::CLERIC, /*floor*/ 1);
            Entity* npc = handleGet(m_entities, h);
            if (npc) {
                npc->homePosition = posts[n];
                npc->questGiver   = givers[n];
                npc->nameTag      = Quest::GIVERS[givers[n]].name;
            }
        }
    }
```

Signature for reference, verified against the tree: `void Engine::spawnZoneContents(const Zone::ZoneDef& def, Vec3 center)` (`engine_zone.cpp:448`).

- [ ] **Step 4: Build and verify live**

Run: `cmake --build build && timeout 120 DISPLAY=:1 ./build/src/DungeonEngine --load 4 --town`

Manual check:
- two named NPCs flank the stash, with nameplates,
- walking up to one shows `[E] Speak to Akara, the Allocator`,
- pressing interact prints their greeting to chat AND opens the inventory on the Journal, focused on *Free the Allocation*,
- the quest's TALK row is ticked,
- talking again with nothing outstanding gives the "Nothing more from me" line and does not reopen the journal on a stale quest.

Then Act 2:

```bash
timeout 120 DISPLAY=:1 ./build/src/DungeonEngine --load 4 --endgame --zone 61
```

Expected: two givers on the platform, same behaviour.

- [ ] **Step 5: Verify the act soak still passes**

This is the check that the TALK rule really is non-blocking — the bot cannot talk to anyone.

Run: `python3 tools/overworld_soak.py`
Expected: **9/9** classes complete Act 2's last quest, `deaths == revives`, zero crashes.

**If any class fails to complete, STOP.** Either a TALK objective became blocking, or a giver's spawn displaced something the bot needs.

- [ ] **Step 6: Commit**

```bash
git add src/engine/engine_zone.cpp src/engine/engine_town.cpp src/engine/engine.h
git commit -m "feat(quest): talking to a giver opens the journal on their quest

The Journal opening IS the dialogue: quest text lives in exactly one place so it can
never truncate or disagree with itself, and the first conversation teaches the player
where the Journal is. Act 1's givers flank the town stash (a giver nobody walks past
is a giver nobody talks to); Act 2's stand on Null Terminus, the act's own hub.

Verified against the 9-class act soak: the bot cannot talk to anyone and still
completes both acts, which is the real proof the TALK objective never gates.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

# PHASE 4 — The Cairn Stones

## Task 13: Generate the stone

**Files:**
- Modify: `tools/gen_mesh.py`, `tools/gen_skin.py`, `tools/build_assets.py`, `src/engine/asset_manifest.h`, `assets/materials.json`

- [ ] **Step 1: Write the mesh generator**

In `tools/gen_mesh.py`, add a `gen_cairn_stone()` modelled on the existing `gen_cave_mouth` / voxel-model functions. Build a `set` of filled `(gx, gy, gz)` voxels with the local `fill_box` helper, feet at Y=0, then `add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))`.

Shape: a leaning monolith roughly **0.8 m wide x 2.4 m tall x 0.5 m deep**, wider at the base than the top, with the top corner chipped away so it reads as *raised and weathered* rather than as a wall segment. **Record the filled grid extents** (`min/max gx`, `min/max gy`) — Step 2 needs them, and `add_voxel_model` derives `tex_w`/`tex_h` from the filled set itself, so a skin sized to a nominal grid would be stretched across the model.

Register it in `MESH_TYPES`:

```python
"cairn_stone": {"func": gen_cairn_stone, "desc": "Standing stone for the Cairn Stones quest. Params: --height", "default_file": "cairn_stone.obj"},
```

- [ ] **Step 2: Write the skin generator**

In `tools/gen_skin.py`, add `skin_cairn_stone()` returning `(w, h, pixel_map)` where `w = max_gx - min_gx + 1` and `h = max_gy - min_gy + 1` **from Step 1's measured extents**, not from a nominal size.

Palette: weathered grey granite, with a band of faintly luminous carved glyphs across the upper third — the glyphs are what let a player tell an *aligned* stone from an unaligned one at a glance, since the engine tints the whole body by its material.

Register it:

```python
"cairn_stone": ("cairn_stone_skin_42.png", skin_cairn_stone),
```

- [ ] **Step 3: Add both to the asset build**

In `tools/build_assets.py`, add to `build_meshes()`'s `meshes = [...]`:

```python
["--type", "cairn_stone", "--height", "2.4", "--out", os.path.join(mesh_dir, "cairn_stone.obj")],
```

and to `build_skins()`'s `skins = [...]`:

```python
("cairn_stone", "cairn_stone_skin_42.png"),
```

- [ ] **Step 4: Register the mesh with the engine**

In `src/engine/asset_manifest.h`, add a row to the mesh table:

```cpp
    {"cairn_stone", "assets/meshes/cairn_stone.obj"},
```

**Both this and `build_assets.py` must be edited.** `tools/build_assets.py` hard-fails if the engine names a mesh it does not generate, which is the guard that stops a missing mesh shipping as an invisible fallback cube.

- [ ] **Step 5: Add the material**

Append to `assets/materials.json`, with `id` equal to its array index. Verified against the file:
it currently holds **193 entries, last id 192 (`service_door_skin`)**, so the new row is **id 193**.
Re-check before writing — if anything else landed a material meanwhile, the id must still equal the
array index or `MaterialSystem` resolves the wrong texture.

```json
{ "id": 193, "name": "cairn_stone_skin", "texture": "cairn_stone_skin_42.png" }
```

- [ ] **Step 6: Generate and verify**

Run: `python3 tools/build_assets.py --meshes --skins`
Expected: `assets/meshes/cairn_stone.obj` and `assets/textures/cairn_stone_skin_42.png` are written, with no hard-fail.

Run: `python3 tools/check_skin_grids.py`
Expected: no size mismatch reported for `cairn_stone`.

Run: `cmake --build build && ./build/tests/dungeon_tests -tc="*asset manifest*"`
Expected: PASS — the manifest test checks the table against the generated `.obj` files.

- [ ] **Step 7: Commit**

```bash
git add tools/gen_mesh.py tools/gen_skin.py tools/build_assets.py src/engine/asset_manifest.h assets/materials.json
git commit -m "feat(assets): cairn_stone mesh + skin for the Standing Stones quest

A leaning weathered monolith with a band of carved glyphs across the upper third — the
glyphs are what let an aligned stone read differently from an unaligned one at a glance,
since the engine tints the whole body by its material.

Skin sized to the mesh's MEASURED filled extents, not a nominal grid: add_voxel_model
derives tex_w/tex_h from the filled set, so a nominally-sized skin is stretched across
the model and every band lands somewhere else.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 14: The stones as world fixtures

**Files:**
- Modify: `src/game/item.h:421`, `src/game/world_item.cpp`, `src/engine/engine_zone.cpp`, `src/engine/engine_update.cpp`, `src/engine/engine_render_world.cpp`, `src/renderer/minimap.cpp`

- [ ] **Step 1: Add the sentinel**

In `src/game/item.h`, after `ZONE_GATE_ID` (line 421):

```cpp
// The Cairn Stones (quest 56). Riding the sentinel path buys spawning, replication, server-side
// validation and the fixture despawn exemption — the last for free, since that rule is now derived
// as "any sentinel except the globe" rather than hand-listed.
static constexpr u16 CAIRN_STONE_ID = 0xFFF3;
inline bool isCairnStone(const ItemInstance& i) { return i.defId == CAIRN_STONE_ID; }
```

Add it to `isSentinelItem()` in the same file.

- [ ] **Step 2: Verify the despawn exemption covers it automatically**

Run: `grep -n -A 12 "isSentinelItem" src/game/world_item.cpp | head -24`

Expected: the decay exemption reads "any sentinel except the globe", so `CAIRN_STONE_ID` is exempt with no edit. **If it is a hand-listed set instead, add the stone to it** — waypoints and zone gates evaporating after 60 seconds was the worst bug of the overworld review, caused by exactly that list.

Then confirm with the existing test:

Run: `./build/tests/dungeon_tests -tc="*fixture*"`
Expected: PASS.

- [ ] **Step 0: Flip quest 56 to ACTIVATE — in THIS commit, not earlier**

Task 2 deliberately left quest 56 as `TALK + CLEAR_ZONE`. `ZoneRoute::linkOpen` gates the onward road
on `zoneSettled(56)`, so a quest whose trigger has no implementation seals the way to TristRAM and
strands anyone on that commit. The trigger and the thing that satisfies it change together, here.

In `src/game/quest_def.h`, replace quest 56's row body with:

```cpp
          "Monuments to abandoned features, and none of them agree. Align them.",
          "Five stones, each raised for something that was going to be finished. They disagree "
          "about what the field was for, and while they disagree the way to TristRAM stays shut. "
          "Touch each in turn and let them settle it.",
          /*giver*/ 1, /*objCount*/ 2,
          { { Trigger::TALK,     "Speak to Charsi", "",      1 },
            { Trigger::ACTIVATE, "Stones aligned",  "cairn", 5 } } },
```

Do this FIRST so the remaining steps build against the trigger they satisfy, and confirm at Step 7
that the quest actually completes at 5/5.

- [ ] **Step 3a: Record the five anchors in `buildZoneLevel` — NOT in `spawnZoneContents`**

This split is load-bearing and is the single easiest thing to get wrong here. `buildZoneLevel` runs
**before** `LevelMeshSystem::buildAll`; `spawnZoneContents` runs **after** it. A pad cleared in
`spawnZoneContents` behaves as floor while still **rendering as rock** — the exact trap the lava exit
pad documents and that the zone arrival points were fixed for. Clearing happens in the builder;
spawning reads what the builder recorded.

In `src/engine/engine.h`, beside `m_zoneWaypointPos` / `m_zonePoiPos`:

```cpp
    // The five Cairn Stone anchors (zone 56 only). Recorded by buildZoneLevel so the cleared ground
    // and the fixture that stands on it can never disagree — the rule the waypoint/POI anchors
    // already follow.
    Vec3 m_zoneCairnPos[5] = {};
```

In `src/engine/engine_zone.cpp`'s `buildZoneLevel`, immediately after the existing waypoint/POI
anchor block (the one commented "Both fixtures ride ROOM CENTRES"), add:

```cpp
    // The Cairn Stones ride room centres for the same reason the waypoint and POI do: every layout
    // style guarantees a room centre open and connected, while a fixture stamped at a fraction of
    // the grid can generate inside rock — which is how the Bank Station portal once put Act 2
    // behind a wall. Five distinct fractions spread them across the field.
    if (def.floor == 56) {
        static constexpr f32 CAIRN_FRAC[5][2] = {
            {0.25f, 0.25f}, {0.75f, 0.25f}, {0.50f, 0.50f}, {0.25f, 0.75f}, {0.75f, 0.75f},
        };
        for (u8 s = 0; s < 5; s++) {
            const Vec3 c = zoneRoomCentre(gen, zoneAnchorRoom(gen, zoneSeed,
                                                              CAIRN_FRAC[s][0], CAIRN_FRAC[s][1]));
            m_zoneCairnPos[s] = c;
            zoneClearPad(static_cast<u32>(c.x), static_cast<u32>(c.z), 2);
        }
    }
```

Two fractions can resolve to the SAME room on a small grid, which would stack two stones. Verify in
Step 7 that five distinct positions come back; if any collide, nudge that fraction rather than
adding a de-duplication pass — the anchors must stay a pure function of the seed, or host and client
would clear different cells from the same seed and desync (the lesson the arrival pads already
carry: anything that varies with how a player got there cannot be part of the world).

- [ ] **Step 3b: Spawn the stones from the recorded anchors**

In `src/engine/engine_zone.cpp`'s `spawnZoneContents`:

```cpp
    // The five Cairn Stones (quest 56), standing on the pads buildZoneLevel already cleared.
    //
    // `affixCount` carries the stone's ORDINAL (0..4) so the interact handler knows which of the
    // five it is without a side table. The ordinal is stable across runs because the anchors are a
    // pure function of the zone seed, so spawn order is too — which is what lets the aligned
    // BITMASK survive the zone being rebuilt on every entry.
    if (def.floor == 56 && m_netRole != NetRole::CLIENT) {
        for (u8 s = 0; s < 5; s++) {
            ItemInstance stone{};
            stone.defId      = CAIRN_STONE_ID;
            stone.uid        = m_worldItems.nextUid++;
            stone.affixCount = s;
            WorldItemSystem::spawn(m_worldItems, stone, m_zoneCairnPos[s], &m_level.grid);
        }
    }
```

- [ ] **Step 4: Handle the interact**

In `src/engine/engine_update.cpp`, add `st.cairnIdx = -1;` to `InteractState` and its reset, score it in the world-item loop beside `waypnt`/`zgate`, and extend the local-fixture block:

```cpp
    if (wantItem && st.cairnIdx >= 0) { touchCairnStone(st.cairnIdx); return; }
```

Declare it in `src/engine/engine.h` beside the other quest hooks:

```cpp
    void touchCairnStone(s32 itemIdx);   // align one of the five stones (zone 56)
```

Implement in `src/engine/engine_zone.cpp`:

```cpp
// Touching one of the five stones. Local on every role, like the waypoint: it records a
// per-character objective bit and grants nothing.
//
// A stone is NOT consumed. All five stay standing once aligned — they are the monument, and a
// player who comes back should find the circle they finished, not an empty field.
void Engine::touchCairnStone(s32 itemIdx) {
    if (itemIdx < 0 || static_cast<u32>(itemIdx) >= MAX_WORLD_ITEMS) return;
    const WorldItem& wi = m_worldItems.items[itemIdx];
    const u8 ordinal = wi.item.affixCount;
    if (ordinal >= 5) return;

    const u8 lane = m_localPlayerIndex;
    const u8 q    = Quest::indexForZone(56);
    if (q == 0xFF) return;
    if (Quest::isComplete(m_questMask[lane], 56)) return;

    const u8 before = Quest::objectiveProgress(m_questProgress[lane], q, 1);
    Quest::noteActivate(m_questProgress[lane], q, ordinal);
    refreshQuestMask(lane);
    const u8 after = Quest::objectiveProgress(m_questProgress[lane], q, 1);

    if (after == before) return;   // already aligned — say nothing rather than repeat the line
    char line[64];
    std::snprintf(line, sizeof(line), "The stone settles. %u of 5.", static_cast<u32>(after));
    addChatMessage("", line, Vec3{0.8f, 0.85f, 1.0f});
    AudioSystem::play(SfxId::SHRINE_ACTIVATE);
    LOG_INFO("[QUEST] cairn stone %u aligned (%u/5)", static_cast<u32>(ordinal),
             static_cast<u32>(after));
    questAnnounce(q, /*wasComplete*/ false);
}
```

- [ ] **Step 5: Render them as fixtures**

The material to resolve is **`cairn_stone_skin`** (the `_skin` suffix matches every sibling fixture —
`cave_mouth_skin`, `stone_circle_skin` — and `engine_render_world.cpp:162-168` looks them up by that
exact string). It is materials.json id **193**, already committed by Task 13.

In `src/engine/engine_render_world.cpp`, add a branch beside the waypoint/gate fixture branch —
**`Engine::renderWorldItems`, at the `bool isWayObj = isWaypoint(wi.item);` line (around 141)**.
Note the code keys off the `isWaypoint(...)` / `isZoneGate(...)` HELPERS, not the raw sentinel ids,
so add `isCairnStone(wi.item)` the same way. Feet on the floor, **no bob, no spin**, scale 1.0 (the
mesh is authored at real size).

Critically, the model-matrix chain gates the item-mesh branch on `defId < m_itemDefCount`, and a sentinel defId is far outside that range — so without an explicit branch a stone falls through to the 0.3-scale spinning cube fallback. That is exactly why the Den's cave mouth was hard to find: it was documented as 2.2x and was in fact 0.3x.

An ALIGNED stone tints brighter (`Vec3{1.3f, 1.3f, 1.1f}` multiplier) so the circle visibly fills in as you work round it. Read which are aligned from `m_questProgress[m_localPlayerIndex].obj[q][1]`'s bitmask against the item's ordinal.

- [ ] **Step 6: Put them on the minimap**

In `src/renderer/minimap.cpp`, paste the waypoint block with a cairn predicate — the fixture branch
is at the `const bool waypnt = isWaypoint(wi.item);` line (around 651), and `emitRing` already
exists (it draws the stone-circle entrance, around line 591). Use `emitRing` (the five-dot glyph already used for the stone circle entrance) in a pale blue, dim while unaligned and bright once aligned. **Winding is load-bearing** (CCW) or the icon silently culls.

- [ ] **Step 7: Build and verify live**

Run: `cmake --build build && timeout 180 DISPLAY=:1 ./build/src/DungeonEngine --load 4 --endgame --zone 56`

Manual check:
- **five stones stand in five DISTINCT positions** (the anchor-collision check from Step 3a), at full size and not spinning,
- each shows an interact prompt,
- touching one prints "The stone settles. 1 of 5." and brightens it,
- re-touching the same stone says nothing and does not advance the count,
- the minimap shows five marks, dim then bright,
- at 5/5 the quest completes and the TristRAM portal opens.

- [ ] **Step 8: Verify persistence**

Align three stones, quit to menu (saving), relaunch, and re-enter zone 56.

Expected: exactly the same three stones are bright and the Journal reads `Stones aligned 3/5`. **If the wrong three are lit, the ordinal is not surviving the rebuild** — the zone is regenerated from its seed, so the ordinal must be derived from spawn order, not from a uid that changes between runs.

- [ ] **Step 9: Commit**

```bash
git add src/game/item.h src/game/world_item.cpp src/engine/engine_zone.cpp src/engine/engine_update.cpp src/engine/engine_render_world.cpp src/renderer/minimap.cpp src/engine/engine.h
git commit -m "feat(quest): the Cairn Stones - five stones you align, not a zone you clear

Quest 56 was called 'Align the Standing Stones' and meant 'kill everything here', which
was the weakest beat in Act 1. It is now D2's beat: five stones, each touched in turn,
opening the way to TristRAM at 5/5.

Anchored on ROOM CENTRES rather than grid fractions — every layout style guarantees a
room centre open and connected, and a fraction-anchored fixture can generate inside rock
(how the Bank portal once put Act 2 behind a wall). Progress is a BITMASK of which stones,
not a count: the zone is rebuilt from its seed on every entry, so a count would re-light
the wrong ones. The stones are never consumed — they are the monument.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 15: Full verification

**Files:** none — this task only runs things.

- [ ] **Step 1: Full unit suite**

Run: `cmake --build build && ./build/tests/dungeon_tests`
Expected: every test passes, including `test_zone_def`, `test_zone_route` and `test_ai_preference`, which pin the acts' data.

- [ ] **Step 2: The act soak**

Run: `python3 tools/overworld_soak.py`
Expected: **9/9** classes complete Act 2's last quest, `deaths == revives`, zero crashes, zero routing strands.

- [ ] **Step 3: Save compatibility, both directions**

```bash
cp tests/fixtures/save_v6_fixture.dat save_09.dat
timeout 60 DISPLAY=:1 ./build/src/DungeonEngine --load 9 --town
```

Expected: loads, quests intact, Journal readable. Play until an autosave, then reload and confirm the v7 file round-trips.

- [ ] **Step 4: Co-op smoke**

Run a host and a client into the town, and confirm: both see the named givers, each player's Journal reflects **their own** progress, and one player talking does not advance the other's quest. Quest state is per-character with no wire change, so divergence here would mean something leaked into shared state.

- [ ] **Step 5: Draw-call budget**

With the Journal open in a zone, read the FPS line's `Draw:` figure.
Expected: unchanged from before this work — the Journal is HUD geometry and the five stones are five small meshes, so the zone should stay in its measured 48-80 range, well inside the 500 budget.

- [ ] **Step 5b: The in-game "Block" prompt draws through the Journal**

`renderHUD`'s common tail draws the Block prompt and its `Ctrl` glyph AFTER the inventory screen, so
it lands on top of the Journal — measured in `scratchpad/j_fullbag.png`, where it covers the giver
line and cuts through the narration. It does the same over the stash panel, so it is pre-existing and
was deliberately not fixed inside Task 8; over a grid of item slots it is cosmetic, over a block of
prose it obscures the thing the panel exists to show.

Fix by suppressing the gameplay prompts while a full-screen inventory panel is up (the Journal and the
stash both), not by reordering the draw — the prompts are meant to be visible during play and the
inventory is the exception. Verify with a screenshot over a NON-empty backpack, which is the only
state that exercises the panel's full extent.

- [ ] **Step 6: Update the documentation**

Per this repo's standing convention, add a section to `CLAUDE.md` covering: the quest engine's state model and the derived mask, the `TALK`-is-never-a-prerequisite rule and why, `SAVE_VERSION` 7 and its migration, the Journal panel's place in the inventory cycle, and the Cairn Stones. Update the `engine-reference` skill with the new `SAVE_VERSION`, the `Quest::` types and the new sentinel id; update `engine-how-to` with "adding a quest" as a recipe.

- [ ] **Step 7: Commit the documentation**

```bash
git add CLAUDE.md .claude/skills/engine-reference/SKILL.md .claude/skills/engine-how-to/SKILL.md
git commit -m "docs: the quest engine

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```
