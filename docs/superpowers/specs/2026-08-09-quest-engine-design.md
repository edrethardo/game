# Quest Engine — Design

**Date:** 2026-08-09
**Status:** approved for planning
**Replaces:** the static `Quest::QUESTS[]` table + `u64` completion mask + chat-line "journal"

---

## 1. Why

The acts ship with ten quests whose entire player-facing surface is a chat line that fades after
ten seconds. There is no journal, no way to re-read what a place wants, and no representation of
partial progress — a quest is one bit, done or not done.

Two concrete defects make the current surface worse than its design intended. `Engine::addChatMessage`
formats `"%s: %s"` into a 48-byte buffer, so **nine of the ten quest blurbs are truncated mid-sentence**
(*The Rebaser* delivers "The graveyard keeps bringing its history back" and drops "Stop whatever is
rewriting it"; *Kill -9* ends at "Terminat"). And because quests pass an empty speaker, every quest
line renders with a stray leading `": "`.

The goal is Diablo 2's shape: a **Journal** you can open and read, quests made of **objectives with
visible progress**, and **NPC quest givers** in the act hubs who hand them out.

## 2. Scope

| In | Out |
|---|---|
| Journal panel inside the inventory (D2R-console style submenu) | Quest **rewards** (skill points, imbues, guaranteed drops) |
| Multi-stage objectives with per-character progress | Voiced dialogue, dialogue trees, branching |
| NPC quest givers in the act hubs | Turn-in-at-NPC (quests still complete in the field) |
| One authored showpiece quest: the Cairn Stones | A prerequisite graph beyond the existing zone-route gating |
| `SAVE_VERSION` 6 → 7, with migration | Any protocol/wire change |

**Decisions taken (do not re-litigate):**

1. The Journal is a **panel in the inventory's shoulder cycle**, not a separate screen.
2. Quest state is **persisted**, sized for 32 quests now while the bump is free.
3. NPCs **offer**; quests **complete in the field**. No walk-back.
4. Objectives are **derived from existing engine signals**, plus **one** authored quest.
5. No rewards.

## 3. Architecture

### 3.1 Data model

`src/game/quest_def.h` gains objectives and narration. A new **pure, engine-free**
`src/game/quest_state.h` owns the state machine, following the `zone_route.h` / `free_play.h`
pattern so it unit-tests with no GL or engine context.

```cpp
namespace Quest {

inline constexpr u32 MAX_QUESTS = 32;   // 10 authored today; sized so Act 3 needs no save bump
inline constexpr u32 MAX_OBJ    = 4;    // objectives per quest

enum struct Trigger : u8 {
    CLEAR_ZONE,   // every hostile in the quest's zone is dead      (live count, not stored)
    SLAY,         // the named enemy died                            (boolean)
    REACH,        // the zone was entered                            (boolean)
    TALK,         // the giver was spoken to                         (boolean, NEVER a prerequisite)
    ACTIVATE,     // N world fixtures interacted with                (bitmask, stored)
    COUNT
};

struct ObjectiveDef {
    Trigger     trigger;
    const char* text;      // "Hostiles remaining", "Stones aligned"
    const char* target;    // enemy name for SLAY, fixture tag for ACTIVATE, "" otherwise
    u8          required;  // 1 for a boolean step; 5 for the Cairn Stones
};

struct GiverDef {
    u8          hubFloor;  // 98 = town (Act 1), 61 = Null Terminus (Act 2)
    const char* name;      // shown on the prompt, the nameplate and the journal
    const char* greeting;  // the one short line spoken to chat on talk
};

struct QuestDef {
    u8           zoneFloor;
    const char*  name;
    const char*  blurb;         // the one-line offer; still goes to chat
    const char*  narration;     // NEW — the journal body. No length limit.
    u8           giverIdx;      // index into GIVERS[]
    u8           objectiveCount;
    ObjectiveDef objectives[MAX_OBJ];
};

enum struct State : u8 { LOCKED, OFFERED, ACTIVE, COMPLETE };

// Exactly what the save holds, and the only mutable quest state in the game.
struct Progress {
    u8 state[MAX_QUESTS];
    u8 obj[MAX_QUESTS][MAX_OBJ];
};

} // namespace Quest
```

`static_assert(COUNT <= MAX_QUESTS)` pins the table against the array.

**An act is still derived, never stored.** `actOf(zoneFloor)` already returns 1 or 2 from the zone
number; a quest's giver resolves through `GIVERS[giverIdx].hubFloor`, so the act appears in exactly
one place. `GiverDef` exists (rather than deriving the giver straight from the act) so an act can
field two or three givers the way D2's Rogue Encampment fields Akara, Kashya and Charsi — at the
cost of one authored byte and no duplicated fact.

### 3.2 The derived mask — the compatibility hinge

```cpp
u64 completionMask(const Progress&);   // bit i set iff state[i] == COMPLETE
```

`ZoneRoute::objectiveZone`, `ZoneRoute::zoneSettled`, the gate-refusal hint, `Quest::actComplete`
and the entire autoplay act branch consume a plain `u64` today. They keep doing so and need **zero
changes**. `Engine::m_questMask[lane]` survives as a cache recomputed from `Progress` whenever the
progress changes — one fact with one home, which is the rule this codebase's bug history keeps
arriving at.

### 3.3 Objective semantics

| Trigger | Stored in `obj[q][i]`? | Progress shown |
|---|---|---|
| `CLEAR_ZONE` | No — derived live from the entity pool | `Hostiles remaining n/m` |
| `SLAY` | Yes, 0/1 | `✓` or `□` |
| `REACH` | Yes, 0/1 | `✓` or `□` |
| `TALK` | Yes, 0/1 | `✓` or `□` |
| `ACTIVATE` | Yes, as a **bitmask** | `popcount(mask)/required` |

`CLEAR_ZONE` is deliberately not persisted: it is already polled every frame off the pool (because
"the last one just died" is a property of the pool, not of any single death), and storing a copy
would be a second home for a fact that already has one.

`ACTIVATE` stores a bitmask rather than a count because **a zone is rebuilt from its seed on every
entry**. A bare count could not tell which stones were already lit, so re-entering the field would
re-light the wrong ones. Five bits fit in the byte with room to spare.

### 3.4 The TALK objective is never a prerequisite

A `TALK` objective is satisfied by speaking to the giver **or** by the quest completing in the field.
It is never required for the quest to complete.

This is a deliberate decision, not an oversight, for three reasons. The chosen loop is
hub-offers/field-completes precisely to avoid a walk-back. `test_zone_def.cpp` already pins the
invariant that **a gate must never be able to strand a character**, and a mandatory conversation
hands that invariant a new way to fail. And the acts are verified end to end by a nine-class
autoplay soak (`tools/overworld_soak.py`, currently 9/9); putting quest progression behind a
conversation would put the bot behind one too.

The NPC's role is narration and populating the Journal — not permission.

## 4. Components

### 4.1 `src/game/quest_state.h` (new, pure, tested)

```cpp
void offer      (Progress&, u8 questIdx);                       // LOCKED -> OFFERED
void noteTalk   (Progress&, u8 questIdx);
void noteKill   (Progress&, u8 questIdx, const char* enemyName);
void noteReached(Progress&, u8 questIdx);
void noteCleared(Progress&, u8 questIdx);
void noteActivate(Progress&, u8 questIdx, u8 fixtureIdx);       // sets one bitmask bit
bool isComplete (const Progress&, u8 questIdx);
u64  completionMask(const Progress&);
u8   objectiveProgress(const Progress&, u8 questIdx, u8 objIdx);
     // Stored progress only. A CLEAR_ZONE objective returns 0 here by design — it has no
     // stored counterpart, and the Journal supplies the live remaining-hostile count itself.
void migrateFromMask(Progress&, u64 legacyMask);                // v6 -> v7
```

Every mutator re-evaluates the quest's own objectives and promotes `OFFERED -> ACTIVE -> COMPLETE`.
No engine types appear in the header.

### 4.2 `src/renderer/hud_journal.cpp` (new)

Draws the panel, mirroring `hud_inventory.cpp`. Layout comes from
`InventoryUI::journalLayout(sw, sh)` so the draw and the hit-test derive from one place — the
discipline `inventory_ui.h` already documents for the quickbar, stash and build grid, added after
each of them drifted.

**Layout.** Left column: quests grouped under `ACT I` / `ACT II`. Right column: the selected quest's
title, its narration **word-wrapped** via `FontSystem::textWidth`, then one row per objective with
`✓` / `□` and `n/m`.

**Colour carries state:** complete gold, active white, offered grey-blue, locked dim. This mirrors
the minimap rule the overworld work settled on — shape carries *what*, colour carries *which*.

**Navigation.** D-pad / WASD up-down walks the quest list; left-right flips the act tab. The panel is
read-only: there is no confirm action, so nothing can be mis-pressed. Mouse clicks a quest row.

### 4.3 Inventory integration

```
INV_PANEL_BACKPACK    = 0
INV_PANEL_EQUIPMENT   = 1
INV_PANEL_CLASS_SKILL = 2
INV_PANEL_EQUIP_SKILL = 3
INV_PANEL_BUILD       = 4
INV_PANEL_JOURNAL     = 5   // NEW — joins the shoulder cycle
INV_PANEL_COUNT       = 6
INV_PANEL_STASH       = 6   // shifts up; outside the cycle, as today
```

The journal joins the L/R shoulder cycle, so controller and Switch reach it exactly as they reach
Build. `m_invCursorQuest` (a `u8`) holds the selected row, alongside the existing
`m_invCursorBuild`.

### 4.4 NPC givers

Two of the town's six plaza NPCs (`spawnTownContents`, `engine_town.cpp`) gain a `nameTag` and a
giver index. Null Terminus (zone 61, the act's one `peaceful` platform) gains two through
`spawnZoneContents`.

`Interact::Target` gains `NPC` — appended, since the enum is a per-frame resolve and is not
serialized. `resolveInteractTargets` offers the nearest friendly giver in range with a
`Speak to <name>` prompt. Interacting:

1. plays the giver's one-line `greeting` to chat (flavour),
2. calls `offer()` / `noteTalk()` for that giver's currently-outstanding quest,
3. opens the inventory on `INV_PANEL_JOURNAL` with the cursor on that quest.

**No wire change.** Quest state is per-character and talking mutates nothing in the world, so in
co-op each player talks for themselves and nothing needs replicating.

### 4.5 The Cairn Stones (the one authored quest)

Quest 56 — *Align the Standing Stones*, currently a `CLEAR_ZONE` in the Field of Unmerged Branches —
becomes D2's beat: five standing stones, each interacted with individually; 5/5 opens the way to
TristRAM.

Objectives:

```
□ Speak to the keeper                (TALK,     required 1)
□ Stones aligned              0/5    (ACTIVATE, required 5)
```

The stones ride the **waypoint sentinel pattern**: a new `CAIRN_STONE_ID = 0xFFF3` (the next free
sentinel below `ZONE_GATE_ID`), which buys spawning, replication, server-side pickup validation and
the fixture despawn-exemption for free — the last of these matters, because the exemption is now
derived as "any sentinel except the globe", so a new sentinel is exempt by default.

Placement follows the rule the overworld already learned the hard way: **the anchors are room
centres**, never fractions of the grid, because a fixture stamped at a grid fraction can generate
walled in (the Bank Station portal did).

Assets: one `cairn_stone` mesh + skin generated by `tools/gen_mesh.py` / `tools/gen_skin.py` and
registered in **both** `src/engine/asset_manifest.h` and `tools/build_assets.py` — the asset build
hard-fails if only one is edited.

### 4.6 Chat repair

`Engine::addChatMessage` widens `CHAT_LINE_LEN` and skips the `": "` separator when the speaker is
empty. Narration moves to the Journal, so quest chat lines become short by design — but the
truncation bug is real for every other caller too and dies here.

## 5. Persistence — `SAVE_VERSION` 7

Appended to the existing v6 per-player tail, after `questMask`:

```
u8 questState[32];        // 32 bytes
u8 objProgress[32][4];    // 128 bytes
```

One writer (`saveCharacter`) and two version-conditional readers (`loadGame`,
`loadCharacterInto`) — the same shape the v6 waypoint mask uses. No existing struct changes size,
so every `static_assert` layout guard stays green.

**The migration is the load-bearing part.** A v6 save already carries a `questMask` with real
completions. Loading one must run `migrateFromMask()`, setting `state[i] = COMPLETE` for every set
bit. Reading the new tail as zeros instead would silently un-complete both acts for every existing
hero — the same class of failure the Inferno save-clamp bug represented, and unrecoverable once the
next autosave writes it back.

`questMask` stays in the file, but by construction rather than by choice: the v7 tail is appended
*after* it, so removing it would shift the v6 portion's layout and break the legacy read path it
exists to serve. On a v7 load it is **read and discarded** — `questState` is the authority. It is
consulted on exactly one path, the v6 migration.

## 6. Testing

**Unit — `tests/game/test_quest_state.cpp`** (pure, no engine):

- offer → advance → complete for each trigger type, including the `ACTIVATE` bitmask and its
  `popcount` progress;
- `completionMask()` reproduces the old `isComplete` semantics exactly for every quest;
- **the v6 migration**: a mask with an arbitrary bit pattern round-trips to the matching
  `COMPLETE` states;
- **`TALK` is never a prerequisite** — a quest whose deed is done in the field completes with its
  `TALK` objective unticked. Sabotage check: making `TALK` blocking must fail this by name;
- **never-strand**: walked across the whole quest order, no reachable quest can become
  uncompletable — extending the invariant `test_zone_def.cpp` already pins for zone routing.

**Save round-trip** — a v7 write/read of a non-trivial `Progress`, plus a **real v6 file kept aside
as a fixture** that must still load with both acts intact. The last two save bumps each nearly
shipped a reader that misread the previous version; the fixture is what catches it.

**Layout** — `journalLayout()` hit-test rects match the drawn rects, the check every other
inventory panel carries.

**Live** — the act soak (`tools/overworld_soak.py`) must still return **9/9**. That is the real
proof the `TALK` rule is non-blocking: a bot that cannot talk must still finish both acts.

## 7. Files

**New**
- `src/game/quest_state.h` — the pure state machine
- `src/renderer/hud_journal.cpp` — the panel draw
- `tests/game/test_quest_state.cpp`
- `assets/meshes/cairn_stone.obj`, `assets/textures/cairn_stone_skin_42.png` (generated)

**Modified**
- `src/game/quest_def.h` — objectives, narration, `GiverDef`, `MAX_QUESTS` / `MAX_OBJ`
- `src/game/inventory_ui.h` / `.cpp` — `journalLayout()`
- `src/engine/engine.h` — panel constants, `m_invCursorQuest`, `Quest::Progress` per lane
- `src/engine/engine_inventory.cpp` — journal panel navigation
- `src/engine/engine_zone.cpp` — the three quest hooks route into `quest_state`
- `src/engine/engine_town.cpp` — named givers at plaza posts
- `src/engine/engine_persist.cpp` — `SAVE_VERSION` 7 tail + `migrateFromMask`
- `src/engine/engine.cpp` — `addChatMessage` repair
- `src/game/interact.h`, `src/engine/engine_update.cpp` — the `NPC` interact target
- `src/game/item.h`, `src/game/world_item.cpp` — `CAIRN_STONE_ID`
- `src/engine/asset_manifest.h`, `tools/build_assets.py` — the stone mesh (**both**)

## 8. Build order

Four steps, each shippable **provided a trigger and the thing that satisfies it land in the same
commit**. The first draft of the plan flipped quest 56 to `ACTIVATE` in step 1 while its fixtures
arrived in step 4 — and `ZoneRoute::linkOpen` gates the onward road on `zoneSettled(56)`, so every
intermediate commit sealed the way to TristRAM. Quest 56 now changes trigger in step 4, beside the
stones:

1. **Data model + state machine + save v7 + migration.** Nothing visible; the derived mask keeps
   every existing consumer working byte-identically.
2. **The Journal panel**, reading that state. This alone fixes the reported complaint.
3. **NPC givers** and talk → journal.
4. **The Cairn Stones**, the authored quest.
