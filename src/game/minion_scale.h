#pragma once
// minion_scale.h — what a summoned minion hits for, given the summoner's weapon.
//
// WHY THIS EXISTS. A Tinkerer's drones and a Combat Engineer's turret were authored as FLAT numbers
// (6, 7, 8, 3 damage) scaled only by a floor/difficulty ramp. Nothing about them read the player's
// gear. So the two classes whose damage is meant to come from their minions were the only two whose
// damage did not improve when they found a better weapon — a Tinkerer in full mythics summoned the
// same drones as a Tinkerer in starting kit.
//
// That is measurable, and it is the shape of the class-power gap the soaks keep finding: across a
// 3 h all-class dungeon soak the two SUMMON classes were the two weakest, Tinkerer last at 367 wdps
// against a Wanderer's 38,371 — a hundredfold spread. In the acts it shows up as a gating SLAY quest
// that takes fifteen minutes instead of two.
//
// THE RULE. A minion hits for a SHARE of its summoner's weapon damage, and never less than the
// number it was authored with:
//
//     damage = max(authoredBase * floorMult, weaponDamage * share)
//
// Two properties matter and both are deliberate:
//
//   * The authored value is a FLOOR, not a base to multiply. Early game the share is smaller than
//     the authored number, so nothing about levels 1-10 changes at all — this cannot regress the
//     tuning that already exists, it can only lift the top end where the gap actually is.
//   * The share is expressed against the weapon's PER-HIT damage rather than against DPS, because a
//     minion has its own attack cooldown. Scaling by the summoner's DPS would double-count the
//     attack-speed rolls the minion does not have.
//
// Pure and engine-free so the rule is unit-tested rather than buried in a spawn callback where four
// minion types would each have their own copy of it.

#include "core/types.h"

namespace MinionScale {

// Per-minion share of the summoner's per-hit weapon damage.
//
// Sized against how many of each a build fields at once and how exposed they are, not by feel: the
// turret is one stationary unit and the swarm is many disposable ones, so the same share would make
// the swarm strictly better at every gear level.
constexpr f32 SHARE_SPIDER_DRONE = 0.25f;   // Tinkerer's Combat Drone — one durable melee unit
constexpr f32 SHARE_SWARM_DRONE  = 0.16f;   // Swarm/Deploy bats — cheap, numerous, short-lived
constexpr f32 SHARE_SWARM_QUEEN  = 0.35f;   // the 20 s elite that spawns more of the above
constexpr f32 SHARE_TURRET       = 0.30f;   // Combat Engineer's turret — one unit, ranged, immobile

// The damage a minion should deal.
//
// `authoredBase` and `floorMult` are exactly what the spawn site already computed, so a call site
// that has not been converted still behaves identically — which is what makes this safe to adopt one
// minion at a time.
inline f32 damage(f32 authoredBase, f32 floorMult, f32 weaponDamage, f32 share) {
    const f32 floorValue = authoredBase * floorMult;
    const f32 scaled     = weaponDamage * share;
    return scaled > floorValue ? scaled : floorValue;
}

} // namespace MinionScale
