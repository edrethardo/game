// src/game/weapon_dps.h — THE sustained-DPS cycle formula, single-sourced.
//
// Reduces a weapon's numbers to damage-per-second the way the game actually plays out:
// a clip weapon pays its reload every magazine (shots*cd + reload per cycle), everything
// else is simply perHit / cooldown. BuildScore::score and the balance lab both call this;
// it was extracted from build_score.h because a hand-mirrored copy of this exact math
// drifted once already (the 2026-07-22 loot-scoring fixes).
//
// Caller contract: effCooldown > 0 (the engine floors it at 0.05 in buildWeaponDef; the
// scorer's effective cooldown is strictly positive by construction). Reload flooring
// (0.2 s when the base
// reload is nonzero) also stays with the callers — the engine applies it in
// buildWeaponDef, the scorer mirrors it pre-call — because it needs the BASE reload to
// decide, which this pure cycle formula deliberately doesn't see.
#pragma once
#include "core/types.h"

namespace WeaponDps {

// shots < 1 means "no magazine" (melee / energy weapons): plain perHit / cooldown.
inline f32 sustained(f32 perHit, f32 effCooldown, f32 shots, f32 reloadSeconds) {
    if (shots < 1.0f) return perHit / effCooldown;
    return shots * perHit / (shots * effCooldown + reloadSeconds);
}

// Expected-value damage multiplier from crits: 1 + chance*(mult-1). The scorer ignores
// crit (near-constant across weapons); the lab includes it because daggers' 20%/2.5x is
// a real ~+24% sustained output over the 5%/2.0x baseline (1.30 vs 1.05).
inline f32 expectedCritMult(f32 critChance, f32 critMult) {
    return 1.0f + critChance * (critMult - 1.0f);
}

} // namespace WeaponDps
