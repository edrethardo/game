// tests/balance/balance_lab.cpp — see balance_lab.h. Enemy/boss curves plus the typical-gear
// Monte Carlo (real ItemGen drops selected with the real Auto-Loot scorer); player power
// lands in a later task.
#include "balance/balance_lab.h"
#include "game/game_constants.h"
#include "game/build_score.h"
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
    // Min/max by scan BEFORE any medianOf call — medianOf sorts in place, and leaning on
    // that side-effect made correctness depend on statement order (a reorder trap).
    c.hpMin = c.hpMax = hp[0];
    for (u32 i = 1; i < n; i++) {
        if (hp[i] < c.hpMin) c.hpMin = hp[i];
        if (hp[i] > c.hpMax) c.hpMax = hp[i];
    }
    c.hpMedian  = medianOf(hp, n);
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

// FNV-1a over the trial coordinates: stable, order-independent seeding. ItemGen's LCG maps
// seed->stream 1:1, so distinct trial coords give distinct (if correlated-looking) streams.
static u32 trialSeed(u8 rawFloor, u8 difficulty, u32 trial) {
    u32 h = 2166136261u;
    const u32 parts[3] = {rawFloor, difficulty, trial};
    for (u32 p : parts) { h ^= p; h *= 16777619u; }
    return h ? h : 1u;   // LCG seed 0 is legal but keep it nonzero for hygiene
}

void rollWindowDrops(u8 rawFloor, u8 difficulty, u32 trial,
                     const ItemDef* defs, u32 defCount,
                     const AffixDef* affixDefs, u32 affixDefCount, DropSet& out) {
    out.count = 0;
    ItemGen::init(trialSeed(rawFloor, difficulty, trial));
    const u8 first = (rawFloor > WINDOW_FLOORS - 1)
                   ? static_cast<u8>(rawFloor - (WINDOW_FLOORS - 1)) : 1;
    for (u8 f = first; f <= rawFloor; f++) {
        const u8 lvl = static_cast<u8>(effectiveFloor(f, difficulty));   // max 150, fits u8
        for (u32 k = 0; k < DROPS_PER_FLOOR && out.count < MAX_WINDOW_DROPS; k++)
            out.items[out.count++] = ItemGen::rollItem(lvl, defs, defCount,
                                                       affixDefs, affixDefCount);
    }
}

void selectLoadout(const DropSet& drops, u8 cell,
                   const ItemDef* defs, u32 defCount, PlayerInventory& outInv) {
    outInv = PlayerInventory{};
    for (u32 sl = 0; sl < static_cast<u32>(ItemSlot::COUNT); sl++) {
        s32 bestIdx = -1; f32 bestScore = 0.0f;
        for (u32 i = 0; i < drops.count; i++) {
            const ItemInstance& it = drops.items[i];
            if (it.defId == 0xFFFF || it.defId >= defCount) continue;
            if (static_cast<u32>(defs[it.defId].slot) != sl) continue;
            const f32 s = BuildScore::score(it, defs[it.defId], cell);
            if (s > bestScore) { bestScore = s; bestIdx = static_cast<s32>(i); }
        }
        if (bestIdx < 0) continue;                      // window dropped nothing for this slot
        const s8 bp = Inventory::addToBackpack(outInv, drops.items[bestIdx]);
        if (bp >= 0) Inventory::equip(outInv, static_cast<u8>(bp), defs);
    }
}

} // namespace BalanceLab
