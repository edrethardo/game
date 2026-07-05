// hud_cooldown_util.h — pure, header-only helpers for HUD cooldown/urgency visuals.
// Extracted so the seconds formatting, low-HP urgency predicate, and "ready pop"
// timing math can be unit-tested without a GL context. Consumed by the skill bars
// (hud_skill_bar.cpp), the potion flask (hud.cpp / engine_hud.cpp), and drawReadyPop.
#pragma once

#include "core/types.h"
#include <cmath>

namespace HudCooldown {

// Seconds the green "ability is back" pop lasts — shared by skills AND the potion so
// every "it's back" reads identically.
inline constexpr f32 POP_DURATION = 0.4f;
// How far (px at the 720p baseline) the pop ring expands past the slot half-size.
inline constexpr f32 POP_GROW = 7.0f;

// Whole-seconds countdown shown centered on a cooling slot. Ceil so a 4.1s timer
// reads 5..1 and only disappears when truly ready. Minimum 1 while still visible.
inline s32 cooldownSeconds(f32 remaining) {
    s32 s = static_cast<s32>(std::ceil(remaining));
    return s < 1 ? 1 : s;
}

// Suppress the number in the last sliver so the pop — not a flickering "1" — carries
// the final fraction of a second.
inline bool showCooldownNumber(f32 remaining) {
    return remaining > 0.2f;
}

// "Drink NOW": the heal is ready AND HP is at/below the low-HP threshold. The threshold
// is a parameter so this header carries no game-layer dependency (the caller passes
// GameConst::LOW_HP_FRACTION).
inline bool potionUrgent(f32 healthFrac, f32 cooldownRemaining, f32 lowHpFrac) {
    return cooldownRemaining <= 0.0f && healthFrac <= lowHpFrac;
}

// Pop progress 1..0 from a flash timer counting POP_DURATION -> 0. Clamped.
inline f32 readyPopT(f32 flashTimer) {
    f32 t = flashTimer / POP_DURATION;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

// Ring radius: starts at the slot half-size (t=1, just fired) and grows outward as it
// fades (t->0). `scale` converts the baseline POP_GROW to the caller's uiScale.
inline f32 readyPopRadius(f32 t01, f32 baseHalf, f32 scale) {
    return baseHalf + (1.0f - t01) * POP_GROW * scale;
}

// Ring opacity proxy 1..0. The HUD line batch has no alpha, so callers lerp the ring
// color toward black by this factor.
inline f32 readyPopAlpha(f32 t01) {
    return t01;
}

} // namespace HudCooldown
