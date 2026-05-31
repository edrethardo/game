---
name: create-class-skin
description: Use when adding a new player class (or replacing the visual identity of an existing one) in DungeonEngine — generating its voxel mesh and skin via the Python asset tools, wiring the material / mesh registry / ClassDef. Trigger for "add a class skin", "give the Warrior a unique mesh", "make the player look distinct per class".
---

# Add a player-class skin (mesh + material)

End-to-end workflow for giving one `PlayerClass` a dedicated voxel mesh and skin texture.
Templates to copy live in `templates/` next to this file. Reference material on the broader
engine lives in the `engine-reference` skill; the sibling `create-enemy` skill has the same
voxel/skin tooling notes for monsters.

**Golden rules** (from CLAUDE.md):
- **Never hand-author `.obj`/`.png`.** Meshes and skins are *generated* by the Python tools.
- A mesh and its skin are **voxel-grid aligned** — the skin is a tiny per-voxel-column
  palette, not a diffuse map. If the grids don't match, the texture lands on the wrong voxels.
- Inline-comment the *why* of any non-obvious code you add.

## Where the renderer reads the per-class mesh

`ClassDef` in [src/game/item.h](../../../src/game/item.h) carries `meshName` and `materialName`.
The renderer resolves them at draw time in [src/engine/engine_render_world.cpp](../../../src/engine/engine_render_world.cpp)
(two sites: the network/host-remote branch around line 257, and the split-screen branch around
line 325). The chain is:

```
PlayerClass → kClassDefs[idx].meshName    → findMeshByName  → MeshDef
                          .materialName   → getIdByName     → Material → texture
```

If either lookup misses, the renderer falls back to `"human"` / `"human_skin"`, so a half-built
class won't crash — it just renders as the default humanoid until the assets land.

## How the class travels over the network

The chosen class is replicated to clients via `SnapPlayer.playerClass` (added to
[src/net/snapshot.h](../../../src/net/snapshot.h) when this skill landed). Clients populate
`m_renderInterp.playerClass[i]` from each snapshot in `Client::interpolateRemotePlayers`.
You shouldn't need to touch the wire to add a new skin — it's already on it.

## Step 1 — Pin the design

Decide and write down: the **target class** (one of `PlayerClass`), a **one-line visual
brief** (silhouette + color identity + one or two iconic accents), and the rough voxel
grid (16 voxels tall = humanoid default; bump for bulky armor, drop for slim).

If you're adding a brand-new class (not just reskinning an existing one), do the class
plumbing first — `PlayerClass` enum, `kClassDefs` row, lobby UI — then come back here.

## Step 2 — Generate the mesh

1. Copy `templates/mesh_generator.py` into `tools/gen_mesh.py` as `gen_player_<class>()`.
   Model the body on `gen_humanoid` (`tools/gen_mesh.py`, ~line 322): build a `set` of
   filled `(gx,gy,gz)` voxels with the local `fill_box` helper, **feet at Y=0**, then call
   `add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))`. **Write down the grid extents**
   (min/max `gx` and `gy`) — Step 3 needs them.
   Players are 1.8 m tall in the renderer (`engine_render_world.cpp` `targetH = 1.8f`), so
   default `height=1.8`.
2. Register it in the `MESH_TYPES` dict (`tools/gen_mesh.py`, ~line 3328):
   ```python
   "player_<class>": {"func": gen_player_<class>, "desc": "<one line>. Params: --height",
                       "default_file": "player_<class>.obj"},
   ```
3. Add it to `build_meshes()` in `tools/build_assets.py` (the `meshes = [...]` list,
   under the `# Player class meshes` block):
   ```python
   ["--type", "player_<class>", "--out", os.path.join(mesh_dir, "player_<class>.obj")],
   ```

## Step 3 — Generate the skin

1. Copy `templates/skin_generator.py` into `tools/gen_skin.py` as `skin_player_<class>()`.
   Model on `skin_skeleton` (`tools/gen_skin.py`, ~line 45): return `(w, h, pixel_map)`
   where `pixel_map` is `{(px, py): (r, g, b, a)}`.
   **The grid MUST match the mesh** from Step 2: `w = max_gx - min_gx + 1`,
   `h = max_gy - min_gy + 1`, and a mesh voxel at `(gx, gy)` reads skin pixel
   `(px, py) = (gx - min_gx, gy - min_gy)`.
2. Register in the `SKIN_TYPES` dict (`tools/gen_skin.py`, ~line 3812):
   ```python
   "player_<class>": ("player_<class>_skin_42.png", skin_player_<class>),
   ```
3. Add it to `build_skins()` in `tools/build_assets.py` (the `skins = [...]` list,
   under the `# Player class skins` block):
   ```python
   ("player_<class>", "player_<class>_skin_42.png"),
   ```

## Step 4 — Add the material

Append an entry to `assets/materials.json`. **`id` must equal the array index** (use the next
free index — check the last entry):
```json
{ "id": <next>, "name": "player_<class>_skin", "texture": "player_<class>_skin_42.png" }
```
Headroom: `MAX_MATERIALS` in `src/renderer/material.h` (bump if full).

## Step 5 — Register the mesh with the engine

Add a row to the `kMeshes` table in `src/engine/engine_init_assets.cpp`:
```cpp
{"player_<class>", "assets/meshes/player_<class>.obj"},
```
Headroom: `MAX_MESH_DEFS` in `src/engine/engine.h` (bump if full).
`meshName` → `meshId` is resolved at init.

## Step 6 — Wire it into `ClassDef`

In [src/engine/engine_init.cpp](../../../src/engine/engine_init.cpp) (`kClassDefs` table),
set the target class's `meshName` and `materialName` fields to the names from Steps 2-4:
```cpp
"player_<class>", "player_<class>_skin"
```
(These are the last two fields in each row — see existing entries for the pattern.)

If you're replacing an existing class's look, this is the only `ClassDef` edit needed.
If you added a new class, this is part of the wider class plumbing (Step 1).

## Step 7 — Build the assets

```bash
python3 tools/build_assets.py --meshes --skins
```
Confirm `assets/meshes/player_<class>.obj` and `assets/textures/player_<class>_skin_42.png`
were written.

## Step 8 — Compile

```bash
cmake --build build
```

## Step 9 — Verify in-game

Launch `./build/dungeon_game`, enter the class-select menu, pick the target class, spawn
into the dungeon and look at your character (split-screen or co-op shows the third-person
mesh directly; first-person shows the weapon viewmodel — third-person needs split-screen or
a remote viewer to see).

## Pitfalls

- **Grid mismatch between mesh and skin.** Off-by-one in `w`/`h` puts the wrong pixel under
  every voxel — the texture appears "shifted" and unrelated to the silhouette. Recount the
  mesh's gx/gy range and make `w = max_gx - min_gx + 1`, `h = max_gy - min_gy + 1`.
- **All-faces-same-color voxels.** Every face of a voxel column shares one pixel color
  (avoiding texture seams). Keep accent colors (eyes, glow) muted — they show on the SIDES
  of those voxels too, not just the front. Use the "carve a front face only" trick from
  `gen_humanoid` if a feature has to be face-only.
- **Forgetting either build step.** A new mesh without its skin loads but renders untextured
  (white/default). A skin without its mesh registration loads but isn't drawn. Always run
  `--meshes --skins` together.
- **NPC vs player meshes collide.** NPCs in towns use the older `rogue.obj`/`mage.obj`/etc.
  meshes. Player class meshes use the `player_*` namespace to keep the two separate — don't
  re-use an NPC mesh as a player skin unless you also want the NPCs to change.
- **Forgetting to update `kMeshes` in `engine_init_assets.cpp`.** The OBJ is built but
  `findMeshByName("player_<class>")` returns 0, the renderer falls back to `"human"`, and
  the new mesh never appears.
