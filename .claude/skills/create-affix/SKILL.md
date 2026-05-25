---
name: create-affix
description: Use when adding a new affix type (item magic property) to DungeonEngine — a new AffixType enum value, its loader mapping, the stat accumulation in recalculateStats, the consumption at the point of use, and the affixes.json entry. Pure code + JSON, no assets. Trigger for "add an affix", "new item modifier/property", "items should be able to roll X".
---

# Create a new affix type

End-to-end workflow for adding one new affix (e.g. "+X% attack speed", "+X resistance").
An affix has a **5-point sync chain** — miss any link and the bonus silently never applies.
Reference material on the engine lives in the `engine-reference` skill.

## The 5-point sync chain (all required)

1. `AffixType` enum — `src/game/item.h` (~line 42)
2. string→enum — `affixTypeFromString()` in `src/game/item_loader.cpp` (~line 133)
3. a `bonus*` field on `PlayerInventory` (and `NpcEquipment` if NPCs share it) — `src/game/item.h` (~line 388 / ~360)
4. accumulate it — `accumulateCommonAffix()` (shared) or the player-only switch in `Inventory::recalculateStats()` — `src/game/inventory.cpp` (~line 29 / ~74); also zero-init it in `recalculateStats` (and `recalculateNpcStats` if shared)
5. **consume** the bonus where it actually changes gameplay (weapon stats in `buildWeaponDef`, health in `getEffectiveMaxHealth`, movement/energy/combat in engine code)
   …then add the `affixes.json` entry.

## Step 1 — Decide the affix

Name (e.g. "of Haste"), what stat it changes, flat vs %, which slots it can roll on
(`weapon`/`offhand`/`helmet`/`armor`/`boots`/`ring`), value range, and **where it gets
consumed** (this is the part people forget — pick the consumption site now). Check
`MAX_AFFIX_DEFS` (`src/game/item.h`, currently 32) for headroom.

## Step 2 — Apply the code edits

Apply `templates/affix_snippets.md` (the five edits, with the exact insertion points and
exemplars to copy). Key decisions baked into the template:
- **Shared (player + NPC) vs player-only:** if NPCs should benefit, add the `bonus*` field to
  **both** `PlayerInventory` and `NpcEquipment` and accumulate in `accumulateCommonAffix()`;
  if player-only (like clip/reload/energy), add the field to `PlayerInventory` only and
  accumulate in the player-only switch in `recalculateStats()`.
- **Never reuse** the deprecated `_REMOVED_RANGE_BONUS` enum slot; always append before `COUNT`.
- `COOLDOWN_REDUCTION` stacks multiplicatively — copy its pattern only if you want diminishing
  returns; most affixes just `+=`.

## Step 3 — Add the affixes.json entry

Append the filled-in `templates/affix_entry.json` to the `"affixes"` array in
`assets/config/affixes.json`. `type` must equal your snake_case string from Step 2; `slots`
is an array of slot names (omit ⇒ all slots).

## Step 4 — Compile

```bash
cmake --build build
```
Fix errors. **Stop here** (build + compile only). *(Optional manual check for the user: equip
an item that rolled the affix and confirm the stat changes.)*

## Step 5 — Document

Inline-comment the consumption math (the *why*). Affix lists live in code, not the cheat
sheet, so no `engine-reference` update is needed unless you changed a cap.

## Gotchas

- Enum ↔ loader ↔ accumulate ↔ consume must all agree; a missing `affixTypeFromString` case
  silently maps your string to `DAMAGE_FLAT`.
- A `bonus*` field that's accumulated but never *consumed* compiles fine and does nothing —
  Step 1's "where it's consumed" decision is what makes the affix real.
- `bonusRange` exists but is legacy/unused — don't model new work on it.
