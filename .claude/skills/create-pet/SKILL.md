---
name: create-pet
description: Use when adding or changing a pet companion in DungeonEngine — a petSummon consumable item that summons/dismisses a cosmetic follower entity (the Mini Loot Goblin jackpot or the per-enemy "Mini <Enemy>" drops), including the summon path, follow AI, co-op net use-path, drop beacon, and icon. Trigger for "add a pet", "new companion", "change pet behavior/drop".
---

# Create or extend a pet companion

The pet system was built for the Mini Loot Goblin (the loot goblin's 1% jackpot) and then
generalized to one COMMON "Mini <Enemy>" per enemies.json entry (1-in-10000 kill roll). This
skill is the full pipeline plus the traps found while building it. Base item mechanics
(append-only defId, icons, tooltips) live in `create-item`; read that first.

## Architecture in one paragraph

A pet is TWO things: an **items.json consumable** (`petSummon: true`, unrollable — minLevel 255 +
dropWeight 0 — so a scripted roll is its only source) and a **cosmetic follower entity** spawned
by `Engine::togglePetCompanion(ownerSlot, petDefId)` (`engine_spawn.cpp`). The item stays in the
backpack forever (infinite uses); "using" it toggles the entity. Everything is
server-authoritative: the entity replicates to guests through the ordinary snapshot
(mesh/material/size all ride `SnapEntity`), and a guest's use-click arrives as a reliable
`CL_USE_PET` packet.

## The item def

Per-enemy pets are GENERATED from enemies.json (name "Mini <Enemy>", `mesh`/`material` copied
from the enemy, `petEnemy: "<enemy name>"`). `petEnemy` resolves to `ItemDef.petEnemyIdx` after
both tables load (`engine_init_assets.cpp`), which also fills the reverse map
`Engine::m_petItemForEnemy[enemyDefIdx]` used by the drop roll.
**`tests/game/test_pet_item.cpp` pins the cross-JSON sync: a new enemy without a pet def fails
the suite** — when adding an enemy, add its pet entry too (mirror an existing one).
The goblin's own def is the one petSummon def WITHOUT `petEnemy` (`petEnemyIdx == 0xFF`) — code
that must single it out gates on exactly that.

## Drop sources

- Per-enemy: end of `handleNormalLootDrop` (`engine_death.cpp`) — `Entity.enemyDefIdx` →
  `m_petItemForEnemy[]`, `(std::rand() % 10000) == 0`, rarity COMMON, direct-construction
  `ItemInstance` with `uid = m_worldItems.nextUid++`, then `spawn(..., killerSlot)` +
  `broadcastLootSpawn`. **Outside** the ordinary drop-chance gate — a jackpot must not be eaten
  by the 40% roll. Only reached for deaths the boss/goblin/champion handlers didn't claim.
- Goblin jackpot: `handleGoblinLootDrop`, 1%, LEGENDARY rarity.
- Despawn immunity: petSummon defs are exempt in `WorldItemSystem::update` (def-aware — survives
  the player dropping the item again) and spared by `spawnEssential`'s eviction. COMMON pets
  NEED this; don't "simplify" it away.
- Visibility: `renderWorldItems` draws the rarity-colored tri-beam beacon (3 crossed unlit
  quads on a rotating triangle, batched into the rarity-disc pass) for any world item whose def
  is petSummon. Tune `BEAM_*` constants there.
- Q drop-all skips petSummon items (`engine_inventory.cpp`) — a bag-clear must not shed a
  1-in-10000 companion; the single-item drop stays deliberate and allowed.

## The summon (`togglePetCompanion`)

Semantics: same-kind pet out → dismiss; different kind → swap; none → summon. Kind is matched
via the pet entity's `enemyDefIdx` (stamped at summon = the item's `petEnemyIdx`).

Entity recipe (all learned the hard way):
- **Dismissal must NOT pay loot**: set `flags |= ENT_DEAD; aiState = DEAD; deathTimer = 0.01f`
  directly — `Combat::killEntity` always fires the death/loot callback (Swarm-Queen pattern).
- `NpcClass::PET` + `ENT_FRIENDLY | ENT_UNTARGETABLE`; damage-immunity lives in
  `Combat::applyDamage` (covers splash/AoE too).
- Visuals: source enemy's `meshId`/`materialId`/`enemyType` (enemyType drives the attack/idle
  animation), `halfExtents * 0.5f` — the renderer scales the shared mesh to 2×halfExtents, so
  "mini" needs **no new asset**. The goblin pet wears `gold_trim` instead (legendary flair).
- `ownerNetSlot` = follow anchor; `ownerLocalPlayer` fallback for split-screen.
- **`EntitySystem::spawn` does NOT zero recycled slots** — any new Entity field a pet relies on
  must be reset in the spawn() reset block or a recycled slot inherits garbage.
- Follow AI: the `NpcClass::PET` branch in `enemy_ai_friendly.cpp` (heel at 2 m, catch-up
  sprint past 6 m, teleport past 25 m; grounded — flying sources walk).
- Pets do not survive floor transitions (entity pool rebuild); re-summoning is one click. Fine.

## The use path (every entry point, or one of them equips your consumable)

`Engine::tryUsePetItem(backpackIndex)` is called FIRST by all three equip entry points
(controller-A in `engine_inventory.cpp`, mouse double-click, quickbar in `engine_combat.cpp` —
before the EQUIPPED_REF conversion), with `Inventory::equip`'s petSummon refusal as backstop.
- SP/host: `togglePetCompanion(m_localPlayerIndex, defId)` directly.
- CLIENT: `sendUsePetPacket(defId)` — reliable `CL_USE_PET` (header + u16 defId). Server side:
  `net.cpp` parses, `Engine::onUsePet` re-validates (petSummon def AND present in that slot's
  synced inventory — a forged packet is a no-op) then toggles. **History**: this was originally
  an `INPUT_EX_PET` input-stream bit; it was replaced because a payload-less edge cannot say
  WHICH pet once there is more than one. A new pet-adjacent action that needs a payload should
  follow the CL_USE_PET pattern (own reliable packet + server validation + PROTOCOL_VERSION
  bump), not a new input bit.

## Menagerie

Summoning is COLLECTED: `tryUsePetItem` calls `recordPetSummon(defId)`, which sets the pet's
bit in the profile-wide `menagerie.dat` (bit = `petEnemyIdx`; the goblin has its own flag) and
unhides the pause menu's Menagerie page. A NEW pet needs nothing here — the bitmask keys off
the enemy-def index and the page enumerates `m_petItemForEnemy[]` — but if you ever add a pet
whose source is NOT an enemies.json def (like the goblin), it needs its own flag + a row in
`renderMenagerie`, and the `menagerie.dat` version bumped if the layout grows.

## Verify

- `./build/tests/dungeon_tests -tc="*pet*" -tc="*Config*"` — sync pin, unrollable, equip-refusal,
  no-despawn.
- In-game (temporarily raise the roll if needed): drop shows beacon + paw icon; use summons the
  right mini; same item dismisses; a different pet item swaps; pet ignores damage and follows
  through doors; co-op guest can use theirs (watch the server log for `Pet: ... summoned`).
- Keep `engine-reference` (pet paragraph) in sync with behavior changes.
