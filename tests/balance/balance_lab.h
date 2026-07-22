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

} // namespace BalanceLab
