// tests/game/test_autoplay_doctrine.cpp — the 3x3 build cell -> combat doctrine mapping. The ROW
// (posture) sets risk (potion %, dodge/block bias); the COLUMN (archetype) sets the engagement
// band. These are the "play the build, not just wear it" parameters Aaron asked for.
#include "doctest/doctest.h"
#include "game/autoplay_doctrine.h"
#include "game/build_score.h"   // DEFAULT_BUILD_CELL, buildRow/buildCol

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
