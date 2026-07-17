# Town Hub + Account Stash — Design

**Date:** 2026-07-17
**Status:** Forks locked with the user (shared stash / town after credits + on Continue / portal → fresh run via free-play select)

## Goal

Beating the Dungeon Engine earns a home: an **outdoor town** the credits roll into, populated
by the friendly NPC cast (Cleric, Archer, Rogue), holding a **stash chest** — 5 pages × 48
slots (2× the 24-slot backpack, 240 items) shared across every character — and a portal that
starts the next run. From then on, that character's Continue lands in town, not the dungeon.

## Locked decisions

1. **Shared account stash**: one `stash.dat` for all characters (the gear-transfer stash).
2. **Town entry**: immediately after the Engine-kill credits (you emerge outside), AND on
   Continue for any cleared save (`FreePlay::saveCleared` — Hell, floor > 50).
3. **Town portal**: opens the existing Free-Play level select (difficulty + floor picker),
   confirm descends into a fresh run. Town is a hub, not a trophy room.
4. Save format untouched: `save_NN.dat` stays SAVE_VERSION 3; the stash is a versioned
   SIDECAR file (the stats_NN.dat / menagerie.dat precedent).

## The town level

- `buildTownLevel()` — deterministic (no seed), the `buildSourceChamber` pattern: a ~44×44
  open field. **Outdoor sky comes free**: floor cells simply omit `CELL_CEILING` (per-cell in
  the mesher), bright ambient light, sky-blue clear color while in town.
- New generated **grass tile texture** (`tools/gen_texture.py` → `grass_42.png`, registered in
  `tools/build_assets.py` like every texture) + a `town_grass` materials.json entry; low stone
  perimeter walls reuse existing wall materials; a few prop meshes (existing barrels/props) and
  short wall segments sketch a plaza — no new meshes.
- Sentinel floor **98** (`TOWN_SENTINEL_FLOOR`, beside SOURCE_SENTINEL_FLOOR 99): entering town
  in co-op broadcasts `SV_LEVEL_SEED` with 98 and clients build the same deterministic town
  (the proven source-chamber rails). Mid-join into town: same accepted gap class as the chamber.
- No enemies, no floor door, no boss state; `floorHasBoss=false`, `floorDoorActive=false`.

## Population

`spawnFriendlyNpc` already exists (`NpcClass::CLERIC/ARCHER/ROGUE`). Town spawns ~6 NPCs
(2× each class at plaza posts). They must NOT follow the player like dungeon companions: a
town flag makes friendly AI hold near `homePosition` with idle wander (the RETREAT-home walk
pattern), and they never enter combat states (there is nothing to fight). Speech bubbles give
ambient lines on proximity (existing speech pipeline; guests get them via SPEECH events).

## The stash

- **Storage**: `stash.dat` in `Platform::userDataPath()` (Steam-Cloud synced dir), atomic
  write like saves. Format: `u32 STASH_VERSION=1` + `u32 count=240` + 240 × raw `ItemInstance`
  (52 B) ≈ 12.5 KB. Loaded once at `Engine::init` (after item defs), saved on every stash
  change (atomicReplace — a crash can't corrupt it). Empty slots are `defId 0xFFFF`.
- **The chest**: a world-item sentinel `STASH_ID = 0xFFF7` (next free below CHEST_ID) +
  `isStash()` + `isSentinelItem` extension; renders as the chest mesh scaled up with a gold
  tint, placed at the plaza center. E-interact opens the stash UI (contents are local — no
  server round-trip to open).
- **UI**: opens the inventory screen with a stash panel beside the backpack. 8×6 grid per
  page, **5 page tabs** (click + Q/E / LB/RB to cycle), layout single-sourced in
  `InventoryUI` (a `stashLayout()` beside `quickbarLayout` — draw and hit-test share it, the
  drift lesson). **Tap-to-transfer v1**: click/confirm on a backpack item deposits into the
  first free slot of the current page; click a stash item to withdraw (backpack-full refusal
  with the usual message). Tooltips work on stash items. Equipment must be unequipped first
  (only backpack ↔ stash moves). Drag support is v2.
- **Co-op**: each player's stash is their own local file. Deposits/withdrawals mutate the
  local `m_inventories[lane]` and push via the existing `CL_INVENTORY_SYNC` (the same trust
  model every equip already uses). Guests therefore get full stash function in town.

## Flow wiring

- **Credits → town**: the VICTORY screen's key-press currently returns to MENU (with net
  teardown). When the ending was the Engine kill (`s_engineSlain`) the host/SP instead calls
  `enterTown()` — net stays alive, sentinel 98 broadcasts, guests follow. The Hell-complete
  (non-secret) ending keeps today's menu return.
- **Continue → town**: `loadGame` on a save where `FreePlay::saveCleared(savedFloor,
  difficulty)` routes to `enterTown()` instead of `startGame` (replacing the current
  straight-to-free-play-select flow — the select now lives behind the town portal).
- **Town portal**: E-interact opens the existing Free-Play select; confirm runs the existing
  descend/start flow. In co-op only the HOST's portal drives the party (guests see a "host
  chooses the descent" hint), matching how floor descents already work.
- **Saving in town**: `saveCharacter` must keep writing the CLEARED marker floor (the header's
  pinned >50 floor), never the 98 sentinel — the no-downgrade guard already protects the
  marker; town explicitly passes the saved header floor through unchanged.
- **Death/combat state in town**: no damage sources exist; skills are castable but harmless
  (no entities to hit). Respawn logic untouched.

## Testing

- Unit: stash round-trip (save → load → byte-equal items), page/slot indexing math,
  tap-transfer rules (full page → next free ON PAGE only; full backpack refusal), cleared-save
  routing predicate, STASH_ID sentinel exclusions (never enters inventories/loot).
- Runtime SP: kill-Engine → credits → town (NPCs idle, sky visible); stash deposit/withdraw
  survives quit + relaunch; Continue lands in town; portal → free-play select → run starts.
- Runtime co-op: host + guest both reach town after credits; guest deposits/withdraws with
  its own stash; portal descent brings both into the new run.

## Risks

- Friendly-AI "stay home" mode touches companion behavior — must be town-gated so dungeon
  companions keep following.
- The stash UI is the largest single piece; tap-to-transfer keeps it tractable (drag is v2).
- Outdoor look is v1-simple (open sky + grass + props); it's a hub, not a showcase — polish
  passes can come later.
