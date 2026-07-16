// Legendary-exclusive uniques — the rarity-window contract.
//
// A legendary drop must be a REAL legendary: one of the named, skill-bearing unique defs
// (minRarity == LEGENDARY in items.json), never an ordinary base wearing an orange tint.
// And the inverse: a unique never drops below legendary, so its identity isn't spent as a
// grey stat stick. rollItem picks rarity FIRST and filters defs by their
// [minRarity, maxRarity] window; `rarityFloor` raises the tier for guaranteed drops
// (boss / champion / goblin payouts) with the level band widening before the tier ever
// degrades. These tests pin the generator contract on synthetic defs, then lint the real
// items.json so a future def can't be half-marked.

#include <doctest/doctest.h>
#include "game/item.h"

#include <json/nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <string>

namespace {

// Two-def world: one ordinary rare-capped base, one legendary-exclusive unique.
void makeDefs(ItemDef* defs) {
    defs[0] = ItemDef{};
    std::strcpy(defs[0].name, "Ordinary Sword");
    defs[0].slot = ItemSlot::WEAPON;
    defs[0].minLevel = 1;  defs[0].maxLevel = 50;
    defs[0].dropWeight = 1.0f;
    defs[0].minRarity = Rarity::COMMON;
    defs[0].maxRarity = Rarity::RARE;

    defs[1] = ItemDef{};
    std::strcpy(defs[1].name, "Named Unique");
    defs[1].slot = ItemSlot::WEAPON;
    defs[1].minLevel = 20; defs[1].maxLevel = 30;   // deliberately narrow band
    defs[1].dropWeight = 0.5f;
    defs[1].minRarity = Rarity::LEGENDARY;
    defs[1].maxRarity = Rarity::LEGENDARY;
}

} // namespace

TEST_CASE("rollItem: legendary tier draws ONLY from the unique pool, and uniques never below it") {
    ItemDef defs[2];
    makeDefs(defs);
    AffixDef noAffixes[1] = {};

    ItemGen::init(777);
    u32 legendaries = 0;
    for (u32 n = 0; n < 3000; n++) {
        // Level 25 = inside the unique's band, deep enough that rollRarity hits legendary often.
        ItemInstance it = ItemGen::rollItem(25, defs, 2, noAffixes, 0);
        if (it.rarity == Rarity::LEGENDARY) {
            legendaries++;
            CHECK(it.defId == 1);   // an orange drop is always the unique
        } else {
            CHECK(it.defId == 0);   // the unique never appears at any lower tier
        }
    }
    CHECK(legendaries > 0);          // the tier actually occurred (level 25 ⇒ ~26%)
    CHECK(legendaries < 3000);       // and so did the lower tiers
}

TEST_CASE("rollItem: rarityFloor guarantees a unique even outside its level band") {
    // Boss/goblin/champion guarantees can fire at levels where no unique's authored band
    // matches (e.g. a floor-2 goblin). The band must widen rather than the tier degrade.
    ItemDef defs[2];
    makeDefs(defs);
    AffixDef noAffixes[1] = {};

    ItemGen::init(1234);
    for (u32 n = 0; n < 200; n++) {
        ItemInstance it = ItemGen::rollItem(2, defs, 2, noAffixes, 0, Rarity::LEGENDARY);
        REQUIRE(it.rarity == Rarity::LEGENDARY);
        REQUIRE(it.defId == 1);
    }
}

TEST_CASE("rollItem: tier degrades only when NO def supports it") {
    // A world with no legendary-capable def at all: an organic legendary roll must fall
    // back to the best supported tier instead of failing or mislabeling.
    ItemDef two[2];
    makeDefs(two);             // reuse the fixture, hand rollItem only the ordinary def
    ItemDef defs[1] = {two[0]};   // rare-capped base
    AffixDef noAffixes[1] = {};

    ItemGen::init(99);
    for (u32 n = 0; n < 500; n++) {
        ItemInstance it = ItemGen::rollItem(40, defs, 1, noAffixes, 0, Rarity::LEGENDARY);
        REQUIRE(it.defId == 0);
        REQUIRE(it.rarity == Rarity::RARE);   // degraded to the def's ceiling, never orange
    }
}

TEST_CASE("rollAffixes: a legendary always carries 3-4 affixes") {
    // 4 distinct weapon-valid affix types so the roll can always fill the band.
    AffixDef affixes[4] = {};
    const AffixType kTypes[4] = {AffixType::DAMAGE_FLAT, AffixType::DAMAGE_PCT,
                                 AffixType::MOVE_SPEED_FLAT, AffixType::HEALTH_FLAT};
    for (u32 i = 0; i < 4; i++) {
        affixes[i].type = kTypes[i];
        affixes[i].minValue = 1.0f; affixes[i].maxValue = 2.0f;
        affixes[i].validSlots = 0xFF;   // valid everywhere
    }

    ItemGen::init(555);
    for (u32 n = 0; n < 300; n++) {
        ItemInstance it{};
        it.rarity = Rarity::LEGENDARY;
        ItemGen::rollAffixes(it, 10, ItemSlot::WEAPON, affixes, 4, WeaponType::MELEE);
        REQUIRE(it.affixCount >= 3);
        REQUIRE(it.affixCount <= 4);
    }
}

TEST_CASE("items.json: unique marking is complete and consistent") {
    std::ifstream f(DUNGEON_REPO_ROOT "/assets/config/items.json");
    REQUIRE(f.good());
    nlohmann::json doc = nlohmann::json::parse(f);
    const auto& items = doc["items"];

    u32 uniques = 0;
    for (size_t i = 0; i < items.size(); i++) {
        const auto& it = items[i];
        const std::string name = it.value("name", "?");
        const bool skilled   = !it.value("legendarySkill", std::string()).empty();
        const bool rollable  = it.value("minLevel", 0) <= 50;
        const bool legendMax = it.value("maxRarity", "common") == "legendary";
        const bool legendMin = it.value("minRarity", "common") == "legendary";
        CAPTURE(i); CAPTURE(name);

        // Every rollable skill-bearing def must be marked legendary-exclusive — a skilled
        // def left in the common pool would spend its identity as a grey stat stick again.
        if (skilled && rollable) CHECK(legendMin);
        // No half-marked windows: a legendary-exclusive def must also CAP at legendary.
        if (legendMin) CHECK(legendMax);
        // And the legendary tier must contain nothing anonymous: every rollable def that
        // can BE legendary is a marked unique (skill-bearing, or a signature weapon like
        // the bouncing Infinity Chakram whose behavior IS its identity).
        if (legendMax && rollable) CHECK(legendMin);

        if (legendMin && rollable) uniques++;
    }
    // The pool is real content, not an accident of parsing.
    CHECK(uniques >= 40);
}
