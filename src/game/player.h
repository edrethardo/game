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
    f32  rollProg       = 0.0f;    // 0->1->0 arc over the roll (peaks mid-roll): drives the
                                   // camera head-dip + viewmodel lean so they stay in sync
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
    f32  renderedHealth = 100.0f;   // M13: lerps toward `health` at 4 Hz visual rate — HP bar reads this so hits look smooth, not snapped
    f32  screenFlashTimer = 0.0f;   // M13: 0.5 s full-screen dark flash on large prediction divergences (>=10 m teleport)
    f32  damageFlashTimer = 0.0f;
    f32  hitShakeTimer    = 0.0f;  // screen shake on taking damage
    f32  hurtVignette     = 0.0f;  // 0..1 per-hit red edge-vignette intensity, decays each frame
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
    bool lifesaverArmed   = true;  // near-death i-frame available; consumed on use, re-armed only at >=40% HP
    bool graceInvuln      = false; // tags invulnTimer as near-death-grace-sourced (not dodge/spawn/skill) so
                                   // the "clear grace once healthy (>85% HP)" rule can't strip dodge i-frames
    f32  damageReduction  = 0.0f;  // 0.0–1.0, fraction of damage absorbed (class passive)
    // Shrine buff (see ShrineBuff:: in game/shrine.h). Timed: without shrineBuffTimer a buff, once
    // granted, would simply never expire — which is the state this field was actually in for a long
    // time, being read every frame and written by nothing.
    u8   shrineBuff       = 0;     // ShrineBuff::NONE/POWER/SPEED/VITALITY
    f32  shrineBuffValue  = 0.0f;  // multiplier/bonus amount
    f32  shrineBuffTimer  = 0.0f;  // countdown; 0 = no buff
    // The EXACT max-HP that VITALITY added, so it can always be given back. Deriving the amount
    // from shrineBuffValue at expiry (the old approach) only worked while the buff slot still SAID
    // vitality — and there is one slot, so taking any other shrine overwrote it and the max-HP grant
    // was never reverted. It became permanent, compounded on every re-take, and was written to the
    // save. A live character reached 44,922 HP against a legitimate ~1,195.
    f32  shrineHealthBonus = 0.0f;

    // The CLASS component of max HP: class base, compounded by the +1.5%-per-floor descent growth.
    // maxHealth is now DERIVED from this plus the equipped gear (Inventory::refreshMaxHealth), so it
    // is no longer a free-running accumulator that anything could permanently nudge — which is how a
    // leaked shrine buff compounded to 44,922 HP and got written to disk. Only this base is saved;
    // gear and buffs are recomputed at runtime and can never contaminate the file.
    f32  baseMaxHealth    = 100.0f;
    u8   ringPassive      = 0;    // SkillId of equipped legendary ring (0 = none)
    f32  lastDamageTaken  = 0.0f; // damage from last hit (for thorns reflection + Blood Nova armor retaliation)
    u16  lastDamageAttackerIdx = 0xFFFF; // entity index of the last attacker (0xFFFF = unknown, e.g. enemy projectile) — thorns targeting
    f32  bloodNovaCooldown = 0.0f; // Blood Nova ARMOR aura (Demonhide Cuirass): internal retaliation cooldown.
                                   // Transient — Player is serialized field-by-field, so this adds no save-format change.

    // Defensive-pack equipment cache (recomputed each frame from equipped affixes in
    // tickPassiveEquipment; transient/never serialized, like damageReduction above). Summed on
    // demand via Inventory::armorRating/healthRegenRate/thornsPct so PlayerInventory gains no field.
    f32  armorRating      = 0.0f; // flat armor → diminishing-returns mitigation in applyDamageToPlayer
    f32  healthRegen      = 0.0f; // HP restored per second by HEALTH_REGEN affixes
    f32  thornsPctBonus   = 0.0f; // % of damage-taken reflected (affix; stacks with the THORNS ring passive)

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

    // Glove passive state (Frenzy: on-hit attack-speed stacks, legendary gloves)
    u8   frenzyStacks       = 0;     // +5% attack speed each, max 6
    f32  frenzyTimer        = 0.0f;  // shared buff duration; refreshed on hit, drops all stacks at 0

    // Shield blocking (Ctrl/Shift)
    bool blocking         = false;
    f32  blockTimer        = 0.0f;  // time since block started (for perfect block window)

    // --- Wanderer ---
    DodgeState dodgeState;
    f32  deflectTimer     = 0.0f;  // absorb window countdown (0.4s)
    f32  deflectAbsorbed  = 0.0f;  // total accumulated damage during deflect window
    u8   deflectHitCount  = 0;     // number of hits absorbed (each fires 8 projectiles)
    f32  deflectSpeedTimer = 0.0f; // 8% move speed buff after deflect burst (3s)
    f32  markTimer        = 0.0f;  // Exploit Weakness mark duration (timer-based AoE mark)
    u8   markSpeedStacks  = 0;     // Exploit Weakness speed buff stacks (5% each, max 20)
    f32  markSpeedTimers[20] = {}; // per-stack 3s non-refreshing decay
    f32  deathsDanceTimer = 0.0f;  // ultimate duration countdown
    bool adrenalineUnlocked = false; // Wanderer: available from floor 1 (set each tick)
    bool adrenalineUpgraded = false; // true once floor >= 30 (move speed bonus active)
    u8   adrenalineMaxStacks = 3;    // 3 on floors 1-4, 5 from floor 5 on (set each tick)

    // Soft target lock — currently inert (lockActive never set true). Kept because the
    // values flow through the sync helpers into NetPlayer / the snapshot wire (R7-6).
    u16  lockIndex      = 0xFFFF; // entity index (or 0xFFFF if none)
    u16  lockGeneration = 0;
    bool lockActive     = false;

    // Cached forward vector (computed once per frame in update)
    Vec3 forward = {0.0f, 0.0f, -1.0f};
};

namespace PlayerController {
    // Original: reads Input:: directly (used for singleplayer and local capture)
    void update(Player& player, f32 dt);

    // Apply to NetPlayer (same logic, different struct). movementOnly=true (reconcile
    // replay) re-integrates position but skips timer/i-frame side effects (slow decay,
    // dodge invuln) that the authoritative snapshot already owns.
    void updateNetPlayerFromInput(NetPlayer& np, const NetInput& input, f32 dt,
                                  bool movementOnly = false);

    void applyToCamera(const Player& player, Camera& cam);

    // Capture current Input:: state into a NetInput. The Player& is read for the live
    // yaw/pitch/position baseline — captureLocalInput packs absolute (not delta) aim
    // and position, which it computes by applying this frame's pending mouse delta to
    // `player.yaw/pitch/position` without mutating them (PlayerController::update later
    // in the frame produces the identical update via applyMovement).
    NetInput captureLocalInput(const Player& player, u32 tick, u8 weaponId);

    // Wanderer dodge roll direction from the WASD held at dodge-start + facing yaw.
    // Pure + shared so the client (PlayerController::update) and the server
    // (updateNetPlayerFromInput, deriving w/s/a/d from NetInput.moveFlags) compute the
    // IDENTICAL roll vector — they must agree or the server-replicated dodge diverges
    // from the client's prediction. No directional input held → rolls straight forward.
    Vec3 computeRollDirection(bool w, bool s, bool a, bool d, f32 yaw);
}
