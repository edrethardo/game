// tests/balance/test_balance_lab.cpp — the balance lab: typical-gear player power vs
// enemy curves per floor (spec: docs/superpowers/specs/2026-07-22-balance-lab-design.md).
// This file holds the always-on sanity pins; the BALANCE_REPORT CSV dump case is added
// in a later task.
#include "doctest/doctest.h"
#include "balance/balance_lab.h"
#include "game/enemy_loader.h"
#include "game/boss_loader.h"
#include "game/game_constants.h"
#include "game/free_play.h"     // DIFFICULTY_COUNT — the real difficulty-tier bound
#include <algorithm>

// Shared fixture: the real shipped tables, loaded once (doctest runs cases in one process).
static const EnemyDefTable& enemyTable() {
    static EnemyDefTable t;
    static bool loaded = EnemyLoader::load(DUNGEON_REPO_ROOT "/assets/config/enemies.json", t);
    REQUIRE(loaded);
    return t;
}
static const BossDefTable& bossTable() {
    static BossDefTable t;
    static bool loaded = BossLoader::load(DUNGEON_REPO_ROOT "/assets/config/bosses.json", t);
    REQUIRE(loaded);
    return t;
}

TEST_CASE("enemyTierForFloor matches the spawn ladder at every band boundary") {
    CHECK(enemyTierForFloor(1)  == 1);
    CHECK(enemyTierForFloor(10) == 1);
    CHECK(enemyTierForFloor(11) == 2);
    CHECK(enemyTierForFloor(20) == 2);
    CHECK(enemyTierForFloor(21) == 3);
    CHECK(enemyTierForFloor(30) == 3);
    CHECK(enemyTierForFloor(31) == 4);
    CHECK(enemyTierForFloor(40) == 4);
    CHECK(enemyTierForFloor(41) == 5);
    CHECK(enemyTierForFloor(50) == 5);
}

TEST_CASE("enemy trash curve: multiplier path matches the spawn code exactly") {
    const EnemyDefTable& t = enemyTable();
    // Recompute one point by hand straight from the roster + GameConst — the lab function
    // must agree bit-for-bit (same functions, same order of operations).
    const u8 rawFloor = 25, difficulty = 2;                    // Hell 25 -> effective 125
    const u32 eff = 125;
    const EnemyDef* defs[MAX_ENEMY_DEFS];
    const u32 n = collectTierDefs(t, enemyTierForFloor(rawFloor), defs, MAX_ENEMY_DEFS);
    REQUIRE(n > 0);
    f32 hp[MAX_ENEMY_DEFS], hit[MAX_ENEMY_DEFS], dps[MAX_ENEMY_DEFS];
    const f32 dmMul = GameConst::floorDamageMult(eff) * GameConst::difficultyDamageBump(difficulty);
    for (u32 i = 0; i < n; i++) {
        hp[i]  = defs[i]->health * GameConst::floorHealthMult(eff);
        hit[i] = defs[i]->damage * dmMul;
        // Per-hit fallback when cooldown<=0 — mirrors the lab's guard for a malformed def.
        dps[i] = (defs[i]->attackCooldown > 0.0f) ? hit[i] / defs[i]->attackCooldown : hit[i];
    }
    std::sort(hp,  hp  + n);
    std::sort(hit, hit + n);
    std::sort(dps, dps + n);
    const auto median = [n](const f32* v) {
        return (n % 2) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
    };

    const BalanceLab::EnemyCurve c = BalanceLab::enemyTrashAt(t, rawFloor, difficulty);
    CHECK(c.hpMedian  == doctest::Approx(median(hp)));
    CHECK(c.hpMin     == doctest::Approx(hp[0]));
    CHECK(c.hpMax     == doctest::Approx(hp[n - 1]));
    CHECK(c.hitMedian == doctest::Approx(median(hit)));
    CHECK(c.dpsMedian == doctest::Approx(median(dps)));
}

TEST_CASE("enemy trash curve rises within every tier band, every difficulty") {
    const EnemyDefTable& t = enemyTable();
    const u8 bands[5][2] = {{1,10},{11,20},{21,30},{31,40},{41,50}};
    for (u8 d = 0; d < FreePlay::DIFFICULTY_COUNT; d++)
        for (const auto& b : bands)
            for (u8 f = b[0]; f < b[1]; f++) {
                const BalanceLab::EnemyCurve a = BalanceLab::enemyTrashAt(t, f, d);
                const BalanceLab::EnemyCurve n2 = BalanceLab::enemyTrashAt(t, f + 1, d);
                CHECK(n2.hpMedian  > a.hpMedian);    // same roster, bigger multiplier
                CHECK(n2.hitMedian > a.hitMedian);
            }
}

TEST_CASE("boss curve exists on every authored boss floor and scales like the spawner") {
    const BossDefTable& bt = bossTable();
    REQUIRE(bt.count > 0);
    for (u32 i = 0; i < bt.count; i++) {
        const BossDef& bd = bt.defs[i];
        const BalanceLab::BossCurve c = BalanceLab::bossAt(bt, bd.floor, 1);   // Nightmare
        CHECK(c.present);
        const u32 eff = bd.floor + 50u;
        CHECK(c.hp  == doctest::Approx(bd.baseHp  * GameConst::floorHealthMult(eff)));
        CHECK(c.hit == doctest::Approx(bd.baseDmg * GameConst::floorDamageMult(eff)
                                       * GameConst::difficultyDamageBump(1)));
        if (bd.atkCooldown > 0.0f)
            CHECK(c.dps == doctest::Approx(c.hit / bd.atkCooldown));
    }
    CHECK_FALSE(BalanceLab::bossAt(bt, 2, 0).present);   // floor 2 has no boss

    // Every milestone floor (5,10,...,50) must have an authored boss: the engine falls back
    // to a hardcoded table when a floor is missing from bosses.json, so a silently dropped
    // entry would otherwise pass the suite while shipping the wrong fight.
    for (u8 f = 5; f <= 50; f = static_cast<u8>(f + 5))
        CHECK(BalanceLab::bossAt(bt, f, 0).present);
}
