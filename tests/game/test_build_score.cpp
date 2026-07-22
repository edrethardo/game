// test_build_score.cpp — the Auto Loot & Equip scorer (game/build_score.h). Pins the contract the
// whole mode rests on: the weapon family gate (a Magic build can NEVER be handed an axe), the row
// weighting (a defense roll beats a damage roll on Tanky and loses on Glass Cannon), the hysteresis
// rule (near-ties never churn gear), and the rarity tiebreak. Pure header — no engine linkage.

#include <doctest/doctest.h>
#include "game/build_score.h"

namespace {

ItemDef weaponDef(WeaponSubtype st, f32 dmg = 10.0f) {
    ItemDef d{};
    d.slot = ItemSlot::WEAPON;
    d.weaponSubtype = st;
    d.baseDamage = dmg;
    return d;
}

ItemDef armorDef(f32 hp = 20.0f) {
    ItemDef d{};
    d.slot = ItemSlot::ARMOR;
    d.baseHealth = hp;
    return d;
}

ItemInstance instance(u8 affixCount = 0) {
    ItemInstance it{};
    it.defId = 1;                  // anything but the 0xFFFF empty sentinel
    it.affixCount = affixCount;
    return it;
}

// cells: row*3+col — row 0 Tanky / 1 Moderate / 2 Glass; col 0 Magic / 1 Melee / 2 Ranged
constexpr u8 TANKY_MELEE  = 0 * 3 + 1;
constexpr u8 GLASS_MELEE  = 2 * 3 + 1;
constexpr u8 MOD_MAGIC    = 1 * 3 + 0;
constexpr u8 MOD_MELEE    = 1 * 3 + 1;
constexpr u8 MOD_RANGED   = 1 * 3 + 2;

} // namespace

TEST_CASE("BuildScore: the weapon family gate is absolute") {
    // A Magic build must never be handed an axe, however good its rolls — the gate is 0, not a
    // penalty, so no roll can buy its way past it. Armor is never gated by column.
    ItemInstance it = instance();
    CHECK(BuildScore::score(it, weaponDef(WeaponSubtype::AXE, 999.0f),  MOD_MAGIC)  == 0.0f);
    CHECK(BuildScore::score(it, weaponDef(WeaponSubtype::WAND),        MOD_MAGIC)  > 0.0f);
    CHECK(BuildScore::score(it, weaponDef(WeaponSubtype::WAND, 999.0f), MOD_MELEE) == 0.0f);
    CHECK(BuildScore::score(it, weaponDef(WeaponSubtype::SWORD),       MOD_MELEE)  > 0.0f);
    CHECK(BuildScore::score(it, weaponDef(WeaponSubtype::BOW),         MOD_RANGED) > 0.0f);
    CHECK(BuildScore::score(it, weaponDef(WeaponSubtype::CARBINE),     MOD_RANGED) > 0.0f);
    CHECK(BuildScore::score(it, weaponDef(WeaponSubtype::SWORD, 999.0f), MOD_RANGED) == 0.0f);
    // NONE belongs to no family: never auto-equipped as a weapon.
    CHECK(BuildScore::score(it, weaponDef(WeaponSubtype::NONE, 999.0f), MOD_MELEE) == 0.0f);
    // Armor passes under every column.
    CHECK(BuildScore::score(it, armorDef(), MOD_MAGIC)  > 0.0f);
    CHECK(BuildScore::score(it, armorDef(), MOD_RANGED) > 0.0f);
}

TEST_CASE("BuildScore: row posture decides whether a defense roll beats a damage roll") {
    // Same slot, same def, one item rolled +12 armor, the other +12 damage. Tanky must prefer the
    // armor roll, Glass Cannon the damage roll — this IS the row axis meaning anything at all.
    ItemDef def = armorDef();
    ItemInstance armorRoll = instance(1);
    armorRoll.affixes[0] = {AffixType::ARMOR, 12.0f};
    ItemInstance dmgRoll = instance(1);
    dmgRoll.affixes[0] = {AffixType::DAMAGE_FLAT, 12.0f};

    CHECK(BuildScore::score(armorRoll, def, TANKY_MELEE) > BuildScore::score(dmgRoll, def, TANKY_MELEE));
    CHECK(BuildScore::score(dmgRoll,  def, GLASS_MELEE) > BuildScore::score(armorRoll, def, GLASS_MELEE));
}

TEST_CASE("BuildScore: Magic builds double spell damage; empty items score zero") {
    ItemDef ring{};
    ring.slot = ItemSlot::RING;
    ItemInstance spell = instance(1);
    spell.affixes[0] = {AffixType::SPELL_DAMAGE_PCT, 10.0f};
    ItemInstance plain = instance(1);
    plain.affixes[0] = {AffixType::DAMAGE_PCT, 10.0f};

    // Under Magic the spell roll wins; under Melee they tie back (spell keeps base value).
    CHECK(BuildScore::score(spell, ring, MOD_MAGIC) > BuildScore::score(plain, ring, MOD_MAGIC));
    CHECK(BuildScore::score(spell, ring, MOD_MELEE) == doctest::Approx(BuildScore::score(plain, ring, MOD_MELEE)));

    ItemInstance empty{};   // defId 0xFFFF
    CHECK(BuildScore::score(empty, ring, MOD_MELEE) == 0.0f);
}

TEST_CASE("BuildScore: hysteresis stops near-tie churn; empty slot is always an upgrade") {
    CHECK(BuildScore::isUpgrade(10.0f, 0.0f));            // empty slot: anything positive goes on
    CHECK_FALSE(BuildScore::isUpgrade(0.0f, 0.0f));       // nothing beats nothing
    CHECK_FALSE(BuildScore::isUpgrade(10.4f, 10.0f));     // +4% — inside the 5% band, no churn
    CHECK(BuildScore::isUpgrade(10.6f, 10.0f));           // +6% — a real upgrade
}

TEST_CASE("BuildScore: rarity breaks stat ties, so a legendary is never 'the worst item'") {
    // The self-managing bag drops the LOWEST-scoring item when full. Equal-stat items must order by
    // rarity or a fresh legendary could be evicted ahead of a lucky common.
    ItemDef ring{};
    ring.slot = ItemSlot::RING;
    ItemInstance common = instance();
    common.rarity = Rarity::COMMON;
    ItemInstance leg = instance();
    leg.rarity = Rarity::LEGENDARY;
    CHECK(BuildScore::score(leg, ring, MOD_MELEE) > BuildScore::score(common, ring, MOD_MELEE));
}

TEST_CASE("BuildScore: cell encoding round-trips and the default is Moderate/Melee") {
    for (u8 row = 0; row < BuildScore::BUILD_ROWS; row++)
        for (u8 col = 0; col < BuildScore::BUILD_COLS; col++) {
            const u8 cell = static_cast<u8>(row * BuildScore::BUILD_COLS + col);
            CHECK(BuildScore::buildRow(cell) == row);
            CHECK(BuildScore::buildCol(cell) == col);
        }
    CHECK(BuildScore::buildRow(BuildScore::DEFAULT_BUILD_CELL) == 1);
    CHECK(BuildScore::buildCol(BuildScore::DEFAULT_BUILD_CELL) == 1);
}

TEST_CASE("PlayerInventory v4 tail: classic by default, deterministic bytes") {
    // Every pre-v4 save loads through a mirror into PlayerInventory{} — so these defaults ARE the
    // migration: old characters must come up in classic mode with the default build. The reserved
    // bytes must be zero because the struct is serialized as a raw dump (padding would be
    // indeterminate; explicit bytes are not).
    PlayerInventory inv{};
    CHECK(inv.autoMode == 0);
    CHECK(inv.buildCell == BuildScore::DEFAULT_BUILD_CELL);
    CHECK(inv.reservedAuto0 == 0);
    CHECK(inv.reservedAuto1 == 0);
    CHECK(sizeof(PlayerInventory) == 1680);   // v4 size — also pinned by engine_persist static_asserts
}

TEST_CASE("Multi-build: pickup filter, dominance prune, and the better-build signal") {
    // The inventory keeps BEST-IN-SLOT for all nine builds: worse loot stays on the ground,
    // dominated bag items get discarded, and a build whose achievable total pulls ahead of the
    // active one triggers the "switch builds" nudge.
    ItemDef defs[4] = {};
    defs[1].slot = ItemSlot::ARMOR;  defs[1].baseHealth = 20.0f;   // defId 1: armor
    defs[2].slot = ItemSlot::WEAPON; defs[2].weaponSubtype = WeaponSubtype::SWORD; defs[2].baseDamage = 10.0f;
    defs[3].slot = ItemSlot::WEAPON; defs[3].weaponSubtype = WeaponSubtype::WAND;  defs[3].baseDamage = 30.0f;

    PlayerInventory inv{};
    inv.buildCell = 1 * 3 + 1;   // Moderate/Melee

    // Wear a plain armor and a sword; bag has one strictly-better armor (an armor affix roll).
    inv.equipped[static_cast<u32>(ItemSlot::ARMOR)]  = {}; inv.equipped[static_cast<u32>(ItemSlot::ARMOR)].defId = 1;
    inv.equipped[static_cast<u32>(ItemSlot::WEAPON)] = {}; inv.equipped[static_cast<u32>(ItemSlot::WEAPON)].defId = 2;
    inv.backpack[0] = {}; inv.backpack[0].defId = 1; inv.backpack[0].affixCount = 1;
    inv.backpack[0].affixes[0] = {AffixType::ARMOR, 15.0f};
    inv.backpackCount = 1;

    SUBCASE("bestSlotScore sees worn AND bag, and self-exclusion works") {
        const f32 withBag = BuildScore::bestSlotScore(inv, defs, 4, ItemSlot::ARMOR, inv.buildCell);
        const f32 without = BuildScore::bestSlotScore(inv, defs, 4, ItemSlot::ARMOR, inv.buildCell, 0);
        CHECK(withBag > without);                       // the bag armor is the best we can field
    }
    SUBCASE("a strict duplicate of gear we own is NOT worth picking up") {
        ItemInstance dupe{}; dupe.defId = 1;            // identical to the worn armor, no rolls
        CHECK_FALSE(BuildScore::worthPickingUp(dupe, defs[1], inv, defs, 4));
    }
    SUBCASE("a wand IS worth picking up even on a Melee build — the Magic builds want it") {
        ItemInstance wand{}; wand.defId = 3;
        CHECK(BuildScore::worthPickingUp(wand, defs[3], inv, defs, 4));
    }
    SUBCASE("the better bag armor is a keeper; a dominated duplicate is not") {
        CHECK(BuildScore::isKeeper(inv, defs, 4, 0));   // beats the worn piece for every row
        inv.backpack[1] = {}; inv.backpack[1].defId = 1;   // plain armor, dominated by BOTH others
        inv.backpackCount = 2;
        CHECK_FALSE(BuildScore::isKeeper(inv, defs, 4, 1));
    }
    SUBCASE("bestBuildCell flags the build the gear actually supports") {
        // Add a monster wand: the Magic columns can now field 30 base damage vs the sword's 10,
        // so the best achievable build is a Magic one and the nudge condition trips.
        inv.backpack[1] = {}; inv.backpack[1].defId = 3;
        inv.backpackCount = 2;
        f32 bestScore = 0.0f;
        const u8 best = BuildScore::bestBuildCell(inv, defs, 4, bestScore);
        CHECK(BuildScore::buildCol(best) == 0);         // a Magic cell
        const f32 current = BuildScore::gearScoreForCell(inv, defs, 4, inv.buildCell);
        CHECK(bestScore > current * BuildScore::BUILD_SUGGEST_FACTOR);
    }
    SUBCASE("maxCellScore: a wrong-family weapon still scores via its own family") {
        ItemInstance wand{}; wand.defId = 3;
        CHECK(BuildScore::maxCellScore(wand, defs[3]) > 0.0f);   // 0 under Melee, >0 under Magic
    }
}

TEST_CASE("Weapons are scored on DPS, not damage per hit") {
    // Real-roster numbers: Rusty Dagger 14 dmg @ 0.2 s = 70 DPS; Heavy Crossbow 50 dmg @ 0.78 s =
    // 64 DPS. Per-hit scoring ranked the crossbow 3.5x the dagger and would have purged every fast
    // weapon from every build; by DPS the dagger must WIN within its family comparison.
    ItemDef dagger{};
    dagger.slot = ItemSlot::WEAPON; dagger.weaponSubtype = WeaponSubtype::DAGGER;
    dagger.baseDamage = 14.0f; dagger.baseCooldown = 0.2f;
    ItemDef claymore{};
    claymore.slot = ItemSlot::WEAPON; claymore.weaponSubtype = WeaponSubtype::CLAYMORE;
    claymore.baseDamage = 30.0f; claymore.baseCooldown = 1.2f;   // 25 DPS — half the dagger's

    ItemInstance it = instance();
    const f32 dScore = BuildScore::score(it, dagger,   MOD_MELEE);
    const f32 cScore = BuildScore::score(it, claymore, MOD_MELEE);
    CHECK(dScore > cScore);                     // 70 DPS beats 25 DPS despite 14 < 30 per hit

    // An attack-speed roll on a WEAPON multiplies its DPS — worth more than the same value as a
    // generic utility contribution ever was.
    ItemInstance fast = instance(1);
    fast.affixes[0] = {AffixType::ATTACK_SPEED_PCT, 20.0f};
    CHECK(BuildScore::score(fast, dagger, MOD_MELEE) > dScore * 1.15f);

    // A weapon with no cooldown data hits the 0.2 s floor rather than dividing by zero.
    ItemDef weird{};
    weird.slot = ItemSlot::WEAPON; weird.weaponSubtype = WeaponSubtype::SWORD;
    weird.baseDamage = 10.0f; weird.baseCooldown = 0.0f;
    CHECK(BuildScore::score(it, weird, MOD_MELEE) > 0.0f);
}
