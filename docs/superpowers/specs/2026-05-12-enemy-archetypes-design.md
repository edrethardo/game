# Enemy Archetypes: Summoner, Healer, Aura, Ambush

## Goal

Add 4 new enemy archetypes with staggered introduction across tiers, plus custom meshes and textures for each. Each archetype introduces a new mechanic that forces tactical adaptation.

## Archetypes

### Tier 1: Gargoyle (Ambush)
- **Mesh**: New `gen_gargoyle()` — stone humanoid with stubby wings, hunched posture
- **Skin**: Grey stone body, glowing amber eyes, mossy green patches
- **Behavior**: Starts in DORMANT state (stationary, looks like a statue). Wakes when player is within 4m + has LOS. Lunges with high burst damage, then fights normally.
- **Stats**: HP 50, Speed 4.5, Damage 18, Range 3.0, Cooldown 0.8s. No debuff.
- **Spawn**: Low frequency (~15% of room spawns replace one normal enemy)

### Tier 2: Necromancer (Summoner — resurrects dead)
- **Mesh**: New `gen_necromancer()` — tall hooded skeleton with staff, flowing robes
- **Skin**: Dark purple bones, green glowing eyes, tattered black robes
- **Behavior**: Ranged (11m range, staff projectiles). Every 8s, scans for nearest dead entity within 10m and resurrects it at 50% HP. Resurrections stop when necromancer dies. Max 2 resurrections active.
- **Stats**: HP 30, Speed 2.0, Damage 10, Range 11.0, Cooldown 1.5s. Poison on-hit (2s, 3 DPS).
- **Spawn**: 1 per room max, only in rooms with 3+ enemies

### Tier 3: Cavern Shaman (Healer)
- **Mesh**: New `gen_shaman()` — stocky humanoid with horned headdress, staff
- **Skin**: Green-brown body, bone-white headdress, blue glowing hands
- **Behavior**: Ranged (10m), low damage. Every 5s, heals the lowest-HP non-dead hostile ally within 8m for 15 HP (visual: green pulse). Prioritizes healing over attacking. Won't heal itself.
- **Stats**: HP 35, Speed 2.5, Damage 8, Range 10.0, Cooldown 1.2s. Slow on-hit (2s).
- **Spawn**: 1 per room max

### Tier 4: Infernal Herald (Aura)
- **Mesh**: New `gen_herald()` — tall skeleton with extended arms, glowing rib cage
- **Skin**: Orange-red glowing bones, fiery core visible through ribs, ember eyes
- **Behavior**: Melee. Passive burn aura: players within 3m take 4 DPS continuously (applied via scorch zone centered on entity, refreshed each tick). Burn on-hit too (2.5s, 6 DPS).
- **Stats**: HP 55, Speed 3.0, Damage 14, Range 3.5, Cooldown 0.9s.
- **Spawn**: 1 per room max

### Tier 5: All three archetypes appear as void variants
- **Void Necromancer**: Resurrects dead, freeze on-hit. HP 40, Range 12. Dark purple void skin.
- **Void Shaman**: Heals allies for 20 HP, freeze on-hit. HP 45, Range 11. Teal void skin.
- **Void Herald**: Freeze aura (3m, applies 0.5s freeze pulse every 2s). HP 65. Black void skin with blue core.

## Asset Pipeline

### New meshes (tools/gen_mesh.py)

All use the voxel humanoid system (`add_voxel_model`), 16 voxels tall, skeleton rig compatible (w=7, h=16 skin grid).

| Mesh | Key features | Boxes/voxels |
|---|---|---|
| `gen_gargoyle()` | Hunched humanoid, stubby wing plates on back, thick limbs | Wider shoulders, 2 wing-plate boxes behind torso |
| `gen_necromancer()` | Tall hooded figure, staff in hand, robed | Extended skull upward (hood), wider torso (robe), thin legs hidden |
| `gen_shaman()` | Stocky build, horned headdress, broad shoulders | 2 horn boxes on head, wider torso, shorter legs |
| `gen_herald()` | Tall thin skeleton, extended arms, open rib cage | Taller than humanoid (18 voxels), thin limbs, gap in chest |

Register all in `MESH_TYPES` dict and `kMeshes[]` array in engine.cpp.

### New skins (tools/gen_skin.py)

All use w=7, h=16 grid (skeleton rig). 9 new skins total:

| Skin | Grid | Colors |
|---|---|---|
| `skin_gargoyle()` | 7x16 | Grey stone (150,150,155), amber eyes (200,150,40), moss green patches |
| `skin_necromancer()` | 7x16 | Dark purple bone (80,40,120), green eyes (40,200,40), black robe (25,20,30) |
| `skin_cavern_shaman()` | 7x16 | Green-brown (90,100,60), bone-white head (220,210,200), blue hands (60,120,200) |
| `skin_infernal_herald()` | 7x16 | Orange-red (200,80,30), fiery core (255,180,40), ember eyes (255,120,20) |
| `skin_void_necromancer()` | 7x16 | Dark purple-void (60,30,90), ice-blue eyes (80,160,255) |
| `skin_void_shaman()` | 7x16 | Teal void (40,100,100), white headdress (180,180,200) |
| `skin_void_herald()` | 7x16 | Black void (25,25,35), blue core (60,120,255) |

Register all in `SKIN_TYPES` dict. Generate with `gen_skin.py --type <name>`.

### New materials (assets/materials.json)

7 new entries (IDs 115-121):
```
gargoyle_skin, necromancer_skin, cavern_shaman_skin,
infernal_herald_skin, void_necromancer_skin, void_shaman_skin, void_herald_skin
```

### build_assets.py

Add all new meshes to `build_meshes()` and all new skins to `build_skins()`.

## AI Implementation

### Resurrect mechanic (Necromancer)

In `enemy_ai.cpp`, add to the necromancer's ATTACK state update:

```
Every 8s (use tacticalTimer countdown):
1. Scan pool.entities[] for (flags & ENT_DEAD) && deathTimer > 0
   (entity is dead but slot not yet freed)
2. Find nearest dead entity within 10m of necromancer
3. If found and resurrect count < 2:
   - Clear ENT_DEAD flag, restore health to maxHealth * 0.5
   - Set aiState = IDLE, reset velocity
   - Extend deathTimer to prevent slot reuse
   - Increment necromancer's resurrect counter (store in tacticalTimer2 or similar)
4. Fire green nova visual at resurrect position
```

Track resurrections: use `entity.sprintTimer` (repurposed) as resurrect count for necromancers. On necromancer death, iterate entities and kill any it resurrected (or let them live — simpler).

### Heal mechanic (Shaman)

In `enemy_ai.cpp`, add to shaman's update (check by material name or a new flag):

```
Every 5s (tacticalTimer):
1. Find lowest-HP non-dead hostile entity within 8m
2. If found and target.health < target.maxHealth:
   - target.health += 15 (clamp to maxHealth)
   - Fire green nova visual at target position
3. Reset tacticalTimer = 5.0
```

### Aura mechanic (Herald)

In `engine.cpp` gameUpdate, after entity AI but before projectile update:

```
For each active herald entity (identify by materialId == herald material):
1. Apply burn DPS to all players within 3m:
   - player.burnTimer = max(player.burnTimer, 0.5f)
   - player.burnDps = 4.0f
2. Optionally spawn a visual scorch zone at herald position (refresh every 0.5s)
```

### Ambush mechanic (Gargoyle)

Reuse existing DORMANT state from mimic code. Set gargoyle entities to `aiState = AIState::DORMANT` on spawn. Trigger distance: 4m (vs mimic's 2.5m). On wake: set `aiState = CHASE`, play speech bubble "...", apply initial burst.

## Enemy Template Entries

### kTier1 addition:
```cpp
// Gargoyle — stone ambush, high burst, wakes when player is close
{50, 4.5f, 15, 3.0f, 0.8f, 18, {0.45f,0.9f,0.45f}, false, 4, EnemyType::SKELETON, "gargoyle_skin", 0, 0, 0},
```
meshIdx=4 (new gargoyle mesh, add to meshLookup)

### kTier2 addition:
```cpp
// Necromancer — ranged caster, resurrects dead enemies
{30, 2.0f, 20, 11.0f, 1.5f, 10, {0.4f,1.0f,0.4f}, false, 5, EnemyType::SKELETON, "necromancer_skin", 1, 2.0f, 3.0f},
```
meshIdx=5 (new necromancer mesh)

### kTier3 addition:
```cpp
// Cavern Shaman — healer, low damage, heals allies
{35, 2.5f, 18, 10.0f, 1.2f, 8, {0.45f,0.9f,0.45f}, false, 6, EnemyType::SKELETON, "cavern_shaman_skin", 2, 2.0f, 0},
```
meshIdx=6 (new shaman mesh)

### kTier4 addition:
```cpp
// Infernal Herald — burn aura, melee, area denial
{55, 3.0f, 20, 3.5f, 0.9f, 14, {0.4f,1.0f,0.4f}, false, 7, EnemyType::SKELETON, "infernal_herald_skin", 3, 2.5f, 6.0f},
```
meshIdx=7 (new herald mesh)

### kTier5 additions (3):
```cpp
// Void Necromancer — resurrects dead, freeze
{40, 2.2f, 22, 12.0f, 1.2f, 12, {0.4f,1.0f,0.4f}, false, 5, EnemyType::SKELETON, "void_necromancer_skin", 4, 1.5f, 0},
// Void Shaman — heals allies, freeze
{45, 2.5f, 20, 11.0f, 1.0f, 10, {0.45f,0.9f,0.45f}, false, 6, EnemyType::SKELETON, "void_shaman_skin", 4, 1.5f, 0},
// Void Herald — freeze aura, melee
{65, 2.8f, 22, 3.5f, 0.8f, 16, {0.4f,1.0f,0.4f}, false, 7, EnemyType::SKELETON, "void_herald_skin", 4, 2.0f, 0},
```

## Identifying Archetype in AI

Add a field to Entity to mark the archetype role:

```cpp
enum struct EnemyRole : u8 {
    NORMAL = 0,
    AMBUSH,      // gargoyle — starts dormant
    SUMMONER,    // necromancer — resurrects dead
    HEALER,      // shaman — heals allies
    AURA,        // herald — passive damage aura
};
u8 enemyRole = 0; // EnemyRole, stored as u8 to avoid header dependency
```

Set during spawn based on material name or template index. Check in enemy_ai.cpp to branch into special behaviors.

## Files to modify

| File | Change |
|---|---|
| `tools/gen_mesh.py` | Add gen_gargoyle, gen_necromancer, gen_shaman, gen_herald + MESH_TYPES |
| `tools/gen_skin.py` | Add 7 new skin functions + SKIN_TYPES entries |
| `tools/build_assets.py` | Add 4 meshes + 7 skins to build pipeline |
| `assets/materials.json` | Add 7 new material entries (IDs 115-121) |
| `src/game/entity.h` | Add `EnemyRole` enum + `enemyRole` field |
| `src/engine/engine.cpp` | Register meshes in kMeshes, expand meshLookup, add templates to kTier1-5, set role on spawn, add herald aura logic |
| `src/game/enemy_ai.cpp` | Add resurrect logic, heal logic, dormant gargoyle trigger |

## Verification

1. Generate assets: `python3 tools/build_assets.py --all`
2. Build: `cmake --build build`
3. Run game:
   - Floor 1-10: Check corners for dormant gargoyles, walk close to trigger ambush
   - Floor 11-20: Find necromancer, kill nearby enemies, watch necromancer resurrect them
   - Floor 21-30: Find shaman, damage nearby enemies, watch shaman heal them
   - Floor 31-40: Find herald, walk within 3m, verify burn aura damages player
   - Floor 41-50: All three void variants present + functioning
4. F4 spawn enemies to verify new types appear in the mix
