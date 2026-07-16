---
name: create-item
description: Use when adding a new item to DungeonEngine — an items.json def (weapon/armor/ring/offhand, or a special consumable/unrollable def), its mesh/material/icon/tooltip wiring, and the loot-table + despawn + save-compat rules. Trigger for requests like "add an item", "add a new ring/armor/consumable", "make an item that does X".
---

# Create a new item

End-to-end workflow for adding one item to DungeonEngine. Weapons have their own deeper skill
(`create-weapon` — subtypes, firing behavior); pet consumables have `create-pet`. This skill is
the base recipe plus the traps that were learned the hard way on the Mini Loot Goblin.

**Golden rules:**
- **items.json is APPEND-ONLY.** `ItemInstance.defId` (persisted in saves, u16) is the item's
  **array index** in items.json. Inserting or reordering entries silently turns every player's
  saved gear into different items. Append before nothing; delete never.
- **`ItemDef` is NOT serialized** (JSON-loaded at init) — adding fields to it is save-safe.
  `ItemInstance` (52 B) IS serialized raw: never touch its layout without a `SAVE_VERSION` bump
  (see the no-unprompted-save-format rule).
- **Never hand-author meshes/textures** — `tools/gen_mesh.py` etc., and a new mesh must be added
  to BOTH `src/engine/asset_manifest.h` and `tools/build_assets.py` (either alone fails loudly now).

## Step 1 — The def (assets/config/items.json)

Append an entry. Common fields: `name` (must fit `char[32]`), `slot`
(`weapon|helmet|armor|boots|gloves|ring|offhand`), `mesh`, `material`, `minLevel`/`maxLevel`
(drop band, 1-50 wrapped), `maxRarity` (`common|magic|rare|legendary`), `dropWeight`.
Weapon-only: `weaponType`, `weaponSubtype`, `baseDamage/Range/Cooldown/...`. Armor: `baseHealth`.
Legendary actives/passives: `legendarySkill` (see the tooltip-rot pitfall in `engine-how-to`).

**Def-shape recipes:**
- **Ordinary gear**: pick a level band + `dropWeight` and it enters the loot table automatically.
  Cap `maxRarity` at `rare` — the legendary tier is reserved for marked uniques (below), and
  `tests/game/test_legendary_pool.cpp` fails the suite on a legendary-capable def that isn't one.
- **LEGENDARY UNIQUE** (named identity item): `"minRarity": "legendary"` + `"maxRarity":
  "legendary"` + a `legendarySkill` (slot-appropriate — see the tooltip-rot pitfall). The rarity
  window makes it the ONLY kind of item a legendary roll can produce AND keeps it out of every
  lower tier; drops always carry 3-4 affixes. Give it a level band like ordinary gear — guaranteed
  drops (boss/champion/goblin `rarityFloor`) widen the band automatically when needed.
- **UNROLLABLE def** (only obtainable from a scripted source — jackpots, quest rewards):
  `"minLevel": 255, "maxLevel": 255, "dropWeight": 0.0`. minLevel 255 excludes it from BOTH of
  `ItemGen::rollItem`'s candidate passes; dropWeight 0 is the belt to those braces. Pinned by
  `tests/game/test_pet_item.cpp` — copy that test's pattern for a new unrollable def.
- **Consumable-on-use** (item that DOES something instead of equipping): give it a real slot to
  satisfy the loader, add a `bool` flag on `ItemDef` (cf. `petSummon`), intercept the use in
  `Engine::tryUse*` BEFORE every equip path (controller-A, mouse double-click, quickbar —
  `useQuickbarSlot` must intercept before the EQUIPPED_REF conversion), and add a refusal
  backstop in `Inventory::equip`. All three entry points, or a path equips your consumable.
- **Not-really-an-item** (globes, shards, shrines, chests): do NOT make a def at all — use a
  sentinel defId (`0xFFFE` down; next free is below `CHEST_ID` 0xFFF8) + an `is*()` helper in
  `item.h`, and extend `isSentinelItem` so it can't enter inventories. Direct-constructed
  sentinels take `uid = m_worldItems.nextUid++`, which starts at 0x80000000 — DISJOINT from
  ItemGen's low-range rolled-item uids, so a guest's uid-matched CL_PICKUP_ITEM can never hit
  the wrong object. Never hand a sentinel a low uid.

Cap: `MAX_ITEM_DEFS` (`item.h`, currently 224, table is at 196). Bump the constant if needed —
memory-only, not on the wire, not in saves.

## Step 2 — Loader + resolution

New JSON field ⇒ matching `ItemDef` field + one `entry.value(...)` line in
`ItemLoader::loadItemDefs` (keep schema and loader in sync). Names (mesh/material/other defs)
are stored as strings and resolved to IDs after all tables load — items resolve in
`ItemLoader::resolveVisuals`; cross-table links (e.g. `petEnemy` → `petEnemyIdx`) resolve in
`engine_init_assets.cpp` after BOTH tables exist. Resolve failures must `LOG_WARN`, never crash.

## Step 3 — Icon + tooltip

- Inventory icon: 16×16 packed-u16 glyphs. Slot/subtype defaults come free; a bespoke glyph is
  authored in `tools/gen_item_icons.py` (`draw_*` + register in `main()`), regenerated into
  `src/renderer/item_icons_gen.h` (committed by design), appended to `s_iconData` in
  `item_icons.cpp`, and dispatched at the TOP of `drawIcon` if it must override the slot glyph
  (a special item wearing the generic ring glyph is invisible in a full backpack — learned on
  the goblin).
- Tooltip: `hud_inventory.cpp` `emitBody()`. Special items should present as what they ARE
  (e.g. "Companion" instead of the claimed slot name) and say what "Use" does.

## Step 4 — Drop source + despawn rules

- Scripted drops construct the `ItemInstance` directly: `defId`, `rarity`, `itemLevel`,
  `affixCount 0`, and **`uid = m_worldItems.nextUid++`** (the globes/shards pattern — a 0 uid
  breaks client mirroring), then `WorldItemSystem::spawn(..., killerSlot)` +
  `broadcastLootSpawn`. **Check spawn()'s return** — a full pool silently swallows drops.
- Despawn: world items expire after 60 s EXCEPT legendaries, shrines, shards, chests, and
  petSummon defs (def-aware exemption in `WorldItemSystem::update`). A rare non-legendary drop needs an
  exemption or it rots; prefer extending the def-aware check over a spawn-time flag (a flag
  dies when the player drops the item again — the update-side check survives every path).
- Guaranteed/rare drops on crowded floors: consider `spawnEssential` (evicts the most
  expendable expiring item) — that's why the source shard uses it.

## Step 5 — Verify

- `cmake --build build && ./build/tests/dungeon_tests` — extend `tests/game/` if you added an
  invariant (unrollable, equip-refusal, despawn exemption: all have patterns in
  `test_pet_item.cpp`).
- In-game: drop it via its source (or temporarily bump the roll), check icon, tooltip, pickup,
  equip/use, and that it survives/expires per its despawn rule.
- Keep docs in sync: `engine-reference` (constants/lifecycles) and CLAUDE.md if architecture
  moved.
