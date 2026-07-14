#pragma once

#include "core/types.h"
#include "core/math.h"

// champion.h — Diablo-style CHAMPION monsters: an elite pack leader carrying rolled, BEHAVIOURAL
// affixes, escorted by buffed minions of the same type.
//
// Why this exists: the enemy pool is tier-gated to 6-9 types per 10-floor band, so the player fights
// the same handful of monsters for ten floors. Champions multiply that small pool into fights the
// player has to READ — a Molten pack must be kited off its burning trail, a Shielding one punishes
// burst-DPS, a Health-Linked one has to be cleared minions-first. The affixes are deliberately
// behavioural, never "+20% HP": a champion that is only bigger is a chore, not an encounter.
//
// Affixes are LEADER-ONLY (Diablo 2 style). Minions are same-type copies with a stat bump and no
// affixes of their own; HEALTH_LINK is the one affix whose effect still reaches them.
//
// Everything in this header is a PURE function of (floor, rng) — no engine, no pools, no globals —
// so the roll rules are unit-testable in isolation (tests/game/test_champion.cpp). The behaviours
// themselves hang off existing hooks (Combat::applyDamage, the death callback, EnemyAI::update);
// see the table in the plan.

namespace ChampAffix {
    constexpr u8 NONE        = 0x00;
    constexpr u8 MOLTEN      = 0x01;  // burning ground trail while moving; fire nova on death
    constexpr u8 FROZEN      = 0x02;  // chilling nova on death (sets Entity.freezeTimer)
    constexpr u8 VAMPIRIC    = 0x04;  // heals itself for a share of the damage it deals
    constexpr u8 EXTRA_FAST  = 0x08;  // +50% move speed
    constexpr u8 SHIELDING   = 0x10;  // periodic brief immunity — punishes pure burst damage
    constexpr u8 TELEPORTING = 0x20;  // blinks to close the gap — punishes pure kiting
    constexpr u8 THUNDERING  = 0x40;  // periodic lightning nova — punishes standing in melee
    constexpr u8 HEALTH_LINK = 0x80;  // damage is split with its living minions — clear the pack first
    constexpr u8 COUNT       = 8;     // number of bits, not a mask
}

namespace Champion {

// Tunables. Kept here (not scattered at the use sites) so balance is one file to open.
constexpr f32 HEALTH_MULT        = 4.0f;   // leader HP vs a normal enemy of its type
constexpr f32 DAMAGE_MULT        = 1.5f;   // leader damage
constexpr f32 SCALE_MULT         = 1.25f;  // leader halfExtents — also the hitbox, and replicated
constexpr f32 MINION_HEALTH_MULT = 1.6f;   // minions: buffed, but not affixed
constexpr f32 MINION_DAMAGE_MULT = 1.2f;

constexpr f32 EXTRA_FAST_MULT    = 1.5f;
constexpr f32 VAMPIRIC_HEAL_PCT  = 0.30f;  // of damage dealt
constexpr f32 SHIELDING_UP_SEC   = 1.5f;   // immune window
constexpr f32 SHIELDING_GAP_SEC  = 5.0f;   // vulnerable window between shields
constexpr f32 HEALTH_LINK_SHARE  = 0.50f;  // share of damage redirected onto living minions

// THUNDERING — a lightning nova on a cycle. It has to hurt the PLAYER: an affix that chained damage
// into the champion's own pack would help you, not threaten you.
constexpr f32 THUNDER_PERIOD_SEC = 3.0f;
constexpr f32 THUNDER_RADIUS     = 4.5f;
constexpr f32 THUNDER_DMG_PCT    = 0.60f;  // of the champion's own attack damage

// MOLTEN — erupts fire at its own feet on a short cycle while alive, and a big fire nova when it
// dies. Being next to a Molten champion is a losing position, which is what makes the pack a
// positioning problem rather than a damage race.
// (Deliberately NOT a persistent ground-fire trail: the engine's ScorchZone pool damages hostile
// ENTITIES — it's a player weapon effect — and is not replicated, so a trail built on it would be
// invisible to co-op guests and hurt the wrong things. The eruption uses emitNovaFX, which
// broadcasts, so every guest sees exactly what is hurting them.)
constexpr f32 MOLTEN_ERUPT_SEC   = 1.2f;   // eruption cadence
constexpr f32 MOLTEN_ERUPT_RAD   = 2.4f;
constexpr f32 MOLTEN_ERUPT_PCT   = 0.45f;  // of the champion's damage, per eruption
constexpr f32 MOLTEN_NOVA_RADIUS = 4.0f;
constexpr f32 MOLTEN_NOVA_PCT    = 1.20f;  // death nova, as a share of the champion's damage

// FROZEN — a chilling nova on death. No damage: the threat is that it freezes you in place while
// the rest of the pack closes in.
constexpr f32 FROZEN_NOVA_RADIUS = 4.5f;
constexpr f32 FROZEN_FREEZE_SEC  = 1.6f;

// TELEPORTING — blinks toward the player when it has been kept at range.
constexpr f32 TELEPORT_PERIOD_SEC = 5.0f;
constexpr f32 TELEPORT_MIN_DIST   = 7.0f;  // only blinks if you have actually opened a gap
constexpr f32 TELEPORT_LAND_DIST  = 2.5f;  // how close it lands to you

constexpr u8  MIN_FLOOR          = 3;      // no champions in the first two floors
constexpr u8  MIN_MINIONS        = 2;
constexpr u8  MAX_MINIONS        = 4;
constexpr u8  MAX_PACKS_PER_FLOOR = 2;     // hard cap — the entity pool is only 128
// Per eligible enemy, until MAX_PACKS_PER_FLOOR is hit. A floor holds ~25 enemies early and ~43
// later, so this compounds fast — at 6% a pack appeared on 79% of early floors and 93% of deep ones,
// which is often enough that "champion" stops meaning anything and hands out a guaranteed rare
// nearly every floor. At 3% it is ~54% early / ~73% deep: a champion is a thing that HAPPENS to a
// floor, not furniture. Tune here, not at the call site.
constexpr f32 SPAWN_CHANCE       = 0.03f;
// Never let champion bodies crowd out the floor's normal spawns. A pack needs this much free room
// in the 128-entity pool before it may spawn at all.
constexpr u32 ENTITY_HEADROOM    = 12;

// How many affixes a champion rolls at this depth. effectiveFloor = currentFloor + difficulty*50.
u8 affixCountForFloor(u32 effectiveFloor);

// True if `mask` is a legal combination. Two rules:
//   - EXTRA_FAST + TELEPORTING is unfightable (it closes every gap AND outruns you).
//   - HEALTH_LINK is meaningless without minions, so it is only legal on a pack leader.
bool affixesValid(u8 mask, bool hasMinions);

// Roll a legal affix mask. `rngState` is an in/out LCG state so the caller controls the stream and
// the test can pin an exact sequence. Never returns an invalid mask.
u8 rollAffixes(u32 effectiveFloor, bool hasMinions, u32& rngState);

// The champion's tell colour, derived from the affix mask. Deliberately a pure function of the mask
// so the CLIENT can compute it from the replicated byte alone — the tint must never depend on
// host-only state. (Entity.hasAuraBuff is a cautionary tale: it drives a tint but is not replicated,
// so that tell is invisible to every co-op guest.)
Vec3 tintFor(u8 mask);

// Human-readable affix name, for the kill feed / debug overlay. Never null.
const char* affixName(u8 singleBit);

} // namespace Champion
