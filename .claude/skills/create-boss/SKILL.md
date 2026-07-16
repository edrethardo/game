---
name: create-boss
description: Use when adding a new milestone boss to DungeonEngine — a bosses.json entry (stats, roles, personality, skill, projectile, loot guarantee, floor placement), optionally a dedicated generated mesh + skin + limb config. Trigger for "add a boss", "new floor boss", "milestone encounter on floor N".
---

# Create a new boss

Bosses are `BossDef`s keyed by **floor** and spawned by `Engine::spawnFloorBoss`. Most of a boss
is data-driven (personality AI, roles, loot are all JSON) — the only C++ a boss ever needs is a
*new dedicated mesh* and/or a *new limb config*. Reference material lives in the
`engine-reference` skill; assets work as in `create-enemy`.

## Step 1 — Pin the concept, then ask the user the case

Decide: name, **floor** (milestone: 5,10,…,50), `isMajor` (major = bigger arena + iron maidens +
bonus drops), stats, `roles` (array), `personality` (`berserker`/`kiter`/`teleporter`/`duelist`),
`skillId` (must exist — see `create-skill`), `lootGuarantee` (`legendary` for every shipped boss since the unique-pool
rework — the guarantee rides rollItem's rarityFloor and always pays a named unique; `rare`
stays valid for a deliberately lesser encounter), and the projectile sub-object. (The name needs no netcode work: `Entity.bossDefIdx`
replicates in `SnapEntity` since v16 and guests resolve the nameplate from their own bosses.json —
`nameTag` itself is a host-side pointer and never crosses the wire.) Then **ask the user** which case:

- **(A) Data-only** — reuses an existing boss/enemy mesh, an existing `personality`, and an
  existing `limbConfig` (0–6). **No C++.** Just bosses.json (+ a material if new). Covers most
  bosses.
- **(B) New visuals** — a dedicated mesh + skin and/or a **new limb config**. Adds asset
  generation + `limb_system.cpp` + `kMeshes` + materials.json (Step 3).

Check `MAX_BOSS_DEFS` (`src/game/boss_def.h`, 16; ~10 used) for headroom.

## Step 2 — bosses.json entry

Append the filled-in `templates/boss_entry.json` to the `"bosses"` array in
`assets/config/bosses.json`. One BossDef per floor; `findBossDefIdx(floor)` selects it at spawn.
Loader + the role/personality/skill/rarity string maps: `BossLoader::load` in
`src/game/boss_loader.cpp`.
> **Existing personality ⇒ zero C++** — `BossAI::update` (`src/game/boss_ai.cpp`) already
> implements all four; the JSON just selects one.

## Step 3 — New visuals (ONLY case B)

- **Mesh + skin:** reuse the `create-enemy` asset workflow (`templates/mesh_generator.py`,
  `templates/skin_generator.py`); boss exemplars are `lich`/`warden`/`reaper` in
  `tools/gen_mesh.py` and the boss skins in `tools/gen_skin.py`. Register in
  `MESH_TYPES`/`SKIN_TYPES` + `build_assets.py`, add to `kMeshes`
  (`src/engine/engine_init_assets.cpp`), and add a `boss_<name>` material to
  `assets/materials.json`.
- **New limb config** (only if the body needs limbs that no existing config 0–6 provides — e.g.
  a robed floater that hides legs, or unique appendages): apply
  `templates/boss_limb_config_snippet.md` to `src/game/limb_system.cpp` and set `limbConfig` to
  your new id in the JSON. Reuse an existing config (0–6) whenever you can.

## Step 4 — Build assets + compile

```bash
python3 tools/build_assets.py --all      # only if you generated a new mesh/skin
cmake --build build
```
Fix errors. **Stop here** (build + compile only). *(Optional manual check: descend to the
boss's floor, or temporarily set it to floor 5 to reach it fast.)*

## Step 5 — Document

Inline-comment any new code (the *why*). If you added a limb config or dedicated mesh, note it
in the `engine-reference` skill (the boss-mesh/limb-config notes) per the doc-sync rule.

## Gotchas

- **`minionShield: true` requires the `summoner` role** in `roles` — without minions to hide
  behind, the 75% damage reduction never engages.
- **`secondPhase` ("false death") is NOT data-only** — it needs a hand-coded case in the
  per-floor switch in `enemy_ai.cpp` (only Malachar/floor 20 implements it today). Leave it
  `false` unless you're prepared to add that code.
- **Loot is automatic** — `handleBossLootDrop` (`src/engine/engine_death.cpp`) enforces
  `lootGuarantee` (passed to rollItem as its rarityFloor — a LEGENDARY floor can only return a named unique) + `bonusDrops` + a health globe.
  Just set the JSON fields.
- **`limbConfig > 6` needs code** — `getBossConfig`/`getBossLimbMeshId` only handle 0–6; an
  unhandled id falls back to the base skeleton. Add a case (Step 3).
- A new `skillId` or `personality` string that isn't mapped in the loaders silently falls back
  (skill → none, personality → berserker). Verify the string exists.
- Mesh/material name typos fall back to the cube mesh / material 0.
