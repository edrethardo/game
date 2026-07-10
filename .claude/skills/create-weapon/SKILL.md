---
name: create-weapon
description: Use when adding a new weapon to DungeonEngine — an items.json entry (slot/weaponType/subtype/stats), its generated mesh + skin + material, and (only if it needs new behavior) a new WeaponSubtype with crit stats and firing logic. Trigger for "add a weapon", "new sword/gun/bow", "make a weapon that does X".
---

# Create a new weapon

Weapons are `ItemDef`s with `slot: "weapon"`. There are **two cases** — most weapons are
**data-only** (no C++). Reference material lives in the `engine-reference` skill; the asset
tools work exactly as in `create-enemy` (generated voxel mesh + per-column skin; never
hand-author `.obj`/`.png`).

## Step 1 — Pin the concept, then ask the user the case

Decide: name, `weaponType` (`melee`/`hitscan`/`projectile`), the subtype, stats, rarity, and
whether it grants a legendary skill. Then **ask the user** (AskUserQuestion) which case applies:

- **(A) Data-only** — reuses an existing `weaponSubtype` (`sword`/`dagger`/`axe`/`claymore`/
  `cleaver`/`pistol`/`smg`/`carbine`/`revolver`/`bow`/`crossbow`/`throwing_knife`/`molotov`/
  `wand`) and existing firing behavior. **No C++.** Just items.json + (optionally) new mesh/skin/
  material. This covers nearly all weapons.
- **(B) New subtype / new behavior** — a new `weaponSubtype` (different crit profile) and/or a
  new firing pattern. Adds C++ (Step 4).

## Step 2 — Visuals (generated mesh + skin + material)

Reuse the asset workflow and generic stubs from the **`create-enemy`** skill
(`templates/mesh_generator.py`, `templates/skin_generator.py`) — the mechanism is identical.
Weapon-specific exemplars to model on:
- mesh: `gen_sword`/`gen_dagger`/`gen_bow`/`gen_wand` in `tools/gen_mesh.py`; register in
  `MESH_TYPES` and add to `build_meshes()` in `tools/build_assets.py`.
- skin: `skin_weapon_sword_tex`/`skin_weapon_bow_tex` in `tools/gen_skin.py`; register in
  `SKIN_TYPES` and add to `build_skins()`.

Then add a material in `assets/materials.json` (`id` == array index; `texture` = the skin png),
and register the mesh in the `kMeshes` table in `src/engine/engine_init_assets.cpp`.
*(Skip this whole step if reusing an existing weapon's mesh + material — e.g. another `"sword"`
using `mesh:"sword"`, `material:"weapon_sword"`.)*
Headroom: `MAX_ITEM_DEFS` (`src/game/item.h`, 160), `MAX_MESH_DEFS`/`MAX_MATERIALS` (see
`engine-reference`).

## Step 3 — items.json entry

Append the filled-in `templates/weapon_entry.json` to the `"items"` array in
`assets/config/items.json`. Set `slot:"weapon"`, `weaponType`, `weaponSubtype`, `mesh`,
`material`, and stats. Field meanings by type:
- **melee**: `baseRange` = reach, `baseConeAngle` = swing arc (deg); clip/projectile fields ignored.
- **hitscan**: `baseRange` = raycast distance; `baseClipSize`/`baseReloadTime` used.
- **projectile**: `baseProjectileSpeed`/`baseProjectileRadius` used; range/cone ignored.

Loader + the exact string→enum maps: `ItemLoader::loadItemDefs` /
`weaponSubtypeFromString` in `src/game/item_loader.cpp`.

## Step 4 — New subtype / behavior (ONLY case B)

Apply `templates/weapon_subtype_snippets.md`:
1. add the value to `WeaponSubtype` (`src/game/weapon.h`, ~line 18);
2. map the string in `weaponSubtypeFromString` (`src/game/item_loader.cpp`, ~line 38);
3. set its crit profile in `buildWeaponDef` (`src/game/inventory.cpp`, ~line 244 — the
   dagger `else if` is the pattern; default is 5% / 2×);
4. *if firing differs* — branch in `Combat::fireMelee`/`fireHitscan`/`fireProjectile`
   (`src/game/combat.cpp`) and/or the dispatch in `Engine::handleWeaponFireForPlayer`.
5. **Fire sound (don't forget — a new subtype is otherwise silent).** Add a
   `SfxId::WEAPON_<X>` + `"sfx_weapon_<x>.wav"` **in lockstep** in `src/audio/audio.{h,cpp}`
   (the `static_assert` guards the enum/table alignment), then a
   `case WeaponSubtype::<X>: AudioSystem::play(SfxId::WEAPON_<X>, 0.5f); break;` in the
   fire-sound switch (`engine_combat.cpp` ~615). Make the slot **hand-pickable** (`tools/pick_sfx.py`
   SLOTS/SLOT_CLASS + a `fetch_audio.py` default) and **let the user pick the actual sound** —
   see the `pick-sfx` skill. A bounce projectile also wants a wall-reflect sound at its ricochet
   site (`projectile.cpp`).

## Step 5 — Build assets + compile

```bash
python3 tools/build_assets.py --all      # only if you generated a new mesh/skin
cmake --build build
```
Fix errors. **Stop here** (build + compile only). *(Optional manual check: spawn/equip the
weapon and fire it.)*

## Step 6 — Document

Inline-comment new code (the *why*). If you added a `WeaponSubtype`, note it in the
`engine-reference` skill per the doc-sync rule.

## Gotchas

- **`assets/config/weapons.json` is dead** — the live stats come from items.json via
  `Inventory::getEffectiveWeapon`/`buildWeaponDef`. Editing weapons.json does nothing.
- **Crit lives in `buildWeaponDef` (inventory.cpp), per subtype** — not in items.json. A new
  crit profile means a new subtype (case B).
- **`legendarySkill` on a weapon** only does something if the matching skill exists (see the
  `create-skill` skill) and the item rolls/equips as LEGENDARY.
- Mesh id 0 (cube) is the fallback if `mesh` doesn't match a `kMeshes` name — typos render a cube.
