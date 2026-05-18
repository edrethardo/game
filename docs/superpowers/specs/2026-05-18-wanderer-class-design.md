# Wanderer Class Design Spec

## Overview

New player class: **Wanderer**. Evasive counter-attacker inspired by Bloodborne's hunter. Replaces blocking with a dodge roll on Shift. Dodging through enemy attacks rewards instant counter-hits and stacking attack speed buffs. The Wanderer thrives in melee range, dancing through danger.

## Base Stats

| Stat | Value | Notes |
|------|-------|-------|
| Health | 90 | Below average -- dodge is the defense |
| Move Speed | 6.5 | Fast, tied with Ranger |
| Energy | 110 | Moderate |
| Preferred Weapon | Melee | +20% melee damage bonus |
| Damage Reduction | 0.0 | No passive reduction |
| Starting Weapon | "Sword" | Classic longsword |

**Class passive:** No block. Wanderer's `blocking` flag is never set. The BLOCK action (`GameAction::BLOCK` / LCTRL / left trigger) is ignored for this class. Instead, `GameAction::DODGE` (Shift / right stick click) triggers a dodge roll.

## Dodge Roll Mechanic

### Input

- **Key**: Shift (keyboard), Right Stick Click (gamepad)
- **Action**: New `GameAction::DODGE` added to input system
- **Activation**: Press once to trigger. Not held.

### Movement

- **Direction**: WASD direction at time of press. If no direction held, defaults to player's forward (yaw).
- **Distance**: 4 meters
- **Duration**: 0.5 seconds total
- **Speed**: 8 m/s constant during roll (4m / 0.5s)
- **Physics**: Uses `Collision::moveAndSlide()` with roll velocity vector. Not a teleport -- the player physically traverses space and collides with walls.
- **Wall collision**: Roll stops early if hitting a wall. I-frames still apply for the remaining i-frame duration.
- **Gravity**: Gravity still applies during roll. Player stays grounded (no floating mid-air rolls).

### I-Frames (Invulnerability)

- **Window**: First 0.3s of the 0.5s roll duration (60% of the roll)
- **Implementation**: Sets `player.invulnTimer = 0.3f` at roll start. The existing invuln system blocks all damage and clears status effects.
- **End of i-frames**: After 0.3s, player is vulnerable for the remaining 0.2s of the roll animation (recovery frames).

### Cooldown

- **Duration**: 1.0 second, starts when the roll ends (not when it starts)
- **No resource cost**: Dodge is free, cooldown-gated only
- **CDR interaction**: NOT affected by Cooldown Reduction affixes (dodge CD is fixed)
- **HUD**: Cooldown shown on a small indicator near the crosshair (replaces block indicator for Wanderer)

### Camera Effect (First-Person Barrel Roll)

- **Type**: Full 360-degree rotation on the camera's roll axis
- **Duration**: 0.5s (matches roll duration exactly)
- **Direction**:
  - Dodge left (A) = counter-clockwise roll
  - Dodge right (D) = clockwise roll
  - Dodge forward (W) or no direction = clockwise roll (arbitrary default)
  - Dodge backward (S) = counter-clockwise roll
- **Interpolation**: Sine ease-in-out curve: `roll = 2*PI * (3t^2 - 2t^3)` where `t = elapsed / duration`. Smooth start and end, fastest in the middle.
- **Implementation**: New `rollAngle` field on Player (or DodgeState). Applied in `PlayerController::applyToCamera()` as a rotation around the camera's forward axis. Resets to 0 when roll ends.
- **Look input**: Mouse/stick look is disabled during the roll. Player cannot change yaw/pitch mid-roll.

### State Machine

```
IDLE -> ROLLING (on Shift press, if cooldown == 0)
ROLLING -> RECOVERING (after 0.3s, i-frames end)
RECOVERING -> COOLDOWN (after 0.5s total, roll animation ends)
COOLDOWN -> IDLE (after 1.0s cooldown expires)
```

Player struct addition:
```cpp
struct DodgeState {
    f32 rollTimer;      // countdown from 0.5 to 0 during roll
    f32 cooldownTimer;  // countdown from 1.0 to 0 after roll
    Vec3 rollDirection; // normalized XZ direction of roll
    f32 rollAngle;      // current camera roll in radians (0 to 2*PI)
    bool rolling;       // true during the 0.5s roll
    u8 counterStacks;   // adrenaline surge stacks (0-5)
    f32 counterTimers[5]; // per-stack decay timers (4s each)
};
```

## Counter-Attack System (Dodge-Through)

### Detection

During the i-frame window (first 0.3s of roll), the existing `Combat::applyDamageToPlayer()` is called but damage is blocked by `invulnTimer > 0`. A **dodge-through** is detected when:

1. Player is in rolling state (`dodgeState.rolling == true`)
2. `invulnTimer > 0` (still in i-frame window)
3. An enemy attack that would have dealt damage is blocked

The attacker's entity index is captured at the point of damage rejection.

### Instant Riposte

On dodge-through:
- Automatic counter-hit on the attacking entity
- Damage: 50% of the player's effective weapon damage
- Uses `Combat::applyDamage()` on the attacker
- Spawns a damage number at the attacker's position
- Visual: brief white flash on screen edge

### Adrenaline Stacks

On dodge-through:
- Add 1 stack of Adrenaline (max 5)
- Each stack: +10% attack speed (reduces weapon cooldown multiplicatively)
- Each stack has an independent 4s decay timer
- At max stacks: +50% attack speed
- Visual: subtle speed-line overlay intensity scales with stack count

### Weapon Cooldown Application

Adrenaline stacks reduce weapon cooldown:
```
effectiveCooldown = baseCooldown * (1.0 - counterStacks * 0.10)
```
At 5 stacks: `cooldown * 0.5` (50% faster attacks).

Applied in `handleWeaponFire()` when reading weapon cooldown.

## Class Skills

### Skill 1: Deflect (Unlock: Floor 1, Upgrade: Floor 5)

**Timed parry.** Brief active window that reflects projectiles and stuns melee attackers.

- **Activation**: Skill slot 1 (key "1" / d-pad up)
- **Active window**: 0.3 seconds
- **Cooldown**: 2.0 seconds
- **Energy cost**: 20
- **Melee parry**: If a melee enemy hits during the 0.3s window, the attacker is stunned for 2.0s. Player takes no damage. Spawns a "PARRY!" text.
- **Projectile reflect**: If a projectile hits during the window, it reverses direction and targets the original attacker. Deals the projectile's original damage.
- **Implementation**: New `deflectTimer` on Player. While `deflectTimer > 0`, incoming damage triggers parry logic instead of normal damage. Projectile reflection reverses `projectile.velocity` and sets `fromPlayer = true`.
- **Upgrade (Floor 5)**: Reflected projectiles deal 2x damage. Melee parry stun duration 2.0s -> 3.0s.

### Skill 2: Exploit Weakness (Unlock: Floor 10, Upgrade: Floor 20)

**Mark a target** for massively increased damage.

- **Activation**: Skill slot 2 (key "2" / d-pad right)
- **Target**: The entity under the crosshair (raycast from eye, similar to target lock)
- **Mark duration**: 5.0 seconds
- **Cooldown**: 8.0 seconds
- **Energy cost**: 25
- **Effect**: Marked entity takes +60% damage from all sources (player attacks, projectiles, skill damage)
- **Dodge-through refresh**: If you dodge through the marked enemy's attack, the mark timer resets to 5.0s
- **Implementation**: `markedEntityIdx` + `markedEntityGen` + `markTimer` on Player. In `Combat::applyDamage()`, check if target is marked and multiply damage by 1.6.
- **Visual**: Glowing outline or overhead icon on marked entity
- **Upgrade (Floor 20)**: When a marked enemy dies, the mark spreads to the nearest enemy within 5m with full duration.

### Skill 3: Adrenaline Surge (Unlock: Floor 20, Upgrade: Floor 30)

**Passive skill** that enhances the dodge-through counter system. This is the skill-slot representation of the Adrenaline stack mechanic.

- **Passive**: Always active once unlocked. Each dodge-through grants +10% attack speed, max 5 stacks, 4s per-stack decay.
- **Active display**: HUD shows current stack count and decay timers
- **Upgrade (Floor 30)**: Stacks also grant +5% move speed each (max +25% at 5 stacks). Applied as `moveSpeed *= (1.0 + counterStacks * 0.05)` in PlayerController.
- **Note**: Before this skill is unlocked (floors 1-19), dodge-throughs still grant counter-hits (riposte) but NOT adrenaline stacks. The stacking mechanic is the skill's contribution.

### Skill 4: Death's Dance (Unlock: Floor 30, Upgrade: Floor 40)

**Ultimate: 8-second combat trance.** Dodge-throughs trigger devastating AoE slashes.

- **Activation**: Skill slot 4 (key "4" / d-pad left)
- **Duration**: 8.0 seconds
- **Cooldown**: 30.0 seconds
- **Energy cost**: 40
- **Effect**: During Death's Dance, every dodge-through triggers a free 360-degree melee slash:
  - Radius: 3.0 meters
  - Damage: 100% of effective weapon damage
  - Uses `Combat::queryEntitiesInSphere()` to hit all entities in range
  - Visual: circular slash effect (similar to Whirlwind but single hit)
- **Upgrade (Floor 40)**: AoE slashes heal player for 10% of total damage dealt (life steal on the AoE).
- **Visual**: Player has a dark purple/red aura during Death's Dance. Slash effect is a fast expanding ring.

## Skill JSON Definition

```json
[
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
    "damage": 0,
    "stackBonus": 0.10,
    "maxStacks": 5,
    "stackDuration": 4.0
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
]
```

## Network Sync

### New Input Flag
- `INPUT_DODGE` bit added to `NetInput.extFlags`
- Server validates: cooldown must be 0, player must be Wanderer class

### Snapshot Data
- `DodgeState.rolling` and `counterStacks` included in `SnapPlayer` (2 extra bytes: 1 bit rolling + 3 bits stacks + cooldown quantized)
- `markTimer` and `markedEntityIdx` also synced for Exploit Weakness visual on other clients

### Server Authority
- Server runs dodge physics (moveAndSlide with roll velocity)
- Server detects dodge-throughs (damage blocked during roll = counter)
- Server applies riposte damage and adrenaline stacks
- Clients interpolate dodge position and play camera roll locally on prediction

## Files to Modify

| File | Changes |
|------|---------|
| `src/game/item.h` | Add `WANDERER` to `PlayerClass`, add `DEFLECT`/`EXPLOIT_WEAKNESS`/`ADRENALINE_SURGE`/`DEATHS_DANCE` to `SkillId`, add `DodgeState` struct to Player, add `deflectTimer`/`markTimer`/`markedEntityIdx`/`markedEntityGen`/`deathsDanceTimer` fields |
| `src/game/player.h` | Add `DodgeState dodgeState` to Player struct, add `GameAction::DODGE` |
| `src/game/player.cpp` | Handle dodge activation in `PlayerController::update()`, apply roll velocity, apply camera roll in `applyToCamera()` |
| `src/platform/input.h` | Add `GameAction::DODGE` with Shift/R3 binding |
| `src/game/skill.cpp` | Add Deflect, Exploit Weakness, Adrenaline Surge, Death's Dance activation and update logic |
| `src/game/combat.cpp` | Dodge-through detection in `applyDamageToPlayer()`, mark damage multiplier in `applyDamage()`, deflect/parry logic |
| `src/engine/engine_init.cpp` | Add Wanderer ClassDef to class table |
| `src/engine/engine_update.cpp` | Tick dodge timers, counter stack decay, Death's Dance timer, deflect timer |
| `src/engine/engine_hud.cpp` | Dodge cooldown indicator, adrenaline stack display, mark indicator, Death's Dance timer |
| `src/net/net_player.h` | Add `INPUT_DODGE` flag, sync dodge state |
| `src/net/snapshot.h` | Add dodge/counter fields to `SnapPlayer` |
| `assets/config/skills.json` | Add 4 Wanderer skill definitions |

## Verification

1. **Dodge roll**: Press Shift as Wanderer -> camera does full barrel roll, player moves 4m in WASD direction, 1s cooldown before next dodge
2. **I-frames**: Dodge into enemy melee swing -> take no damage during first 0.3s, take damage if hit during last 0.2s
3. **Counter-hit**: Dodge through enemy attack -> automatic damage number on attacker (50% weapon damage)
4. **Adrenaline stacks**: Dodge through 3 attacks -> weapon attacks 30% faster, stacks decay after 4s each
5. **Deflect**: Activate skill 1, get hit by projectile within 0.3s -> projectile reverses and hits sender
6. **Exploit Weakness**: Mark enemy -> all attacks deal +60% -> dodge through marked enemy -> mark timer refreshes
7. **Death's Dance**: Activate skill 4 -> dodge through enemy -> AoE slash hits all enemies within 3m
8. **No blocking**: As Wanderer, press LCTRL -> nothing happens (no block stance)
9. **Wall collision**: Dodge roll into a wall -> roll stops early, no wall clip
10. **Network**: In multiplayer, dodge roll is visible to other players, counter-hits are server-authoritative
