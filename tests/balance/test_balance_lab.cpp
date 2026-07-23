// tests/balance/test_balance_lab.cpp — the balance lab: typical-gear player power vs
// enemy curves per floor (spec: docs/superpowers/specs/2026-07-22-balance-lab-design.md).
// This file holds the always-on sanity pins plus the env-gated BALANCE_REPORT CSV dump.
#include "doctest/doctest.h"
#include "balance/balance_lab.h"
#include "game/enemy_loader.h"
#include "game/boss_loader.h"
#include "game/game_constants.h"
#include "game/free_play.h"     // DIFFICULTY_COUNT — the real difficulty-tier bound
#include "game/build_score.h"   // the REAL Auto-Loot scorer the gear Monte Carlo selects with
#include "game/weapon_dps.h"    // the shared sustained-DPS cycle powerOf runs weapons through
#include "game/combat.h"        // armorMitigation — the real armor curve behind EHP
#include <algorithm>
#include <cstdio>               // the CSV writers take FILE*
#include <cstdlib>              // getenv — the BALANCE_REPORT gate
#include <cstring>

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

static void loadGearTables(ItemDef* items, u32& itemCount, AffixDef* affixes, u32& affixCount) {
    REQUIRE(ItemLoader::loadItemDefs (DUNGEON_REPO_ROOT "/assets/config/items.json",   items,   itemCount));
    REQUIRE(ItemLoader::loadAffixDefs(DUNGEON_REPO_ROOT "/assets/config/affixes.json", affixes, affixCount));
}

TEST_CASE("typical gear: same (floor,difficulty,trial) is bit-identical every run") {
    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    u32 ic = 0, ac = 0; loadGearTables(items, ic, affixes, ac);

    BalanceLab::DropSet a, b;
    BalanceLab::rollWindowDrops(25, 1, 7, items, ic, affixes, ac, a);
    BalanceLab::rollWindowDrops(25, 1, 7, items, ic, affixes, ac, b);
    REQUIRE(a.count == b.count);
    REQUIRE(a.count == BalanceLab::MAX_WINDOW_DROPS);
    CHECK(std::memcmp(a.items, b.items, sizeof(ItemInstance) * a.count) == 0);

    // A different trial must produce a different stream (or the Monte Carlo is a no-op).
    BalanceLab::DropSet c;
    BalanceLab::rollWindowDrops(25, 1, 8, items, ic, affixes, ac, c);
    CHECK(std::memcmp(a.items, c.items, sizeof(ItemInstance) * a.count) != 0);
}

TEST_CASE("typical gear: every build cell fields a weapon from mid-game windows") {
    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    u32 ic = 0, ac = 0; loadGearTables(items, ic, affixes, ac);

    const u8 floors[] = {10, 25, 50};
    for (u8 f : floors)
        for (u8 d = 0; d < FreePlay::DIFFICULTY_COUNT; d += 2)   // Normal + Hell
            for (u32 trial = 0; trial < 5; trial++) {
                BalanceLab::DropSet drops;
                BalanceLab::rollWindowDrops(f, d, trial, items, ic, affixes, ac, drops);
                for (u8 cell = 0; cell < 9; cell++) {
                    PlayerInventory inv;
                    BalanceLab::selectLoadout(drops, cell, items, ic, inv);
                    const ItemInstance& w = inv.equipped[static_cast<u32>(ItemSlot::WEAPON)];
                    // CAPTUREs so a starved column names its exact (floor,diff,trial,cell).
                    CAPTURE(static_cast<u32>(f)); CAPTURE(static_cast<u32>(d));
                    CAPTURE(trial); CAPTURE(static_cast<u32>(cell));
                    REQUIRE(w.defId != 0xFFFF);          // family gate starved a column = balance bug
                    CHECK(BuildScore::weaponInFamily(items[w.defId].weaponSubtype,
                                                     BuildScore::buildCol(cell)));
                }
            }
}

TEST_CASE("typical gear: difficulty-entry windows inherit the previous difficulty's tail") {
    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    u32 ic = 0, ac = 0; loadGearTables(items, ic, affixes, ac);

    // NM floor 1 (effective 51): the window must span effective levels 48..51 — a full
    // 48-drop bag, not a thin 12-drop one. Before the fix this held only level-51 items.
    BalanceLab::DropSet nm1;
    BalanceLab::rollWindowDrops(1, 1, 0, items, ic, affixes, ac, nm1);
    REQUIRE(nm1.count == BalanceLab::MAX_WINDOW_DROPS);
    u8 lvlMin = 255, lvlMax = 0;
    for (u32 i = 0; i < nm1.count; i++) {
        lvlMin = (nm1.items[i].itemLevel < lvlMin) ? nm1.items[i].itemLevel : lvlMin;
        lvlMax = (nm1.items[i].itemLevel > lvlMax) ? nm1.items[i].itemLevel : lvlMax;
    }
    CHECK(lvlMin == 48);
    CHECK(lvlMax == 51);

    // Normal floor 1 has no previous difficulty: still a genuine fresh-start 12-drop window.
    BalanceLab::DropSet n1;
    BalanceLab::rollWindowDrops(1, 0, 0, items, ic, affixes, ac, n1);
    CHECK(n1.count == BalanceLab::DROPS_PER_FLOOR);
}

TEST_CASE("player power: bare crafted sword gives exactly the hand-computable numbers") {
    // Two synthetic defs — index 0 a plain sword, index 1 unused. No affixes anywhere, so
    // every engine conversion (effective weapon, max health, armor) runs on knowns.
    static ItemDef defs[2] = {};
    std::strcpy(defs[0].name, "Lab Sword");
    defs[0].slot = ItemSlot::WEAPON;
    defs[0].weaponType = WeaponType::MELEE;
    defs[0].weaponSubtype = WeaponSubtype::SWORD;
    defs[0].baseDamage = 30.0f;
    defs[0].baseCooldown = 0.5f;

    ItemInstance sword{};
    sword.defId = 0; sword.damage = 30.0f; sword.rarity = Rarity::COMMON;

    PlayerInventory inv{};
    const s8 bp = Inventory::addToBackpack(inv, sword);
    REQUIRE(bp >= 0);
    Inventory::equip(inv, static_cast<u8>(bp), defs);

    static SkillDef noSkills[1] = {};
    const u8 cell = 4;   // Moderate / Melee -> representative class WARRIOR
    const BalanceLab::PlayerPower p = BalanceLab::powerOf(inv, cell, 1, 0, defs, noSkills, 0);

    // sustained(30, 0.5, no clip) = 60; baseline crit 5% x2.0 -> x1.05 = 63.
    CHECK(p.weaponDps == doctest::Approx(63.0f));
    CHECK(p.castDps   == doctest::Approx(0.0f));        // no skill defs passed
    CHECK(p.totalDps  == doctest::Approx(63.0f));
    // No gear health, no armor: EHP == the representative class's base pool.
    const f32 warriorBase = kClassDefs[static_cast<u32>(PlayerClass::WARRIOR)].baseHealth;
    CHECK(p.ehp     == doctest::Approx(warriorBase));
    CHECK(p.sustain == doctest::Approx(0.0f));
}

TEST_CASE("player power: cast DPS uses the class skill list, unlock gating and CDR") {
    static ItemDef defs[1] = {};
    std::strcpy(defs[0].name, "Lab Wand");
    defs[0].slot = ItemSlot::WEAPON;
    defs[0].weaponType = WeaponType::PROJECTILE;
    defs[0].weaponSubtype = WeaponSubtype::WAND;
    defs[0].baseDamage = 10.0f;
    defs[0].baseCooldown = 0.5f;

    ItemInstance wand{};
    wand.defId = 0; wand.damage = 10.0f; wand.rarity = Rarity::COMMON;
    PlayerInventory inv{};
    const s8 bp = Inventory::addToBackpack(inv, wand);
    REQUIRE(bp >= 0);
    Inventory::equip(inv, static_cast<u8>(bp), defs);

    // A synthetic skill def bound to the SORCERER's SECOND skill (slot 1 unlocks at floor 10
    // for every class, so the below-unlock branch really runs at Normal): 100 dmg / 2 s cd.
    const ClassDef& sorc = kClassDefs[static_cast<u32>(PlayerClass::SORCERER)];
    static SkillDef sd[1] = {};
    sd[0].id = sorc.skills[1];
    sd[0].damage = 100.0f;
    sd[0].cooldown = 2.0f;

    // Stamp the cached CDR AFTER equip (recalculateStats runs inside equip and would reset
    // it) — 25% CDR means 1/0.75x casts, so the formula's CDR term is genuinely exercised.
    inv.bonusCooldownReduction = 0.25f;

    const u8 cell = 0;   // Tanky / Magic -> SORCERER
    const u8 unlock = sorc.skillUnlockFloor[1];         // floor 10 in the shipped table
    const f32 expected = 100.0f / (2.0f * 0.75f);       // dmg / (cd * (1 - CDR)) ~= 66.667

    // Below the unlock floor on Normal: no cast output (slot 0 has no def to resolve).
    const BalanceLab::PlayerPower locked =
        BalanceLab::powerOf(inv, cell, static_cast<u8>(unlock - 1), 0, defs, sd, 1);
    CHECK(locked.castDps == doctest::Approx(0.0f));

    // At the unlock floor on Normal: the CDR-scaled cast rate.
    const BalanceLab::PlayerPower p = BalanceLab::powerOf(inv, cell, unlock, 0, defs, sd, 1);
    CHECK(p.castDps == doctest::Approx(expected));

    // Nightmare raw floor 1 = effective floor 51: the engine unlocks on EFFECTIVE floor, so
    // the same skill is already live — a raw-floor gate would wrongly report 0 here.
    const BalanceLab::PlayerPower nm = BalanceLab::powerOf(inv, cell, 1, 1, defs, sd, 1);
    CHECK(nm.castDps == doctest::Approx(expected));
}

static void loadAllTables(ItemDef* items, u32& ic, AffixDef* affixes, u32& ac,
                          SkillDef* skills, u32& sc) {
    loadGearTables(items, ic, affixes, ac);
    REQUIRE(ItemLoader::loadSkillDefs(DUNGEON_REPO_ROOT "/assets/config/skills.json", skills, sc));
}

TEST_CASE("sweep row: percentiles ordered, TTK metrics consistent with their inputs") {
    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    static SkillDef skills[MAX_SKILL_DEFS];
    u32 ic = 0, ac = 0, sc = 0; loadAllTables(items, ic, affixes, ac, skills, sc);

    BalanceLab::MetricsRow r;
    BalanceLab::computeRow(0, 25, 4, /*trials=*/40, items, ic, affixes, ac, skills, sc,
                           enemyTable(), bossTable(), r);
    CHECK(r.tDps[0] <= r.tDps[1]);            // p10 <= p50
    CHECK(r.tDps[1] <= r.tDps[2]);            // p50 <= p90
    CHECK(r.ehp[0]  <= r.ehp[1]);
    CHECK(r.ttkTrash     == doctest::Approx(r.enemy.hpMedian / r.tDps[1]));
    CHECK(r.hitsToDie    == doctest::Approx(r.ehp[1] / r.enemy.hitMedian));
    CHECK(r.secondsToDie == doctest::Approx(r.ehp[1] / r.enemy.dpsMedian));
}

TEST_CASE("sanity pin: p50 gear power rises up the floor ladder for every cell") {
    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    static SkillDef skills[MAX_SKILL_DEFS];
    u32 ic = 0, ac = 0, sc = 0; loadAllTables(items, ic, affixes, ac, skills, sc);

    const u8 ladder[] = {10, 25, 40, 50};
    for (u8 d = 0; d < FreePlay::DIFFICULTY_COUNT; d += 2)   // Normal + Hell
        for (u8 cell = 0; cell < 9; cell += 4) {             // one cell per row/col diagonal
            f32 prevDps = 0, prevEhp = 0;
            for (u8 f : ladder) {
                BalanceLab::MetricsRow r;
                // Full TRIALS, not a cut-down count: Hell's affix levelScale (~10x at eff
                // 137-150) makes the DPS distribution heavy-tailed (p90 ~ 5x p50), where a
                // median-of-40 swings ~40% between adjacent floors and fails this pin on
                // pure sampling noise. 200 trials is deterministic (seeded by coordinates)
                // and still ~0.2 s for the whole ladder.
                BalanceLab::computeRow(d, f, cell, BalanceLab::TRIALS,
                                       items, ic, affixes, ac, skills, sc,
                                       enemyTable(), bossTable(), r);
                CAPTURE(static_cast<u32>(d)); CAPTURE(static_cast<u32>(cell));
                CAPTURE(static_cast<u32>(f));
                // 0.98: even p50-of-200 keeps a little between-floor wobble. Be honest
                // about the detection threshold: growth between rungs is 1.05x-3.95x, so
                // this catches order-scale collapses, starved loadouts and formula
                // inversions — a mere ~10% shift passes at most rungs. That is deliberate:
                // absolute magnitudes are pinned by the hand-computed powerOf cases, and
                // tuning-scale shifts are the BALANCE_REPORT CSV's job to surface.
                CHECK(r.tDps[1] >= prevDps * 0.98f);
                CHECK(r.ehp[1]  >= prevEhp * 0.98f);
                prevDps = r.tDps[1]; prevEhp = r.ehp[1];
            }
        }
}

// Comma-split field count that honors RFC-4180 double-quoted regions (a comma inside
// quotes is data, not a separator; "" inside a quoted field is an escaped quote).
static u32 csvFieldCount(const char* line) {
    u32 fields = 1;
    bool inQuotes = false;
    for (const char* p = line; *p && *p != '\n'; p++) {
        if (*p == '"') inQuotes = !inQuotes;             // "" toggles twice = net unchanged
        else if (*p == ',' && !inQuotes) fields++;
    }
    return fields;
}

TEST_CASE("CSV smoke: header + one row per cell, parseable, 33 fields per line") {
    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    static SkillDef skills[MAX_SKILL_DEFS];
    u32 ic = 0, ac = 0, sc = 0; loadAllTables(items, ic, affixes, ac, skills, sc);

    // cwd-relative: the test writes into whatever dir the binary runs from (the build dir
    // under the normal invocations). Clear any stale file from an earlier aborted run first.
    const char* path = "balance_smoke.csv";
    std::remove(path);
    FILE* fp = std::fopen(path, "w");
    REQUIRE(fp);
    BalanceLab::writeCsvHeader(fp);
    for (u8 cell = 0; cell < 9; cell++) {
        BalanceLab::MetricsRow r;
        // Floor 10 is deliberate: its boss is "Ygara, the Broodqueen" — a comma'd name —
        // so the field-count pin below actually exercises the RFC-4180 quoting.
        BalanceLab::computeRow(0, 10, cell, 10, items, ic, affixes, ac, skills, sc,
                               enemyTable(), bossTable(), r);
        BalanceLab::writeCsvRow(fp, r);
    }
    std::fclose(fp);

    fp = std::fopen(path, "r");
    REQUIRE(fp);
    char line[1024]; u32 lines = 0;
    while (std::fgets(line, sizeof line, fp)) {
        lines++;
        CAPTURE(lines);
        // 33 = the header's column count; a comma'd boss name that escapes quoting would
        // read as 34 and crash a strict parser downstream.
        CHECK(csvFieldCount(line) == 33);
    }
    std::fclose(fp);
    std::remove(path);
    CHECK(lines == 10);                                  // header + 9 cells
}

// The report dump. NOT a test: env-gated so normal CI runs skip the ~minutes-long full sweep.
//   BALANCE_REPORT=/tmp/balance.csv ./build/tests/dungeon_tests -tc="*balance report*"
TEST_CASE("balance report: full sweep CSV when BALANCE_REPORT is set") {
    const char* path = std::getenv("BALANCE_REPORT");
    if (!path) return;

    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    static SkillDef skills[MAX_SKILL_DEFS];
    u32 ic = 0, ac = 0, sc = 0; loadAllTables(items, ic, affixes, ac, skills, sc);

    FILE* fp = std::fopen(path, "w");
    // doctest::String: a raw const char* stringifies as a pointer address, not the path.
    REQUIRE_MESSAGE(fp, "BALANCE_REPORT path not writable: ", doctest::String(path));
    BalanceLab::writeCsvHeader(fp);
    for (u8 d = 0; d < FreePlay::DIFFICULTY_COUNT; d++)
        for (u8 f = 1; f <= 50; f++)
            for (u8 cell = 0; cell < 9; cell++) {
                BalanceLab::MetricsRow r;
                BalanceLab::computeRow(d, f, cell, BalanceLab::TRIALS,
                                       items, ic, affixes, ac, skills, sc,
                                       enemyTable(), bossTable(), r);
                BalanceLab::writeCsvRow(fp, r);
            }
    std::fclose(fp);
    MESSAGE("balance report written: ", doctest::String(path), " (1350 rows + header)");
}
