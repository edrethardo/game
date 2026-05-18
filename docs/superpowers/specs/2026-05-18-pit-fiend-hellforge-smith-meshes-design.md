# Pit Fiend & Hellforge Smith — Original Mesh Design

## Context

Both enemies are tier-4 demons in the Hellforge biome. The Pit Fiend already has a mesh (`gen_pit_fiend`) but its wings are flat 1-voxel plates — not imposing enough for a shield_bearer + ranged_caster. The Hellforge Smith has no unique mesh at all — it reuses the butcher mesh, making it visually indistinguishable. Both need unique meshes, EnemyTypes, and limb animations to match their distinct gameplay roles.

## Pit Fiend — Classic Winged Demon (Redesign)

### Mesh Specification

- **Height**: 2.4m, 24 voxels tall, voxel size 0.1m
- **Grid**: 15w (X: -7..7) x 24h (Y: 0..23) x 9d (Z: -4..4)
- **Skin grid**: 15 x 24 pixels
- **Estimated**: ~550 voxels, ~3960 tris, ~7920 verts

### Body Layout (fill_box coordinates: x0, y0, z0, w, h, d)

```
Curved horns (gy 22-23):
  fill_box(-3, 22, 0, 1, 2, 1)       left horn base
  filled.add((-4, 23, 1))             left horn tip (swept back)
  fill_box(2, 22, 0, 1, 2, 1)        right horn base
  filled.add((3, 23, 1))              right horn tip

Head (gy 18-21): 5x4x4
  fill_box(-2, 18, -2, 5, 4, 4)
  discard(-2, 20, -2) and (2, 20, -2) for eye sockets

Neck (gy 16-17): 3x2x3
  fill_box(-1, 16, -1, 3, 2, 3)

Torso (gy 10-15): 7x6x4
  fill_box(-3, 10, -2, 7, 6, 4)
  Barrel chest protrusion: fill_box(-2, 12, -3, 5, 3, 1)

Wing roots (gy 13-15): anchor geometry on upper back
  fill_box(-4, 13, 2, 1, 3, 2)       left
  fill_box(3, 13, 2, 1, 3, 2)        right

Wings — MAIN FEATURE (gy 11-16, X -7..-4 / 4..7):
  Left wing:
    fill_box(-7, 14, 1, 3, 2, 2)     upper strut
    fill_box(-7, 12, 2, 3, 2, 2)     lower strut
    fill_box(-6, 11, 3, 2, 1, 2)     wing tip
    fill_box(-6, 13, 1, 2, 1, 1)     upper membrane
    fill_box(-5, 12, 1, 1, 2, 1)     inner membrane
    fill_box(-7, 13, 3, 1, 1, 1)     outer membrane
  Right wing: mirrored

Arms (gy 7-14): 1x4x2 upper + 1x4x2 lower each side
  Fists: 2x2x2 blocks at gy 6

Tail (gy 5-9): 5 single voxels trailing back (z 2..4)

Legs (gy 1-9): 3x5x3 thighs, 2x3x2 calves
Cloven hooves (gy 0): 3x1x4 blocks
```

### Skin Palette (15x24 px)

| Zone | Color | Description |
|------|-------|-------------|
| Body (default) | `(30, 25, 35)` | Dark obsidian |
| Magma veins | `(220, 100, 20)` | Orange-red cracks on torso at alternating gy |
| Wing membrane | `(80, 20, 25)` | Deep burgundy |
| Wing bone struts | `(50, 15, 18)` | Darker burgundy |
| Eyes | `(240, 140, 20)` | Burning orange |
| Horns | `(25, 20, 28)` | Dark purple-black |
| Hooves | `(40, 20, 10)` | Ember-glow dark |

### Limb Rig: `PIT_FIEND` EnemyType, 4 limbs

| Limb | Pivot | Size | Animation |
|------|-------|------|-----------|
| Left wing | `(0.30, 1.20, 0.10)` | `(0.65, 0.05, 0.40)` | Slow majestic flap (3 Hz idle, 6 Hz moving) |
| Right wing | `(-0.30, 1.20, 0.10)` | `(0.65, 0.05, 0.40)` | Mirrored |
| Left leg | `(0.15, 0.30, 0.0)` | `(0.12, 0.25, 0.12)` | Standard walk cycle |
| Right leg | `(-0.15, 0.30, 0.0)` | `(0.12, 0.25, 0.12)` | Mirrored |

Wing flap is slower and wider than succubus (amplitude 1.0 rad vs 0.9) to convey mass.

---

## Hellforge Smith — Demonic Blacksmith (New)

### Mesh Specification

- **Height**: 2.0m, 18 voxels tall, voxel size 0.111m
- **Grid**: 13w (X: -6..6) x 18h (Y: 0..17) x 7d (Z: -3..3)
- **Skin grid**: 13 x 18 pixels
- **Estimated**: ~480 voxels, ~3200 tris, ~6400 verts

### Body Layout

```
Head (gy 15-17): 3x2x3, sunken between shoulders
  fill_box(-1, 15, -1, 3, 2, 3)
  Brow ridge: fill_box(-2, 16, -2, 5, 1, 1)
  Eye sockets: discard(-1, 16, -2) and (1, 16, -2)

Shoulder hump (gy 13-14): 11x2x3 — widest part, hunched look
  fill_box(-5, 13, -1, 11, 2, 3)

Torso (gy 8-12): 9x5x5
  fill_box(-4, 8, -2, 9, 5, 5)

Iron apron (front, gy 6-11): 7x6x1
  fill_box(-3, 6, -3, 7, 6, 1)

Forge bellows (back, gy 9-12):
  fill_box(-3, 9, 3, 2, 4, 1)    left bellows
  fill_box(1, 9, 3, 2, 4, 1)     right bellows
  fill_box(-1, 10, 3, 2, 2, 1)   connector

Left arm (normal): 1x4x2 upper + 1x4x2 lower, 2x2x2 fist
Right arm (HAMMER): 2x4x2 upper + 2x4x2 lower, 3x3x3 anvil-fist at gy 3-5

Pelvis (gy 5-7): 9x3x3
Legs (gy 2-4): 3x3x2 — short, emphasizes top-heavy hunch
Heavy boots (gy 0-1): 4x2x4
```

### Skin Palette (13x18 px)

| Zone | Color | Description |
|------|-------|-------------|
| Body (default) | `(40, 35, 30)` | Soot-black |
| Iron apron | `(120, 125, 130)` | Grey metal |
| Forge arm (right) | `(200, 120, 30)` | Glowing orange |
| Bellows (back) | `(140, 90, 50)` | Rusted copper |
| Eyes | `(200, 60, 20)` | Red embers |
| Shoulder hump | `(35, 30, 25)` | Darker soot |
| Boots | `(30, 25, 20)` | Darkest charcoal |

### Limb Rig: `HELLFORGE_SMITH` EnemyType, 3 limbs

| Limb | Pivot | Size | Animation |
|------|-------|------|-----------|
| Left leg | `(0.18, 0.25, 0.0)` | `(0.14, 0.22, 0.14)` | Slow heavy walk (0.6x skeleton speed) |
| Right leg | `(-0.18, 0.25, 0.0)` | `(0.14, 0.22, 0.14)` | Mirrored |
| Hammer arm | `(0.40, 0.65, 0.0)` | `(0.16, 0.30, 0.16)` | Slow pendulum idle, slam on attack |

---

## Files to Modify

| File | Change |
|------|--------|
| `src/game/entity.h` | Add `PIT_FIEND`, `HELLFORGE_SMITH` to EnemyType enum (before BOSS) |
| `src/game/enemy_loader.cpp` | Map `"pit_fiend"` and `"hellforge_smith"` to new types in `inferEnemyType()` |
| `src/game/limb_system.cpp` | Add LimbConfigs, mesh lookups, animation cases for both |
| `tools/gen_mesh.py` | Rewrite `gen_pit_fiend()`, add `gen_hellforge_smith()` |
| `tools/gen_skin.py` | Update `skin_pit_fiend()` (12x21 -> 15x24), `skin_hellforge_smith()` (12x21 -> 13x18) |
| `assets/config/enemies.json` | Change hellforge smith meshName `"butcher"` -> `"hellforge_smith"`, update halfExtents |
| `tools/build_assets.py` | Add `hellforge_smith` to mesh build list |

## Verification

1. `python3 -c "from tools.gen_mesh import gen_pit_fiend; mb = gen_pit_fiend(); print(f'{len(mb.verts)} verts')"` — should be ~7900
2. `python3 -c "from tools.gen_mesh import gen_hellforge_smith; mb = gen_hellforge_smith(); print(f'{len(mb.verts)} verts')"` — should be ~6400
3. `python3 -c "from tools.gen_skin import skin_pit_fiend; w,h,p = skin_pit_fiend(); assert w == 15 and h == 24"`
4. `python3 -c "from tools.gen_skin import skin_hellforge_smith; w,h,p = skin_hellforge_smith(); assert w == 13 and h == 18"`
5. `python3 tools/build_assets.py` — regenerate all assets
6. `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build` — compiles clean
7. In-game: pit fiend has visible 3D wings that flap; hellforge smith is hunched with visible hammer arm swinging
