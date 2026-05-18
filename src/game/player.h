#pragma once

#include "core/types.h"
#include "core/math.h"
#include "renderer/camera.h"

struct EntityHandle;
struct NetInput;
struct NetPlayer;

// Wanderer dodge roll state — tracks all transient data for the 0.5s roll and cooldown.
struct DodgeState {
    f32  rollTimer      = 0.0f;    // countdown during roll (0.5 -> 0)
    f32  cooldownTimer  = 0.0f;    // countdown after roll ends (1.0 -> 0)
    Vec3 rollDirection  = {0,0,0}; // normalized XZ direction of roll
    f32  rollAngle      = 0.0f;    // camera roll axis (barrel roll, radians)
    f32  pitchAngle     = 0.0f;    // camera pitch axis (front/back flip, radians)
    f32  rollWeight     = 0.0f;    // blend: 0 = pure pitch flip, 1 = pure barrel roll
    f32  pitchWeight    = 0.0f;    // blend: how much front/back flip
    bool rolling        = false;   // true during the 0.5s roll
    s8   rollSign       = 1;       // +1 clockwise, -1 counter-clockwise
    s8   pitchSign      = 1;       // +1 forward flip, -1 backward flip
    u8   counterStacks  = 0;       // adrenaline surge stacks (0-5)
    f32  counterTimers[5] = {};    // per-stack decay timers (4s each)
};

struct Player {
    Vec3 position   = {0.0f, 0.0f, 0.0f}; // feet position (bottom of collider)
    Vec3 velocity   = {0.0f, 0.0f, 0.0f};
    f32  eyeHeight  = 1.7f;    // metres above feet
    f32  yaw        = 0.0f;    // radians
    f32  pitch      = 0.0f;    // radians, clamped to ±89°
    f32  moveSpeed  = 6.0f;    // m/s (scaled by SPEED_MULT at runtime)
    f32  sensitivity = 0.002f; // radians per pixel
    bool onGround   = false;
    bool noclip     = false;   // fly freely, no collision

    // Combat
    f32  health         = 100.0f;
    f32  maxHealth      = 100.0f;
    f32  damageFlashTimer = 0.0f;
    f32  hitShakeTimer    = 0.0f;  // screen shake on taking damage
    f32  slowTimer        = 0.0f;  // movement speed debuff countdown
    f32  poisonTimer      = 0.0f;
    f32  poisonDps        = 0.0f;
    f32  burnTimer        = 0.0f;
    f32  burnDps          = 0.0f;
    f32  freezeTimer      = 0.0f;  // halves movement speed
    f32  curseTimer       = 0.0f;  // necromancer curse — increased damage taken
    u8   curseStacks      = 0;     // 5% increased damage per stack, max 4
    f32  overdriveTimer   = 0.0f;  // Mech Overdrive buff countdown (damage/speed boost)
    f32  smokeTimer       = 0.0f;  // stealth — enemies can't detect player while > 0
    f32  shadowDanceTimer = 0.0f;  // Shadow Dance: 2× damage + 20% speed, kills extend by 0.3s
    f32  invulnTimer      = 0.0f;  // damage immunity countdown (respawn/floor entry)
    f32  damageReduction  = 0.0f;  // 0.0–1.0, fraction of damage absorbed (class passive)
    u8   shrineBuff       = 0;     // 0=none, 1=power(+30%dmg), 2=speed(+25%), 3=vitality(+40%hp)
    f32  shrineBuffValue  = 0.0f;  // multiplier/bonus amount
    u8   ringPassive      = 0;    // SkillId of equipped legendary ring (0 = none)
    f32  lastDamageTaken  = 0.0f; // damage from last hit (for thorns reflection)

    // CS-style directional damage indicators — arcs showing where hits came from
    static constexpr u32 MAX_HIT_INDICATORS = 4;
    struct HitIndicator {
        f32 angle;  // radians relative to player yaw (0 = front, π = behind)
        f32 timer;  // counts down from 0.8s
    };
    HitIndicator hitIndicators[MAX_HIT_INDICATORS] = {};

    // Ring passive state
    f32  secondWindCooldown = 0.0f;  // internal cooldown for Second Wind (60s)
    u8   soulHarvestStacks  = 0;     // current Soul Harvest kill streak stacks (max 5)
    f32  soulHarvestTimer   = 0.0f;  // time remaining on Soul Harvest buff

    // Shield blocking (Ctrl/Shift)
    bool blocking         = false;
    f32  blockTimer        = 0.0f;  // time since block started (for perfect block window)

    // --- Wanderer ---
    DodgeState dodgeState;
    f32  deflectTimer     = 0.0f;  // absorb window countdown (0.4s)
    f32  deflectAbsorbed  = 0.0f;  // total accumulated damage during deflect window
    u8   deflectHitCount  = 0;     // number of hits absorbed (each fires 8 projectiles)
    f32  deflectSpeedTimer = 0.0f; // 8% move speed buff after deflect burst (3s)
    u16  markedEntityIdx  = 0xFFFF;// Exploit Weakness target
    u16  markedEntityGen  = 0;
    f32  markTimer        = 0.0f;
    u8   markSpeedStacks  = 0;     // Exploit Weakness speed buff stacks (5% each, max 20)
    f32  markSpeedTimers[20] = {}; // per-stack 3s non-refreshing decay
    f32  deathsDanceTimer = 0.0f;  // ultimate duration countdown
    bool adrenalineUnlocked = false; // true once floor >= 20 (skill 3 unlocked)
    bool adrenalineUpgraded = false; // true once floor >= 30 (move speed bonus active)

    // Soft target lock
    u16  lockIndex      = 0xFFFF; // entity index (or 0xFFFF if none)
    u16  lockGeneration = 0;
    bool lockActive     = false;
    f32  lockLosTimer   = 0.0f;   // time since LOS was broken

    // Cached forward vector (computed once per frame in update)
    Vec3 forward = {0.0f, 0.0f, -1.0f};
};

namespace PlayerController {
    // Original: reads Input:: directly (used for singleplayer and local capture)
    void update(Player& player, f32 dt);

    // Network-aware: applies a NetInput struct instead of reading Input::
    void updateFromInput(Player& player, const NetInput& input, f32 dt);

    // Apply to NetPlayer (same logic, different struct)
    void updateNetPlayerFromInput(NetPlayer& np, const NetInput& input, f32 dt);

    void applyToCamera(const Player& player, Camera& cam);

    // Capture current Input:: state into a NetInput
    NetInput captureLocalInput(u32 tick, u8 weaponId);
}
