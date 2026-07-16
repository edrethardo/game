// lead_assist.h — pure intercept math for the throwing-knife aim assist.
//
// Why knives get an assist and nothing else does: they are the fastest (25-35 m/s), thinnest
// (5-8 cm) projectile class, thrown at targets the Phase-2 AI keeps in constant lateral motion
// (encircle/strafe/kite). At 8 m a 3 m/s strafer displaces ~0.8 m during the knife's flight —
// more than its own body width — so an on-crosshair throw physically cannot land without
// leading, and measured hit rates against strafers were ~0-7%. The assist bends the throw
// toward the intercept point of the target's CURRENT velocity, but only when the crosshair is
// already near the target (tight acquisition cone) and only by a small angle (correction cap),
// so it reads as the knife rewarding good aim, not as auto-aim. Per design the rest of the
// ranged arsenal is untouched.
//
// Header-only and engine-free so the quadratic + clamp are unit-testable
// (tests/game/test_lead_assist.cpp) without a GL/engine context.
#pragma once

#include "core/types.h"
#include "core/math.h"

namespace LeadAssist {

inline constexpr f32 MAX_CORRECT_RAD = 0.2094395f;  // 12° — max the assist may bend the throw
inline constexpr f32 ACQUIRE_COS     = 0.9925462f;  // cos 7° — crosshair must already be this close
inline constexpr f32 ACQUIRE_RANGE   = 30.0f;       // beyond this, no assist (also the knife's reach)
inline constexpr f32 MAX_LEAD_SEC    = 1.5f;        // don't lead a shot the knife can't make in time

// Earliest positive time t at which a projectile of `speed` launched from the origin meets a
// target at relative position `rel` moving with constant velocity `vel`:
//   |rel + vel*t| = speed*t  →  (vel·vel − speed²)t² + 2(rel·vel)t + rel·rel = 0.
// Returns false when the target outruns the projectile (no positive root) or the intercept
// takes longer than MAX_LEAD_SEC.
inline bool interceptTime(Vec3 rel, Vec3 vel, f32 speed, f32& tOut) {
    const f32 a = dot(vel, vel) - speed * speed;
    const f32 b = 2.0f * dot(rel, vel);
    const f32 c = dot(rel, rel);
    f32 t;
    if (fabsf(a) < 1e-4f) {
        // Target speed ≈ projectile speed: the quadratic degenerates to linear b*t + c = 0.
        if (fabsf(b) < 1e-6f) return false;
        t = -c / b;
    } else {
        const f32 disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) return false;             // target strictly outruns the projectile
        const f32 sq = sqrtf(disc);
        const f32 t1 = (-b - sq) / (2.0f * a);
        const f32 t2 = (-b + sq) / (2.0f * a);
        // Earliest POSITIVE root — with a < 0 (normal case: knife faster than target) exactly
        // one root is positive; the branchless min-positive pick covers both sign cases.
        t = 1e9f;
        if (t1 > 0.0f && t1 < t) t = t1;
        if (t2 > 0.0f && t2 < t) t = t2;
    }
    if (t <= 0.0f || t > MAX_LEAD_SEC) return false;
    tOut = t;
    return true;
}

// Bend unit vector `aim` toward unit vector `ideal`, by at most `maxRad`. Because the rotation
// axis is perpendicular to `aim`, Rodrigues' formula loses its parallel term and reduces to
// cos/sin of the cap angle.
inline Vec3 clampToward(Vec3 aim, Vec3 ideal, f32 maxRad) {
    if (dot(aim, ideal) >= cosf(maxRad)) return ideal;   // within the cap — take the ideal aim
    Vec3 axis = cross(aim, ideal);
    const f32 axLen = length(axis);
    if (axLen < 1e-5f) return aim;   // parallel/anti-parallel: no well-defined rotation plane
    axis = axis * (1.0f / axLen);
    return normalize(aim * cosf(maxRad) + cross(axis, aim) * sinf(maxRad));
}

} // namespace LeadAssist
