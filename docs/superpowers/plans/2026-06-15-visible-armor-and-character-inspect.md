# Visible Equipped Armor + "C" Character Inspection — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the player character visibly wear equipped armor (tier-based meshes on the 3rd-person body) and add a `C`-key / controller-button inspection screen showing a live rotatable 3D model + a full stats sheet — working on desktop and Switch.

**Architecture:** Armor items gain a derived **tier** (LIGHT/MEDIUM/HEAVY from material class) → a cached `tierMeshId` per slot. A shared `submitPlayerEquipment()` renders the body's weapon + armor overlays wherever the 3rd-person body draws (split-screen partner, remote players, and the inspect FBO), mirroring the existing weapon-attach pattern. Remote visibility rides the snapshot (new `armorMeshId[4]`, mirroring `weaponMeshId`). The inspect screen is an overlay bool (like `m_inventoryOpen`) that renders the player model into an offscreen FBO (reusing `ensureSceneFbo`) and draws a stats sheet with `FontSystem::drawText`.

**Tech Stack:** C++17, OpenGL 3.3 (glad), SDL2, custom Renderer/MaterialSystem, doctest (unit tests for pure logic), Python voxel asset tools (`gen_mesh.py`), ENet snapshots.

**Spec:** `docs/superpowers/specs/2026-06-15-visible-armor-and-character-inspect-design.md`

**Conventions:** `u8/u16/f32` aliases; inline comments for non-obvious logic; bump `MAX_*` rather than allocate; pair GL `init`/`shutdown`. Forward-only tests (pure logic only). Frequent commits. Do NOT bump `SAVE_VERSION` (no persisted struct changes here). The snapshot wire format DOES change → both peers must run the same build (co-op already requires that).

---

## File structure

| File | Responsibility | Action |
|---|---|---|
| `src/game/item.h` | `ArmorTier` enum; `ItemDef.tierMeshId`; `armorTierFromMaterial()` decl | Modify |
| `src/game/item_loader.cpp` | implement `armorTierFromMaterial()` | Modify |
| `tests/game/test_armor_tier.cpp` | unit tests for tier derivation | Create |
| `tools/gen_mesh.py` | light/heavy armor mesh generators + registry | Modify |
| `tools/build_assets.py` | emit the new tier meshes | Modify |
| `src/engine/engine_init_assets.cpp` | resolve `tierMeshId` per armor `ItemDef` | Modify |
| `src/engine/engine_render_world.cpp` | `submitPlayerEquipment()`; armor on partner + remote bodies | Modify |
| `src/net/snapshot.h` / `snapshot.cpp` | `SnapshotPlayer.armorMeshId[4]` + (de)serialize | Modify |
| `src/net/client.cpp`, `src/net/client.h` | wire armor mesh ids to interp out-params | Modify |
| `src/engine/engine.h` | `RenderInterp.playerArmorMeshId`; `m_characterScreenOpen`; inspect FBO fields; `m_inspectYaw`; `gameplayInputFrozen()` | Modify |
| `src/platform/input.h` / `input.cpp` | `GameAction::CHARACTER_SCREEN` + bindings | Modify |
| `src/engine/engine_update.cpp` | toggle + rotation input | Modify |
| `src/engine/engine_init.cpp` | inspect FBO shutdown | Modify |
| `src/engine/engine_render_character.cpp` | **new**: `renderCharacterInspect()` (FBO model + stats sheet) | Create |
| `src/CMakeLists.txt` | add `engine_render_character.cpp` | Modify |
| `src/engine/engine_render.cpp` | route to `renderCharacterInspect()` in the HUD pass; expose `ensureSceneFbo` | Modify |
| `.claude/skills/engine-reference/SKILL.md`, `engine-how-to/SKILL.md` | document the control + armor-mesh recipe | Modify |

---

## PHASE 1 — Armor tier system + meshes

### Task 1: Armor tier enum + derivation helper (TDD)

**Files:**
- Modify: `src/game/item.h` (near the `ItemSlot`/`Rarity` enums ~line 30, and `ItemDef` ~line 235)
- Modify: `src/game/item_loader.cpp` (add helper near top)
- Test: `tests/game/test_armor_tier.cpp` (create)
- Modify: `tests/CMakeLists.txt` (add the test + `src/game/item_loader.cpp` if not already linked)

- [ ] **Step 1: Add the enum + ItemDef field + helper declaration**

In `src/game/item.h`, after the `ItemSlot` enum add:
```cpp
// Armor visual weight class, derived from an armor item's material (cloth/leather/plate).
// Drives which tier mesh renders on the body (see armorTierFromMaterial / engine_init_assets).
enum struct ArmorTier : u8 { LIGHT, MEDIUM, HEAVY, COUNT };
```
In the `ItemDef` struct (near the `u8 meshId; u8 materialId;` visual block), add:
```cpp
u8 tierMeshId = 0;   // armor slots only: resolved tier mesh (helmet/chest/boots/gloves _light/_medium/_heavy)
```
Near the other free-function declarations in `item.h` (e.g. by `rarityColor`), add:
```cpp
// Derive armor weight class from a material name: contains "plate"->HEAVY, "leather"->MEDIUM,
// "cloth"->LIGHT; anything else (incl. legendary_*) -> MEDIUM. Case-insensitive substring match.
ArmorTier armorTierFromMaterial(const char* materialName);
```

- [ ] **Step 2: Write the failing test**

Create `tests/game/test_armor_tier.cpp`:
```cpp
#include "doctest/doctest.h"
#include "game/item.h"

TEST_CASE("armorTierFromMaterial maps material class to weight tier") {
    CHECK(armorTierFromMaterial("armor_plate")    == ArmorTier::HEAVY);
    CHECK(armorTierFromMaterial("helmet_plate")   == ArmorTier::HEAVY);
    CHECK(armorTierFromMaterial("boots_leather")  == ArmorTier::MEDIUM);
    CHECK(armorTierFromMaterial("gloves_leather") == ArmorTier::MEDIUM);
    CHECK(armorTierFromMaterial("armor_cloth")    == ArmorTier::LIGHT);
    CHECK(armorTierFromMaterial("Cloth_Robe")     == ArmorTier::LIGHT); // case-insensitive
    CHECK(armorTierFromMaterial("legendary_armor")== ArmorTier::MEDIUM); // unknown -> MEDIUM default
    CHECK(armorTierFromMaterial("")               == ArmorTier::MEDIUM);
}
```

- [ ] **Step 3: Add the test to the test build**

In `tests/CMakeLists.txt`, add `game/test_armor_tier.cpp` and (if not already in the source list) `${CMAKE_SOURCE_DIR}/src/game/item_loader.cpp` to the `dungeon_tests` `add_executable` source list.

- [ ] **Step 4: Run the test, verify it fails to link/compile**

Run: `cmake --build build --target dungeon_tests 2>&1 | tail -20`
Expected: FAIL — `undefined reference to armorTierFromMaterial`.

- [ ] **Step 5: Implement the helper**

In `src/game/item_loader.cpp`, near the top (after includes), add:
```cpp
#include <cctype>
#include <cstring>

// Case-insensitive substring search (no <algorithm>/locale dependency in hot paths).
static bool containsCI(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    for (; *hay; ++hay) {
        const char* h = hay; const char* n = needle;
        while (*h && *n && std::tolower((unsigned char)*h) == std::tolower((unsigned char)*n)) { ++h; ++n; }
        if (!*n) return true;
    }
    return false;
}

ArmorTier armorTierFromMaterial(const char* materialName) {
    if (containsCI(materialName, "plate"))   return ArmorTier::HEAVY;
    if (containsCI(materialName, "cloth"))   return ArmorTier::LIGHT;
    if (containsCI(materialName, "leather")) return ArmorTier::MEDIUM;
    return ArmorTier::MEDIUM;  // sensible default (incl. legendary_* and unknowns)
}
```

- [ ] **Step 6: Run the test, verify it passes**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*armorTier*"`
Expected: PASS (8 checks).

- [ ] **Step 7: Commit**
```bash
git add src/game/item.h src/game/item_loader.cpp tests/game/test_armor_tier.cpp tests/CMakeLists.txt
git commit -m "feat(armor): ArmorTier enum + material-derived tier helper (TDD)"
```

---

### Task 2: Generate per-tier armor meshes

**Files:**
- Modify: `tools/gen_mesh.py` (gens ~2569-2660; registry ~4908-4925)
- Modify: `tools/build_assets.py` (mesh list ~127-130)

Existing `helmet`/`armor`/`boots`/`gloves` meshes become the **MEDIUM** tier. Add **LIGHT** (slimmer/cloth) and **HEAVY** (chunkier/plate) variants per slot by parameterizing the existing generators.

- [ ] **Step 1: Parameterize each armor generator with a `bulk` factor**

For each of `gen_helmet`, `gen_armor`, `gen_boots`, `gen_gloves` in `tools/gen_mesh.py`, add a `bulk=1.0` parameter and multiply the voxel half-extents / shell thickness by `bulk` (LIGHT=0.8, MEDIUM=1.0, HEAVY=1.25). Example for `gen_helmet`:
```python
def gen_helmet(height=0.3, bulk=1.0):
    # ... existing body, but scale shell thickness / brim by `bulk`, e.g.:
    t = max(1, round(base_thickness * bulk))   # apply `bulk` to each fill_box extent
    # (HEAVY = thicker plates, LIGHT = thinner; keep the same silhouette)
```
Do the equivalent in `gen_armor`/`gen_boots`/`gen_gloves` (thicker pauldrons/soles/cuffs for HEAVY, slimmer for LIGHT).

- [ ] **Step 2: Register the 8 new tier variants**

In the `MESH_TYPES` dict, add entries (keep the bare names as MEDIUM aliases):
```python
"helmet_light":  {"func": lambda: gen_helmet(bulk=0.8),  "desc": "Light helmet", "default_file": "helmet_light.obj"},
"helmet_heavy":  {"func": lambda: gen_helmet(bulk=1.25), "desc": "Heavy helmet", "default_file": "helmet_heavy.obj"},
"chest_light":   {"func": lambda: gen_armor(bulk=0.8),   "desc": "Light chest",  "default_file": "chest_light.obj"},
"chest_heavy":   {"func": lambda: gen_armor(bulk=1.25),  "desc": "Heavy chest",  "default_file": "chest_heavy.obj"},
"boots_light":   {"func": lambda: gen_boots(bulk=0.8),   "desc": "Light boots",  "default_file": "boots_light.obj"},
"boots_heavy":   {"func": lambda: gen_boots(bulk=1.25),  "desc": "Heavy boots",  "default_file": "boots_heavy.obj"},
"gloves_light":  {"func": lambda: gen_gloves(bulk=0.8),  "desc": "Light gloves", "default_file": "gloves_light.obj"},
"gloves_heavy":  {"func": lambda: gen_gloves(bulk=1.25), "desc": "Heavy gloves", "default_file": "gloves_heavy.obj"},
```
(The MEDIUM tier reuses the existing `helmet.obj`/`armor.obj`/`boots.obj`/`gloves.obj`; chest MEDIUM = existing `armor.obj`.)

- [ ] **Step 3: Add them to build_assets.py**

After the existing armor mesh lines (~127-130) in `tools/build_assets.py`:
```python
["--type", "helmet_light",  "--out", os.path.join(mesh_dir, "helmet_light.obj")],
["--type", "helmet_heavy",  "--out", os.path.join(mesh_dir, "helmet_heavy.obj")],
["--type", "chest_light",   "--out", os.path.join(mesh_dir, "chest_light.obj")],
["--type", "chest_heavy",   "--out", os.path.join(mesh_dir, "chest_heavy.obj")],
["--type", "boots_light",   "--out", os.path.join(mesh_dir, "boots_light.obj")],
["--type", "boots_heavy",   "--out", os.path.join(mesh_dir, "boots_heavy.obj")],
["--type", "gloves_light",  "--out", os.path.join(mesh_dir, "gloves_light.obj")],
["--type", "gloves_heavy",  "--out", os.path.join(mesh_dir, "gloves_heavy.obj")],
```

- [ ] **Step 4: Generate + sanity-check**

Run: `python3 tools/build_assets.py 2>&1 | grep -iE "helmet_|chest_|boots_|gloves_|fail|error"`
Then: `ls -la assets/meshes/{helmet_light,helmet_heavy,chest_light,chest_heavy,boots_light,boots_heavy,gloves_light,gloves_heavy}.obj`
Expected: all 8 `.obj` files exist, non-empty.

- [ ] **Step 5: Commit**
```bash
git add tools/gen_mesh.py tools/build_assets.py assets/meshes/*.obj
git commit -m "feat(armor): generate light/heavy tier meshes for helmet/chest/boots/gloves"
```

---

### Task 3: Resolve `tierMeshId` per armor ItemDef at init

**Files:**
- Modify: `src/engine/engine_init_assets.cpp` (where item mesh names are resolved to mesh ids after item load)

- [ ] **Step 1: Find the resolution point**

Run: `grep -n "meshName\|findMeshByName\|m_itemDefs\[" src/engine/engine_init_assets.cpp | head`
Locate the loop that resolves each `ItemDef.meshName -> meshId` (item_loader leaves meshId for Engine, per item_loader.cpp:430).

- [ ] **Step 2: Resolve the armor tier mesh alongside it**

In that loop, after resolving `meshId`, add (for armor slots only):
```cpp
ItemDef& d = m_itemDefs[i];
if (d.slot == ItemSlot::HELMET || d.slot == ItemSlot::ARMOR ||
    d.slot == ItemSlot::BOOTS  || d.slot == ItemSlot::GLOVES) {
    static const char* kSlotBase[] = { /*HELMET*/"helmet", /*ARMOR*/"chest",
                                       /*BOOTS*/"boots",   /*GLOVES*/"gloves" };
    // Map ItemSlot -> 0..3 base index (HELMET=2,ARMOR=3,BOOTS=4,GLOVES=6 in the enum)
    int base = (d.slot == ItemSlot::HELMET) ? 0 : (d.slot == ItemSlot::ARMOR) ? 1
             : (d.slot == ItemSlot::BOOTS)  ? 2 : 3;
    ArmorTier tier = armorTierFromMaterial(d.materialName);
    char meshName[40];
    const char* suffix = (tier == ArmorTier::LIGHT) ? "_light"
                       : (tier == ArmorTier::HEAVY) ? "_heavy" : "";
    // MEDIUM uses the bare existing mesh; chest MEDIUM is "armor", chest L/H are "chest_*"
    if (tier == ArmorTier::MEDIUM)
        std::snprintf(meshName, sizeof(meshName), "%s", base == 1 ? "armor" : kSlotBase[base]);
    else
        std::snprintf(meshName, sizeof(meshName), "%s%s", kSlotBase[base], suffix);
    s32 mid = findMeshByName(meshName);  // same resolver used for d.meshId
    d.tierMeshId = (mid > 0) ? static_cast<u8>(mid) : d.meshId;  // fallback to the item's own mesh
}
```
(Confirm the exact `findMeshByName` call/spelling used in this file and match it.)

- [ ] **Step 3: Build + log-check**

Run: `cmake --build build 2>&1 | grep -iE "error" ; echo done`
Expected: builds clean.
Add a temporary `LOG_INFO("armor '%s' tierMesh=%u", d.name, d.tierMeshId);` if you want to eyeball resolution in the run log, then remove it.

- [ ] **Step 4: Commit**
```bash
git add src/engine/engine_init_assets.cpp
git commit -m "feat(armor): resolve per-slot tier mesh id on armor ItemDefs at init"
```

---

## PHASE 2 — Wear armor on the third-person body

### Task 4: `submitPlayerEquipment()` — render weapon + armor on the body

**Files:**
- Modify: `src/engine/engine_render_world.cpp` (weapon-attach block ~303-338; split-screen partner ~343-402)
- Modify: `src/engine/engine.h` (declare the helper)

- [ ] **Step 1: Declare the helper**

In `engine.h` (private methods, near other render helpers):
```cpp
// Draw the equipped weapon + armor overlays onto a 3rd-person body at (pos, yaw, scale).
// `anim` is the existing weapon anim flag bits (bit0 attacking, bit1 reloading).
// Used by the split-screen partner, remote players, and the inspect FBO.
void submitPlayerEquipment(const Vec3& pos, f32 yaw, f32 scale, u8 anim,
                           const PlayerInventory& inv);
```

- [ ] **Step 2: Implement it (mirror the existing weapon attach + add 4 armor slots)**

In `engine_render_world.cpp`, add the function. The weapon half is the existing block (copy from lines ~303-338); the armor half loops the 4 slots using `tierMeshId`:
```cpp
void Engine::submitPlayerEquipment(const Vec3& pos, f32 yaw, f32 scale, u8 anim,
                                   const PlayerInventory& inv) {
    const u8 defaultTex = m_meshDefs.empty() ? 0 : /* existing default tex used in this file */;

    // --- Weapon (existing hand-attach math) ---
    const ItemInstance& wpn = inv.equipped[static_cast<u32>(ItemSlot::WEAPON)];
    if (wpn.defId < m_itemDefCount) {
        u8 wMesh = m_itemDefs[wpn.defId].meshId;
        u8 wMat  = m_itemDefs[wpn.defId].materialId;
        if (wMesh > 0 && wMesh < m_meshDefCount) {
            f32 thrust = (anim & 1) ? 0.25f : 0.0f;
            f32 drop   = (anim & 2) ? -0.25f : 0.0f;
            Vec3 right = {-sinf(yaw + 1.57f), 0, -cosf(yaw + 1.57f)};
            Vec3 fwd   = {-sinf(yaw), 0, -cosf(yaw)};
            Vec3 wpnPos = pos + Vec3{0, 0.8f + drop, 0} + right * 0.35f * scale + fwd * (0.3f + thrust) * scale;
            Mat4 m = Mat4::translate(wpnPos) * Mat4::rotateY(yaw) * Mat4::scale({0.4f*scale,0.4f*scale,0.4f*scale});
            const Material* mm = MaterialSystem::get(wMat);
            Renderer::submit(m_basicShader, mm ? mm->texture : defaultTex,
                             m_meshDefs[wMesh].mesh, m, m_meshDefs[wMesh].bounds,
                             mm ? mm->tint : Vec4{1,1,1,1});
        }
    }

    // --- Armor overlays: helmet, chest, boots, gloves ---
    struct ArmorSlot { ItemSlot slot; Vec3 off; };
    const ArmorSlot slots[] = {
        { ItemSlot::HELMET, {0, 0.92f, 0} },   // crown of head
        { ItemSlot::ARMOR,  {0, 0.55f, 0} },   // torso (overlays body)
        { ItemSlot::BOOTS,  {0, 0.06f, 0} },   // feet
        { ItemSlot::GLOVES, {0, 0.55f, 0} },   // hands (drawn at both hands below)
    };
    for (const ArmorSlot& a : slots) {
        const ItemInstance& it = inv.equipped[static_cast<u32>(a.slot)];
        if (it.defId >= m_itemDefCount) continue;
        const ItemDef& def = m_itemDefs[it.defId];
        u8 mesh = def.tierMeshId;
        if (mesh == 0 || mesh >= m_meshDefCount) continue;
        const Material* mat = MaterialSystem::get(def.materialId);
        Vec4 tint = mat ? mat->tint : Vec4{1,1,1,1};
        if (it.rarity == Rarity::LEGENDARY) tint = tint * Vec4{1.3f,1.15f,0.7f,1.0f}; // ornate/emissive hint
        Vec3 ap = pos + a.off * scale;
        Mat4 m = Mat4::translate(ap) * Mat4::rotateY(yaw) * Mat4::scale({scale,scale,scale});
        Renderer::submit(m_basicShader, mat ? mat->texture : defaultTex,
                         m_meshDefs[mesh].mesh, m, m_meshDefs[mesh].bounds, tint);
        if (a.slot == ItemSlot::GLOVES) { // second glove on the other hand
            Vec3 right = {-sinf(yaw + 1.57f), 0, -cosf(yaw + 1.57f)};
            Mat4 m2 = Mat4::translate(ap + right * (-0.5f) * scale) * Mat4::rotateY(yaw) * Mat4::scale({scale,scale,scale});
            Renderer::submit(m_basicShader, mat ? mat->texture : defaultTex,
                             m_meshDefs[mesh].mesh, m2, m_meshDefs[mesh].bounds, tint);
        }
    }
}
```
(Match `m_basicShader`, the default texture variable name, `m_meshDefs[].mesh/.bounds`, and `MaterialSystem::get` exactly as used elsewhere in this file. Tune the Y offsets in Step 4 against the actual body mesh.)

- [ ] **Step 3: Call it for the split-screen partner**

In the split-screen partner render (~lines 343-400), **replace** the inline weapon-draw with one call after the body submit:
```cpp
submitPlayerEquipment(pos, yaw, scale, animFlags, m_inventories[otherP]);
```
(Remove the now-duplicated inline weapon block for the partner so the weapon isn't drawn twice.)

- [ ] **Step 4: Build + visual-verify offsets (split-screen)**

Run: `cmake --build build-rel 2>&1 | grep -iE "error"; echo ok`
Then run windowed split-screen (or `--new <class>` and use the existing partner path) with armor equipped; capture via F8 and Read the screenshot. Adjust the 4 Y-offsets until helmet sits on the head, chest overlays the torso, boots at the feet, gloves at the hands. Iterate offsets, rebuild, recapture.

- [ ] **Step 5: Commit**
```bash
git add src/engine/engine.h src/engine/engine_render_world.cpp
git commit -m "feat(armor): submitPlayerEquipment renders weapon+armor on the 3rd-person body"
```

---

### Task 5: Co-op — sync armor mesh ids over the snapshot

**Files:**
- Modify: `src/net/snapshot.h` (`SnapshotPlayer`, ~line 35)
- Modify: `src/net/snapshot.cpp` (size-asserts/comments ~309; full serialize ~460/833; deserialize ~631/897)
- Modify: `src/net/client.h` / `client.cpp` (~526/572 out-params)
- Modify: `src/engine/engine.h` (`RenderInterp`, ~443) and `engine_render_world.cpp` (remote render uses interp)

- [ ] **Step 1: Add the field to SnapshotPlayer**

In `src/net/snapshot.h`, after `u8 weaponMeshId;` (line 35):
```cpp
u8   armorMeshId[4]; // helmet, chest, boots, gloves tier-mesh ids (0 = empty). 3rd-person rendering.
```

- [ ] **Step 2: Populate on the server when building the snapshot**

Where `sp.weaponMeshId = ...` is set (snapshot.cpp:103 region, server-side fill from inventory), add:
```cpp
const PlayerInventory& inv = /* the inventory used to fill sp */;
const ItemSlot aslots[4] = { ItemSlot::HELMET, ItemSlot::ARMOR, ItemSlot::BOOTS, ItemSlot::GLOVES };
for (int k = 0; k < 4; ++k) {
    const ItemInstance& it = inv.equipped[static_cast<u32>(aslots[k])];
    sp.armorMeshId[k] = (it.defId < /*itemDefCount*/) ? itemDefs[it.defId].tierMeshId : 0;
}
```
(Use the same inventory/itemDef access already used to set `weaponMeshId` here.)

- [ ] **Step 3: Serialize + deserialize (both full and delta paths)**

In every place `weaponMeshId` is written, write the 4 armor bytes right after:
- After `w8(sp.weaponMeshId);` (lines ~460 and ~833): `for (int k=0;k<4;++k) w8(sp.armorMeshId[k]);`
- After `sp.weaponMeshId = r.readU8();` (lines ~631 and ~897): `for (int k=0;k<4;++k) sp.armorMeshId[k] = r.readU8();`

Update the byte-count comments at snapshot.cpp:309/314 (+4 bytes/player).

- [ ] **Step 4: Carry through the client interp out-params**

In `client.h`, the snapshot-read API that outputs `outWeaponMeshId[]` — add a parallel `u8 outArmorMeshId[][4]` (or `(*outArmorMeshId)[4]`) param. In `client.cpp` (~526 and ~572), alongside `outWeaponMeshId[slot] = sp.weaponMeshId;` add:
```cpp
if (outArmorMeshId) for (int k=0;k<4;++k) outArmorMeshId[slot][k] = sp.armorMeshId[k];
```

- [ ] **Step 5: Store in RenderInterp + pass at the call site**

In `engine.h` `RenderInterp` (after `playerWeaponMeshId`):
```cpp
u8 playerArmorMeshId[MAX_PLAYERS][4]; // helmet/chest/boots/gloves tier meshes (clients lack remote inventories)
```
At the engine's snapshot-read call (where `m_renderInterp.playerWeaponMeshId` is passed), pass `m_renderInterp.playerArmorMeshId` to the new param.

- [ ] **Step 6: Use it in the remote-player render**

In `engine_render_world.cpp` remote-player branch (~232-301): the host already has `m_inventories[i]` → call `submitPlayerEquipment(pos, yaw, scale, anim, m_inventories[i])`. For **clients** (no remote inventory), build a transient `PlayerInventory` is overkill — instead add a small overload that takes explicit ids:
```cpp
// declared in engine.h:
void submitPlayerEquipmentIds(const Vec3& pos, f32 yaw, f32 scale, u8 anim,
                              u8 weaponMeshId, const u8 armorMeshId[4],
                              const u8 armorMatId[4] /*optional, may be all 0*/);
```
Refactor Task-4's body to share an inner impl taking (weaponMeshId, weaponMatId, armorMeshId[4], armorMatId[4]); the inventory version resolves those from `m_itemDefs`, the ids version takes them directly. Clients pass `m_renderInterp.playerArmorMeshId[i]` (material ids default 0 → use the mesh's default texture/tint; acceptable for remote view). Host/partner use the inventory version (full material + rarity tint).

- [ ] **Step 7: Build + co-op verify**

Run: `cmake --build build-rel 2>&1 | grep -iE "error"; echo ok`
Then host + join on one machine: `./build-rel/src/DungeonEngine --host --new warrior` and a 2nd instance `--join 127.0.0.1 --new ranger`; equip armor on each; confirm each sees the other's armor. (Snapshot format changed — both must be this build.)

- [ ] **Step 8: Commit**
```bash
git add src/net/snapshot.h src/net/snapshot.cpp src/net/client.h src/net/client.cpp src/engine/engine.h src/engine/engine_render_world.cpp
git commit -m "feat(armor): sync equipped armor mesh ids over snapshot for remote players"
```

---

## PHASE 3 — "C" character inspection overlay

### Task 6: Input action + overlay toggle + rotation input

**Files:**
- Modify: `src/platform/input.h` (`GameAction` enum, ~line 17)
- Modify: `src/platform/input.cpp` (`setDefaults` ~line 119)
- Modify: `src/engine/engine.h` (`m_characterScreenOpen`, `m_inspectYaw`, `gameplayInputFrozen()`)
- Modify: `src/engine/engine_update.cpp` (toggle + rotation, near the inventory toggle ~1126-1145)

- [ ] **Step 1: Add the action + binding**

`input.h`: insert `CHARACTER_SCREEN,` before `COUNT` in `GameAction`.
`input.cpp` `setDefaults()` (after the inventory bind): keyboard `C`, gamepad chord `LB + Right-stick click` (a free chord; the right stick is also the model-rotate input while open):
```cpp
// Character/inspect screen. Keyboard: C. Gamepad: LB + R3 chord (R3 alone is dodge in-game).
set(GameAction::CHARACTER_SCREEN, SDL_SCANCODE_C, 0,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
```
(Match the existing `set(...)` chord signature used by BOOT_SKILL/HELMET_SKILL.)

- [ ] **Step 2: Add engine state**

`engine.h` (near `m_inventoryOpen`):
```cpp
bool m_characterScreenOpen = false;
f32  m_inspectYaw = 0.6f;   // model rotation in the inspect FBO (radians)
```
Inspect FBO handles (near `m_sceneFbo` line 583):
```cpp
u32 m_inspectFbo = 0, m_inspectColorTex = 0, m_inspectDepthRbo = 0, m_inspectFboW = 0, m_inspectFboH = 0;
```
Update `gameplayInputFrozen()`:
```cpp
bool gameplayInputFrozen() const { return m_inventoryOpen || m_characterScreenOpen || m_menu.confirmQuit; }
```

- [ ] **Step 3: Toggle + rotate in gameUpdate**

In `engine_update.cpp` after the inventory toggle:
```cpp
if (Input::isActionPressed(GameAction::CHARACTER_SCREEN)) {
    m_characterScreenOpen = !m_characterScreenOpen;
    if (m_localPlayerIndex == 0) Input::setRelativeMouseMode(!m_characterScreenOpen && !m_inventoryOpen);
    AudioSystem::play(SfxId::UI_CONFIRM);
}
if (m_characterScreenOpen) {
    // Drag mouse X or push right stick to spin; gentle idle auto-spin.
    s32 mdx = 0, mdy = 0; Input::getMouseDelta(mdx, mdy);
    f32 stick = Input::getGamepadAxis(0, SDL_CONTROLLER_AXIS_RIGHTX); // -1..1, may be 0
    m_inspectYaw += (f32)mdx * 0.01f + stick * 0.04f + 0.15f * (1.0f/60.0f);
}
```
(Match the real `Input::getMouseDelta` / `getGamepadAxis` names; if absent, use the same accessors the camera uses.)

- [ ] **Step 4: Build**

Run: `cmake --build build-rel 2>&1 | grep -iE "error"; echo ok` — builds clean (no rendering yet; pressing C just toggles + frees mouse).

- [ ] **Step 5: Commit**
```bash
git add src/platform/input.h src/platform/input.cpp src/engine/engine.h src/engine/engine_update.cpp
git commit -m "feat(inspect): CHARACTER_SCREEN action + overlay toggle + model-rotate input"
```

---

### Task 7: Render the player model into the inspect FBO

**Files:**
- Modify: `src/engine/engine_render.cpp` (make `ensureSceneFbo` non-static/visible, or add a header decl)
- Create: `src/engine/engine_render_character.cpp` (the inspect render)
- Modify: `src/CMakeLists.txt` (add the new file)
- Modify: `src/engine/engine_init.cpp` (delete the inspect FBO in `shutdown`)
- Modify: `src/engine/engine.h` (declare `renderCharacterInspect(u32 sw,u32 sh)` and `renderInspectModelToFbo()`)

- [ ] **Step 1: Expose `ensureSceneFbo`**

In `engine_render.cpp`, change `static void ensureSceneFbo(...)` to a file-or-class-visible function (drop `static` and add a forward decl in a shared header, OR make it a private `Engine::ensureFbo(...)` method). Simplest: add to `engine.h`:
```cpp
void ensureFbo(u32& fbo, u32& colorTex, u32& depthRbo, u32& curW, u32& curH, u32 w, u32 h);
```
and move the body of `ensureSceneFbo` into `Engine::ensureFbo` (update the two existing call sites at engine_render.cpp:544).

- [ ] **Step 2: Render the model into the FBO**

Create `src/engine/engine_render_character.cpp`:
```cpp
// engine_render_character.cpp — the C-key character inspection overlay: renders the player's
// class mesh + equipped weapon/armor (via submitPlayerEquipment) into an offscreen FBO with a
// small orbit camera, then composites it + a stats sheet in renderCharacterInspect().
#include "engine/engine.h"
#include "renderer/renderer.h"
#include "renderer/material.h"
#include "renderer/font.h"
#include "renderer/hud.h"
#include "platform/window.h"
#include "game/inventory.h"
#include <glad/glad.h>
#include <cstdio>
#include <cmath>

void Engine::renderInspectModelToFbo() {
    const u32 size = 512; // Switch lowers this (Task 9)
    ensureFbo(m_inspectFbo, m_inspectColorTex, m_inspectDepthRbo, m_inspectFboW, m_inspectFboH, size, size);
    glBindFramebuffer(GL_FRAMEBUFFER, m_inspectFbo);
    glViewport(0, 0, (s32)size, (s32)size);
    glClearColor(0.08f, 0.07f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    // Orbit camera framing a ~1.8 m figure at origin.
    Vec3 target = {0, 0.9f, 0};
    Vec3 eye = target + Vec3{ sinf(m_inspectYaw)*2.6f, 0.35f, cosf(m_inspectYaw)*2.6f };
    Mat4 view = Mat4::lookAt(eye, target, {0,1,0});
    Mat4 proj = Mat4::perspective(0.9f, 1.0f /*square*/, 0.1f, 20.0f);
    Renderer::setViewProj(view, proj);                 // match the real Renderer API
    Renderer::setLightDir({-0.4f, -1.0f, -0.5f});      // if such a setter exists; else rely on default

    // Body
    const PlayerInventory& inv = m_inventories[m_localPlayerIndex];
    const ClassDef& cd = kClassDefs[static_cast<u32>(m_playerClass)];
    u8 bodyMesh = findMeshByName(cd.meshName);
    u8 bodyMat  = MaterialSystem::getIdByName(cd.materialName);
    Mat4 m = Mat4::translate({0,0,0}) * Mat4::rotateY(m_inspectYaw) * Mat4::scale({1,1,1});
    const Material* bm = MaterialSystem::get(bodyMat);
    Renderer::submit(m_basicShader, bm ? bm->texture : 0, m_meshDefs[bodyMesh].mesh, m,
                     m_meshDefs[bodyMesh].bounds, bm ? bm->tint : Vec4{1,1,1,1});
    // Weapon + armor (reuse Task 4); pass m_inspectYaw as the body yaw, idle anim 0.
    submitPlayerEquipment({0,0,0}, m_inspectYaw, 1.0f, 0, inv);

    Renderer::flush();                                  // draw everything submitted into the FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
```
(Adapt `Renderer::setViewProj/flush/setLightDir` and `Mat4::lookAt/perspective` to the actual API — grep `Mat4::perspective`, `Renderer::` to confirm names. The model rotates by `m_inspectYaw`.)

- [ ] **Step 3: Add to CMake + shutdown**

`src/CMakeLists.txt`: add `engine/engine_render_character.cpp` to the `DungeonEngine` sources.
`engine_init.cpp` `shutdown()`: 
```cpp
if (m_inspectFbo)      { glDeleteFramebuffers(1, &m_inspectFbo); m_inspectFbo = 0; }
if (m_inspectColorTex) { glDeleteTextures(1, &m_inspectColorTex); m_inspectColorTex = 0; }
if (m_inspectDepthRbo) { glDeleteRenderbuffers(1, &m_inspectDepthRbo); m_inspectDepthRbo = 0; }
```

- [ ] **Step 4: Build**

Run: `cmake --build build-rel 2>&1 | grep -iE "error"; echo ok`
Expected: builds clean (FBO renders but isn't shown until Task 8). Fix any Renderer/Mat4 API mismatches now.

- [ ] **Step 5: Commit**
```bash
git add src/CMakeLists.txt src/engine/engine.h src/engine/engine_render.cpp src/engine/engine_render_character.cpp src/engine/engine_init.cpp
git commit -m "feat(inspect): render player model+gear into an offscreen FBO"
```

---

### Task 8: Character screen UI (model panel + stats sheet) + routing

**Files:**
- Modify: `src/engine/engine_render_character.cpp` (`renderCharacterInspect`)
- Modify: `src/engine/engine_render.cpp` or `engine_hud.cpp` (route to it in the HUD pass)
- Reference: `src/game/inventory.h`/`.cpp` stat helpers; `src/renderer/font.h` (`FontSystem::drawText(sw,sh,x,y,text,color,scale)`); `src/renderer/hud_inventory.cpp` (tooltip/affix formatting)

- [ ] **Step 1: Implement `renderCharacterInspect`**

Append to `engine_render_character.cpp`:
```cpp
void Engine::renderCharacterInspect(u32 sw, u32 sh) {
    f32 uiScale = (f32)sh / 720.0f;
    // 1) render the 3D model into the FBO (done before the HUD ortho pass — see Step 2 ordering)
    // 2) dark full-screen backdrop
    HUD::drawFilledRect(sw, sh, 0, 0, (f32)sw, (f32)sh, {0.03f,0.03f,0.05f,0.92f}); // match real HUD rect API

    // 3) model panel (left): composite the FBO color texture as a quad
    f32 panel = 0.42f * sh;
    f32 px = 0.10f * sw, py = 0.5f * sh - panel * 0.5f;
    HUD::drawTexturedQuad(sw, sh, px, py, panel, panel, m_inspectColorTex); // match real API (UV 0..1)

    // 4) stats sheet (right) — grouped
    const Player& p = m_localPlayer;
    const PlayerInventory& inv = m_inventories[m_localPlayerIndex];
    WeaponDef w = Inventory::getEffectiveWeapon(inv, m_playerClass); // confirm signature
    f32 x = 0.55f * sw, y = sh - 0.18f * sh; const f32 dy = 22.0f * uiScale;
    auto row = [&](const char* label, const char* val, Vec3 c){
        FontSystem::drawText(sw, sh, x, y, label, {0.78f,0.78f,0.86f}, 2);
        FontSystem::drawText(sw, sh, x + 200.0f*uiScale, y, val, c, 2); y -= dy; };
    char b[48];
    FontSystem::drawText(sw, sh, x, y, "OFFENSE", {0.95f,0.8f,0.3f}, 2); y -= dy;
    std::snprintf(b,sizeof b,"%.1f", w.damage); row("Damage", b, {1,0.8f,0.4f});
    std::snprintf(b,sizeof b,"%.2f/s", 1.0f/w.cooldown); row("Attack Speed", b, {1,0.8f,0.4f});
    std::snprintf(b,sizeof b,"%.0f%%  x%.1f", w.critChance*100, w.critMult); row("Crit", b, {1,0.8f,0.4f});
    std::snprintf(b,sizeof b,"%.1f", w.damage*(1+(w.critMult-1)*w.critChance)/w.cooldown); row("DPS", b, {1,0.9f,0.5f});
    y -= dy*0.5f;
    FontSystem::drawText(sw, sh, x, y, "DEFENSE", {0.95f,0.8f,0.3f}, 2); y -= dy;
    std::snprintf(b,sizeof b,"%.0f / %.0f", p.health, p.maxHealth); row("Health", b, {0.3f,0.85f,0.35f});
    f32 ar = Inventory::armorRating(inv); std::snprintf(b,sizeof b,"%.0f (%.0f%%)", ar, 100*ar/(ar+100)); row("Armor", b, {0.6f,0.8f,1});
    std::snprintf(b,sizeof b,"%.0f%%", p.damageReduction*100); row("Dmg Reduction", b, {0.6f,0.8f,1});
    std::snprintf(b,sizeof b,"%.1f/s", Inventory::healthRegenRate(inv)); row("Regen", b, {0.6f,0.8f,1});
    std::snprintf(b,sizeof b,"%.0f%%", Inventory::lifestealPct(inv)*100); row("Lifesteal", b, {0.6f,0.8f,1});
    std::snprintf(b,sizeof b,"%.0f%%", Inventory::thornsPct(inv)*100); row("Thorns", b, {0.6f,0.8f,1});
    y -= dy*0.5f;
    FontSystem::drawText(sw, sh, x, y, "UTILITY", {0.95f,0.8f,0.3f}, 2); y -= dy;
    std::snprintf(b,sizeof b,"%.1f", p.moveSpeed); row("Move Speed", b, {0.8f,0.8f,0.95f});
    std::snprintf(b,sizeof b,"%.0f%%", inv.bonusCooldownReduction*100); row("Cooldown Red.", b, {0.8f,0.8f,0.95f});
    std::snprintf(b,sizeof b,"%.0f", m_skillStates[m_localPlayerIndex].maxEnergy); row("Energy", b, {0.8f,0.8f,0.95f});

    FontSystem::drawText(sw, sh, sw*0.5f - 120.0f*uiScale, 0.04f*sh,
        "Drag to rotate  -  C to close", {0.55f,0.55f,0.6f}, 1);
}
```
(Confirm `HUD::drawFilledRect`/`drawTexturedQuad` names — if absent, add a tiny textured-quad helper using the same VAO/ortho path as `ItemIconSystem::drawIcon`. Confirm `Inventory::getEffectiveWeapon`, `armorRating`, `lifestealPct`, `thornsPct`, `healthRegenRate` signatures from inventory.h.)

- [ ] **Step 2: Route to it (FBO render before the HUD ortho pass)**

In `render()` (engine_render.cpp): **before** the per-player HUD/ortho work, if `m_characterScreenOpen` call `renderInspectModelToFbo()` (it needs a 3D pass with its own FBO bind/viewport, so do it before the 2D HUD pass and restore the window viewport after). Then in the HUD routing branch (where `m_inventoryOpen ? renderInventoryHUD`):
```cpp
if (m_characterScreenOpen)      renderCharacterInspect(hudW, hudH);
else if (m_inventoryOpen)       renderInventoryHUD(hudW, hudH);
else { /* normal HUD */ }
```
Ensure the window viewport/`glBindFramebuffer(0)` is restored after the FBO render so the HUD draws to the screen.

- [ ] **Step 3: Build + verify the whole feature**

Run: `cmake --build build-rel 2>&1 | grep -iE "error"; echo ok`
Run: `./build-rel/src/DungeonEngine --new warrior --floor 3` → pick up/equip armor → press **C**: model panel shows the armored character, drag rotates it, stat numbers populate. Capture with F8 and Read it; verify numbers match (e.g., equip a +health item and watch Health rise).

- [ ] **Step 4: Commit**
```bash
git add src/engine/engine_render_character.cpp src/engine/engine_render.cpp src/engine/engine.h
git commit -m "feat(inspect): character screen — model panel + full stats sheet + routing"
```

---

## PHASE 4 — Switch + docs + deploy

### Task 9: Switch fit (perf + binding) + docs

**Files:**
- Modify: `src/engine/engine_render_character.cpp` (FBO size guard)
- Modify: `.claude/skills/engine-reference/SKILL.md`, `.claude/skills/engine-how-to/SKILL.md`

- [ ] **Step 1: Shrink the inspect FBO on Switch**

In `renderInspectModelToFbo`, replace the fixed size:
```cpp
#ifdef __SWITCH__
    const u32 size = 320;
#else
    const u32 size = 512;
#endif
```

- [ ] **Step 2: Confirm the gamepad binding is comfortable**

Verify `LB + R3` (Task 6) doesn't collide with an existing in-game chord (grep `LEFTSHOULDER` chords in input.cpp). If it does, switch to another free chord (e.g., `LB + LEFTSTICK`-click is the profiler on Switch — avoid; prefer a face-button chord). Document the final choice.

- [ ] **Step 3: Document**

`engine-reference` SKILL.md (controls/debug-keys area): add "C / (LB+R3 on gamepad) — character inspect screen (rotatable model + stats)". `engine-how-to`: add a short "adding/looking at armor tier meshes" recipe (gen_mesh `bulk` variants → build_assets → `tierMeshId` resolution).

- [ ] **Step 4: Commit**
```bash
git add src/engine/engine_render_character.cpp .claude/skills/engine-reference/SKILL.md .claude/skills/engine-how-to/SKILL.md
git commit -m "feat(inspect): Switch FBO size guard + docs"
```

### Task 10: Switch build + on-device deploy

- [ ] **Step 1: Build the Switch target**

Run the Switch toolchain build (devkitPro): `cmake -B build-switch -DCMAKE_TOOLCHAIN_FILE=<switch toolchain> -DBUILD_TESTS=OFF && cmake --build build-switch` (use the project's existing Switch build invocation). Expected: produces the `.nro`.

- [ ] **Step 2: Deploy + verify on device/emulator**

Deploy the `.nro` to hardware (or emulator). In-game: equip armor (visible on co-op partner), open the inspect screen via the gamepad binding, rotate the model, read stats. Confirm **60 fps / 16.6 ms** holds with armor visible and the inspect screen open (check the F3 profiler). Note any perf regression and lower the inspect FBO size or skip the world 3D pass while the screen is open if needed.

- [ ] **Step 3: Commit any Switch fixups**
```bash
git add -A && git commit -m "chore(switch): build/deploy fixups for armor + inspect screen"
```

---

## Self-review notes (coverage)
- Spec A (tier) → Task 1; B (meshes) → Task 2; resolution → Task 3.
- Spec C (wear on body) → Task 4; co-op interp → Task 5.
- Spec D (input/overlay) → Task 6; E (FBO model) → Task 7; F (stats sheet) → Task 8.
- Spec G (edges: empty slots skip, legendary tint, FBO only while open) → Tasks 4/7/8.
- Spec H (Switch) → Tasks 9-10.

**API names to confirm before/while coding (grep first, they vary):** `Renderer::submit/flush/setViewProj/setLightDir`, `Mat4::lookAt/perspective/rotateY/scale/translate`, `MaterialSystem::get/getIdByName`, `findMeshByName`, `m_basicShader`, `m_meshDefs[].mesh/.bounds`, default-texture var, `Input::getMouseDelta/getGamepadAxis`, `HUD::drawFilledRect/drawTexturedQuad` (add if missing, copy `ItemIconSystem::drawIcon`'s quad path), `Inventory::getEffectiveWeapon/armorRating/lifestealPct/thornsPct/healthRegenRate`, `kClassDefs`, `m_skillStates[].maxEnergy`. The plan's code is grounded in the explored patterns; match exact spellings at the call site.
