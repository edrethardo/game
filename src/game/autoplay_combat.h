// autoplay_combat.h — pure combat decision for one tick: pick a target, aim (leading projectiles
// with LeadAssist::interceptTime), and translate the doctrine's engagement band into fire + move.
// Aim is returned as absolute yaw/pitch; the engine convention is forward = {-sinYaw*cosPitch,
// sinPitch, -cosYaw*cosPitch} (player.cpp:80), so forward at yaw 0 is (0,0,-1) = -Z (NOT +Z), and
// the inverse is yaw = atan2f(-dir.x, -dir.z) — the same one used engine-wide (engine_arena.cpp:55).
// Header-only, engine-free.
#pragma once
#include "core/types.h"
#include "core/math.h"
#include "game/autoplay_intent.h"
#include "game/autoplay_doctrine.h"
#include "game/lead_assist.h"

namespace Autoplay {

// dir (unit) -> (yaw,pitch) in the engine convention. Inverse of the forward formula: forward.x =
// -sinYaw*cosPitch and forward.z = -cosYaw*cosPitch, so yaw = atan2f(-x,-z); forward.y = sinPitch.
inline void dirToAim(Vec3 dir, f32& yaw, f32& pitch) {
    Vec3 d = normalize(dir);
    yaw   = atan2f(-d.x, -d.z);
    pitch = asinf(d.y);
}

// --- aim smoothing (driver-side, but pure + tested here) ---------------------------------------
// The driver used to write the lead-corrected yaw STRAIGHT onto the player every tick, so the bot's
// head teleported onto a target the instant it appeared — the single most aimbot-looking thing it
// did. These two helpers turn that snap into a bounded turn.

// Shortest signed delta from `current` to `desired`, folded into [-pi, pi]. The fmodf first is not
// decoration: the engine never re-wraps Player::yaw (applyMovement just subtracts the look delta),
// so after a few minutes of spinning it sits several turns outside +/-pi and a bare subtraction
// would report a multi-revolution error.
inline f32 angleDelta(f32 current, f32 desired) {
    constexpr f32 kPi = 3.14159265358979f, kTwoPi = 6.28318530717959f;
    f32 d = fmodf(desired - current, kTwoPi);
    if (d >  kPi) d -= kTwoPi;
    if (d < -kPi) d += kTwoPi;
    return d;
}

// Rotate `current` toward `desired` along the SHORT arc, with TWO limits stacked:
//
//   1. PROPORTIONAL EASE-OUT (`gain`, 1/s). The turn speed is proportional to the remaining error,
//      so the crosshair DECELERATES as it converges. This is the half that reads as human. The
//      first version was a hard rate cap alone: it swept at exactly full speed and then stopped
//      dead the frame it arrived — a constant-velocity sweep with an abrupt stop is a machine
//      signature no matter how slow you make it. Integrated exactly (`1 - exp(-gain*dt)` is the
//      closed-form solution of err' = -gain*err) rather than as `gain*err*dt`, so the curve is
//      identical at any tick rate — a 30 Hz and a 60 Hz sim reach the same angle at the same
//      wall-clock time, and a double-stepped catch-up frame can't overshoot into instability.
//   2. RATE CAP (`maxRadPerSec`). Governs the far field, where `gain*err` would still be a
//      teleport: a 180° error under gain alone would start at ~19 rad/s.
//
// Landing exactly on `desired` when one step would reach it is what stops the tail from jittering
// (and, with a large gain, from ringing). Safe for pitch too (|delta| there can never exceed pi,
// so the wrap never bites).
inline f32 stepAngle(f32 current, f32 desired, f32 gain, f32 maxRadPerSec, f32 dt) {
    // Below this the aim is ON target. An exponential approach only ever ASYMPTOTES, so without a
    // floor a hair-width residual would sit there being chased forever; 1e-4 rad is 0.006 deg,
    // three orders under the wobble that rides on top of it.
    constexpr f32 kSnapEps = 1e-4f;
    if (dt <= 0.0f) return current;                // paused: hold the aim
    const f32 d  = angleDelta(current, desired);
    const f32 ad = (d < 0.0f) ? -d : d;
    if (ad <= kSnapEps) return desired;            // already there: land exactly
    // Exponential approach: the fraction of the remaining error consumed this step.
    f32 step = ad * (1.0f - expf(-gain * dt));
    const f32 cap = maxRadPerSec * dt;             // far-field speed limit
    if (cap < step) step = cap;
    if (step <= 0.0f) return current;              // zero gain AND zero rate: hold
    if (step >= ad)   return desired;              // degenerate huge gain: land, never overshoot
    return (d > 0.0f) ? current + step : current - step;
}

// --- FIRE ALIGNMENT GATE -------------------------------------------------------------------
// decideCombat decides `fire` from the DESIRED aim, but the driver only EASES the real crosshair
// toward it (stepAngle above), so for the whole turn the two disagree — sometimes by 90°+ when a
// new target appears off to the side. Pulling the trigger on the intent alone therefore sprays
// every wall the crosshair sweeps ACROSS on its way to the target (measured: 32% of the bot's
// shots had world geometry between the muzzle and the target). Before the smoothing landed the aim
// snapped, so fire was always aligned and this could not happen. The driver re-checks the ACTUAL
// aim after stepping it and holds fire until the crosshair has arrived.
//
// The tolerance cannot be arbitrarily tight. The ease is a first-order lag, so a MOVING target
// leaves a permanent steady-state error of (its angular rate / gain): at the shipped gain of 6, a
// target crossing at 0.4 rad/s sits ~0.067 rad off centre forever. A tolerance under that would
// MUTE the bot against anything that moves — the failure mode to watch for when re-tuning.
constexpr f32 FIRE_ALIGN_RAD = 0.09f;        // ~5.2°: clears the tracking lag, ~0.9 m (a body) at 10 m
// MELEE swings a 70° cone (weapons.json) that queryConeSorted evaluates HORIZONTALLY — the
// `horizontalCone` flag drops the vertical component entirely, because a point-blank enemy's centre
// sits below the eye. So a melee bot must NOT wait for pinpoint alignment (it would stand there not
// swinging at something already well inside its arc) and must not gate on pitch at all. Half the
// cone is 0.61 rad; this keeps the swing inside the arc with margin.
constexpr f32 FIRE_ALIGN_MELEE_RAD = 0.45f;  // ~26°, vs the ±35° half-cone

// True when the ACTUAL aim has converged onto the desired aim closely enough to shoot. Melee uses
// the wide tolerance and ignores pitch (see above); everything else must line up on both axes.
inline bool aimOnTarget(f32 actualYaw, f32 actualPitch,
                        f32 desiredYaw, f32 desiredPitch, bool melee) {
    const f32 tol = melee ? FIRE_ALIGN_MELEE_RAD : FIRE_ALIGN_RAD;
    if (fabsf(angleDelta(actualYaw, desiredYaw)) > tol) return false;
    if (melee) return true;                                  // horizontal cone: pitch never gates it
    // Pitch needs no wrap fold — both are clamped to ±89°, so a plain difference is the short arc.
    return fabsf(actualPitch - desiredPitch) <= tol;
}

// Sub-degree aim WOBBLE, so shots are not pixel-perfect. Two slow incommensurate sinusoids off the
// sim tick — deterministic by construction (rand() would desync a replay/snapshot and make a live
// bug unreproducible), and the mismatched periods keep it from reading as a clean oscillation.
// Amplitude is deliberately tiny: ~0.6 deg is ~15 cm of drift at 15 m, well inside an enemy body.
// FREQUENCIES are ~35% slower than the first pass (yaw 1.70/0.53 -> 1.10/0.36 rad/s, i.e. periods
// 5.7 s and 17 s): at the old speed the drift read as JITTER laid over the aim, which is the very
// thing this tune is removing. Slow and wide reads as breathing.
inline void aimWobble(u32 tick, f32& yawOff, f32& pitchOff) {
    constexpr f32 kAmp = 0.011f;                   // rad (~0.63 deg) peak yaw wobble
    const f32 t = static_cast<f32>(tick) * (1.0f / 60.0f);
    yawOff   = kAmp * (0.62f * sinf(t * 1.10f) + 0.38f * sinf(t * 0.36f + 1.3f));
    pitchOff = kAmp * 0.5f * (0.62f * sinf(t * 0.85f + 0.7f) + 0.38f * sinf(t * 0.41f));
}

// --- perfect-block timing ------------------------------------------------------------------------
// Combat::classifyBlock grades a block by how long it has been HELD: < 0.2 s = PERFECT (all damage
// negated), else BLOCKED (half). A bot that just holds the button therefore only ever earns the weak
// tier AND pays the 0.4x move slow for it. So we RAISE the block only when a swing is about to land.
constexpr f32 PERFECT_BLOCK_WINDOW = 0.2f;   // mirrors Combat::classifyBlock
// How early to raise. Entity::attackTimer counts down and the swing resolves at <= 0, so raising at
// `attackTimer <= LEAD` puts the hit at blockTimer ~= attackTimer < the window. Kept under the 0.2 s
// window with margin for the AI/player update order (the bot's view is one tick stale).
constexpr f32 PERFECT_BLOCK_LEAD = 0.15f;
// Slack on the attacker's own reach: it swings when we are within attackRange*1.1 (enemy_ai_states)
// and it is still closing while the timer runs out.
constexpr f32 BLOCK_REACH_SLACK = 1.25f;

// True if this enemy's swing is about to LAND — the raise trigger.
//
// Two engine facts make this narrower than "attackTimer is small":
//  * MELEE ONLY. A melee swing resolves the instant the timer crosses 0, so the timer IS the time to
//    impact. A ranged enemy's timer only says when the SHOT LEAVES; the projectile then flies for
//    dist/16 s, so blocking on its fire would open the window far too early (measured: an 8 m bolt
//    lands ~0.5 s later, i.e. at the BLOCKED tier). Timing a projectile needs shot-arrival data the
//    view does not carry, so ranged attackers simply do not trigger the tap.
//  * attackTimer MUST BE POSITIVE. The STRAFE-state ranged fire (enemy_ai_states.cpp) only resets the
//    timer when it actually has LOS, so a ranged enemy holding a shot behind cover drifts it
//    unboundedly negative — it reads as "permanently 1 frame from swinging". Live, that was 213 of
//    275 block raises: pure flapping, and each one cost 0.4x move speed.
inline bool swingIsLanding(const BotTarget& t) {
    if (t.isRanged) return false;
    if (t.attackTimer <= 0.0f || t.attackTimer > PERFECT_BLOCK_LEAD) return false;
    return t.dist <= t.attackRange * BLOCK_REACH_SLACK;
}

// --- proactive dodge timing -----------------------------------------------------------------
// A roll must be spent ON SOMETHING. The first version triggered on mere PROXIMITY (an enemy inside
// 0.6 x the kite floor), which for a RANGED doctrine is true for most of every fight — so the bot
// rolled almost continuously ("ranged is dodge rolling too often", "less panicky"). The trigger is
// now the same shape as the perfect-block tap: a real MELEE swing about to land on us.
//
// Two knobs differ from the block's, both because a ROLL is slower to commit than a shield raise:
// it starts a fixed-duration animation and travels, so it must begin earlier (a longer lead) and
// the attacker has more time to close before impact (a slightly wider reach slack). Overshooting
// the lead is cheap — the i-frames outlast a swing — while undershooting eats the hit.
constexpr f32 DODGE_LEAD         = 0.30f;   // s before impact to commit the roll (block uses 0.15)
constexpr f32 DODGE_REACH_SLACK  = 1.35f;   // x the attacker's own reach (block uses 1.25)
// The OFFENSIVE gap-closer charge rides its own, longer leash. Its job is to look like a deliberate
// rush across a firing lane, and one roll covers ~4 m — repeating it every second turns a charge
// into a stutter of little hops.
constexpr f32 GAP_CLOSE_COOLDOWN = 6.0f;    // s between gap-closer rolls (driver-enforced)

// True if this enemy's MELEE swing is close enough to land that rolling now buys the i-frames.
// Mirrors swingIsLanding's two engine facts (ranged timers say when the SHOT LEAVES, not when it
// lands; a negative timer is the stale behind-cover drift, not "about to swing"), at the roll's
// wider timing.
inline bool swingIsIncoming(const BotTarget& t) {
    if (t.isRanged) return false;
    if (t.attackTimer <= 0.0f || t.attackTimer > DODGE_LEAD) return false;
    return t.dist <= t.attackRange * DODGE_REACH_SLACK;
}

// Returns the index of the nearest target with LOS, or -1 if none has LOS.
inline s32 pickTarget(const BotView& v) {
    s32 best = -1; f32 bestD = 1e9f;
    for (u32 i = 0; i < v.targetCount; i++) {
        if (!v.targets[i].hasLOS) continue;
        if (v.targets[i].dist < bestD) { bestD = v.targets[i].dist; best = (s32)i; }
    }
    return best;
}

inline BotIntent decideCombat(const BotView& v, const Doctrine& d) {
    BotIntent out{};
    out.aimYaw = v.yaw; out.aimPitch = v.pitch;
    const s32 ti = pickTarget(v);
    if (ti < 0) return out;                       // no LOS target: caller falls through to TRAVEL
    const BotTarget& t = v.targets[(u32)ti];
    const Vec3 eye = v.pos + Vec3{0, v.eyeHeight, 0};

    // Aim: lead projectile weapons; hitscan/melee aim straight at the centre.
    Vec3 aimPt = t.pos;
    if (v.weaponProjSpeed > 0.1f) {
        f32 tHit;
        // interceptTime's MAX_LEAD_SEC (1.5 s) cap is inherited from the throwing-knife tuning, so a
        // slow projectile at long range silently drops its lead and aims at centre — a heads-up for
        // whoever tunes bot accuracy.
        if (LeadAssist::interceptTime(t.pos - eye, t.vel, v.weaponProjSpeed, tHit))
            aimPt = t.pos + t.vel * tHit;
    }
    dirToAim(aimPt - eye, out.aimYaw, out.aimPitch);

    // Engagement band (x weaponRange). engageMin is a MOVEMENT rule only — it says how much spacing
    // the build wants, not when it may shoot.
    const f32 lo = d.engageMin * v.weaponRange;
    const f32 hi = d.engageMax * v.weaponRange;

    // KITE ONLY FROM A MELEE THREAT. Backing off buys spacing from something that must physically
    // REACH you; a ranged enemy shoots across the retreat, so the same backpedal just surrenders
    // ground while still eating the volley — and, because the bot walks backwards, it drags its own
    // aim off the target too. Observed live and reported ("the bot runs away from ranged enemies").
    // Inside the band against a ranged target the right answer is to HOLD and shoot; the gap-closer
    // roll below still handles the case where it is OUTSIDE the band and wants to charge.
    if (t.dist < lo)      out.moveBack = !t.isRanged;
    else if (t.dist > hi) out.moveFwd  = true;    // too far: close in

    // Fire whenever the target has LOS and is within weapon reach — INCLUDING inside the kite floor.
    // Shooting while retreating is what kiting IS; gating fire on the full band meant a swarm that got
    // inside engageMin was never shot at, so a ranged/caster bot backpedalled forever and killed
    // nothing (live: sorcerers permanently stuck on floor 1).
    out.fire = t.dist <= hi && t.hasLOS && !v.stunned && !v.rolling;

    // CLASS SKILL. Cast whenever we are actually engaging (same gate as fire: LOS + within reach,
    // not stunned/rolling) and the driver reports a slot that would really fire. No cadence of our
    // own: the engine's energy + cooldown gates ARE the rate limit, and castableSkill mirrors them,
    // so pressing every eligible tick just means "cast the moment it comes up" — which is how a
    // Magic build is meant to fight (its skills are its damage, the wand is the filler). Lowest
    // castable slot wins: slot 0 is the always-unlocked basic, so an early character still casts.
    if (out.fire) {
        for (u8 s = 0; s < 4; s++)
            if (v.castableSkill[s]) { out.classSkillSlot = (s8)s; break; }
    }

    // DEFENSIVE ROLL — purposeful, never panicked. Two gates on top of the engine's own dodge
    // cooldown: an actual incoming melee swing (swingIsIncoming, NOT proximity — see the constants)
    // and the driver's multi-second leash (v.dodgeAllowed). Scans ALL targets like the block tap
    // does: the thing about to hit you is usually not the thing you are shooting.
    const bool dodgeReady = v.dodgeCooldown <= 0.0f && !v.stunned && !v.rolling;
    if (d.dodgesProactively && dodgeReady && v.dodgeAllowed) {
        for (u32 i = 0; i < v.targetCount; i++)
            if (swingIsIncoming(v.targets[i])) { out.dodge = true; break; }
    }
    if (!out.dodge && t.isRanged && out.moveFwd && dodgeReady && v.gapCloseAllowed &&
        fabsf(angleDelta(v.yaw, out.aimYaw)) < 0.5f) {
        // OFFENSIVE gap-closer: a RANGED enemy we are trying to walk up to is charged with a roll
        // instead — 0.5 s at 8 m/s covers ~4 m with 0.3 s of i-frames, so we cross its firing lane
        // faster AND eat one volley for free. Only for ranged: a melee enemy is already closing the
        // gap for us, and spending the roll on it just burns the i-frames we'd rather have when it
        // arrives. Gated on already FACING the target (the roll direction is the CURRENT yaw via
        // computeRollDirection, and the aim is rate-limited now, so rolling mid-turn would launch
        // us sideways into a wall). The `!out.dodge` guard pins the DEFENSIVE roll as the winner
        // when both want the same button: eating a swing you can see coming beats closing ground.
        out.dodge = true;
        out.dodgeIsGapClose = true;   // the driver charges the (longer) offensive leash
        out.moveFwd = true;   // explicit: the roll direction is the WASD held on the SAME tick
    }

    // BLOCK — a TAP timed into the perfect window, never a hold (see PERFECT_BLOCK_* above). Scans
    // ALL targets, not just the aim target: the thing about to hit you is usually not the thing you
    // are shooting. Dropped the old `!out.fire` gate — blocking does not gate the swing (it only
    // costs 0.4x move speed for the ~0.15 s the tap lasts), so a tank can keep attacking through it.
    if (d.blocks && !v.stunned && !v.rolling && v.blockHeld < PERFECT_BLOCK_WINDOW) {
        for (u32 i = 0; i < v.targetCount; i++)
            if (swingIsLanding(v.targets[i])) { out.block = true; break; }
    }

    return out;
}

} // namespace Autoplay
