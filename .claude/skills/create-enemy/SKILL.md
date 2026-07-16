---
name: create-enemy
description: Use when adding a brand-new enemy to DungeonEngine — generating its voxel mesh and skin via the Python asset tools, wiring the material / mesh registry / enemies.json, and implementing a behavioral gimmick (composed shipped roles + an onHitEffect, or a brand-new EnemyRole). Trigger for requests like "add a new enemy", "create a monster that does X", "make a new mob with a gimmick".
---

# Create a new enemy (mesh + skin + gimmick)

End-to-end workflow for adding one new enemy to DungeonEngine. Follow the steps in order.
Templates to copy live in `templates/` next to this file. Reference material on the broader
engine lives in the `engine-reference` skill.

**Golden rules** (from CLAUDE.md):
- **Never hand-author `.obj`/`.png`.** Meshes and skins are *generated* by the Python tools.
- A mesh and its skin are **voxel-grid aligned** — the skin is a tiny per-voxel-column
  palette, not a diffuse map. If the grids don't match, the texture lands on the wrong voxels.
- Inline-comment the *why* of any non-obvious code you add.

## Step 1 — Pin the concept

Decide and write down: display **name**, **tier** (1-5 → floor range), rough stats
(health / moveSpeed / damage / ranges / attackCooldown), **flying?**, **melee vs ranged**,
and roughly where it sits in the roster. These drive the JSON entry and the mesh size.

## Step 2 — Ask the user the gimmick approach (REQUIRED)

The "gimmick" is the enemy's special behavior. **Stop and ask the user** which approach to
take (use the AskUserQuestion tool) — do not pick for them:

- **(A) No new code** — compose one or more **shipped roles** and/or an **onHitEffect**.
  - Roles (combine freely as a JSON array): `ambush`, `summoner`, `healer`, `aura`,
    `ranged_caster`, `charger`, `bomber`, `shield_bearer` (see the `engine-reference` skill
    for what each does). NOTE: `ambush` is the full stone-statue disguise (gargoyle) —
    spawns DORMANT, **fully invulnerable**, no nameplate, stone-grey tint, wakes only via
    the weeping-angel rule (player in `detectionRange` while NOBODY watches). Only give it
    to an enemy that should be un-damageable until it moves.
  - `onHitEffect`: `0`=none, `1`=poison, `2`=slow, `3`=burn, `4`=freeze (+ `onHitDuration`,
    `onHitDps`). Applied to the player/NPC on the enemy's hit.
  - Covers most gimmicks (e.g. "summoner that burns" = `["summoner"]` + `onHitEffect:3`).
- **(B) New EnemyRole + behavior** — a genuinely novel behavior needs a new role bit and an
  AI branch. **Costs a type widening** (see Step 8): all 8 bits of the `u8` role mask are
  used. Use this only when (A) can't express it.
- **(Hybrid)** — a new role *plus* composed shipped roles / onHitEffect.

Record the choice. It gates Step 8 (skip Step 8 entirely for pure (A)).

## Step 3 — Generate the mesh

1. Copy `templates/mesh_generator.py` into `tools/gen_mesh.py` as `gen_<name>()`. Model the
   body on `gen_humanoid` (`tools/gen_mesh.py`, ~line 322): build a `set` of filled `(gx,gy,gz)`
   voxels with the local `fill_box` helper, **feet at Y=0**, then call
   `add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))`. **Write down the grid extents**
   (min/max `gx` and `gy`) — Step 4 needs them.
2. Register it in the `MESH_TYPES` dict (`tools/gen_mesh.py`, ~line 3328):
   ```python
   "<name>": {"func": gen_<name>, "desc": "<one line>. Params: --height", "default_file": "<name>.obj"},
   ```
3. Add it to `build_meshes()` in `tools/build_assets.py` (the `meshes = [...]` list):
   ```python
   ["--type", "<name>", "--height", "1.8", "--out", os.path.join(mesh_dir, "<name>.obj")],
   ```

## Step 4 — Generate the skin

1. Copy `templates/skin_generator.py` into `tools/gen_skin.py` as `skin_<name>()`. Model on
   `skin_skeleton` (`tools/gen_skin.py`, ~line 45): return `(w, h, pixel_map)` where
   `pixel_map` is `{(px, py): (r, g, b, a)}`.
   **The grid MUST match the mesh** from Step 3: `w = max_gx - min_gx + 1`,
   `h = max_gy - min_gy + 1`, and a mesh voxel at `(gx, gy)` reads skin pixel
   `(px, py) = (gx - min_gx, gy - min_gy)`.
2. Register in the `SKIN_TYPES` dict (`tools/gen_skin.py`, ~line 3812):
   ```python
   "<name>": ("<name>_skin_42.png", skin_<name>),
   ```
3. Add it to `build_skins()` in `tools/build_assets.py` (the `skins = [...]` list):
   ```python
   ("<name>", "<name>_skin_42.png"),
   ```

## Step 5 — Add the material

Append an entry to `assets/materials.json`. **`id` must equal the array index** (use the next
free index — check the last entry). Point `texture` at the skin PNG from Step 4:
```json
{ "id": <next>, "name": "<name>_skin", "texture": "<name>_skin_42.png" }
```
Headroom: `MAX_MATERIALS` in `src/renderer/material.h` (read the real value; bump if full).

> Why this matters: enemy body meshes carry no `usemtl`, so the renderer textures the whole
> body with the enemy's resolved `materialId` — i.e. *this* material → the skin PNG. The chain
> is `enemies.json materialName` → `materials.json` → texture.

## Step 6 — Register the mesh with the engine

Add a row to the `kMeshes` table in `src/engine/engine_init_assets.cpp`:
```cpp
{"<name>", "assets/meshes/<name>.obj"},
```
Headroom: `MAX_MESH_DEFS` in `src/engine/engine.h` (read the real value; bump if full).
`meshName` → `meshId` is resolved at init.

## Step 7 — Add the enemies.json entry

Append the filled-in `templates/enemy_entry.json` object to the `"enemies"` array in
`assets/config/enemies.json`. Set `meshName: "<name>"`, `materialName: "<name>_skin"`, the
`role` (string, or an array of role strings to combine), `aiPreference` (the COMBAT OPENER the enemy aggros into — strafe/flank/surround/retreat/chase; strafe needs attackRange > 5, surround needs grounded melee, or the stat-fit lint in tests/game/test_ai_preference.cpp fails the suite), `onHitEffect`,
`halfExtents` (≈ the mesh's half-size in metres), `tier`, and stats.
Headroom: `MAX_ENEMY_DEFS` in `src/game/enemy_def.h` (currently 64; ~36 used).
Loader + the valid `role`/`aiPreference`/`onHitEffect` values: `src/game/enemy_loader.cpp`.

## Step 8 — Gimmick code (ONLY if Step 2 chose B or Hybrid)

Apply `templates/new_role_snippets.md` — four edits:
1. **New role constant** in the `EnemyRole` namespace (`src/game/entity.h`, ~line 51). The
   next bit is `0x100`, which **does not fit `u8`** → do edit 2.
2. **Widen the role type** `u8`→`u16` in three places: `Entity.enemyRole`
   (`src/game/entity.h`, ~line 147), `EnemyDef.role` (`src/game/enemy_def.h`, ~line 31), and
   `parseRole()`'s return type + its local `u8 mask` (`src/game/enemy_loader.cpp`, ~line 30
   and ~line 129). **`role` is not on the snapshot wire**, so this is a safe local-only change
   — no protocol/serialization edits.
3. **Map the role string** in `parseRole()` (`src/game/enemy_loader.cpp`):
   `if (s == "<role>") return EnemyRole::<ROLE>;`
4. **Add the behavior branch** in `applyRoleModifiers()` (`src/game/enemy_ai_roles.cpp`),
   before the final `return AIStep::Continue;`. Copy the shape of an existing branch
   (CHARGER ~line 168, SUMMONER ~line 28). Use the multi-purpose scratch fields on `Entity`
   for gimmick state — `tacticalTimer`, `kiteTimer`, `sprintTimer`, `hasRetreated`,
   `animTimer` — rather than adding new fields. Comment the *why*.

## Step 9 — Build the assets

```bash
python3 tools/build_assets.py --all      # or: --meshes --skins
```
Confirm `assets/meshes/<name>.obj` and `assets/textures/<name>_skin_42.png` were written.

## Step 10 — Compile

```bash
cmake --build build
```
Fix any errors (a new role's widening can surface type mismatches — chase them down).
**Stop here.** *(Optional manual check for the user: run the game and press `F4` to spawn a
ring of enemies, or `F5` for 50.)*

## Step 11 — Document

Inline-comment new code (the *why*). If you added a new role (Step 8), note it per the
doc-sync rule: update the role list in the `engine-reference` skill.
