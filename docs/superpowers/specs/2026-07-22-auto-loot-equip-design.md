# Auto Loot & Equip — design

Aaron's ask: a second control/gameplay mode. **Classic** = loot and equip by hand, as today.
**Auto Loot & Equip** = the player picks a build from a 3×3 grid in the inventory —
rows **Tanky / Moderate / Glass Cannon**, columns **Magic / Melee / Ranged** — and the game
picks up nearby loot and wears the best gear for that build automatically. Players are asked
which mode they want at character creation and can change build (or mode) in the inventory.

Decisions locked with Aaron (2026-07-22):

- **Auto-pickup + auto-equip**: drops within radius fly into the bag; upgrades are worn on the spot.
- **Mode chosen at character creation** (couch P2 gets the chooser on their own pad) **and**
  toggleable any time in the inventory screen.
- **SAVE_VERSION 3→4** — explicitly granted. Mode + build persist in `PlayerInventory`.
- **Bag self-manages**: when full, the lowest-scoring backpack item is auto-dropped to make room
  (Aaron chose this over "pause pickup when full").
- **Scoring is stat-derived**, not authored tags: zero JSON burden, covers all defs + affixes.

## State

`PlayerInventory` gains two u8s (persisted, and riding the existing `CL_INVENTORY_SYNC` — the
server just receives the synced inventory as with any equip, so **no protocol bump**):

- `autoMode` — 0 classic (default, and what every pre-v4 save loads as), 1 auto.
- `buildCell` — row*3+col; row 0 Tanky / 1 Moderate / 2 Glass, col 0 Magic / 1 Melee / 2 Ranged.
  Default Moderate/Melee (4).

SAVE_VERSION 4 with `LegacyPlayerInventoryV3` mirror reader, per the v2→v3 pattern in
`engine_persist.cpp`. Size static_asserts re-pinned.

## Scoring — `src/game/build_score.h` (pure, unit-tested)

`BuildScore::score(const ItemInstance&, const ItemDef&, u8 buildCell) -> f32`

- **Weapon family gate** (columns): a weapon whose `WeaponSubtype` is outside the build's family
  scores **0** and is never auto-equipped. Magic = WAND/STAFF/ORB-likes; Melee = SWORD/DAGGER/AXE/
  CLAYMORE/CLEAVER; Ranged = BOW/CROSSBOW/THROWING_KNIFE/MOLOTOV/CHAKRAM + guns. Non-weapon slots
  are never gated by column.
- **Row weights**: offense (baseDamage, damage affixes, crit, attack speed, spell damage) and
  defense (baseHealth, health/armor/CC-resist/thorns affixes) each get a multiplier:
  Tanky 1×off/3×def, Moderate 1.5×/1.5×, Glass 3×off/1×def.
- **Column affix bias**: spell-damage affixes ×2 under Magic; attack-speed ×1.5 under Melee/Ranged.
- **Rarity nudge**: + small per-rarity term so equal-stat higher rarity wins ties.
- **Hysteresis**: auto-equip only when `score(new) > score(worn) * 1.05` — near-ties never churn.
- Worst-item selection for the self-managing bag reuses the same scorer (lowest score drops;
  petSummon consumables and the quickbar-referenced items are exempt, mirroring "drop all").

## Auto-pickup

In `updatePlayerPickup`, when the lane's inventory is in auto mode: any **non-sentinel**,
free-for-all (or own-exclusive) world item within `AUTO_LOOT_RADIUS` (2.5 m) is picked up through
the **existing** path — host/SP directly, CLIENT via the server-validated `CL_PICKUP_ITEM` exactly
as a manual grab. Globes/shrines/chests/stash keep their special flows untouched. Bag full →
auto-drop the worst first (HUD toast names it), then pick up.

## Auto-equip

After any pickup lands while in auto mode (and on build change): score the new item vs the worn
item in its slot; equip on upgrade; **`sendInventorySync()`** (every equip path must — the v16
couch lesson). Runs only on the owning lane's sim. Switching build in the grid re-runs auto-equip
over the whole backpack, so changing builds visibly re-gears the character.

## UI

- **New Game chooser**: after class select, "Classic" / "Auto Loot & Equip" (two rows, same input
  scheme as class select; couch P2 gets it on their pad in their lobby flow).
- **Inventory build grid**: a 3×3 panel on the inventory screen; rows labelled Tanky/Moderate/
  Glass Cannon, columns Magic/Melee/Ranged; current cell highlighted; greyed out entirely in
  classic mode with a hint ("Auto Loot & Equip is off"). Mouse AND controller/keyboard navigable
  (the stash lesson: mouse-only is dead on Switch/couch P2). A mode toggle row sits above it.
- Single-sourced layout helper (`InventoryUI::buildGridLayout`) for draw + hit-test, per the
  quickbar drift lesson.

## Out of scope

- No auto-USE of consumables/pets; no auto-sell; no auto-stash.
- Arena/PvP: auto mode is inert there (no loot exists in the arena).
- NPCs/enemies unaffected.

## Testing

- Unit: scorer (family gate, row weighting, column bias, hysteresis, worst-pick exemptions),
  save round-trip v3→v4 (mirror reader), build-cell encode/decode.
- Headless: boot a floor in auto mode, verify pickups/equips happen via the 1 Hz counters
  (temporary instrumentation), verify classic mode boots with zero auto actions.
- Manual: the new-game chooser on keyboard + pad; build grid navigation; couch P2 independence;
  a CLIENT auto-looting in co-op (server-validated path).
