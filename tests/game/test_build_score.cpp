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

TEST_CASE("BuildScore: spell rolls are worth a caster's cast DPS, not a raw number") {
    // The honest model: +spell% multiplies the COLUMN's skill output (70 cast-DPS on Magic, 15 on
    // Melee/Ranged, where skills are a sidearm), so a spell roll beats a generic damage roll on a
    // caster and genuinely loses to it on a blade build. The old test asserted they were EQUAL on
    // Melee — an artifact of counting raw affix values instead of effects.
    ItemDef ring{};
    ring.slot = ItemSlot::RING;
    ItemInstance spell = instance(1);
    spell.affixes[0] = {AffixType::SPELL_DAMAGE_PCT, 10.0f};
    ItemInstance plain = instance(1);
    plain.affixes[0] = {AffixType::DAMAGE_PCT, 10.0f};

    CHECK(BuildScore::score(spell, ring, MOD_MAGIC) > BuildScore::score(plain, ring, MOD_MAGIC));
    CHECK(BuildScore::score(plain, ring, MOD_MELEE) > BuildScore::score(spell, ring, MOD_MELEE));

    ItemInstance empty{};   // defId 0xFFFF
    CHECK(BuildScore::score(empty, ring, MOD_MELEE) == 0.0f);
}

TEST_CASE("BuildScore: sustain IS tankiness — lifesteal/regen/life-on-hit feed the defense bucket") {
    // Aaron's call: lifesteal is survivability, not damage. A Tanky build must now prefer a max
    // lifesteal roll (1% of ~60 DPS over a 10 s fight = 6 effective HP) or a regen roll over a
    // plain damage roll — under the old scorer lifesteal counted as OFFENSE and no tank wanted it.
    ItemDef ring{};
    ring.slot = ItemSlot::RING;
    ItemInstance steal = instance(1);
    steal.affixes[0] = {AffixType::LIFESTEAL_PCT, 1.0f};       // max shipped roll
    ItemInstance rgn = instance(1);
    rgn.affixes[0] = {AffixType::HEALTH_REGEN, 3.0f};          // max shipped roll -> 30 eHP
    ItemInstance dmg = instance(1);
    dmg.affixes[0] = {AffixType::DAMAGE_PCT, 5.0f};

    CHECK(BuildScore::score(rgn,   ring, TANKY_MELEE) > BuildScore::score(dmg, ring, TANKY_MELEE));
    CHECK(BuildScore::score(steal, ring, TANKY_MELEE) > BuildScore::score(steal, ring, GLASS_MELEE));

    // And the armor formula linearizes correctly: armor A = +A% effective HP, so a 30-armor roll
    // equals a 45-flat-HP roll on the 150 reference pool (30% of 150), scored identically.
    ItemInstance arm = instance(1);
    arm.affixes[0] = {AffixType::ARMOR, 30.0f};
    ItemInstance hp = instance(1);
    hp.affixes[0] = {AffixType::HEALTH_FLAT, 45.0f};
    CHECK(BuildScore::score(arm, ring, TANKY_MELEE) == doctest::Approx(BuildScore::score(hp, ring, TANKY_MELEE)));
}

TEST_CASE("BuildScore: cooldown reduction multiplies a caster's cast rate") {
    // CDR c% means 1/(1-c) more casts per second — on a Magic build that is real DPS, on a Melee
    // build it is modest (skills are a sidearm). It used to be flat utility dribble on both.
    ItemDef ring{};
    ring.slot = ItemSlot::RING;
    ItemInstance cdrRoll = instance(1);
    cdrRoll.affixes[0] = {AffixType::COOLDOWN_REDUCTION, 15.0f};   // max shipped roll
    ItemInstance util = instance(1);
    util.affixes[0] = {AffixType::MOVE_SPEED_FLAT, 15.0f};

    CHECK(BuildScore::score(cdrRoll, ring, MOD_MAGIC) > BuildScore::score(cdrRoll, ring, MOD_MELEE));
    CHECK(BuildScore::score(cdrRoll, ring, MOD_MAGIC) > BuildScore::score(util, ring, MOD_MAGIC));
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

TEST_CASE("BuildScore: sustained DPS — CDR speeds weapons, reload taxes guns, projectile speed lands shots") {
    // Mirrors getEffectiveWeapon exactly: cooldown is divided by attack speed AND cut by CDR (the
    // engine applies CDR to the weapon swing, not just skills), clip weapons pay the reload cycle,
    // and projectile weapons convert projectile-speed rolls into hit reliability.
    ItemInstance plain = instance();

    SUBCASE("CDR is weapon DPS for melee and ranged, not just casters") {
        ItemDef sword{};
        sword.slot = ItemSlot::WEAPON; sword.weaponSubtype = WeaponSubtype::SWORD;
        sword.baseDamage = 26.0f; sword.baseCooldown = 0.4f;
        ItemInstance cdrRoll = instance(1);
        cdrRoll.affixes[0] = {AffixType::COOLDOWN_REDUCTION, 15.0f};
        // 15% CDR = 1/0.85 swings — a real DPS increase on the sword itself.
        CHECK(BuildScore::score(cdrRoll, sword, MOD_MELEE) >
              BuildScore::score(plain,  sword, MOD_MELEE) * 1.10f);
    }
    SUBCASE("guns pay their reload cycle: a Pistol's sustained DPS is below its burst") {
        // Pistol: 10 x 0.25 s + 1.0 s reload -> 3.5 s for 10 shots vs 2.5 s burst-only.
        ItemDef pistol{};
        pistol.slot = ItemSlot::WEAPON; pistol.weaponSubtype = WeaponSubtype::PISTOL;
        pistol.baseDamage = 20.0f; pistol.baseCooldown = 0.25f;
        pistol.baseClipSize = 10; pistol.baseReloadTime = 1.0f;
        ItemDef noClip = pistol;
        noClip.baseClipSize = 0; noClip.baseReloadTime = 0.0f;   // hypothetical reload-free twin
        const f32 real  = BuildScore::score(plain, pistol, MOD_RANGED);
        const f32 burst = BuildScore::score(plain, noClip, MOD_RANGED);
        CHECK(real < burst * 0.80f);                             // the ~29% reload tax is visible
        // And reload% buys the tax back: a 35% reload roll must beat the same-value utility credit.
        ItemInstance rel = instance(1);
        rel.affixes[0] = {AffixType::RELOAD_SPEED_PCT, 35.0f};
        CHECK(BuildScore::score(rel, pistol, MOD_RANGED) > real * 1.05f);
        // Clip size shrinks the share of time spent reloading too.
        ItemInstance clip = instance(1);
        clip.affixes[0] = {AffixType::CLIP_SIZE_PCT, 40.0f};
        CHECK(BuildScore::score(clip, pistol, MOD_RANGED) > real * 1.05f);
    }
    SUBCASE("projectile weapons credit projectile-speed rolls; hitscan ignores them") {
        ItemDef bow{};
        bow.slot = ItemSlot::WEAPON; bow.weaponSubtype = WeaponSubtype::BOW;
        bow.baseDamage = 30.0f; bow.baseCooldown = 0.6f; bow.baseProjectileSpeed = 23.0f;
        ItemInstance spd = instance(1);
        spd.affixes[0] = {AffixType::PROJECTILE_SPEED, 40.0f};   // max roll -> +16% effective
        CHECK(BuildScore::score(spd, bow, MOD_RANGED) >
              BuildScore::score(plain, bow, MOD_RANGED) * 1.10f);
        // A gun (no projectile) gets nothing from the same roll beyond generic handling.
        ItemDef carbine{};
        carbine.slot = ItemSlot::WEAPON; carbine.weaponSubtype = WeaponSubtype::CARBINE;
        carbine.baseDamage = 30.0f; carbine.baseCooldown = 0.6f;
        CHECK(BuildScore::score(spd, carbine, MOD_RANGED) ==
              doctest::Approx(BuildScore::score(plain, carbine, MOD_RANGED)));
    }
}

TEST_CASE("BuildScore: minipets are always picked up and always kept, never scored as gear") {
    // A petSummon consumable claims the RING slot only to satisfy the loader — it has zero gear
    // stats, so the stat filter would leave the rarest loot in the game lying on the ground.
    // Contract: the vacuum grabs it unconditionally, the prune never discards it, and the gear
    // side stays blind to it (score 0 in every cell, so it can never displace a real ring).
    ItemDef defs[2] = {};
    defs[0].slot = ItemSlot::RING;                 // a genuinely good ring def
    defs[1].slot = ItemSlot::RING;                 // the minipet (Mini Loot Goblin shape)
    defs[1].petSummon = true;

    ItemInstance pet{};
    pet.defId = 1;
    pet.rarity = Rarity::LEGENDARY;                // pets drop high-rarity — must not matter

    // The pet never competes as gear: zero in every cell despite the rarity.
    for (u8 cell = 0; cell < 9; cell++)
        CHECK(BuildScore::score(pet, defs[1], cell) == 0.0f);

    // Worth picking up even when the worn ring is excellent (a stat filter would say no).
    PlayerInventory inv{};
    for (auto& e : inv.equipped) e.defId = 0xFFFF;
    for (auto& b : inv.backpack) b.defId = 0xFFFF;
    ItemInstance ring{};
    ring.defId = 0;
    ring.affixCount = 2;
    ring.affixes[0] = {AffixType::DAMAGE_PCT, 20.0f};
    ring.affixes[1] = {AffixType::HEALTH_FLAT, 40.0f};
    inv.equipped[static_cast<u32>(ItemSlot::RING)] = ring;
    CHECK(BuildScore::worthPickingUp(pet, defs[1], inv, defs, 2));

    // And once in the bag it is a keeper for every pass, even beside that ring.
    inv.backpack[0] = pet;
    CHECK(BuildScore::isKeeper(inv, defs, 2, 0));
    // Control: a second stat-less NON-pet ring in the bag is dominated (the guard is petSummon,
    // not "rings are safe") — without this the pet check could pass vacuously.
    ItemInstance blank{};
    blank.defId = 0;
    inv.backpack[1] = blank;
    CHECK(!BuildScore::isKeeper(inv, defs, 2, 1));
}

TEST_CASE("BuildScore: bestRangedBackpackIdx finds the best ranged weapon in the bag") {
    // A defs table indexed by ItemInstance.defId. defId 1 = a bow, 2 = a stronger carbine,
    // 3 = a sword (melee — must be ignored), 4 = armor (not a weapon — must be ignored).
    ItemDef defs[5]{};
    defs[1] = weaponDef(WeaponSubtype::BOW,     20.0f);
    defs[2] = weaponDef(WeaponSubtype::CARBINE, 60.0f);   // higher base damage => higher ranged score
    defs[3] = weaponDef(WeaponSubtype::SWORD,   90.0f);
    defs[4] = armorDef(50.0f);

    PlayerInventory inv{};
    auto put = [&](u8 slot, u8 defId){ inv.backpack[slot].defId = defId; inv.backpack[slot].affixCount = 0; };
    put(0, 1); put(1, 3); put(2, 2); put(3, 4);
    inv.backpackCount = 4;

    // Ranged candidates are scored against the RANGED column, so the carbine (slot 2) wins over the
    // bow (slot 0); the sword and armor score 0 and are ignored.
    const s32 idx = BuildScore::bestRangedBackpackIdx(inv, defs, 5);
    CHECK(idx == 2);
}

TEST_CASE("BuildScore: bestRangedBackpackIdx returns -1 when the bag has no ranged weapon") {
    ItemDef defs[3]{};
    defs[1] = weaponDef(WeaponSubtype::SWORD, 50.0f);
    defs[2] = armorDef(20.0f);
    PlayerInventory inv{};
    inv.backpack[0].defId = 1; inv.backpack[1].defId = 2; inv.backpackCount = 2;
    CHECK(BuildScore::bestRangedBackpackIdx(inv, defs, 3) == -1);
}

TEST_CASE("BuildScore: the Phase Dash boots are the definitive best — always kept, always worn") {
    using namespace BuildScore;
    // Swift Boots: slot BOOTS, legendarySkill phase_dash, at LEGENDARY rarity.
    ItemDef swift{}; swift.slot = ItemSlot::BOOTS; swift.baseHealth = 15.0f;
    swift.legendarySkillId = SkillId::PHASE_DASH;

    // The predicate is exact: only legendary phase_dash BOOTS qualify.
    CHECK(isDefinitiveBest(swift, Rarity::LEGENDARY));
    CHECK_FALSE(isDefinitiveBest(swift, Rarity::RARE));              // must be legendary to grant the skill
    ItemDef phaseSaber{}; phaseSaber.slot = ItemSlot::WEAPON;        // phase_dash on a WEAPON is not it
    phaseSaber.weaponSubtype = WeaponSubtype::SWORD; phaseSaber.legendarySkillId = SkillId::PHASE_DASH;
    CHECK_FALSE(isDefinitiveBest(phaseSaber, Rarity::LEGENDARY));
    ItemDef plainBoots{}; plainBoots.slot = ItemSlot::BOOTS; plainBoots.baseHealth = 80.0f;  // no skill
    CHECK_FALSE(isDefinitiveBest(plainBoots, Rarity::LEGENDARY));

    // A hugely better ORDINARY boots is worn; the Phase Dash boots score far LESS on stats, yet the
    // auto-looter must still grab them and keep them.
    ItemDef defs[4]{};
    defs[1] = plainBoots;   // 80 base HP: outscores Swift Boots' 15 by a mile
    defs[2] = swift;
    defs[3] = ItemDef{}; defs[3].slot = ItemSlot::BOOTS; defs[3].baseHealth = 10.0f;  // a weak boots (dominated)
    PlayerInventory inv{};
    for (auto& e : inv.equipped) e.defId = 0xFFFF;
    for (auto& b : inv.backpack) b.defId = 0xFFFF;
    inv.equipped[static_cast<u32>(ItemSlot::BOOTS)] = {}; inv.equipped[static_cast<u32>(ItemSlot::BOOTS)].defId = 1;

    ItemInstance swiftInst{}; swiftInst.defId = 2; swiftInst.rarity = Rarity::LEGENDARY;
    // Sanity: on raw stats the worn boots really is higher — so only the override makes this pickup.
    CHECK(score(inv.equipped[static_cast<u32>(ItemSlot::BOOTS)], defs[1], DEFAULT_BUILD_CELL) >
          score(swiftInst, defs[2], DEFAULT_BUILD_CELL));
    CHECK(worthPickingUp(swiftInst, defs[2], inv, defs, 4));         // grabbed despite scoring lower

    // In the bag beside that better boots, it is a keeper (never discarded), unlike a strictly weaker boots.
    inv.backpack[0] = swiftInst;
    CHECK(isKeeper(inv, defs, 4, 0));
    inv.backpack[1] = {}; inv.backpack[1].defId = 3;  // a weaker plain boots, dominated by the worn 80-HP one
    CHECK_FALSE(isKeeper(inv, defs, 4, 1));
}

TEST_CASE("BuildScore: a BETTER Phase Dash boots displaces a worse one; a worse duplicate is not hoarded") {
    using namespace BuildScore;
    ItemDef defs[3]{};
    defs[1] = ItemDef{}; defs[1].slot = ItemSlot::BOOTS; defs[1].baseHealth = 15.0f;   // Swift Boots
    defs[1].legendarySkillId = SkillId::PHASE_DASH;
    // Two instances of the SAME Swift Boots def, one rolled better (an armor affix) than the other.
    ItemInstance worse{}; worse.defId = 1; worse.rarity = Rarity::LEGENDARY;
    ItemInstance better{}; better.defId = 1; better.rarity = Rarity::LEGENDARY;
    better.affixCount = 1; better.affixes[0] = {AffixType::ARMOR, 25.0f};
    CHECK(score(better, defs[1], DEFAULT_BUILD_CELL) > score(worse, defs[1], DEFAULT_BUILD_CELL));

    PlayerInventory inv{};
    for (auto& e : inv.equipped) e.defId = 0xFFFF;
    for (auto& b : inv.backpack) b.defId = 0xFFFF;

    // Wearing the WORSE pair: the BETTER pair is worth grabbing (it will displace on equip).
    inv.equipped[static_cast<u32>(ItemSlot::BOOTS)] = worse;
    CHECK(worthPickingUp(better, defs[1], inv, defs, 3));
    // Wearing the BETTER pair: a WORSE duplicate is NOT worth grabbing, and NOT a keeper if in the bag.
    inv.equipped[static_cast<u32>(ItemSlot::BOOTS)] = better;
    CHECK_FALSE(worthPickingUp(worse, defs[1], inv, defs, 3));
    inv.backpack[0] = worse;
    CHECK_FALSE(isKeeper(inv, defs, 3, 0));           // the worse duplicate gives way
    // The BETTER pair in the bag beside a worse worn one is kept (and would be equipped).
    inv.equipped[static_cast<u32>(ItemSlot::BOOTS)] = worse;
    inv.backpack[0] = better;
    CHECK(isKeeper(inv, defs, 3, 0));
}

TEST_CASE("BuildScore: bestRangedBackpackIdx ignores a pet in the bag") {
    // A pet-summon def claims a slot but is never a weapon; it must never be chosen as a sidearm.
    ItemDef defs[3]{};
    defs[1] = weaponDef(WeaponSubtype::BOW, 20.0f);
    defs[2] = weaponDef(WeaponSubtype::CARBINE, 60.0f); defs[2].petSummon = true;  // (contrived) pet flag
    PlayerInventory inv{};
    inv.backpack[0].defId = 1; inv.backpack[1].defId = 2; inv.backpackCount = 2;
    CHECK(BuildScore::bestRangedBackpackIdx(inv, defs, 3) == 0);   // the bow, not the flagged carbine
}

TEST_CASE("BuildScore: Glass Cannon leans harder into damage than Moderate does") {
    // The sharpened 3.5/0.5 row weights (from 3.0/1.0) must keep every row summing to 4 (the nudge
    // relies on it) AND make Glass reliably pick the damage piece over a much tankier one.
    using namespace BuildScore;
    f32 tO, tD, mO, mD, gO, gD;
    rowWeights(0, tO, tD); rowWeights(1, mO, mD); rowWeights(2, gO, gD);
    CHECK(tO + tD == doctest::Approx(4.0f));
    CHECK(mO + mD == doctest::Approx(4.0f));
    CHECK(gO + gD == doctest::Approx(4.0f));
    CHECK(gO > 3.0f);                          // sharpened past the old 3.0
    CHECK(gD < 1.0f);                          // defense barely counts for glass

    // A damage ring vs a max-roll ARMOR ring (30, the shipped ceiling — 45 effective HP, genuinely
    // tanky): Glass takes the damage one, Tanky takes the armor one. The sharpened weight is what
    // makes Glass pick damage over a real defensive roll rather than only over a token one.
    ItemDef ring{}; ring.slot = ItemSlot::RING;
    ItemInstance dmg = instance(1);  dmg.affixes[0]  = {AffixType::DAMAGE_PCT, 20.0f};
    ItemInstance tank = instance(1); tank.affixes[0] = {AffixType::ARMOR, 30.0f};   // max shipped roll
    CHECK(score(dmg, ring, GLASS_MELEE) > score(tank, ring, GLASS_MELEE));
    CHECK(score(tank, ring, TANKY_MELEE) > score(dmg, ring, TANKY_MELEE));   // Tanky still flips it
}

TEST_CASE("BuildScore: comparable-DPS ranged weapons rank by per-hit (weapon-damage skill scaling)") {
    // Marksman/Ranger skills scale off the weapon's per-hit damage, so between two RANGED weapons of
    // the SAME sustained DPS the harder hit wins — a tiebreak, not an override. Two bows at 60 DPS:
    // one 30 dmg @ 0.5 s, one 60 dmg @ 1.0 s.
    ItemDef fastBow{}; fastBow.slot = ItemSlot::WEAPON; fastBow.weaponSubtype = WeaponSubtype::BOW;
    fastBow.baseDamage = 30.0f; fastBow.baseCooldown = 0.5f;   // 60 DPS, small hit
    ItemDef bigBow{};  bigBow.slot  = ItemSlot::WEAPON; bigBow.weaponSubtype  = WeaponSubtype::BOW;
    bigBow.baseDamage  = 60.0f; bigBow.baseCooldown  = 1.0f;   // 60 DPS, big hit
    ItemInstance it = instance();
    CHECK(BuildScore::score(it, bigBow, MOD_RANGED) > BuildScore::score(it, fastBow, MOD_RANGED));
    // Melee gets no such bonus (melee-class skills don't read weapon damage): equal DPS => equal.
    ItemDef fastBlade = fastBow; fastBlade.weaponSubtype = WeaponSubtype::DAGGER;
    ItemDef bigBlade  = bigBow;  bigBlade.weaponSubtype  = WeaponSubtype::SWORD;
    CHECK(BuildScore::score(it, bigBlade, MOD_MELEE) ==
          doctest::Approx(BuildScore::score(it, fastBlade, MOD_MELEE)));
    // And the tiebreak never overturns a real DPS gap: a 40% higher-DPS small-hit bow still wins.
    ItemDef strongFast = fastBow; strongFast.baseCooldown = 0.357f;   // ~84 DPS, still 30/hit
    CHECK(BuildScore::score(it, strongFast, MOD_RANGED) > BuildScore::score(it, bigBow, MOD_RANGED));
}

TEST_CASE("BuildScore: a CDR helmet beats a defensive helmet for Glass Cannon (CDR speeds the weapon)") {
    // CDR on a non-weapon speeds the WEAPON (getEffectiveWeapon divides cooldown by 1-CDR), which the
    // scorer's non-weapon branch used to drop. A Glass Cannon must now prefer a CDR helmet over an
    // equal-tier armor helmet — Aaron: "helmets that provide good CDR over good defense as glass".
    ItemDef helm{}; helm.slot = ItemSlot::HELMET; helm.baseHealth = 20.0f;
    ItemInstance cdr = instance(1);  cdr.affixes[0]  = {AffixType::COOLDOWN_REDUCTION, 15.0f};
    ItemInstance def = instance(1);  def.affixes[0]  = {AffixType::ARMOR, 30.0f};
    CHECK(BuildScore::score(cdr, helm, GLASS_MELEE) > BuildScore::score(def, helm, GLASS_MELEE));
    // For a Tanky build the armor helmet still wins — the CDR credit is real but defense-weighted out.
    CHECK(BuildScore::score(def, helm, TANKY_MELEE) > BuildScore::score(cdr, helm, TANKY_MELEE));
    // CDR is now credited as weapon acceleration even outside the Magic column (was skill-only before).
    ItemInstance plain = instance();
    CHECK(BuildScore::score(cdr, helm, MOD_MELEE) > BuildScore::score(plain, helm, MOD_MELEE));
}

TEST_CASE("BuildScore: a legendary granted skill is worth its DPS-equivalent, and only at legendary") {
    using namespace BuildScore;
    // A SkillDef with a real damage/cooldown (Chain-Lightning-shaped) yields positive offense; a
    // stat-less reactive skill (no damage) yields none from the pure helper. skillOffense/Defense are
    // what the engine stamps onto ItemDef::legendarySkillOffense/Defense at load.
    SkillDef nuke{}; nuke.damage = 35.0f; nuke.cooldown = 0.3f; nuke.bounces = 3; nuke.damageFalloff = 0.8f;
    CHECK(skillOffense(nuke) > 0.0f);
    SkillDef reactive{}; reactive.damage = 0.0f;
    CHECK(skillOffense(reactive) == 0.0f);
    SkillDef guard{}; guard.invulnDuration = 1.0f;
    CHECK(skillDefense(guard) > 0.0f);

    // A skill-granting offhand (offense stamped) must beat a plain higher-defense offhand for Glass
    // Cannon, and only while LEGENDARY (a downgraded copy loses the skill's value).
    ItemDef shockShield{}; shockShield.slot = ItemSlot::OFFHAND; shockShield.baseHealth = 46.0f;
    shockShield.legendarySkillOffense = skillOffense(nuke);   // as the loader would stamp it
    ItemDef wallShield{};  wallShield.slot  = ItemSlot::OFFHAND; wallShield.baseHealth = 70.0f;   // pure defense, tankier

    ItemInstance leg{}; leg.defId = 1; leg.rarity = Rarity::LEGENDARY;
    CHECK(score(leg, shockShield, GLASS_MELEE) > score(leg, wallShield, GLASS_MELEE));
    // Rarity gate: a below-legendary copy loses the skill value entirely — the same shield def scored
    // at RARE is worth strictly less (only the base HP + a lower rarity bonus remain), and no longer
    // beats the pure-defense shield.
    ItemInstance rare{}; rare.defId = 1; rare.rarity = Rarity::RARE;
    CHECK(score(rare, shockShield, GLASS_MELEE) < score(leg, shockShield, GLASS_MELEE));
    CHECK(score(rare, shockShield, GLASS_MELEE) < score(leg, wallShield, GLASS_MELEE));
}

// --- class preferred-weapon bonus ---------------------------------------------------------------
// Every class deals +20% with its ClassDef::preferredWeapon TYPE. The build COLUMN can't express it:
// the Ranged column holds BOTH gun (HITSCAN) and bow/thrown (PROJECTILE) families, so a Combat
// Engineer (HITSCAN) and a Ranger (PROJECTILE) share a column while only one gets the bonus on any
// given weapon. Without the class term the scorer ranks them on raw stats and can hand the engineer
// a bow, silently dropping 20% of its damage.
TEST_CASE("BuildScore: the class preferred-weapon bonus decides between same-column families") {
    ItemDef bow = weaponDef(WeaponSubtype::BOW, 10.0f);
    bow.weaponType = WeaponType::PROJECTILE;
    ItemDef pistol = weaponDef(WeaponSubtype::PISTOL, 10.0f);
    pistol.weaponType = WeaponType::HITSCAN;
    const ItemInstance it = instance();

    // Same column, identical base damage: with no class known they tie.
    CHECK(BuildScore::score(it, bow, MOD_RANGED) == doctest::Approx(BuildScore::score(it, pistol, MOD_RANGED)));

    // A HITSCAN class (Combat Engineer / Marksman) must prefer the pistol...
    const f32 pistolHit = BuildScore::score(it, pistol, MOD_RANGED, WeaponType::HITSCAN);
    const f32 bowHit    = BuildScore::score(it, bow,    MOD_RANGED, WeaponType::HITSCAN);
    CHECK(pistolHit > bowHit);
    // ...and a PROJECTILE class (Ranger) the bow. Same items, opposite answer — that IS the bug.
    CHECK(BuildScore::score(it, bow, MOD_RANGED, WeaponType::PROJECTILE) >
          BuildScore::score(it, pistol, MOD_RANGED, WeaponType::PROJECTILE));

    // The bonus is the engine's +20% on damage, so it must not be a mere tiebreak: it has to be able
    // to carry a slightly WORSE weapon of the right type over a better one of the wrong type.
    ItemDef betterBow = weaponDef(WeaponSubtype::BOW, 11.0f);   // +10% raw damage
    betterBow.weaponType = WeaponType::PROJECTILE;
    CHECK(BuildScore::score(it, betterBow, MOD_RANGED, WeaponType::HITSCAN) < pistolHit);

    // A non-matching type is never PENALISED — it scores exactly as it would with no class at all.
    CHECK(BuildScore::score(it, bow, MOD_RANGED, WeaponType::HITSCAN) ==
          doctest::Approx(BuildScore::score(it, bow, MOD_RANGED)));

    // The gate still wins: a bonus can never smuggle a weapon into the wrong COLUMN.
    CHECK(BuildScore::score(it, pistol, MOD_MELEE, WeaponType::HITSCAN) == doctest::Approx(0.0f));
}

// The Autoplay melee SIDEARM depends on a melee build keeping ranged weapons in its BAG (it draws
// one when a VHALL balcony enemy is only reachable by falling). The class-preferred weapon bonus
// multiplies a class's own family by 1.2, so the obvious worry is that a melee class now rates every
// ranged weapon as junk, empties its bag of them, and silently disables the sidearm. It must not:
// the per-column family gate zeroes a melee weapon in the Ranged column, so the two never compete.
TEST_CASE("BuildScore: a melee class still keeps RANGED weapons for the sidearm") {
    using namespace BuildScore;
    ItemDef defs[3]{};
    defs[1] = weaponDef(WeaponSubtype::SWORD,  12.0f);   // the paladin's own family (gets the 1.2x)
    defs[2] = weaponDef(WeaponSubtype::PISTOL, 10.0f);   // a ranged weapon: the sidearm candidate

    PlayerInventory inv{};
    for (auto& e : inv.equipped) e.defId = 0xFFFF;
    for (auto& b : inv.backpack) b.defId = 0xFFFF;
    inv.equipped[static_cast<u32>(ItemSlot::WEAPON)] = {};
    inv.equipped[static_cast<u32>(ItemSlot::WEAPON)].defId = 1;      // wielding the sword

    ItemInstance pistol{}; pistol.defId = 2;

    // Judged AS A MELEE CLASS (WeaponType::MELEE preferred, i.e. a Paladin/Warrior).
    CHECK(worthPickingUp(pistol, defs[2], inv, defs, 3, WeaponType::MELEE));
    inv.backpack[0] = pistol;
    CHECK(isKeeper(inv, defs, 3, 0, WeaponType::MELEE));

    // ...and the sidearm helper can actually find it in the bag.
    CHECK(bestRangedBackpackIdx(inv, defs, 3) == 0);

    // The class bonus really is active on the melee side (guards against this test passing because
    // the bonus silently does nothing at all).
    ItemInstance sword{}; sword.defId = 1;
    CHECK(score(sword, defs[1], MOD_MELEE, WeaponType::MELEE) >
          score(sword, defs[1], MOD_MELEE, WeaponType::COUNT));
    // ...and does NOT leak into the Ranged column, where the family gate rules.
    CHECK(score(sword, defs[1], MOD_RANGED, WeaponType::MELEE) == doctest::Approx(0.0f));
}
