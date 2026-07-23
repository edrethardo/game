// tests/balance/balance_lab.cpp — see balance_lab.h. Enemy/boss curves plus the typical-gear
// Monte Carlo (real ItemGen drops selected with the real Auto-Loot scorer); player power
// lands in a later task.
#include "balance/balance_lab.h"
#include "game/game_constants.h"
#include "game/build_score.h"
#include "game/weapon_dps.h"
#include "game/combat.h"
#include <algorithm>
#include <cstring>

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

// FNV-1a over the trial coordinates: stable, order-independent seeding. Within one
// (floor,difficulty) cell the final (h ^ trial) * prime step is a bijection over trial, so
// distinct trials are guaranteed distinct streams; ACROSS cells FNV can collide, which is
// harmless (different effective levels make the drops differ anyway).
static u32 trialSeed(u8 rawFloor, u8 difficulty, u32 trial) {
    u32 h = 2166136261u;
    const u32 parts[3] = {rawFloor, difficulty, trial};
    for (u32 p : parts) { h ^= p; h *= 16777619u; }
    return h ? h : 1u;   // LCG seed 0 is legal but keep it nonzero for hygiene
}

void rollWindowDrops(u8 rawFloor, u8 difficulty, u32 trial,
                     const ItemDef* defs, u32 defCount,
                     const AffixDef* affixDefs, u32 affixDefCount, DropSet& out) {
    // Zero the WHOLE output (padding bytes included): the determinism test memcmps entire
    // DropSets, so uninitialized padding must not leak previous-call garbage into the compare.
    std::memset(&out, 0, sizeof out);
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

PlayerPower powerOf(const PlayerInventory& inv, u8 cell, u8 rawFloor, u8 difficulty,
                    const ItemDef* itemDefs,
                    const SkillDef* skillDefs, u32 skillDefCount) {
    PlayerPower p;
    const u8 col = BuildScore::buildCol(cell);
    const ClassDef& cls = kClassDefs[static_cast<u32>(columnClass(col))];

    // Weapon DPS: the real effective weapon (attack speed, CDR, clip already applied by
    // buildWeaponDef), the shared cycle formula, expected-value crit.
    const WeaponDef unarmed{};   // fallback only; loadouts always fill the weapon slot
    const WeaponDef w = Inventory::getEffectiveWeapon(inv, itemDefs, unarmed);
    // An empty weapon slot returns `unarmed` UNCHANGED (the 0.05 cooldown floor lives in
    // buildWeaponDef, which the empty path skips), so cooldown 0 would make sustained()
    // divide 0/0 — guard so no weapon reads as zero output, not as NaN poisoning
    // totalDps/sustain and every percentile sort downstream.
    if (w.cooldown > 0.0f)
        p.weaponDps = WeaponDps::sustained(w.damage, w.cooldown,
                                           static_cast<f32>(w.clipSize), w.reloadTime)
                    * WeaponDps::expectedCritMult(w.critChance, w.critMult);

    // Cast DPS: the class's unlocked damage skills. Formula per spec:
    // (damage + spellFlat) * (1 + spellPct/100) / (cooldown * (1 - CDR)).
    const f32 spellFlat = Inventory::spellDamageFlat(inv);
    const f32 spellPct  = Inventory::spellDamagePct(inv);
    const f32 cdr       = inv.bonusCooldownReduction;      // recalculateStats caps at 0.5
    for (u32 s = 0; s < 4; s++) {
        if (cls.skills[s] == SkillId::NONE) continue;
        // The engine unlocks on EFFECTIVE floor (handleClassSkillActivation), so Nightmare/
        // Hell have every skill live from raw floor 1 — a raw-floor gate understated them.
        if (cls.skillUnlockFloor[s] > effectiveFloor(rawFloor, difficulty)) continue;
        const SkillDef* sd = nullptr;
        for (u32 i = 0; i < skillDefCount; i++)
            if (skillDefs[i].id == cls.skills[s]) { sd = &skillDefs[i]; break; }
        if (!sd || sd->damage <= 0.0f || sd->cooldown <= 0.0f) continue;
        p.castDps += (sd->damage + spellFlat) * (1.0f + spellPct * 0.01f)
                   / (sd->cooldown * (1.0f - cdr));
    }
    p.totalDps = p.weaponDps + p.castDps;

    // EHP: real max health divided through the real armor curve — same units as enemy damage.
    const f32 maxHp = Inventory::getEffectiveMaxHealth(inv, cls.baseHealth);
    p.ehp = maxHp / (1.0f - Combat::armorMitigation(Inventory::armorRating(inv)));

    // Sustain (HP/s), reported beside EHP rather than folded in (spec: folding needs a
    // fight-length assumption). Life-on-hit converts via the weapon's real swing rate.
    const f32 hitRate = (w.cooldown > 0.0f) ? 1.0f / w.cooldown : 0.0f;
    p.sustain = Inventory::healthRegenRate(inv)
              + inv.bonusLifeOnHit * hitRate
              + Inventory::lifestealPct(inv) * 0.01f * p.weaponDps;
    return p;
}

} // namespace BalanceLab
