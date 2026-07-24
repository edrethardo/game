// tests/game/test_autoplay_doctrine.cpp — the 3x3 build cell -> combat doctrine mapping. The ROW
// (posture) sets risk (potion %, dodge/block bias); the COLUMN (archetype) sets the engagement
// band. These are the "play the build, not just wear it" parameters Aaron asked for.
#include "doctest/doctest.h"
#include "game/autoplay_doctrine.h"
#include "game/build_score.h"   // DEFAULT_BUILD_CELL, buildRow/buildCol
#include "game/item.h"          // kClassDefs + ItemLoader (the starting-weapon cross-check)
#include <cstring>

TEST_CASE("column sets engagement band: melee hugs, ranged holds, magic mid") {
    using namespace Autoplay;
    const Doctrine melee  = doctrineFor(3*1 + 1);   // Moderate/Melee
    const Doctrine ranged = doctrineFor(3*1 + 2);   // Moderate/Ranged
    const Doctrine magic  = doctrineFor(3*1 + 0);   // Moderate/Magic
    CHECK(melee.engageMin  == doctest::Approx(0.0f));   // close all the way in
    CHECK(melee.engageMax  <  ranged.engageMax);        // melee fights closer than ranged
    CHECK(ranged.engageMin >  0.0f);                    // ranged keeps a gap (kite band)
    CHECK(ranged.engageMin <  ranged.engageMax);
    CHECK(magic.engageMax  <  ranged.engageMax);        // magic mid-range, not max-range
    CHECK(magic.engageMax  >  melee.engageMax);
}

TEST_CASE("row sets risk: tanky drinks late & blocks, glass drinks early & dodges") {
    using namespace Autoplay;
    const Doctrine tankyMelee = doctrineFor(3*0 + 1);
    const Doctrine glassMelee = doctrineFor(3*2 + 1);
    CHECK(tankyMelee.potionHpFrac <  glassMelee.potionHpFrac);   // 0.35 vs 0.60
    CHECK(tankyMelee.blocks);
    CHECK(glassMelee.dodgesProactively);
    CHECK_FALSE(tankyMelee.dodgesProactively);
    CHECK(glassMelee.usesCover);                                 // glass breaks LOS
    CHECK_FALSE(tankyMelee.usesCover);
}

TEST_CASE("engagement band is expressed as a fraction of the weapon's attackRange") {
    using namespace Autoplay;
    const Doctrine d = doctrineFor(BuildScore::DEFAULT_BUILD_CELL);  // Moderate/Melee
    CHECK(d.engageMin >= 0.0f);
    CHECK(d.engageMax <= 2.0f);            // never more than 2x attackRange
    CHECK(d.disengageCount >= 3);          // "surrounded" threshold, in enemies
}

TEST_CASE("every cell 0..8 yields a valid doctrine (no gaps)") {
    using namespace Autoplay;
    for (u8 cell = 0; cell < 9; cell++) {
        const Doctrine d = doctrineFor(cell);
        CHECK(d.potionHpFrac > 0.0f);
        CHECK(d.potionHpFrac < 1.0f);
        CHECK(d.engageMax > d.engageMin);
    }
}

// --- the fresh-character build seed ------------------------------------------------------------
// A new Autoplay hero used to keep PlayerInventory's Moderate/MELEE default whatever its class, so
// Auto-Equip put a sword on a Sorcerer. These pin the class -> column table.

TEST_CASE("a fresh hero's build column comes from its class archetype") {
    using namespace Autoplay;
    CHECK(buildColForClass(PlayerClass::SORCERER)        == 0);  // Magic  (Wand of Sparks)
    CHECK(buildColForClass(PlayerClass::WARRIOR)         == 1);  // Melee  (Iron Sword)
    CHECK(buildColForClass(PlayerClass::ROGUE)           == 1);  // Melee  (Rusty Dagger)
    CHECK(buildColForClass(PlayerClass::PALADIN)         == 1);  // Melee  (Iron Sword)
    CHECK(buildColForClass(PlayerClass::WANDERER)        == 1);  // Melee  (Iron Sword, +20% melee)
    CHECK(buildColForClass(PlayerClass::RANGER)          == 2);  // Ranged (Short Bow)
    CHECK(buildColForClass(PlayerClass::MARKSMAN)        == 2);  // Ranged (Revolver)
    CHECK(buildColForClass(PlayerClass::COMBAT_ENGINEER) == 2);  // Ranged (Pistol)
    CHECK(buildColForClass(PlayerClass::TINKERER)        == 2);  // Ranged (Pistol)
}

TEST_CASE("the seeded cell is always in range and always on the Moderate row") {
    using namespace Autoplay;
    for (u8 c = 0; c < static_cast<u8>(PlayerClass::CLASS_COUNT); c++) {
        const u8 cell = defaultCellForClass(static_cast<PlayerClass>(c));
        CHECK(cell < BuildScore::BUILD_ROWS * BuildScore::BUILD_COLS);
        CHECK(BuildScore::buildRow(cell) == 1);                                  // Moderate posture
        CHECK(BuildScore::buildCol(cell) == buildColForClass(static_cast<PlayerClass>(c)));
    }
}

TEST_CASE("a MELEE-preferring class never seeds a non-melee column") {
    // Cross-check the explicit table against the real class table: preferredWeapon can't separate
    // Magic from Ranged (both PROJECTILE), but MELEE is unambiguous, so it must agree.
    using namespace Autoplay;
    for (u8 c = 0; c < static_cast<u8>(PlayerClass::CLASS_COUNT); c++) {
        if (kClassDefs[c].preferredWeapon != WeaponType::MELEE) continue;
        CHECK(buildColForClass(static_cast<PlayerClass>(c)) == 1);
    }
}

TEST_CASE("every class's STARTING weapon is fieldable in its seeded column") {
    // The real invariant behind the table: the column must not score the weapon the class is born
    // with at 0, or auto-equip throws it away on frame one and the hero fights bare-handed. Reads
    // the shipped items.json through the real loader, so changing a class's starting weapon (or
    // adding a class) fails here instead of silently in a live run.
    using namespace Autoplay;
    static ItemDef defs[MAX_ITEM_DEFS];   // static: MAX_ITEM_DEFS x sizeof(ItemDef) is stack-hostile
    u32 defCount = 0;
    REQUIRE(ItemLoader::loadItemDefs(DUNGEON_REPO_ROOT "/assets/config/items.json", defs, defCount));

    for (u8 c = 0; c < static_cast<u8>(PlayerClass::CLASS_COUNT); c++) {
        const char* wep = kClassDefs[c].startingWeaponName;
        REQUIRE(wep != nullptr);
        const ItemDef* d = nullptr;
        for (u32 i = 0; i < defCount; i++)
            if (std::strcmp(defs[i].name, wep) == 0) { d = &defs[i]; break; }
        REQUIRE_MESSAGE(d != nullptr, "starting weapon missing from items.json: ", wep);
        CHECK_MESSAGE(BuildScore::weaponInFamily(d->weaponSubtype,
                                                 buildColForClass(static_cast<PlayerClass>(c))),
                      kClassDefs[c].name, " starts with ", wep,
                      " but its seeded build column would score it 0");
    }
}

TEST_CASE("a seeded cell still drives a sane doctrine (the two tables compose)") {
    using namespace Autoplay;
    const Doctrine sorc = doctrineFor(defaultCellForClass(PlayerClass::SORCERER));
    const Doctrine warr = doctrineFor(defaultCellForClass(PlayerClass::WARRIOR));
    const Doctrine mark = doctrineFor(defaultCellForClass(PlayerClass::MARKSMAN));
    CHECK(warr.engageMin == doctest::Approx(0.0f));   // melee closes all the way
    CHECK(sorc.engageMin >  warr.engageMin);          // caster holds a gap
    CHECK(mark.engageMin >  sorc.engageMin);          // the sniper holds the widest gap
    CHECK(mark.engageMax >  sorc.engageMax);
}
