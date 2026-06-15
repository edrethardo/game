# Visible Equipped Armor + "C" Character Inspection — Design

## Context & Goal

Today equipped armor (helmet/chest/boots/gloves) is **stats-only** — items carry `meshId`/`materialId`
names but nothing renders them on the character, and the player can't review their numbers anywhere.
We want the character to **visibly wear** equipped armor and to be able to **inspect the 3D model +
full stats** with a dedicated button ("C" on desktop, a controller button on Switch).

The game is first-person; the full third-person body is rendered for **remote players** and the
**split-screen partner** (`engine_render_world.cpp`), where the equipped **weapon is already attached
at the hand** — that is the proven pattern this feature extends. The HUD is currently strictly 2D, so
showing a live model in a panel needs a small offscreen render.

**Approved decisions:** per-**tier** armor meshes; a **live rotatable 3D model** in the inspect
screen; a **full stats sheet**; and the feature must **work on Switch** with a **full Switch
build/deploy** as part of delivery.

## A. Armor tier system
Add an armor **tier** `LIGHT / MEDIUM / HEAVY`, **derived from the item's material class** at load
(`*_cloth → LIGHT`, `*_leather → MEDIUM`, `*_plate → HEAVY`); **Legendary**-rarity items keep their
tier mesh but get an emissive/ornate tint. No per-item authoring needed (materials already encode
plate/leather/cloth). Resolve once at item-load into a cached `tierMeshId` per armor `ItemDef`.
- Files: `src/game/item.h` (`ItemDef` += `u8 tierMeshId`; small `ArmorTier` enum or helper),
  `src/game/item_loader.cpp` (derive tier from `materialName`, resolve `tierMeshId`).

## B. Armor meshes
Generate per-slot × per-tier voxel meshes via `tools/gen_mesh.py` (+ `tools/build_assets.py`):
`{helmet,chest,boots,gloves} × {light,medium,heavy}`. **Reuse the existing `helmet/armor/boots/gloves`
meshes as the MEDIUM tier** → ~8 new meshes. Skins/materials reuse existing `armor_*` materials.
Follow the asset-tool convention (never hand-author OBJ).

## C. Wearing it — render armor on the body
Factor `submitPlayerEquipment(pos, yaw, scale, anim, inventory)` in `engine_render_world.cpp`: after
the body mesh, attach each equipped slot's `tierMeshId` at fixed body offsets (helmet→head,
chest→torso, boots→feet, gloves→hands), tinted by material + rarity — mirroring the existing
weapon-attach block. Reused by: remote-player render, split-screen-partner render, and the inspect
FBO. Game stays first-person in normal play.
- **Co-op:** wire each slot's equipped tier-mesh id into the render-interp / snapshot path alongside
  the existing `weaponMeshId` so clients show remote players' armor (`src/net/`, the render-interp
  struct in `engine.h`, client path in `engine_render_world.cpp`).

## D. Inspect overlay — input & behavior
`GameAction::CHARACTER_SCREEN` (new) bound to **C** (free) + a controller button. Toggling sets
`m_characterScreenOpen` (mirrors `m_inventoryOpen`): release mouse, add to `gameplayInputFrozen()`,
play a UI sfx. It's an **overlay bool** (not a `GameState`) — game runs underneath; it's its **own
screen** (not a Tab-inventory tab); in split-screen it belongs to the lane that opened it.
- Files: `src/platform/input.h`/`.cpp` (enum + binding), `src/engine/engine.h` (flag + FBO handles),
  `src/engine/engine_update.cpp` (toggle), `engine_update_player.cpp` (rotation input).

## E. 3D model panel (FBO)
A dedicated offscreen **FBO** (~512px desktop / ~256–384px Switch), created in `Engine::init`,
destroyed in `shutdown`. While the screen is open: clear to a dark backdrop, set a small perspective
camera framing the player, draw class mesh + `submitPlayerEquipment` + weapon with simple lighting at
an **accumulating yaw** (mouse-drag / right-stick; slow idle auto-spin), then composite the FBO color
texture as a HUD quad on the left half. Renders only while open.

## F. Stats sheet (right half)
Full character sheet via `FontSystem::drawText`, grouped:
- **Offense:** damage, attack speed, crit % / crit mult, DPS (`Inventory::getEffectiveWeapon`)
- **Defense:** max HP, armor → mitigation %, damage reduction, regen/sec, thorns %, lifesteal %
- **Utility:** move speed, cooldown reduction, max energy
- **Equipped column:** each slot's item name (rarity color) + key affixes (reuse item-icon + tooltip
  patterns from `hud_inventory.cpp`).
Values from `m_localPlayer` + `m_inventories[lane]` cached bonuses + on-demand helpers
(`armorRating`/`lifestealPct`/`thornsPct`/`healthRegenRate` in `inventory.cpp`). Scales by `sh/720`.
Footer: "drag to rotate · press C/[button] to close". New `renderCharacterScreen()` in
`engine_hud.cpp` (or a new `engine_render_character.cpp`); add a branch to the HUD render routing.

## G. Data flow & edge cases
- **Flow:** equip/unequip → existing `Inventory::recalculateStats()`; the screen only reads state and
  recomputes display values. Armor mesh resolution cached at item-load.
- **Edges:** empty slots skip their overlay; legendary tint; clients need tier-mesh ids in interp;
  FBO renders only while open (~4 extra draw calls per visible armored player otherwise).

## H. Switch compatibility & deployment
- **Input:** no keyboard — open via a **dedicated free controller button or an LB-chord** (the Switch
  layout is crowded: A/B/X/Y, LB modifier, R3 dodge, +/− inventory/pause), finalized for comfort;
  **right stick rotates** the model.
- **Perf:** on-body armor ≤4 draw calls/player (within 300–500); inspect FBO only while open, kept
  small (~256–384 px), respecting the existing 0.65 render-scale path; target 60 fps / 16.6 ms.
- **Deploy:** build via devkitPro/Switch toolchain (`build-switch`, `BUILD_TESTS=OFF`) → `.nro` → run
  on hardware/emulator; verify 60 fps with armor visible and the inspect screen open. **Full Switch
  build + on-device verification is part of delivery**, after the desktop implementation.

## Files to modify (representative)
`src/game/item.h`, `src/game/item_loader.cpp`, `src/game/inventory.cpp` (read-only reuse) ·
`tools/gen_mesh.py`, `tools/build_assets.py`, `assets/meshes/*` · `src/engine/engine_init_assets.cpp`
(cache armor tier mesh ids) · `src/engine/engine_render_world.cpp` (`submitPlayerEquipment`) ·
`src/net/*` + `engine.h` render-interp (co-op armor ids) · `src/platform/input.h`/`.cpp` ·
`src/engine/engine.h`, `engine_update.cpp`, `engine_update_player.cpp` (overlay + rotation) ·
`src/engine/engine_init.cpp` (FBO init/shutdown) · `src/engine/engine_hud.cpp` (or new
`engine_render_character.cpp`) · docs: `engine-reference` (controls), `engine-how-to` (armor recipe).

## Verification
1. **Desktop build** clean; offline render-preview of the new armor meshes.
2. `--new <class>`, equip armor of varying **tiers + rarities** → body shows the tier armor; press
   **C** → model + armor rotates; stat numbers match equipped bonuses (`getEffectiveWeapon`, armor
   mitigation, etc.).
3. **Co-op:** a split-screen partner / remote player visibly shows their equipped armor.
4. **Switch:** `build-switch` build, `.nro` deploy, on-device 60 fps with armor + inspect open, and a
   comfortable controller binding.
