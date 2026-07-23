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

    // Engagement band (x weaponRange).
    const f32 lo = d.engageMin * v.weaponRange;
    const f32 hi = d.engageMax * v.weaponRange;
    const bool inBand = t.dist >= lo && t.dist <= hi;

    if (t.dist < lo)      out.moveBack = true;    // too close: kite out (ranged/magic)
    else if (t.dist > hi) out.moveFwd  = true;    // too far: close in

    // Fire when in band with LOS (melee: also require facing, which the aim above provides).
    out.fire = inBand && t.hasLOS && !v.stunned && !v.rolling;

    // Defense posture from the row. Melee columns have engageMin=0 (no kite floor), so lo is ~0 and
    // a lo-based dodge threshold would be `dist < 0` — dead. Fall back to the weapon's (short) reach
    // there so the "never get touched" hit-and-run posture actually fires for Glass Cannon Melee.
    const f32 dodgeRef = (lo > 0.1f) ? lo : (d.engageMax * v.weaponRange);
    if (d.dodgesProactively && v.dodgeCooldown <= 0.0f && t.dist < dodgeRef * 0.6f && !v.stunned)
        out.dodge = true;                         // glass cannon rolls away from a closer
    if (d.blocks && !out.fire && t.dist <= v.weaponRange && !v.stunned)
        out.block = true;                          // tank blocks between swings

    return out;
}

} // namespace Autoplay
