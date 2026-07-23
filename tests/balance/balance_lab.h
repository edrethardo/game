// tests/balance/balance_lab.h — the balance lab: typical-equipment player power vs enemy
// power per floor (spec: docs/superpowers/specs/2026-07-22-balance-lab-design.md).
//
// TESTS-ONLY code: linked into dungeon_tests, never into the game binary. Everything here
// CALLS real engine code (ItemGen / BuildScore / Inventory / GameConst / the JSON defs) —
// the lab holds model structure (windows, trials, percentiles), never game math.
#pragma once
#include "core/types.h"
#include "game/item.h"
#include "game/enemy_def.h"
#include "game/boss_def.h"

namespace BalanceLab {

// --- model parameters: assumptions about a TYPICAL PLAYER, not engine truth ------------------
// A player at floor F wears the best of the drops they saw over the last few floors.
static constexpr u32 DROPS_PER_FLOOR  = 12;   // ~25-35 kills x LOOT_DROP_CHANCE (40%+1%/lvl)
static constexpr u32 WINDOW_FLOORS    = 4;    // gear comes from floors F-3..F
static constexpr u32 TRIALS           = 200;  // Monte-Carlo trials per (difficulty, floor)
static constexpr u32 MAX_WINDOW_DROPS = DROPS_PER_FLOOR * WINDOW_FLOORS;

inline u32 effectiveFloor(u8 rawFloor, u8 difficulty) { return rawFloor + difficulty * 50u; }

// --- enemy side ------------------------------------------------------------------------------
// Post-multiplier trash-roster stats at one (floor, difficulty). Medians are PER-METRIC
// (median HP and median damage may come from different defs) — the report reads each curve
// independently, so a single "median enemy" would just be a worse summary.
struct EnemyCurve {
    f32 hpMedian = 0, hpMin = 0, hpMax = 0;
    f32 hitMedian = 0;    // damage per landed hit
    f32 dpsMedian = 0;    // median over defs of scaledDamage / attackCooldown
};
EnemyCurve enemyTrashAt(const EnemyDefTable& table, u8 rawFloor, u8 difficulty);

struct BossCurve {
    bool present = false;
    const char* name = "";
    f32 hp = 0, hit = 0, dps = 0;
};
BossCurve bossAt(const BossDefTable& table, u8 rawFloor, u8 difficulty);

// --- typical-equipment Monte Carlo -----------------------------------------------------------
struct DropSet { ItemInstance items[MAX_WINDOW_DROPS]; u32 count = 0; };

// The drops a floor-F player saw: DROPS_PER_FLOOR real ItemGen rolls per window floor, each
// at that floor's own effective level. Reseeds ItemGen from (floor,difficulty,trial) so a
// trial is deterministic and independent of sweep order — and the SAME drops are then shown
// to all nine build cells (the same loot fell; each build just wears it differently).
void rollWindowDrops(u8 rawFloor, u8 difficulty, u32 trial,
                     const ItemDef* defs, u32 defCount,
                     const AffixDef* affixDefs, u32 affixDefCount,
                     DropSet& out);

// Best-of-window per slot under BuildScore for `cell`, equipped into a real PlayerInventory
// via Inventory::equip (so recalculateStats runs and the stat caches are engine-true).
void selectLoadout(const DropSet& drops, u8 cell,
                   const ItemDef* defs, u32 defCount, PlayerInventory& outInv);

// --- player power off a loadout --------------------------------------------------------------
struct PlayerPower { f32 weaponDps = 0, castDps = 0, totalDps = 0, ehp = 0, sustain = 0; };

// Representative class per damage COLUMN — a declared model assumption (spec): the class
// whose base HP and skill list stand in for everyone playing that archetype.
inline PlayerClass columnClass(u8 col) {
    switch (col) {
        case 0:  return PlayerClass::SORCERER;   // Magic
        case 2:  return PlayerClass::MARKSMAN;   // Ranged
        default: return PlayerClass::WARRIOR;    // Melee
    }
}

// All real engine functions: getEffectiveWeapon -> WeaponDps cycle x EV crit; class skills
// gated by unlock floor with gear spell rolls + real CDR; getEffectiveMaxHealth through
// armorMitigation for EHP; regen/life-on-hit/lifesteal as a separate sustain column.
// Deliberately NOT modeled (documented in the spec): skill energy costs, weapon range,
// enemy armor, player skill-aim. EFFECTIVE floor (raw + difficulty*50) gates skill unlocks,
// matching the engine's handleClassSkillActivation — on Nightmare/Hell everything is live
// from raw floor 1, which is exactly what the cross-difficulty comparison must reflect.
PlayerPower powerOf(const PlayerInventory& inv, u8 cell, u8 rawFloor, u8 difficulty,
                    const ItemDef* itemDefs,
                    const SkillDef* skillDefs, u32 skillDefCount);

} // namespace BalanceLab
