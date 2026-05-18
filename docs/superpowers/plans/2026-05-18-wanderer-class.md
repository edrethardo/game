# Wanderer Class Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a 9th player class "Wanderer" — an evasive counter-attacker with a first-person dodge roll on Shift, i-frame counter-attacks, and 4 prediction-based skills.

**Architecture:** Dodge roll replaces blocking for this class. New `DodgeState` on Player struct handles roll timer, cooldown, camera roll angle, and adrenaline counter stacks. Dodge-through detection hooks into existing `applyDamageToPlayer()` invuln check. Four new skills (Deflect, Exploit Weakness, Adrenaline Surge, Death's Dance) added to the existing skill system. Camera barrel roll is a new `roll` field on Camera applied in `applyToCamera()`.

**Tech Stack:** C++17, OpenGL 3.3, SDL2, existing engine systems (Collision, Combat, SkillSystem, HUD)

**Spec:** `docs/superpowers/specs/2026-05-18-wanderer-class-design.md`

---

## File Map

| File | Responsibility | Action |
|------|---------------|--------|
| `src/platform/input.h` | GameAction enum | Add `DODGE` action |
| `src/platform/input.cpp` | Key bindings | Bind Shift + R3 to DODGE |
| `src/game/item.h` | PlayerClass, SkillId enums, SkillDef | Add WANDERER, 4 skill IDs, new SkillDef fields |
| `src/game/player.h` | Player struct | Add `DodgeState` struct and fields for mark/deflect/death's dance |
| `src/game/player.cpp` | PlayerController | Dodge activation, roll movement, camera roll |
| `src/renderer/camera.h` | Camera struct | Add `roll` field |
| `src/game/combat.h` | Combat declarations | Add dodge-through callback, mark damage mult |
| `src/game/combat.cpp` | Damage application | Dodge-through detection, deflect parry, mark multiplier |
| `src/game/skill.cpp` | Skill activation | Deflect, Exploit Weakness, Death's Dance activation |
| `src/engine/engine_init.cpp` | Class table | Add Wanderer ClassDef entry |
| `src/engine/engine_menu.cpp` | Class selection | Apply Wanderer stats on select |
| `src/engine/engine_update.cpp` | Game loop | Tick dodge timers, block bypass, counter stacks, skill timers |
| `src/engine/engine_hud.cpp` | HUD rendering | Dodge cooldown indicator, adrenaline stacks, mark icon |
| `src/engine/engine_combat.cpp` | Weapon fire | Apply adrenaline attack speed |
| `src/net/net_player.h` | Net input flags | Add INPUT_EX_DODGE |
| `src/net/snapshot.h` | Snapshot packing | Add dodge/counter bits to SnapPlayer |
| `assets/config/skills.json` | Skill data | Add 4 Wanderer skill definitions |

---

### Task 1: Add Wanderer Enum Values and DodgeState Struct

Add the new class, skill IDs, input action, and core data structures that everything else depends on.

**Files:**
- Modify: `src/platform/input.h:8-18`
- Modify: `src/game/item.h:62-72` (PlayerClass), `76-162` (SkillId), `290-328` (SkillDef)
- Modify: `src/game/player.h:11-70`
- Modify: `src/renderer/camera.h:36-57`

- [ ] **Step 1: Add DODGE to GameAction enum**

In `src/platform/input.h`, add `DODGE` before `COUNT`:

```cpp
// line 15, before COUNT
    QUICKBAR_PREV, QUICKBAR_NEXT,
    MENU_UP, MENU_DOWN, MENU_CONFIRM, MENU_BACK,
    DODGE,
    COUNT
```

- [ ] **Step 2: Add WANDERER to PlayerClass enum**

In `src/game/item.h`, add `WANDERER` before `CLASS_COUNT`:

```cpp
    TINKERER,
    WANDERER,
    CLASS_COUNT
```

- [ ] **Step 3: Add 4 Wanderer skill IDs to SkillId enum**

In `src/game/item.h`, add after the Tinkerer skills block (after SWARM_QUEEN, before the weapon proc/ring passive section):

```cpp
    // --- Wanderer ---
    DEFLECT,
    EXPLOIT_WEAKNESS,
    ADRENALINE_SURGE,
    DEATHS_DANCE,
```

- [ ] **Step 4: Add new SkillDef fields for Wanderer skills**

In `src/game/item.h`, add after the Holy Nova fields (after line ~327):

```cpp
    // --- Wanderer: Deflect ---
    f32 activeWindow      = 0.0f;  // parry active duration (seconds)
    f32 stunDuration      = 0.0f;  // stun applied on melee parry

    // --- Wanderer: Exploit Weakness ---
    f32 markDuration      = 0.0f;  // how long mark lasts
    f32 damageMultiplier  = 1.0f;  // damage mult on marked target

    // --- Wanderer: Death's Dance ---
    f32 slashRadius       = 0.0f;  // AoE slash radius on dodge-through
    f32 slashDamageMult   = 0.0f;  // slash damage as fraction of weapon damage
```

- [ ] **Step 5: Add DodgeState struct and Wanderer fields to Player**

In `src/game/player.h`, add the DodgeState struct before the Player struct, and add new fields inside Player:

Before the Player struct:
```cpp
// Wanderer dodge roll state
struct DodgeState {
    f32  rollTimer      = 0.0f;    // countdown during roll (0.5 -> 0)
    f32  cooldownTimer  = 0.0f;    // countdown after roll ends (1.0 -> 0)
    Vec3 rollDirection  = {0,0,0}; // normalized XZ direction of roll
    f32  rollAngle      = 0.0f;    // current camera roll in radians (0 to 2*PI)
    bool rolling        = false;   // true during the 0.5s roll
    s8   rollSign       = 1;       // +1 clockwise, -1 counter-clockwise
    u8   counterStacks  = 0;       // adrenaline surge stacks (0-5)
    f32  counterTimers[5] = {};    // per-stack decay timers (4s each)
};
```

Inside the Player struct, after the block fields (after line ~60):
```cpp
    // --- Wanderer ---
    DodgeState dodgeState;
    f32  deflectTimer     = 0.0f;  // active parry window countdown
    u16  markedEntityIdx  = 0xFFFF;// Exploit Weakness target
    u16  markedEntityGen  = 0;
    f32  markTimer        = 0.0f;
    f32  deathsDanceTimer = 0.0f;  // ultimate duration countdown
    bool adrenalineUnlocked = false; // true once skill 3 is unlocked
```

- [ ] **Step 6: Add roll field to Camera struct**

In `src/renderer/camera.h`, add after the `pitch` field:

```cpp
    f32  roll      = 0.0f;   // radians, for dodge roll barrel effect
```

- [ ] **Step 7: Build and verify compilation**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build 2>&1 | tail -20
```

Expected: Compiles with no errors. New enums/structs are defined but unused.

- [ ] **Step 8: Commit**

```bash
git add src/platform/input.h src/game/item.h src/game/player.h src/renderer/camera.h
git commit -m "feat(wanderer): add enum values, DodgeState struct, and Camera roll field"
```

---

### Task 2: Add DODGE Input Binding and Wanderer ClassDef

Wire up the Shift key and register the Wanderer in the class table.

**Files:**
- Modify: `src/platform/input.cpp:83-129`
- Modify: `src/engine/engine_init.cpp:68-116`

- [ ] **Step 1: Add DODGE key binding**

In `src/platform/input.cpp`, inside `setDefaults()`, add after the BLOCK binding (after the line with `SDL_SCANCODE_LCTRL`):

```cpp
    set(GameAction::DODGE, SDL_SCANCODE_LSHIFT, 0, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
```

- [ ] **Step 2: Add Wanderer ClassDef to kClassDefs table**

In `src/engine/engine_init.cpp`, add a new entry after the TINKERER entry in the `kClassDefs` array (before the closing `};`):

```cpp
    // WANDERER — evasive counter-attacker, dodge roll replaces block
    {
        "Wanderer",
        "Evasive counter-attacker. Dodge through enemy attacks for counter-hits and stacking attack speed.",
        90.0f,   // baseHealth — low, dodge is the defense
        6.5f,    // baseMoveSpeed — fast, tied with Ranger
        110.0f,  // baseEnergy
        "Iron Sword",
        {SkillId::DEFLECT, SkillId::EXPLOIT_WEAKNESS, SkillId::ADRENALINE_SURGE, SkillId::DEATHS_DANCE},
        {1, 10, 20, 30},   // skill unlock floors
        {5, 20, 30, 40},   // skill upgrade floors
        WeaponType::MELEE   // +20% melee damage
    },
```

- [ ] **Step 3: Build and verify**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build 2>&1 | tail -20
```

- [ ] **Step 4: Commit**

```bash
git add src/platform/input.cpp src/engine/engine_init.cpp
git commit -m "feat(wanderer): add Shift/R3 dodge binding and Wanderer ClassDef"
```

---

### Task 3: Implement Dodge Roll Movement and Camera Barrel Roll

The core dodge mechanic: Shift triggers a 0.5s roll with 0.3s i-frames, 4m distance, and 360° camera barrel roll.

**Files:**
- Modify: `src/game/player.cpp:73-122` (PlayerController::update), `232-242` (applyToCamera)
- Modify: `src/engine/engine_update.cpp:946-958` (block code), `364-368` (timer ticks)

- [ ] **Step 1: Add dodge activation and roll physics to PlayerController::update()**

In `src/game/player.cpp`, add dodge logic inside `PlayerController::update()` AFTER the `applyMovement()` call (after line ~120, before `player.forward = s_lastForward;`):

```cpp
    // --- Wanderer dodge roll ---
    DodgeState& ds = player.dodgeState;
    if (ds.rolling) {
        // Override horizontal velocity with roll direction during roll
        constexpr f32 ROLL_SPEED = 8.0f; // 4m in 0.5s
        player.velocity.x = ds.rollDirection.x * ROLL_SPEED;
        player.velocity.z = ds.rollDirection.z * ROLL_SPEED;

        ds.rollTimer -= dt;

        // Camera barrel roll: smooth 360° using cubic ease (3t²-2t³)
        constexpr f32 ROLL_DURATION = 0.5f;
        f32 t = 1.0f - (ds.rollTimer / ROLL_DURATION); // 0→1 over duration
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        f32 smooth = t * t * (3.0f - 2.0f * t); // cubic ease in-out
        ds.rollAngle = smooth * 2.0f * 3.14159265f * ds.rollSign;

        if (ds.rollTimer <= 0.0f) {
            // Roll finished — enter cooldown
            ds.rolling = false;
            ds.rollTimer = 0.0f;
            ds.rollAngle = 0.0f;
            ds.cooldownTimer = 1.0f;
        }
    } else if (ds.cooldownTimer > 0.0f) {
        ds.cooldownTimer -= dt;
        if (ds.cooldownTimer < 0.0f) ds.cooldownTimer = 0.0f;
    }
```

- [ ] **Step 2: Add dodge activation trigger**

In `src/game/player.cpp`, in `PlayerController::update()`, add BEFORE the `applyMovement()` call (after the WASD booleans are read, around line ~110):

```cpp
    // --- Wanderer dodge roll activation ---
    if (Input::isActionPressed(GameAction::DODGE) && !player.dodgeState.rolling
        && player.dodgeState.cooldownTimer <= 0.0f) {
        DodgeState& ds = player.dodgeState;
        ds.rolling = true;
        ds.rollTimer = 0.5f;
        ds.rollAngle = 0.0f;

        // Direction from WASD, or forward if none held
        f32 cosY = cosf(player.yaw);
        f32 sinY = sinf(player.yaw);
        Vec3 flatFwd   = normalize(Vec3{-sinY, 0.0f, -cosY});
        Vec3 flatRight = normalize(cross(flatFwd, {0.0f, 1.0f, 0.0f}));

        Vec3 dir = {0, 0, 0};
        if (w) dir += flatFwd;
        if (s) dir -= flatFwd;
        if (d) dir += flatRight;
        if (a) dir -= flatRight;
        if (lengthSq(dir) < 0.001f) dir = flatFwd;
        ds.rollDirection = normalize(dir);

        // Camera roll direction: left=CCW, right=CW, forward/back=CW
        if (a && !d) ds.rollSign = -1;
        else ds.rollSign = 1;

        // I-frames: set invuln for 0.3s (first 60% of roll)
        player.invulnTimer = 0.3f;

        // Disable mouse look during roll (handled by skipping applyMovement look)
    }

    // During roll, zero out look deltas so camera doesn't move with mouse
    if (player.dodgeState.rolling) {
        mx = 0.0f;
        my = 0.0f;
    }
```

- [ ] **Step 3: Apply camera roll in applyToCamera()**

In `src/game/player.cpp`, modify `applyToCamera()` (line ~232) to apply barrel roll:

```cpp
void PlayerController::applyToCamera(const Player& player, Camera& cam) {
    cam.position = player.position + Vec3{0.0f, player.eyeHeight, 0.0f};
    cam.yaw      = player.yaw;
    cam.pitch    = player.pitch;
    cam.roll     = player.dodgeState.rollAngle; // barrel roll from dodge
    cam.forward  = normalize(Vec3{
        -sinf(player.yaw) * cosf(player.pitch),
         sinf(player.pitch),
        -cosf(player.yaw) * cosf(player.pitch)
    });
    cam.right = normalize(cross(cam.forward, {0.0f, 1.0f, 0.0f}));
}
```

- [ ] **Step 4: Apply camera roll in the view matrix**

Find where `cam.view` is built (search for `Camera::` or `lookAt` in the renderer). The view matrix must incorporate `cam.roll`. In the view matrix computation (likely in the render pass), after building the standard view matrix from position/yaw/pitch, apply a rotation around the forward axis:

```cpp
// After computing the standard view matrix:
if (cam.roll != 0.0f) {
    // Rotate view around the forward (look) axis by cam.roll radians
    Mat4 rollMat = Mat4::rotateZ(cam.roll);
    cam.view = rollMat * cam.view;
}
```

Note: Check `src/renderer/camera.h` or `src/engine/engine_render.cpp` for where the view matrix is built. The roll rotation should use the camera's forward axis, which in view space is -Z, so `rotateZ` is correct if applied to the view matrix.

- [ ] **Step 5: Skip blocking for Wanderer**

In `src/engine/engine_update.cpp`, modify the block code at lines 946-958 to skip for Wanderer:

```cpp
// --- Shield blocking (Ctrl) — disabled for Wanderer (uses dodge roll instead) ---
{
    if (m_playerClass != PlayerClass::WANDERER) {
        bool wantsBlock = Input::isActionDown(GameAction::BLOCK) && !m_inventoryOpen;
        if (wantsBlock && !m_localPlayer.blocking) {
            m_localPlayer.blocking = true;
            m_localPlayer.blockTimer = 0.0f;
        } else if (!wantsBlock) {
            m_localPlayer.blocking = false;
        }
        if (m_localPlayer.blocking) {
            m_localPlayer.blockTimer += dt;
        }
    }
}
```

- [ ] **Step 6: Build, run, and test dodge roll**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/dungeon_game
```

Test: Select Wanderer class. Press Shift → player moves 4m in WASD direction with 360° camera barrel roll over 0.5s. Press LCTRL → nothing happens (no block). Cooldown prevents immediate re-dodge.

- [ ] **Step 7: Commit**

```bash
git add src/game/player.cpp src/engine/engine_update.cpp
git commit -m "feat(wanderer): implement dodge roll movement, camera barrel roll, block bypass"
```

---

### Task 4: Dodge-Through Detection and Counter-Attack Riposte

When an enemy attack is blocked by i-frames during a roll, fire an automatic counter-hit on the attacker.

**Files:**
- Modify: `src/game/combat.h`
- Modify: `src/game/combat.cpp:111-156`
- Modify: `src/game/player.h` (add dodge-through callback type)

- [ ] **Step 1: Add dodge-through callback and detection to combat**

In `src/game/combat.h`, add a callback type and setter:

```cpp
    // Dodge-through callback: called when damage is blocked during a dodge roll
    // Parameters: attacker entity index, attacker position
    using DodgeThroughCallback = void(*)(u16 attackerIdx, Vec3 attackerPos);
    void setDodgeThroughCallback(DodgeThroughCallback cb);
```

In `src/game/combat.cpp`, add the callback storage (near other static callbacks):

```cpp
static Combat::DodgeThroughCallback s_dodgeThroughCallback = nullptr;
void Combat::setDodgeThroughCallback(DodgeThroughCallback cb) { s_dodgeThroughCallback = cb; }
```

- [ ] **Step 2: Modify applyDamageToPlayer() for dodge-through detection**

In `src/game/combat.cpp`, modify `applyDamageToPlayer()` (line ~113). The function needs an attacker index parameter. Add an overload or modify the signature:

```cpp
void Combat::applyDamageToPlayer(Player& player, f32 damage, const Vec3* attackerPos, u16 attackerIdx) {
    // Invulnerability blocks all damage
    if (player.invulnTimer > 0.0f) {
        // Dodge-through detection: if rolling and invuln, this is a successful dodge
        if (player.dodgeState.rolling && s_dodgeThroughCallback && attackerIdx != 0xFFFF) {
            s_dodgeThroughCallback(attackerIdx, attackerPos ? *attackerPos : Vec3{0,0,0});
        }
        return;
    }
    // ... rest of existing damage code unchanged ...
}
```

Add a default parameter `u16 attackerIdx = 0xFFFF` to the declaration in `combat.h` so existing callers don't break.

- [ ] **Step 3: Wire the dodge-through callback in engine_init.cpp**

In `src/engine/engine_init.cpp`, inside `Engine::init()` where other combat callbacks are set, add:

```cpp
Combat::setDodgeThroughCallback([](u16 attackerIdx, Vec3 attackerPos) {
    // Called from applyDamageToPlayer — need access to engine state
    // Use the file-scope s_engine pointer (same pattern as death callback)
    if (!s_engine) return;
    Player& player = s_engine->m_localPlayer;

    // 1. Instant riposte: 50% weapon damage counter-hit
    WeaponDef effectiveWeapon = Inventory::getEffectiveWeapon(
        s_engine->m_inventories[s_engine->m_localPlayerIndex],
        s_engine->m_weaponDefs, s_engine->m_weaponDefCount);
    f32 riposteDmg = effectiveWeapon.damage * 0.5f;

    EntityHandle h = {attackerIdx, 0}; // generation not checked for immediate hit
    // Find valid handle
    if (attackerIdx < MAX_ENTITIES && (s_engine->m_entities.entities[attackerIdx].flags & ENT_ACTIVE)) {
        h.generation = s_engine->m_entities.entities[attackerIdx].generation;
        Combat::applyDamage(s_engine->m_entities, h, riposteDmg, &player.position);
        Combat::spawnDamageNumber(attackerPos, riposteDmg);
    }

    // 2. Adrenaline stacks (only if skill 3 unlocked)
    if (player.adrenalineUnlocked) {
        DodgeState& ds = player.dodgeState;
        if (ds.counterStacks < 5) {
            ds.counterTimers[ds.counterStacks] = 4.0f;
            ds.counterStacks++;
        } else {
            // Refresh oldest stack timer
            f32 minT = ds.counterTimers[0];
            u8 minIdx = 0;
            for (u8 i = 1; i < 5; i++) {
                if (ds.counterTimers[i] < minT) { minT = ds.counterTimers[i]; minIdx = i; }
            }
            ds.counterTimers[minIdx] = 4.0f;
        }
    }

    // 3. Death's Dance AoE slash
    if (player.deathsDanceTimer > 0.0f) {
        WeaponDef ew = Inventory::getEffectiveWeapon(
            s_engine->m_inventories[s_engine->m_localPlayerIndex],
            s_engine->m_weaponDefs, s_engine->m_weaponDefCount);
        f32 slashDmg = ew.damage;
        Vec3 eyePos = player.position + Vec3{0, player.eyeHeight, 0};
        // Hit all entities within 3m sphere
        EntityHandle hits[MAX_ENTITIES];
        f32 dists[MAX_ENTITIES];
        // Use a wide cone (360°) as a sphere query approximation
        // cosine of 180° = -1.0 means everything in range
        u32 hitCount = CombatQuery::queryConeSorted(
            s_engine->m_entities, eyePos, player.forward, -1.0f, 3.0f,
            hits, dists, MAX_ENTITIES);
        f32 totalDmg = 0.0f;
        for (u32 i = 0; i < hitCount; i++) {
            Combat::applyDamage(s_engine->m_entities, hits[i], slashDmg, &eyePos);
            totalDmg += slashDmg;
        }
        // Upgrade: heal 10% of damage dealt (check floor >= 40)
        // Floor check done via s_engine->m_currentFloor
    }

    // Screen shake for dodge-through feedback
    if (s_engine->m_camera.shake.intensity < 0.03f) {
        s_engine->m_camera.shake.trigger(0.03f, 0.2f);
    }
});
```

- [ ] **Step 4: Pass attacker entity index through damage calls**

Search for all calls to `Combat::applyDamageToPlayer()` and add the attacker index where available. Key call sites:

- **Entity melee attacks** in `src/game/enemy_ai.cpp`: The attacking entity's index is available in the AI loop. Pass `e.poolIndex` or the loop index.
- **Projectile hits** in `src/game/projectile.cpp` or wherever projectile-player collision runs: Pass the `proj.ownerEntityIdx` if available.
- **Boss attacks**: Same as entity melee.

For calls where no attacker is known (environmental damage, DoT), pass `0xFFFF` (no dodge-through).

- [ ] **Step 5: Build, run, and test dodge-through**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/dungeon_game
```

Test: As Wanderer, dodge-roll through an enemy melee swing. Verify: damage number appears on the attacker (50% weapon damage), no damage taken during i-frames.

- [ ] **Step 6: Commit**

```bash
git add src/game/combat.h src/game/combat.cpp src/engine/engine_init.cpp src/game/enemy_ai.cpp
git commit -m "feat(wanderer): dodge-through detection with riposte counter-hit and adrenaline stacks"
```

---

### Task 5: Tick Adrenaline Stacks and Apply Attack Speed Bonus

Decay adrenaline stacks over time. Apply attack speed bonus to weapon cooldown.

**Files:**
- Modify: `src/engine/engine_update.cpp:364-386` (timer ticking area)
- Modify: `src/engine/engine_combat.cpp:65-70` (weapon cooldown)

- [ ] **Step 1: Tick adrenaline stack decay timers**

In `src/engine/engine_update.cpp`, after the invulnTimer tick block (after line ~368), add:

```cpp
    // --- Wanderer: tick adrenaline counter stack decay ---
    {
        DodgeState& ds = m_localPlayer.dodgeState;
        for (u8 i = 0; i < ds.counterStacks; ) {
            ds.counterTimers[i] -= dt;
            if (ds.counterTimers[i] <= 0.0f) {
                // Remove this stack, shift remaining down
                for (u8 j = i; j + 1 < ds.counterStacks; j++) {
                    ds.counterTimers[j] = ds.counterTimers[j + 1];
                }
                ds.counterStacks--;
            } else {
                i++;
            }
        }
    }
```

- [ ] **Step 2: Apply adrenaline attack speed bonus to weapon cooldown**

In `src/engine/engine_combat.cpp`, inside `handleWeaponFire()`, where the weapon cooldown is applied after firing (look for where `weaponState.cooldownTimer = weapon.cooldown` or similar), multiply by the adrenaline reduction:

```cpp
    // Apply adrenaline attack speed bonus (Wanderer)
    f32 atkSpeedMult = 1.0f;
    if (m_localPlayer.dodgeState.counterStacks > 0) {
        atkSpeedMult = 1.0f - (m_localPlayer.dodgeState.counterStacks * 0.10f);
        if (atkSpeedMult < 0.5f) atkSpeedMult = 0.5f; // cap at 50% reduction
    }
    ws.cooldownTimer = effectiveCooldown * atkSpeedMult;
```

- [ ] **Step 3: Apply adrenaline move speed bonus (skill 3 upgrade)**

In `src/game/player.cpp`, inside `PlayerController::update()`, where `effectiveSpeed` is computed (around line ~100), add after soul harvest bonus:

```cpp
    // Wanderer adrenaline move speed bonus (+5% per stack, requires upgrade)
    if (player.adrenalineUnlocked && player.dodgeState.counterStacks > 0) {
        effectiveSpeed *= (1.0f + player.dodgeState.counterStacks * 0.05f);
    }
```

Note: The `adrenalineUnlocked` flag alone doesn't indicate the upgrade. Add a separate `bool adrenalineUpgraded = false;` to Player for the move speed bonus, set it in `engine_update.cpp` when `m_currentFloor >= 30`. Or check the floor directly. Simplest: add the field to Player and set it alongside `adrenalineUnlocked`.

- [ ] **Step 4: Build, run, and test adrenaline**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/dungeon_game
```

Test: Dodge-through multiple enemy attacks. Verify attacks get faster with each stack. Verify stacks decay after 4s.

- [ ] **Step 5: Commit**

```bash
git add src/engine/engine_update.cpp src/engine/engine_combat.cpp src/game/player.cpp src/game/player.h
git commit -m "feat(wanderer): adrenaline stack decay and attack speed bonus"
```

---

### Task 6: Add Wanderer Skills JSON and Skill Loading

Define the 4 Wanderer skills in JSON and ensure they load correctly.

**Files:**
- Modify: `assets/config/skills.json`
- Modify: `src/game/item.cpp:309-387` (loadSkillDefs — parse new fields)

- [ ] **Step 1: Add Wanderer skills to skills.json**

In `assets/config/skills.json`, add 4 entries to the `"skills"` array:

```json
    {
        "id": "deflect",
        "name": "Deflect",
        "cooldown": 2.0,
        "energyCost": 20,
        "damage": 0,
        "activeWindow": 0.3,
        "stunDuration": 2.0
    },
    {
        "id": "exploit_weakness",
        "name": "Exploit Weakness",
        "cooldown": 8.0,
        "energyCost": 25,
        "damage": 0,
        "markDuration": 5.0,
        "damageMultiplier": 1.6
    },
    {
        "id": "adrenaline_surge",
        "name": "Adrenaline Surge",
        "cooldown": 0,
        "energyCost": 0,
        "damage": 0
    },
    {
        "id": "deaths_dance",
        "name": "Death's Dance",
        "cooldown": 30.0,
        "energyCost": 40,
        "damage": 0,
        "duration": 8.0,
        "slashRadius": 3.0,
        "slashDamageMult": 1.0
    }
```

- [ ] **Step 2: Parse new SkillDef fields in loadSkillDefs()**

In `src/game/item.cpp`, inside `loadSkillDefs()` (around lines 349-371 where skill-specific fields are parsed), add parsing for the new Wanderer fields:

```cpp
    // Wanderer: Deflect
    if (skill.count("activeWindow"))    def.activeWindow    = (*skill)["activeWindow"].get<f32>();
    if (skill.count("stunDuration"))    def.stunDuration    = (*skill)["stunDuration"].get<f32>();

    // Wanderer: Exploit Weakness
    if (skill.count("markDuration"))    def.markDuration    = (*skill)["markDuration"].get<f32>();
    if (skill.count("damageMultiplier"))def.damageMultiplier= (*skill)["damageMultiplier"].get<f32>();

    // Wanderer: Death's Dance
    if (skill.count("slashRadius"))     def.slashRadius     = (*skill)["slashRadius"].get<f32>();
    if (skill.count("slashDamageMult")) def.slashDamageMult = (*skill)["slashDamageMult"].get<f32>();
```

- [ ] **Step 3: Parse "deflect", "exploit_weakness", "adrenaline_surge", "deaths_dance" skill ID strings**

In `src/game/item.cpp`, find the skill ID string-to-enum parsing (search for where `"phase_dash"` is parsed to `SkillId::PHASE_DASH`). Add:

```cpp
    else if (idStr == "deflect")           def.id = SkillId::DEFLECT;
    else if (idStr == "exploit_weakness")  def.id = SkillId::EXPLOIT_WEAKNESS;
    else if (idStr == "adrenaline_surge")  def.id = SkillId::ADRENALINE_SURGE;
    else if (idStr == "deaths_dance")      def.id = SkillId::DEATHS_DANCE;
```

- [ ] **Step 4: Build and verify skills load**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/dungeon_game
```

Test: Game starts without JSON parse errors. Wanderer class appears in class selection.

- [ ] **Step 5: Commit**

```bash
git add assets/config/skills.json src/game/item.cpp
git commit -m "feat(wanderer): add 4 skill definitions to skills.json and parse new fields"
```

---

### Task 7: Implement Deflect Skill (Timed Parry)

0.3s active window that reflects projectiles and stuns melee attackers.

**Files:**
- Modify: `src/game/skill.cpp:1743-2050` (tryActivate switch)
- Modify: `src/game/combat.cpp:111-156` (applyDamageToPlayer — deflect check)
- Modify: `src/engine/engine_update.cpp` (tick deflectTimer)

- [ ] **Step 1: Add Deflect activation in tryActivate()**

In `src/game/skill.cpp`, inside the skill activation switch statement (around line 1889), add a case for DEFLECT:

```cpp
    case SkillId::DEFLECT: {
        // Start parry window — actual effect is in applyDamageToPlayer
        player.deflectTimer = def->activeWindow; // 0.3s
        fired = true;
        break;
    }
```

- [ ] **Step 2: Add deflect logic in applyDamageToPlayer()**

In `src/game/combat.cpp`, inside `applyDamageToPlayer()`, add BEFORE the blocking check (after line ~120, before `if (player.blocking)`):

```cpp
    // Wanderer Deflect: timed parry window
    if (player.deflectTimer > 0.0f) {
        // Melee parry: stun the attacker
        if (attackerIdx != 0xFFFF && attackerIdx < MAX_ENTITIES) {
            Entity& attacker = s_entityPool->entities[attackerIdx];
            if (attacker.flags & ENT_ACTIVE) {
                // Stun: force IDLE state and freeze movement
                attacker.aiState = AIState::IDLE;
                attacker.stunTimer = 2.0f; // base stun duration
                attacker.velocity = {0, 0, 0};
            }
        }
        // Negate damage completely (like perfect block)
        if (s_perfectBlockCallback) s_perfectBlockCallback(player);
        return;
    }
```

Note: Entity needs a `stunTimer` field. Check if one exists; if not, it likely exists as part of the on-hit effect system. Search for `stunTimer` or `freezeTimer` on Entity. If there's no stun timer, use `freezeTimer` or add one. The AI loop in `enemy_ai.cpp` should already skip entities with `stunTimer > 0` (check existing stun mechanics from War Cry or Thunderclap skills).

- [ ] **Step 3: Add projectile reflection in the projectile-player collision**

Find where projectiles hit the player (search for `applyDamageToPlayer` in projectile update code). Before applying damage, check `player.deflectTimer > 0.0f`:

```cpp
    if (player.deflectTimer > 0.0f) {
        // Reflect projectile back at sender
        proj.velocity = -proj.velocity; // reverse direction
        proj.fromPlayer = true;          // now counts as player projectile
        // Don't destroy the projectile or apply damage
        continue; // skip damage application
    }
```

- [ ] **Step 4: Tick deflectTimer in engine_update.cpp**

In `src/engine/engine_update.cpp`, in the timer ticking area (after the adrenaline tick added in Task 5):

```cpp
    // --- Wanderer: tick deflect parry window ---
    if (m_localPlayer.deflectTimer > 0.0f) {
        m_localPlayer.deflectTimer -= dt;
        if (m_localPlayer.deflectTimer < 0.0f) m_localPlayer.deflectTimer = 0.0f;
    }
```

- [ ] **Step 5: Build, run, and test Deflect**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/dungeon_game
```

Test: As Wanderer, press skill 1 just as a skeleton swings. Verify: skeleton is stunned for 2s, no damage taken. Test with a ranged enemy: projectile reverses and hits them.

- [ ] **Step 6: Commit**

```bash
git add src/game/skill.cpp src/game/combat.cpp src/engine/engine_update.cpp
git commit -m "feat(wanderer): implement Deflect skill — timed parry with projectile reflect"
```

---

### Task 8: Implement Exploit Weakness Skill (Mark Target)

Raycast to mark an enemy for +60% damage, refreshes on dodge-through.

**Files:**
- Modify: `src/game/skill.cpp` (tryActivate — EXPLOIT_WEAKNESS case)
- Modify: `src/game/combat.cpp` (applyDamage — mark multiplier)
- Modify: `src/engine/engine_update.cpp` (tick markTimer)

- [ ] **Step 1: Add Exploit Weakness activation**

In `src/game/skill.cpp`, inside the activation switch:

```cpp
    case SkillId::EXPLOIT_WEAKNESS: {
        // Raycast from eye position to find target entity
        Vec3 eyeP = player.position + Vec3{0, player.eyeHeight, 0};
        Vec3 fwd = player.forward;
        // Find closest entity in a narrow cone (5° half-angle) within 30m
        EntityHandle hits[1];
        f32 dists[1];
        u32 hitCount = CombatQuery::queryConeSorted(
            entities, eyeP, fwd, cosf(radians(5.0f)), 30.0f,
            hits, dists, 1);
        if (hitCount > 0) {
            Entity* target = handleGet(entities, hits[0]);
            if (target) {
                player.markedEntityIdx = hits[0].index;
                player.markedEntityGen = hits[0].generation;
                player.markTimer = def->markDuration; // 5.0s
                fired = true;
            }
        }
        break;
    }
```

- [ ] **Step 2: Apply mark damage multiplier in Combat::applyDamage()**

In `src/game/combat.cpp`, inside `applyDamage()` for entities (around line ~37-109), before applying damage, check if the target is marked:

```cpp
    // Wanderer Exploit Weakness: +60% damage to marked target
    if (s_engine && idx == s_engine->m_localPlayer.markedEntityIdx) {
        // Verify generation matches
        Entity& e = pool.entities[idx];
        if (e.generation == s_engine->m_localPlayer.markedEntityGen
            && s_engine->m_localPlayer.markTimer > 0.0f) {
            damage *= 1.6f; // +60%
        }
    }
```

Note: `applyDamage` takes an `EntityHandle`. Extract the index from `handle.index` for the check. The access to `s_engine` follows the existing pattern used by the death callback.

- [ ] **Step 3: Refresh mark on dodge-through**

In the dodge-through callback (added in Task 4, in `engine_init.cpp`), after the riposte damage, add:

```cpp
    // Refresh Exploit Weakness mark if dodge-through the marked enemy
    if (player.markTimer > 0.0f && attackerIdx == player.markedEntityIdx) {
        player.markTimer = 5.0f; // refresh to full duration
    }
```

- [ ] **Step 4: Tick markTimer and clear stale marks**

In `src/engine/engine_update.cpp`, in the timer area:

```cpp
    // --- Wanderer: tick Exploit Weakness mark ---
    if (m_localPlayer.markTimer > 0.0f) {
        m_localPlayer.markTimer -= dt;
        if (m_localPlayer.markTimer <= 0.0f) {
            m_localPlayer.markTimer = 0.0f;
            m_localPlayer.markedEntityIdx = 0xFFFF;
        }
    }
```

- [ ] **Step 5: Mark spread on kill (upgrade, floor >= 20)**

In the entity death callback (in `engine_init.cpp`, where loot drops happen), add:

```cpp
    // Wanderer Exploit Weakness upgrade: mark spreads on kill
    if (s_engine->m_playerClass == PlayerClass::WANDERER
        && s_engine->m_currentFloor >= 20
        && deadEntityIdx == s_engine->m_localPlayer.markedEntityIdx) {
        // Find nearest enemy within 5m
        Vec3 deadPos = s_engine->m_entities.entities[deadEntityIdx].position;
        f32 bestDist = 5.0f * 5.0f;
        u16 bestIdx = 0xFFFF;
        for (u32 i = 0; i < MAX_ENTITIES; i++) {
            Entity& e = s_engine->m_entities.entities[i];
            if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD) || i == deadEntityIdx) continue;
            f32 d2 = lengthSq(e.position - deadPos);
            if (d2 < bestDist) { bestDist = d2; bestIdx = (u16)i; }
        }
        if (bestIdx != 0xFFFF) {
            s_engine->m_localPlayer.markedEntityIdx = bestIdx;
            s_engine->m_localPlayer.markedEntityGen = s_engine->m_entities.entities[bestIdx].generation;
            s_engine->m_localPlayer.markTimer = 5.0f;
        }
    }
```

- [ ] **Step 6: Build, run, and test Exploit Weakness**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/dungeon_game
```

Test: Mark an enemy with skill 2. Attack it — damage numbers should be ~60% higher. Dodge through marked enemy's attack — mark timer refreshes. Kill marked enemy on floor 20+ — mark jumps to nearest.

- [ ] **Step 7: Commit**

```bash
git add src/game/skill.cpp src/game/combat.cpp src/engine/engine_update.cpp src/engine/engine_init.cpp
git commit -m "feat(wanderer): implement Exploit Weakness — mark target for +60% damage with dodge refresh"
```

---

### Task 9: Implement Death's Dance Skill (Ultimate)

8s buff: every dodge-through triggers a 3m AoE melee slash.

**Files:**
- Modify: `src/game/skill.cpp` (tryActivate — DEATHS_DANCE case)
- Modify: `src/engine/engine_update.cpp` (tick deathsDanceTimer)

- [ ] **Step 1: Add Death's Dance activation**

In `src/game/skill.cpp`, inside the activation switch:

```cpp
    case SkillId::DEATHS_DANCE: {
        player.deathsDanceTimer = def->duration; // 8.0s
        fired = true;
        break;
    }
```

- [ ] **Step 2: Tick Death's Dance timer**

In `src/engine/engine_update.cpp`, in the timer area:

```cpp
    // --- Wanderer: tick Death's Dance duration ---
    if (m_localPlayer.deathsDanceTimer > 0.0f) {
        m_localPlayer.deathsDanceTimer -= dt;
        if (m_localPlayer.deathsDanceTimer < 0.0f) m_localPlayer.deathsDanceTimer = 0.0f;
    }
```

Note: The actual AoE slash on dodge-through is already handled in the dodge-through callback (Task 4, Step 3) which checks `player.deathsDanceTimer > 0.0f`.

- [ ] **Step 3: Adrenaline Surge passive — set unlock flag**

In `src/engine/engine_update.cpp` or `engine_menu.cpp`, wherever skill unlock floors are checked, set `adrenalineUnlocked`:

```cpp
    // Set Wanderer adrenaline unlock based on current floor and class
    if (m_playerClass == PlayerClass::WANDERER) {
        m_localPlayer.adrenalineUnlocked = (m_currentFloor >= 20);
    }
```

Place this in `startGame()` or at the start of `gameUpdate()` (only needs to run once per floor).

- [ ] **Step 4: Build, run, and test Death's Dance**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/dungeon_game
```

Test: On floor 30+, activate skill 4 as Wanderer. Dodge through enemy attack. Verify: AoE slash damages all enemies within 3m (check damage numbers on multiple enemies).

- [ ] **Step 5: Commit**

```bash
git add src/game/skill.cpp src/engine/engine_update.cpp
git commit -m "feat(wanderer): implement Death's Dance ultimate and Adrenaline Surge unlock"
```

---

### Task 10: HUD Indicators for Wanderer Mechanics

Show dodge cooldown, adrenaline stacks, Exploit Weakness mark, and Death's Dance timer.

**Files:**
- Modify: `src/engine/engine_hud.cpp`

- [ ] **Step 1: Add dodge cooldown indicator near crosshair**

In `src/engine/engine_hud.cpp`, in the non-inventory HUD section (after crosshair drawing, around line ~166), add:

```cpp
    // --- Wanderer: dodge roll cooldown indicator ---
    if (m_playerClass == PlayerClass::WANDERER) {
        const DodgeState& ds = m_localPlayer.dodgeState;
        if (ds.rolling || ds.cooldownTimer > 0.0f) {
            // Small arc below crosshair showing cooldown
            f32 pct = ds.rolling ? 0.0f : (1.0f - ds.cooldownTimer / 1.0f);
            u32 color = ds.rolling ? 0xFF00CCFF : 0xFF888888; // cyan while rolling, grey on cooldown
            f32 cx = screenW * 0.5f;
            f32 cy = screenH * 0.5f + 20.0f; // just below crosshair
            HUD::drawCooldownArc(cx, cy, 8.0f, pct, color);
        }
    }
```

Note: `HUD::drawCooldownArc` may not exist. Check if there's a radial cooldown draw function used by the skill bar. If not, draw a simple horizontal bar instead:

```cpp
    // Simple bar version if no arc function:
    f32 barW = 30.0f, barH = 3.0f;
    f32 bx = screenW * 0.5f - barW * 0.5f;
    f32 by = screenH * 0.5f + 18.0f;
    HUD::drawRect(bx, by, barW, barH, 0x44FFFFFF); // background
    HUD::drawRect(bx, by, barW * pct, barH, color);  // fill
```

- [ ] **Step 2: Add adrenaline stack indicator**

Below the dodge indicator:

```cpp
    // --- Wanderer: adrenaline stacks ---
    if (m_localPlayer.adrenalineUnlocked && m_localPlayer.dodgeState.counterStacks > 0) {
        u8 stacks = m_localPlayer.dodgeState.counterStacks;
        f32 sx = screenW * 0.5f - (stacks * 6.0f);
        f32 sy = screenH * 0.5f + 26.0f;
        for (u8 i = 0; i < stacks; i++) {
            // Orange pips for each stack
            HUD::drawRect(sx + i * 12.0f, sy, 8.0f, 4.0f, 0xFFFF8800);
        }
    }
```

- [ ] **Step 3: Add Death's Dance timer bar**

```cpp
    // --- Wanderer: Death's Dance active indicator ---
    if (m_localPlayer.deathsDanceTimer > 0.0f) {
        f32 pct = m_localPlayer.deathsDanceTimer / 8.0f;
        f32 barW = 60.0f, barH = 4.0f;
        f32 bx = screenW * 0.5f - barW * 0.5f;
        f32 by = screenH * 0.5f + 34.0f;
        HUD::drawRect(bx, by, barW, barH, 0x44880088);       // dark purple bg
        HUD::drawRect(bx, by, barW * pct, barH, 0xFFCC44FF); // purple fill
    }
```

- [ ] **Step 4: Add mark indicator on marked entity**

In the entity rendering pass (in `src/engine/engine_render.cpp`), after drawing each entity, if it's the marked entity, draw a highlight. Search for where entity names or health bars are drawn above entities. Add:

```cpp
    // Wanderer: Exploit Weakness mark glow
    if (m_playerClass == PlayerClass::WANDERER
        && m_localPlayer.markTimer > 0.0f
        && entityIdx == m_localPlayer.markedEntityIdx) {
        // Draw a red-orange pulsing circle/diamond above entity
        // Use existing drawBillboard or debug draw
    }
```

The exact rendering depends on how entity overlays work. Use the same technique as target lock indicators or entity name labels.

- [ ] **Step 5: Build, run, and test HUD**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/dungeon_game
```

Test: Dodge roll shows cooldown bar under crosshair. Dodge-throughs show orange pips (stacks). Death's Dance shows purple timer bar. Marked enemy has visible indicator.

- [ ] **Step 6: Commit**

```bash
git add src/engine/engine_hud.cpp src/engine/engine_render.cpp
git commit -m "feat(wanderer): add HUD indicators for dodge cooldown, adrenaline, mark, Death's Dance"
```

---

### Task 11: Wanderer Class Selection and Network Sync

Wire Wanderer into class selection menu and add basic network support.

**Files:**
- Modify: `src/engine/engine_menu.cpp:194-205`
- Modify: `src/net/net_player.h:30-36`
- Modify: `src/net/snapshot.h:11-31`

- [ ] **Step 1: Ensure Wanderer applies correct stats on class selection**

In `src/engine/engine_menu.cpp`, the existing code at line ~205 sets `damageReduction` only for Warrior. No changes needed if the code uses `kClassDefs[m_menu.subSelection]` for stats — Wanderer's ClassDef already specifies 90 HP, 6.5 speed, 110 energy. But verify the menu can display 9 classes (the selection UI might be hardcoded to 8). Search for `CLASS_COUNT` in menu code and ensure the class list is dynamic.

- [ ] **Step 2: Add INPUT_EX_DODGE flag**

In `src/net/net_player.h`, add after the last ext flag (line ~36):

```cpp
    static constexpr u8 INPUT_EX_DODGE = 1 << 7; // bit 7 — Wanderer dodge roll
```

- [ ] **Step 3: Capture dodge input in captureLocalInput()**

In `src/game/player.cpp`, inside `captureLocalInput()`, add:

```cpp
    if (Input::isActionPressed(GameAction::DODGE)) input.extFlags |= INPUT_EX_DODGE;
```

- [ ] **Step 4: Handle dodge in server-side updateNetPlayerFromInput()**

In `src/game/player.cpp`, inside `updateNetPlayerFromInput()`, add dodge handling similar to the singleplayer path (check INPUT_EX_DODGE flag, set invulnTimer, start roll). The NetPlayer struct may need a `DodgeState` — add one if not present.

- [ ] **Step 5: Add dodge state bits to SnapPlayer**

In `src/net/snapshot.h`, add to `SnapPlayer` struct:

```cpp
    u8   dodgeFlags;  // 1: bit0=rolling, bits1-3=counterStacks (0-5)
```

Pack/unpack in the snapshot serialization code.

- [ ] **Step 6: Build and verify**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/dungeon_game
```

Test: Start a singleplayer game as Wanderer — all mechanics work. Network: host as Wanderer in a listen server, verify dodge works for host.

- [ ] **Step 7: Commit**

```bash
git add src/engine/engine_menu.cpp src/net/net_player.h src/net/snapshot.h src/game/player.cpp
git commit -m "feat(wanderer): class selection, network input flag, and snapshot sync"
```

---

### Task 12: Final Polish and Skill Upgrades

Wire skill upgrade effects and ensure all Wanderer mechanics degrade gracefully.

**Files:**
- Modify: `src/engine/engine_update.cpp` (upgrade checks)
- Modify: `src/game/combat.cpp` (Deflect upgrade: 2x reflect, 3s stun)
- Modify: `src/engine/engine_init.cpp` (Death's Dance upgrade: heal on AoE)

- [ ] **Step 1: Deflect upgrade (floor >= 5)**

In the deflect parry code in `combat.cpp`, check floor:

```cpp
    // Deflect upgrade: 3s stun (base 2s) + 2x reflected projectile damage
    f32 stunDur = 2.0f;
    if (s_engine && s_engine->m_currentFloor >= 5) stunDur = 3.0f;
    attacker.stunTimer = stunDur;
```

For projectile reflect, multiply reflected damage by 2 on upgrade:
```cpp
    if (s_engine && s_engine->m_currentFloor >= 5) {
        proj.damage *= 2.0f; // reflected projectile does 2x
    }
```

- [ ] **Step 2: Exploit Weakness upgrade (floor >= 20) — mark spread**

Already implemented in Task 8, Step 5. Verify the floor check uses `m_currentFloor >= 20`.

- [ ] **Step 3: Adrenaline Surge upgrade (floor >= 30) — move speed**

Already implemented in Task 5, Step 3. Set `adrenalineUpgraded = true` when floor >= 30:

```cpp
    if (m_playerClass == PlayerClass::WANDERER) {
        m_localPlayer.adrenalineUnlocked = (m_currentFloor >= 20);
        m_localPlayer.adrenalineUpgraded = (m_currentFloor >= 30);
    }
```

Guard the move speed bonus with `adrenalineUpgraded`.

- [ ] **Step 4: Death's Dance upgrade (floor >= 40) — heal on AoE**

In the dodge-through callback's Death's Dance section (Task 4), add after the AoE slash loop:

```cpp
    // Death's Dance upgrade: heal 10% of AoE damage dealt
    if (s_engine->m_currentFloor >= 40 && totalDmg > 0.0f) {
        player.health += totalDmg * 0.1f;
        if (player.health > player.maxHealth) player.health = player.maxHealth;
    }
```

- [ ] **Step 5: Reset Wanderer state on death/floor change**

In the respawn/floor transition code, clear Wanderer-specific state:

```cpp
    if (m_playerClass == PlayerClass::WANDERER) {
        m_localPlayer.dodgeState = {};
        m_localPlayer.deflectTimer = 0.0f;
        m_localPlayer.markedEntityIdx = 0xFFFF;
        m_localPlayer.markTimer = 0.0f;
        m_localPlayer.deathsDanceTimer = 0.0f;
    }
```

- [ ] **Step 6: Build, run, full playtest**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ./build/dungeon_game
```

Full playtest checklist:
1. Select Wanderer → correct HP (90), speed (6.5), energy (110)
2. Shift → dodge roll with camera barrel roll, 1s cooldown
3. LCTRL → no block
4. Dodge through melee attack → riposte damage + no damage taken
5. Adrenaline stacks appear (floor 20+), attacks get faster
6. Skill 1 (Deflect) → parry window, stun melee, reflect projectiles
7. Skill 2 (Exploit Weakness) → mark enemy, +60% damage
8. Skill 3 (Adrenaline Surge) → passive indicator on HUD
9. Skill 4 (Death's Dance) → AoE slash on dodge-through
10. Upgrades work at correct floors (5, 20, 30, 40)

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(wanderer): skill upgrades, state reset, and polish"
```

---

## Verification Summary

| Feature | Test | Expected |
|---------|------|----------|
| Dodge roll | Press Shift as Wanderer | 4m movement, 360° camera roll, 1s cooldown |
| I-frames | Dodge into enemy swing | No damage first 0.3s, damage after 0.2s recovery |
| No block | Press LCTRL as Wanderer | Nothing happens |
| Riposte | Dodge-through enemy attack | 50% weapon damage counter-hit on attacker |
| Adrenaline | 3 dodge-throughs | Weapon attacks 30% faster, decays after 4s |
| Deflect | Skill 1 + melee hit within 0.3s | Enemy stunned 2s, no damage |
| Deflect reflect | Skill 1 + projectile hit | Projectile reverses, hits sender |
| Exploit Weakness | Skill 2 on enemy | +60% damage, refreshes on dodge-through |
| Mark spread | Kill marked enemy (floor 20+) | Mark jumps to nearest enemy |
| Death's Dance | Skill 4 + dodge-through | 3m AoE slash on all nearby enemies |
| DD heal | Skill 4 AoE (floor 40+) | Heal 10% of AoE damage dealt |
| HUD | All above | Cooldown bar, orange pips, purple timer, mark icon |
| Network | Host as Wanderer | Dodge works, counter-hits server-authoritative |
