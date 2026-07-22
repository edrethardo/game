// tests/balance/balance_lab.cpp — see balance_lab.h. Enemy/boss curves in this task;
// typical-gear Monte Carlo and player power land in later tasks.
#include "balance/balance_lab.h"
#include "game/game_constants.h"
#include <algorithm>

namespace BalanceLab {

static f32 medianOf(f32* v, u32 n) {
    if (n == 0) return 0.0f;
    std::sort(v, v + n);
    return (n % 2) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

EnemyCurve enemyTrashAt(const EnemyDefTable& table, u8 rawFloor, u8 difficulty) {
    EnemyCurve c;
    const EnemyDef* defs[MAX_ENEMY_DEFS];
    const u32 n = collectTierDefs(table, enemyTierForFloor(rawFloor), defs, MAX_ENEMY_DEFS);
    if (n == 0) return c;

    // The exact spawn-time scaling path (engine_spawn.cpp): HP compounds via floorHealthMult,
    // damage is linear floorDamageMult x the per-tier difficulty bump.
    const u32 eff   = effectiveFloor(rawFloor, difficulty);
    const f32 hpMul = GameConst::floorHealthMult(eff);
    const f32 dmMul = GameConst::floorDamageMult(eff) * GameConst::difficultyDamageBump(difficulty);

    f32 hp[MAX_ENEMY_DEFS], hit[MAX_ENEMY_DEFS], dps[MAX_ENEMY_DEFS];
    for (u32 i = 0; i < n; i++) {
        hp[i]  = defs[i]->health * hpMul;
        hit[i] = defs[i]->damage * dmMul;
        dps[i] = (defs[i]->attackCooldown > 0.0f) ? hit[i] / defs[i]->attackCooldown : hit[i];
    }
    c.hpMedian  = medianOf(hp, n);
    c.hpMin     = hp[0];              // medianOf sorted the array in place
    c.hpMax     = hp[n - 1];
    c.hitMedian = medianOf(hit, n);
    c.dpsMedian = medianOf(dps, n);
    return c;
}

BossCurve bossAt(const BossDefTable& table, u8 rawFloor, u8 difficulty) {
    BossCurve c;
    const BossDef* bd = findBossDefByFloor(table, rawFloor);
    if (!bd) return c;
    const u32 eff = effectiveFloor(rawFloor, difficulty);
    c.present = true;
    c.name    = bd->name;
    c.hp      = bd->baseHp  * GameConst::floorHealthMult(eff);
    c.hit     = bd->baseDmg * GameConst::floorDamageMult(eff)
              * GameConst::difficultyDamageBump(difficulty);
    c.dps     = (bd->atkCooldown > 0.0f) ? c.hit / bd->atkCooldown : c.hit;
    return c;
}

} // namespace BalanceLab
