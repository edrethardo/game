#pragma once
// Hit-feedback tiering: classifies every hit into one impact tier and maps each
// tier to a feedback "recipe" (shake, knockback, particle counts). Pure data +
// one pure classify function so it is trivially correct and tunable in one place.
// See docs/superpowers/specs/2026-05-23-hit-feedback-design.md.
#include "core/types.h"

enum struct ImpactTier : u8 { LIGHT = 0, HEAVY = 1, CRIT = 2, KILL = 3, COUNT = 4 };

struct HitFeedbackTier {
    f32 shakeIntensity;   // metres of camera shake (0 = none)
    f32 shakeDuration;    // seconds
    f32 knockback;        // base impulse (m/s) added to victim velocity (0 = none)
    u8  bloodCount;       // blood particles
    u8  sparkCount;       // spark particles
    u8  debrisCount;      // debris particles (kills)
    bool smoke;           // emit a smoke puff (kills)
    f32 hitStopMs;        // RESERVED for deferred freeze-frame; 0 in v1
};

// Tunable recipes, indexed by ImpactTier. Tune game feel here.
static constexpr HitFeedbackTier kHitTiers[static_cast<u32>(ImpactTier::COUNT)] = {
    /* LIGHT */ { 0.015f, 0.10f, 0.0f,  2, 1, 0, false, 0.0f },
    /* HEAVY */ { 0.05f,  0.18f, 4.0f,  5, 2, 0, false, 0.0f },
    /* CRIT  */ { 0.06f,  0.20f, 5.0f,  4, 6, 0, false, 0.0f },
    /* KILL  */ { 0.09f,  0.30f, 0.0f,  6, 3, 5, true,  0.0f },
};

// A hit counts as HEAVY when it removes at least this fraction of the victim's max HP.
static constexpr f32 HEAVY_HIT_HP_FRACTION = 0.20f;

// Pure classification. Precedence: KILL > CRIT > HEAVY > LIGHT.
constexpr ImpactTier classifyTier(f32 damage, f32 victimMaxHp, bool isCrit, bool isKill) {
    if (isKill) return ImpactTier::KILL;
    if (isCrit) return ImpactTier::CRIT;
    if (victimMaxHp > 0.0f && damage >= victimMaxHp * HEAVY_HIT_HP_FRACTION)
        return ImpactTier::HEAVY;
    return ImpactTier::LIGHT;
}

// Compile-time tests (this project has no runtime test framework).
static_assert(classifyTier(5.0f, 100.0f, false, false) == ImpactTier::LIGHT,  "small hit = LIGHT");
static_assert(classifyTier(25.0f, 100.0f, false, false) == ImpactTier::HEAVY, "25% of maxHP = HEAVY");
static_assert(classifyTier(5.0f, 100.0f, true,  false) == ImpactTier::CRIT,   "crit overrides LIGHT");
static_assert(classifyTier(50.0f, 100.0f, false, true) == ImpactTier::KILL,   "kill overrides all");
static_assert(classifyTier(50.0f, 100.0f, true,  true) == ImpactTier::KILL,   "kill beats crit");
static_assert(classifyTier(1.0f, 0.0f,   false, false) == ImpactTier::LIGHT,  "zero maxHP = LIGHT, no div");
