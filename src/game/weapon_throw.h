#pragma once
// weapon_throw.h — Melee "weapon throw" tunables + pure decision logic.
//
// A short TAP of Fire while a melee weapon is equipped hurls it as a projectile: it deals the
// weapon's damage on hit, is on a FIXED 1.5 s cooldown that CDR never touches, and applies a short
// slow (rides the enemy freezeTimer) whose duration is reduced by the target's tenacity (CC-resist).
// HOLDING Fire past a short threshold does the normal melee swing instead.
//
// Engine-free (mirrors crowd_control.h / arena.h / stash.h) so the numbers and the tap-vs-hold
// arbitration unit-test on plain values. The engine (handleWeaponFire / processRemoteActivation)
// supplies live state — cooldown readiness, the equipped weapon, the projectile spawn.
#include "core/types.h"
#include "game/crowd_control.h"

namespace WeaponThrow {

// --- Tunables (one source of truth; the engine reads these) -------------------------------------
constexpr f32 COOLDOWN_SEC = 1.5f;   // fixed; CDR never applies (see cooldownTicks)
constexpr f32 SLOW_SEC     = 0.2f;   // base slow on hit, before tenacity
constexpr f32 TAP_SEC      = 0.2f;   // a press shorter than this (measured on release) throws
constexpr f32 SPEED        = 22.0f;  // projectile speed (m/s)
constexpr f32 RADIUS       = 0.25f;  // fat enough that a thrown blade connects
constexpr f32 LIFETIME     = 1.0f;   // ~22 m reach — a short/mid poke, deliberately under a bow

// CDR-immune by CONSTRUCTION: there is no (1 - cdr) term here, unlike SkillSystem::computeCooldown
// Ticks. Kept in TICKS for the server's tick-watermark gate (GameConst::cooldownReady). = 90.
inline u32 cooldownTicks() { return static_cast<u32>(COOLDOWN_SEC * 60.0f + 0.5f); }

// "affected by tenacity": reduce the slow by the target's capped CC-resistance. With resist 0
// (every enemy today) this returns SLOW_SEC unchanged — the hook is live but currently a no-op.
inline f32 slowDuration(f32 enemyCcResist) {
    return CrowdControl::scaleDuration(SLOW_SEC, CrowdControl::capResist(enemyCcResist));
}

// --- Autoplay bot policy ------------------------------------------------------------------------
// The bot drives the SAME tap/hold path a human does (it has to — the throw is decided in
// handleWeaponFire off the raw Fire button), so throwing means emitting a short synthetic TAP.
// These say WHEN that is worth doing; the driver owns the tap sequencer and the "occasional" leash.
constexpr f32 BOT_THROW_MAX_M   = 18.0f;  // don't hurl the weapon at something across the map
constexpr f32 BOT_THROW_MIN_MUL = 1.2f;   // × melee reach: inside this just swing, it's free
constexpr f32 BOT_THROW_LEASH   = 6.0f;   // s between bot throws — "occasional", not a machine gun

// Should the melee bot hurl its weapon at this target right now? Worth it against a RANGED enemy
// standing beyond swing reach: the throw crosses the gap the bot would otherwise have to walk under
// fire. Melee attackers are excluded on purpose — they close the distance for you, so the weapon is
// better kept in hand. Pure so the policy is unit-testable without an engine.
inline bool botShouldThrow(bool weaponIsMelee, bool throwReady, bool targetIsRanged,
                           bool targetHasLOS, f32 targetDist, f32 meleeRange) {
    if (!weaponIsMelee || !throwReady || !targetIsRanged || !targetHasLOS) return false;
    if (targetDist <= meleeRange * BOT_THROW_MIN_MUL) return false;   // in swing range: just hit it
    return targetDist <= BOT_THROW_MAX_M;
}

// The synthetic TAP the bot must emit for `decide` to read a throw, as a phase of a small timer.
// A bot that is auto-firing has held Fire far past TAP_SEC, so the sequence must RELEASE first to
// zero the hold accumulator, then press briefly, then release again (the release is what throws).
// Returns true while Fire should be HELD, false while it must be RELEASED. `done` at the end.
constexpr f32 BOT_TAP_CLEAR = 0.06f;   // release: resets `held` in decide()
constexpr f32 BOT_TAP_PRESS = 0.16f;   // press window ends here (0.10 s held < TAP_SEC)
constexpr f32 BOT_TAP_END   = 0.24f;   // release long enough for the throw to fire, then done
inline bool botTapFireHeld(f32 t) { return t >= BOT_TAP_CLEAR && t < BOT_TAP_PRESS; }
inline bool botTapDone(f32 t)     { return t >= BOT_TAP_END; }

// Result of one tick of tap/hold arbitration for a MELEE weapon.
struct Decision { bool doThrow; bool doSwing; f32 newHeld; };

// Tap-vs-hold arbiter, evaluated once per tick for a melee weapon.
//   fireDown/wasDown : Fire held this tick / last tick.   held : accumulated hold seconds so far.
//   throwReady       : a throwable melee weapon is equipped AND the throw is off its 1.5 s cooldown.
// Rules (chosen so melee stays responsive):
//   * throw NOT ready              -> swing immediately (fully responsive between throws).
//   * throw ready, held >= TAP_SEC -> committed to a hold -> swing; no throw fires on release.
//   * throw ready, still down, held < TAP_SEC -> WAIT (this press could still become a tap-throw).
//   * release after a short press while ready -> THROW.
inline Decision decide(bool fireDown, bool wasDown, f32 held, f32 dt, bool throwReady) {
    Decision d{false, false, held};
    if (fireDown) {
        d.newHeld = held + dt;
        if (!throwReady)                d.doSwing = true;   // can't be a throw -> behave like today
        else if (d.newHeld >= TAP_SEC)  d.doSwing = true;   // committed to a hold -> melee
        // else: throw ready and press still short -> neither yet (disambiguating tap vs hold).
    } else {
        if (wasDown && throwReady && held > 0.0f && held < TAP_SEC) d.doThrow = true;
        d.newHeld = 0.0f;   // reset the hold accumulator on release
    }
    return d;
}

} // namespace WeaponThrow
