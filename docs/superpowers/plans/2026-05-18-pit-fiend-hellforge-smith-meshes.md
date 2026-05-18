# Pit Fiend & Hellforge Smith Mesh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Pit Fiend and Hellforge Smith unique voxel meshes, EnemyTypes, and limb animations so they are visually distinct tier-4 enemies instead of sharing the butcher rig.

**Architecture:** Add two new `EnemyType` enum values (`PIT_FIEND`, `HELLFORGE_SMITH`) with dedicated `LimbConfig`s and animation cases. Rewrite the Pit Fiend mesh generator with real 3D wings, and create a new Hellforge Smith mesh generator with a hunched blacksmith silhouette. Update skins to match new grid dimensions. Wire through enemy_loader and render code.

**Tech Stack:** C++17, Python 3 (gen_mesh.py, gen_skin.py), OpenGL 3.3 (limb rendering)

---

### Task 1: Add EnemyType enum values

**Files:**
- Modify: `src/game/entity.h:33-45`

- [ ] **Step 1: Add PIT_FIEND and HELLFORGE_SMITH to EnemyType**

In `src/game/entity.h`, insert two new enum values after `SUCCUBUS` and before `BOSS`:

```cpp
enum struct EnemyType : u8 {
    GENERIC = 0,  // no limbs, single mesh
    SKELETON,     // 2 legs, 2 arms, weapon carrying
    BAT,          // 2 wings, 2 claws
    SPIDER,       // 8 legs, 2 mandibles
    MIMIC,        // disguised as chest, attacks when approached
    HELLHOUND,    // quadruped canine demon — 4 legs, galloping animation
    SENTINEL,     // armored shield-bearer — 2 legs, shield arm, blocking stance
    SUCCUBUS,     // harpy-style flyer — 2 bat wings, 2 dangling talons, no walking legs
    PIT_FIEND,    // winged demon — 2 bat wings, 2 legs, wing-flap + walk animation
    HELLFORGE_SMITH, // hunched blacksmith — 2 legs, hammer arm, hammer-swing idle
    BOSS,         // large boss enemy (uses skeleton rig, oversized)
    PROP,         // static decoration — no AI, no collision response, no animation
    COUNT
};
```

---

### Task 2: Wire enemy_loader to map mesh names to new types

**Files:**
- Modify: `src/game/enemy_loader.cpp:56-72`

- [ ] **Step 1: Update inferEnemyType()**

Replace the `pit_fiend` line that currently maps to `EnemyType::BOSS` and add the `hellforge_smith` mapping. The updated function:

```cpp
static EnemyType inferEnemyType(const char* meshName) {
    if (std::strcmp(meshName, "bat") == 0)    return EnemyType::BAT;
    if (std::strcmp(meshName, "spider") == 0) return EnemyType::SPIDER;
    if (std::strcmp(meshName, "butcher") == 0) return EnemyType::BOSS;
    if (std::strcmp(meshName, "hellhound") == 0) return EnemyType::HELLHOUND;
    if (std::strcmp(meshName, "sentinel") == 0) return EnemyType::SENTINEL;
    if (std::strcmp(meshName, "succubus") == 0) return EnemyType::SUCCUBUS;
    // Pit Fiend: winged demon with dedicated rig (was BOSS, now own type)
    if (std::strcmp(meshName, "pit_fiend") == 0) return EnemyType::PIT_FIEND;
    // Hellforge Smith: hunched blacksmith with hammer arm
    if (std::strcmp(meshName, "hellforge_smith") == 0) return EnemyType::HELLFORGE_SMITH;
    // Remaining butcher-rig enemies keep BOSS type
    if (std::strcmp(meshName, "cave_troll") == 0)     return EnemyType::BOSS;
    if (std::strcmp(meshName, "abyssal_titan") == 0)  return EnemyType::BOSS;
    return EnemyType::SKELETON;
}
```

---

### Task 3: Add LimbConfigs and animation for both types

**Files:**
- Modify: `src/game/limb_system.cpp`

- [ ] **Step 1: Add s_pitFiendConfig after s_succubusConfig (after line ~137)**

```cpp
// Pit Fiend: 2 large bat wings + 2 walking legs
static const LimbConfig s_pitFiendConfig = {
    4,
    {
        // Wings (0-1) — large, at upper back, roll axis (pivotAxis=2) for flapping
        {{ 0.30f, 1.20f, 0.10f}, {0.65f, 0.05f, 0.40f}, 0.0f, 2, false},  // left wing
        {{-0.30f, 1.20f, 0.10f}, {0.65f, 0.05f, 0.40f}, 0.0f, 2, true},   // right wing
        // Legs (2-3) — bipedal walk, wider stance than skeleton
        {{ 0.15f, 0.30f, 0.0f}, {0.12f, 0.25f, 0.12f}, 0.0f, 0, false},   // left leg
        {{-0.15f, 0.30f, 0.0f}, {0.12f, 0.25f, 0.12f}, 0.0f, 0, true},    // right leg
    }
};
```

- [ ] **Step 2: Add s_hellforgeSmithConfig after s_pitFiendConfig**

```cpp
// Hellforge Smith: 2 heavy legs + 1 oversized hammer arm (right side)
static const LimbConfig s_hellforgeSmithConfig = {
    3,
    {
        // Legs (0-1) — wide stance, heavy walk
        {{ 0.18f, 0.25f, 0.0f}, {0.14f, 0.22f, 0.14f}, 0.0f, 0, false},   // left leg
        {{-0.18f, 0.25f, 0.0f}, {0.14f, 0.22f, 0.14f}, 0.0f, 0, true},    // right leg
        // Hammer arm (2) — right side, large, pivotAxis=0 for swing
        {{-0.40f, 0.65f, 0.0f}, {0.16f, 0.30f, 0.16f}, -0.2f, 0, false},  // rests slightly raised
    }
};
```

- [ ] **Step 3: Add cases in getConfig() (after line ~221)**

```cpp
        case EnemyType::PIT_FIEND:       return s_pitFiendConfig;
        case EnemyType::HELLFORGE_SMITH: return s_hellforgeSmithConfig;
```

- [ ] **Step 4: Add cases in getLimbMeshId() (after line ~248)**

```cpp
        case EnemyType::PIT_FIEND:
            // 0-1 = wings, 2-3 = legs
            return (limbIdx < 2) ? s_wingMeshId : s_legMeshId;
        case EnemyType::HELLFORGE_SMITH:
            // 0-1 = legs, 2 = hammer arm
            return (limbIdx < 2) ? s_legMeshId : s_armMeshId;
```

- [ ] **Step 5: Add PIT_FIEND animation case in computeAngle() (after the SUCCUBUS case ~line 445)**

```cpp
        case EnemyType::PIT_FIEND: {
            if (limbIdx < 2) {
                // Wings: slow majestic flap — slower and wider than succubus
                f32 flapSpeed = isMoving ? 6.0f : 3.0f;
                f32 phase = fmodf(e.animTimer * flapSpeed, 6.2832f);
                f32 angle;
                if (phase < 2.0f) {
                    // Powerful downstroke — wider arc (1.0 vs succubus 0.9)
                    angle = -sinf(phase * 1.57f) * 1.0f;
                } else {
                    // Slow recovery upstroke
                    f32 t = (phase - 2.0f) / 4.2832f;
                    angle = -1.0f * (1.0f - t * t);
                }
                if (e.attackAnimT > 0.0f) {
                    // Attack: wings flare wide (intimidation display)
                    f32 t = e.attackAnimT / 0.4f;
                    angle = -0.7f + sinf(t * 3.14159f) * 1.3f;
                }
                return angle;
            } else {
                // Legs: standard walk cycle (same as skeleton)
                f32 walkFreq = 8.0f;
                f32 walkAmp = 0.6f;
                f32 phase = (limbIdx == 2) ? 0.0f : 3.14159f;
                f32 swing = sinf(e.animTimer * walkFreq + phase) * walkAmp * speed01;
                if (e.attackAnimT > 0.0f) {
                    swing *= 0.3f; // dampen walk during attack
                }
                return swing;
            }
        }
```

- [ ] **Step 6: Add HELLFORGE_SMITH animation case after PIT_FIEND**

```cpp
        case EnemyType::HELLFORGE_SMITH: {
            if (limbIdx < 2) {
                // Legs: slow heavy walk — 0.6x normal frequency
                f32 walkFreq = 4.8f;
                f32 walkAmp = 0.5f;
                f32 phase = (limbIdx == 0) ? 0.0f : 3.14159f;
                f32 swing = sinf(e.animTimer * walkFreq + phase) * walkAmp * speed01;
                if (e.attackAnimT > 0.0f) {
                    swing *= 0.2f;
                }
                return swing;
            } else {
                // Hammer arm: slow pendulum idle, slam on attack
                f32 idle = sinf(e.animTimer * 1.0f) * 0.15f;
                if (e.attackAnimT > 0.0f) {
                    // Heavy downward slam — fast snap, -1.2 radians
                    f32 t = e.attackAnimT / 0.5f;
                    return -1.2f * sinf(t * 3.14159f);
                }
                return idle;
            }
        }
```

---

### Task 4: Update render code for new EnemyTypes

**Files:**
- Modify: `src/engine/engine_render.cpp`

The render code has several `EnemyType::BOSS` checks for visual effects and limb handling. The new types need to be included where appropriate.

- [ ] **Step 1: Update skipMirrorAngle check (line ~1200-1201)**

The pit fiend wings should mirror (like succubus), and legs should alternate. The hellforge smith legs should alternate. Neither should skip mirror. No change needed — the current check only applies to SKELETON and BOSS, so the new types already fall through correctly.

- [ ] **Step 2: Update isArm check (line ~1244-1245)**

The hellforge smith's hammer arm (limbIdx 2) uses OBJ arm mesh. Add it to the arm detection:

```cpp
                            bool isArm = (li < 2) && (e.enemyType == EnemyType::SKELETON ||
                                                       e.enemyType == EnemyType::BOSS);
                            // Hellforge smith hammer arm is limb index 2
                            if (e.enemyType == EnemyType::HELLFORGE_SMITH && li == 2) isArm = true;
```

- [ ] **Step 3: Update ground ring glow (line ~1443)**

Pit fiends and hellforge smiths are imposing tier-4 enemies — give them the boss-sized ring:

```cpp
        if (e.enemyType == EnemyType::BOSS ||
            e.enemyType == EnemyType::PIT_FIEND ||
            e.enemyType == EnemyType::HELLFORGE_SMITH) {
```

- [ ] **Step 4: Update light starburst (line ~1520)**

Same treatment — larger glow for these imposing enemies:

```cpp
        } else if (e.enemyType == EnemyType::BOSS ||
                   e.enemyType == EnemyType::PIT_FIEND ||
                   e.enemyType == EnemyType::HELLFORGE_SMITH) {
```

- [ ] **Step 5: Commit C++ changes**

```bash
git add src/game/entity.h src/game/enemy_loader.cpp src/game/limb_system.cpp src/engine/engine_render.cpp
git commit -m "feat: add PIT_FIEND and HELLFORGE_SMITH EnemyTypes with limb rigs

New EnemyType enum values with dedicated LimbConfigs:
- PIT_FIEND: 4 limbs (2 wings + 2 legs), majestic wing-flap animation
- HELLFORGE_SMITH: 3 limbs (2 legs + hammer arm), slow heavy walk + hammer swing

Wired through enemy_loader inferEnemyType() and render visual effects."
```

---

### Task 5: Rewrite gen_pit_fiend() mesh generator

**Files:**
- Modify: `tools/gen_mesh.py:1797-1845`

- [ ] **Step 1: Replace gen_pit_fiend() with redesigned mesh**

Replace the entire `gen_pit_fiend` function (lines 1797-1845) with:

```python
def gen_pit_fiend(height=2.4):
    """Pit Fiend — classic winged demon. Massive bat wings (real 3D geometry),
    muscular torso, curved horns, barrel chest, cloven hooves. Balrog-style.
    Origin at feet (Y=0). 24 voxels tall, 15 wide for wing span."""
    mb = MeshBuilder()
    vs = height / 24.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Curved horns (gy 22-23) — swept back
    fill_box(-3, 22, 0, 1, 2, 1)      # left horn base
    filled.add((-4, 23, 1))            # left horn tip (swept back)
    fill_box(2, 22, 0, 1, 2, 1)       # right horn base
    filled.add((3, 23, 1))             # right horn tip

    # Head (gy 18-21) — 5x4x4, imposing
    fill_box(-2, 18, -2, 5, 4, 4)
    # Deep eye sockets
    filled.discard((-2, 20, -2))
    filled.discard((2, 20, -2))

    # Thick neck (gy 16-17)
    fill_box(-1, 16, -1, 3, 2, 3)

    # Massive torso (gy 10-15) — 7 wide, 4 deep
    fill_box(-3, 10, -2, 7, 6, 4)

    # Barrel chest protrusion (front face, gy 12-14)
    fill_box(-2, 12, -3, 5, 3, 1)

    # Wing roots on upper back (gy 13-15) — anchor geometry
    fill_box(-4, 13, 2, 1, 3, 2)      # left wing root
    fill_box(3, 13, 2, 1, 3, 2)       # right wing root

    # Left wing — MAIN FEATURE — real 3D bat wings
    # Bone struts (2 voxels deep for visual mass)
    fill_box(-7, 14, 1, 3, 2, 2)      # upper strut
    fill_box(-7, 12, 2, 3, 2, 2)      # lower strut
    fill_box(-6, 11, 3, 2, 1, 2)      # wing tip
    # Membrane panels (fill gaps between struts)
    fill_box(-6, 13, 1, 2, 1, 1)      # upper membrane
    fill_box(-5, 12, 1, 1, 2, 1)      # inner membrane
    fill_box(-7, 13, 3, 1, 1, 1)      # outer membrane

    # Right wing (mirrored)
    fill_box(4, 14, 1, 3, 2, 2)
    fill_box(4, 12, 2, 3, 2, 2)
    fill_box(4, 11, 3, 2, 1, 2)
    fill_box(4, 13, 1, 2, 1, 1)
    fill_box(4, 12, 1, 1, 2, 1)
    fill_box(6, 13, 3, 1, 1, 1)

    # Arms — upper (gy 11-14)
    fill_box(-4, 11, -1, 1, 4, 2)
    fill_box(3, 11, -1, 1, 4, 2)
    # Arms — lower (gy 7-10)
    fill_box(-4, 7, -1, 1, 4, 2)
    fill_box(3, 7, -1, 1, 4, 2)
    # Fists (2x2x2)
    fill_box(-5, 6, -1, 2, 2, 2)
    fill_box(3, 6, -1, 2, 2, 2)

    # Tail — trailing from lower back
    filled.add((0, 9, 2))
    filled.add((0, 8, 3))
    filled.add((0, 7, 3))
    filled.add((0, 6, 4))
    filled.add((0, 5, 4))

    # Legs (gy 4-8) — thick thighs
    fill_box(-3, 4, -1, 3, 5, 3)
    fill_box(1, 4, -1, 3, 5, 3)
    # Calves (gy 1-3)
    fill_box(-2, 1, -1, 2, 3, 2)
    fill_box(1, 1, -1, 2, 3, 2)
    # Cloven hooves (gy 0)
    fill_box(-3, 0, -2, 3, 1, 4)
    fill_box(1, 0, -2, 3, 1, 4)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb
```

- [ ] **Step 2: Update MESH_TYPES entry for pit_fiend (line ~2760)**

The entry already exists. Update the height parameter if needed — the current entry uses `height=2.4` which matches. No change needed.

---

### Task 6: Add gen_hellforge_smith() mesh generator

**Files:**
- Modify: `tools/gen_mesh.py` (insert after gen_pit_fiend)
- Modify: `tools/gen_mesh.py` MESH_TYPES dict

- [ ] **Step 1: Add gen_hellforge_smith() function**

Insert after `gen_pit_fiend()`:

```python
def gen_hellforge_smith(height=2.0):
    """Hellforge Smith — hunched demonic blacksmith. Stocky, wide shoulders,
    massive hammer arm (right), iron apron, forge bellows on back.
    Shorter and wider than butcher. Origin at feet (Y=0). 18 voxels tall."""
    mb = MeshBuilder()
    vs = height / 18.0
    filled = set()

    def fill_box(x0, y0, z0, w, h, d):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                for z in range(z0, z0 + d):
                    filled.add((x, y, z))

    # Head (gy 15-16) — small, sunken between shoulders
    fill_box(-1, 15, -1, 3, 2, 3)
    # Brow ridge / visor (gy 16)
    fill_box(-2, 16, -2, 5, 1, 1)
    # Eye sockets
    filled.discard((-1, 16, -2))
    filled.discard((1, 16, -2))

    # Massive shoulder hump (gy 13-14) — widest part, hunched look
    fill_box(-5, 13, -1, 11, 2, 3)

    # Broad torso (gy 8-12)
    fill_box(-4, 8, -2, 9, 5, 5)

    # Iron apron on front (gy 6-11)
    fill_box(-3, 6, -3, 7, 6, 1)

    # Forge bellows on back (gy 9-12)
    fill_box(-3, 9, 3, 2, 4, 1)       # left bellows
    fill_box(1, 9, 3, 2, 4, 1)        # right bellows
    fill_box(-1, 10, 3, 2, 2, 1)      # bellows connector

    # Left arm — normal (gy 5-12)
    fill_box(-5, 9, -1, 1, 4, 2)      # upper
    fill_box(-5, 5, -1, 1, 4, 2)      # lower
    fill_box(-6, 4, -1, 2, 2, 2)      # left fist

    # Right arm — HAMMER ARM (oversized, gy 5-13)
    fill_box(4, 9, -1, 2, 4, 2)       # upper arm (thicker)
    fill_box(4, 5, -1, 2, 4, 2)       # lower arm (thicker)
    # Anvil fist / hammer head (3x3x3)
    fill_box(4, 3, -2, 3, 3, 3)

    # Wide pelvis (gy 5-7)
    fill_box(-4, 5, -1, 9, 3, 3)

    # Short thick legs (gy 2-4)
    fill_box(-3, 2, -1, 3, 3, 2)
    fill_box(1, 2, -1, 3, 3, 2)

    # Heavy boots (gy 0-1)
    fill_box(-4, 0, -2, 4, 2, 4)
    fill_box(1, 0, -2, 4, 2, 4)

    ox = -0.5 * vs
    oz = -0.5 * vs
    add_voxel_model(mb, filled, vs, offset=(ox, 0, oz))
    return mb
```

- [ ] **Step 2: Add MESH_TYPES entry**

Add after the `pit_fiend` entry in the MESH_TYPES dict:

```python
    "hellforge_smith": {
        "func": lambda height=2.0: gen_hellforge_smith(height),
        "desc": "Hunched demonic blacksmith with hammer arm. Params: --height",
        "default_file": "hellforge_smith.obj",
    },
```

---

### Task 7: Update skin_pit_fiend() for new grid dimensions

**Files:**
- Modify: `tools/gen_skin.py:2163-2199`

- [ ] **Step 1: Replace skin_pit_fiend() to match 15x24 grid**

The new mesh grid is 15 wide (gx -7..7) x 24 tall (gy 0..23). Replace the function:

```python
def skin_pit_fiend():
    """Pit Fiend: dark obsidian body, 3D bat wings (burgundy membrane),
    magma crack veins, burning orange eyes, curved horns."""
    w, h = 15, 24
    p = {}
    obsidian = (30, 25, 35, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = obsidian

    # Horns (gy 22-23) — dark purple-black
    for py in range(22, 24):
        for px in range(w):
            p[(px, py)] = (25, 20, 28, 255)

    # Head (gy 18-21)
    for py in range(18, 22):
        for px in range(w):
            p[(px, py)] = (35, 28, 38, 255)

    # Burning orange eyes — gx -2 and +2 at gy 20
    # Grid offset: gx -7 maps to px 0, so gx -2 = px 5, gx +2 = px 9
    p[(5, 20)] = (240, 140, 20, 255)
    p[(9, 20)] = (240, 140, 20, 255)

    # Wing membrane (gx -7..-4 and 4..7, gy 11-16) — deep burgundy
    # gx -7 = px 0, gx -4 = px 3, gx 4 = px 11, gx 7 = px 14
    for py in range(11, 17):
        for px in [0, 1, 2, 3]:
            p[(px, py)] = (80, 20, 25, 255)
        for px in [11, 12, 13, 14]:
            p[(px, py)] = (80, 20, 25, 255)

    # Wing bone struts — darker burgundy within wing area
    for py in [12, 13, 14, 15]:
        p[(0, py)] = (50, 15, 18, 255)
        p[(14, py)] = (50, 15, 18, 255)

    # Magma crack veins on torso (alternating pattern)
    # Torso gx -3..3 = px 4..10
    for py in [10, 12, 14]:
        for px in range(4, 11):
            if (px + py) % 2 == 0:
                p[(px, py)] = (220, 100, 20, 255)

    # Barrel chest — slightly lighter obsidian (gx -2..2, gy 12-14)
    # px 5..9
    for py in [12, 13]:
        for px in range(5, 10):
            if p[(px, py)] != (220, 100, 20, 255):  # don't overwrite veins
                p[(px, py)] = (38, 32, 42, 255)

    # Dark extremities — hooves and lower legs (gy 0-3)
    for py in range(0, 4):
        for px in range(w):
            p[(px, py)] = (20, 18, 25, 255)

    # Hooves — ember glow
    for px in range(w):
        p[(px, 0)] = (40, 20, 10, 255)

    # Fists — dark charcoal
    # gx -5..-4 = px 2..3, gx 3..4 = px 10..11 at gy 6-7
    for py in [6, 7]:
        for px in [2, 3]:
            p[(px, py)] = (22, 18, 26, 255)
        for px in [10, 11]:
            p[(px, py)] = (22, 18, 26, 255)

    # Tail (gy 5-9) — darker obsidian
    # gx 0 = px 7
    for py in range(5, 10):
        p[(7, py)] = (25, 20, 30, 255)

    return w, h, p
```

---

### Task 8: Update skin_hellforge_smith() for new grid dimensions

**Files:**
- Modify: `tools/gen_skin.py:2202-2241`

- [ ] **Step 1: Replace skin_hellforge_smith() to match 13x18 grid**

The new mesh grid is 13 wide (gx -6..6) x 18 tall (gy 0..17). Replace the function:

```python
def skin_hellforge_smith():
    """Hellforge Smith: soot-black body, glowing forge-arm (right), iron apron,
    forge bellows (rusted copper), ember eyes, massive shoulder hump."""
    w, h = 13, 18
    p = {}
    soot = (40, 35, 30, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = soot

    # Brow ridge / head (gy 15-17) — lighter soot for face definition
    for py in range(15, 18):
        for px in range(w):
            p[(px, py)] = (45, 38, 32, 255)

    # Red ember eyes — gx -1 and +1 at gy 16
    # Grid offset: gx -6 maps to px 0, so gx -1 = px 5, gx +1 = px 7
    p[(5, 16)] = (200, 60, 20, 255)
    p[(7, 16)] = (200, 60, 20, 255)

    # Shoulder hump (gy 13-14) — darker soot
    for py in [13, 14]:
        for px in range(w):
            p[(px, py)] = (35, 30, 25, 255)

    # Iron-grey apron (front, gx -3..3, gy 6-11)
    # gx -3 = px 3, gx 3 = px 9
    for py in range(6, 12):
        for px in range(3, 10):
            p[(px, py)] = (120, 125, 130, 255)

    # Glowing orange forge-arm on right side (gx 4..5, gy 3-12)
    # gx 4 = px 10, gx 5 = px 11
    for py in range(3, 13):
        p[(10, py)] = (200, 120, 30, 255)
        p[(11, py)] = (200, 120, 30, 255)

    # Hammer head extra glow (gx 4..6, gy 3-5) — brighter orange
    # gx 6 = px 12
    for py in range(3, 6):
        p[(10, py)] = (230, 150, 40, 255)
        p[(11, py)] = (230, 150, 40, 255)
        p[(12, py)] = (230, 150, 40, 255)

    # Forge bellows on back — rusted copper (back face voxels at gz=3)
    # These voxels have gx -3..-2 and 1..2, gy 9-12
    # gx -3 = px 3, gx -2 = px 4, gx 1 = px 7, gx 2 = px 8
    # UVs map by (gx, gy) so the bellows color shows on all faces
    for py in range(9, 13):
        for px in [3, 4]:
            p[(px, py)] = (140, 90, 50, 255)
        for px in [7, 8]:
            p[(px, py)] = (140, 90, 50, 255)

    # Left arm — normal soot (gx -5..-6, gy 4-12)
    # gx -6 = px 0, gx -5 = px 1
    for py in range(4, 13):
        p[(0, py)] = (35, 30, 25, 255)
        p[(1, py)] = (35, 30, 25, 255)

    # Boots — darkest charcoal (gy 0-1)
    for py in range(0, 2):
        for px in range(w):
            p[(px, py)] = (30, 25, 20, 255)

    # Legs — dark (gy 2-4)
    for py in range(2, 5):
        for px in range(w):
            if p[(px, py)] == soot:
                p[(px, py)] = (35, 30, 25, 255)

    return w, h, p
```

- [ ] **Step 2: Commit skin changes**

```bash
git add tools/gen_skin.py
git commit -m "feat: update pit fiend and hellforge smith skins for new grid dimensions

pit_fiend: 12x21 -> 15x24, burgundy wings, magma veins, ember hooves
hellforge_smith: 12x21 -> 13x18, forge-orange hammer arm, iron apron, copper bellows"
```

---

### Task 9: Update build_assets.py and enemies.json

**Files:**
- Modify: `tools/build_assets.py` (mesh build list, ~line 77)
- Modify: `assets/config/enemies.json` (hellforge smith entry, ~line 618)

- [ ] **Step 1: Add hellforge_smith to mesh build list**

After the `pit_fiend` line (line 77), add:

```python
        ["--type", "hellforge_smith", "--height", "2.0", "--out", os.path.join(mesh_dir, "hellforge_smith.obj")],
```

- [ ] **Step 2: Update enemies.json hellforge smith meshName**

Change `"meshName": "butcher"` to `"meshName": "hellforge_smith"` in the Hellforge Smith entry. Also update halfExtents to match the new wider, slightly shorter mesh:

```json
    "meshName": "hellforge_smith",
```

And update halfExtents from `[0.6, 1.1, 0.6]` to `[0.65, 1.0, 0.5]`.

- [ ] **Step 3: Commit config changes**

```bash
git add tools/build_assets.py assets/config/enemies.json
git commit -m "feat: wire hellforge smith mesh into build pipeline and enemy config

Add hellforge_smith to build_assets.py mesh list.
Update enemies.json: meshName butcher -> hellforge_smith, adjusted halfExtents."
```

---

### Task 10: Regenerate assets and verify build

- [ ] **Step 1: Regenerate meshes and skins**

```bash
cd /home/aaron/game
python3 tools/build_assets.py
```

Expected: Both `pit_fiend.obj` and `hellforge_smith.obj` generated in `assets/meshes/`, both skins generated in `assets/textures/`.

- [ ] **Step 2: Verify mesh dimensions**

```bash
python3 -c "
from tools.gen_mesh import gen_pit_fiend, gen_hellforge_smith
pf = gen_pit_fiend()
print(f'Pit Fiend: {len(pf.verts)} verts')
hs = gen_hellforge_smith()
print(f'Hellforge Smith: {len(hs.verts)} verts')
"
```

Expected: Pit Fiend ~7000-8000 verts, Hellforge Smith ~5000-7000 verts.

- [ ] **Step 3: Verify skin dimensions**

```bash
python3 -c "
from tools.gen_skin import skin_pit_fiend, skin_hellforge_smith
w, h, p = skin_pit_fiend()
assert w == 15 and h == 24, f'pit_fiend: got {w}x{h}'
print(f'pit_fiend skin: {w}x{h} OK')
w, h, p = skin_hellforge_smith()
assert w == 13 and h == 18, f'hellforge_smith: got {w}x{h}'
print(f'hellforge_smith skin: {w}x{h} OK')
"
```

- [ ] **Step 4: Build the project**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
```

Expected: Clean compile, no errors.

- [ ] **Step 5: Verify in-game**

Run `./build/dungeon_game`, navigate to Hellforge tier (floor 16-20). Check:
- Pit Fiend has visible 3D wings that flap, distinct from butcher
- Hellforge Smith is hunched with asymmetric hammer arm that swings
- Both have correct skin colors (burgundy wings, glowing forge arm)
- No rendering artifacts or misaligned limbs

- [ ] **Step 6: Final commit**

```bash
git add tools/gen_mesh.py
git commit -m "feat: redesign pit fiend mesh with 3D wings, add hellforge smith mesh

Pit Fiend: 24-voxel tall winged demon with real 3D bat wings (bone struts +
membrane), barrel chest, curved horns, tail. 15x24 grid, ~550 voxels.

Hellforge Smith: 18-voxel tall hunched blacksmith with massive shoulder hump,
oversized hammer arm, iron apron, forge bellows on back. 13x18 grid, ~480 voxels."
```
