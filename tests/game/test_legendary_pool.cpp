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

// --- MYTHIC: the Inferno-only tier above legendary (2026-08-02) ---------------------------------
// enemyLevel is the EFFECTIVE floor (raw + difficulty*50), so tier = (level-1)/50: 1-50 Normal,
// 51-100 Nightmare, 101-150 Hell, 151-200 Inferno.
TEST_CASE("mythic drops only in Inferno") {
    static ItemDef defs[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    u32 dc = 0, ac = 0;
    REQUIRE(ItemLoader::loadItemDefs (DUNGEON_REPO_ROOT "/assets/config/items.json",   defs,    dc));
    REQUIRE(ItemLoader::loadAffixDefs(DUNGEON_REPO_ROOT "/assets/config/affixes.json", affixes, ac));

    // Below Inferno the tier must never appear, however many rolls we take.
    for (u8 lvl : {u8(1), u8(25), u8(50), u8(100), u8(150)}) {
        ItemGen::init(0xBEEF ^ lvl);
        for (u32 i = 0; i < 4000; i++)
            REQUIRE(ItemGen::rollRarity(lvl) != Rarity::MYTHIC);
    }

    // In Inferno it appears, and at roughly the carved share of the legendary slice rather than
    // as a new bucket bolted on: legendary+mythic together must still respect the 7.5% ceiling.
    ItemGen::init(0xBEEF);
    u32 mythic = 0, legendary = 0;
    const u32 kRolls = 200000;
    for (u32 i = 0; i < kRolls; i++) {
        const Rarity r = ItemGen::rollRarity(200);
        if (r == Rarity::MYTHIC) mythic++;
        else if (r == Rarity::LEGENDARY) legendary++;
    }
    const f32 mythicPct = 100.0f * static_cast<f32>(mythic) / static_cast<f32>(kRolls);
    const f32 topPct    = 100.0f * static_cast<f32>(mythic + legendary) / static_cast<f32>(kRolls);
    CHECK(mythic > 0);
    CHECK(mythicPct == doctest::Approx(7.5f * ItemGen::MYTHIC_SHARE_OF_LEGENDARY).epsilon(0.15));
    CHECK(topPct   == doctest::Approx(7.5f).epsilon(0.10));   // the ceiling did NOT rise
}

TEST_CASE("a mythic is a real unique, rolled harder") {
    static ItemDef defs[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    u32 dc = 0, ac = 0;
    REQUIRE(ItemLoader::loadItemDefs (DUNGEON_REPO_ROOT "/assets/config/items.json",   defs,    dc));
    REQUIRE(ItemLoader::loadAffixDefs(DUNGEON_REPO_ROOT "/assets/config/affixes.json", affixes, ac));

    ItemGen::init(4242);
    u32 seen = 0;
    for (u32 i = 0; i < 400 && seen < 40; i++) {
        const ItemInstance it = ItemGen::rollItem(200, defs, dc, affixes, ac, Rarity::MYTHIC);
        if (it.rarity != Rarity::MYTHIC) continue;   // guaranteed by the floor, but be explicit
        seen++;
        const ItemDef& d = defs[it.defId];
        // It must come from the LEGENDARY pool — no def authors maxRarity "mythic", so a literal
        // window test would have found nothing, degraded the tier, and the rarity would never drop.
        CHECK(d.maxRarity == Rarity::LEGENDARY);
        CHECK(it.affixCount == MAX_AFFIXES_PER_ITEM);   // always the full complement
    }
    CHECK(seen > 0);
}

TEST_CASE("mythic out-rolls the same legendary def") {
    static ItemDef defs[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    u32 dc = 0, ac = 0;
    REQUIRE(ItemLoader::loadItemDefs (DUNGEON_REPO_ROOT "/assets/config/items.json",   defs,    dc));
    REQUIRE(ItemLoader::loadAffixDefs(DUNGEON_REPO_ROOT "/assets/config/affixes.json", affixes, ac));

    // Same def, same level, same RNG stream: only the rarity differs. Averaged over many rolls so
    // rollVariance (a 1.0-1.1 bell) can't decide the comparison.
    const auto meanDamage = [&](Rarity floorTier) {
        ItemGen::init(99);
        f32 sum = 0.0f; u32 n = 0;
        for (u32 i = 0; i < 300; i++) {
            const ItemInstance it = ItemGen::rollItem(200, defs, dc, affixes, ac, floorTier);
            if (defs[it.defId].slot != ItemSlot::WEAPON) continue;
            sum += it.damage; n++;
        }
        return (n > 0) ? sum / static_cast<f32>(n) : 0.0f;
    };
    const f32 legMean = meanDamage(Rarity::LEGENDARY);
    const f32 mytMean = meanDamage(Rarity::MYTHIC);
    REQUIRE(legMean > 0.0f);
    REQUIRE(mytMean > 0.0f);
    CHECK(mytMean > legMean);   // the base-power step is real, not cosmetic
}

TEST_CASE("every legendary behaviour extends to mythic") {
    // The helper is the whole contract: ~25 sites used to open-code `== Rarity::LEGENDARY`, and a
    // missed one would have made a mythic strictly WORSE than a legendary (no granted skill, or
    // despawning off the floor). Anything that must treat the top tiers alike calls this.
    CHECK(isLegendaryOrBetter(Rarity::LEGENDARY));
    CHECK(isLegendaryOrBetter(Rarity::MYTHIC));
    CHECK_FALSE(isLegendaryOrBetter(Rarity::RARE));
    CHECK_FALSE(isLegendaryOrBetter(Rarity::MAGIC));
    CHECK_FALSE(isLegendaryOrBetter(Rarity::COMMON));
    // Power order is load-bearing (ItemGen compares tiers, BuildScore casts to float).
    CHECK(static_cast<u8>(Rarity::MYTHIC) > static_cast<u8>(Rarity::LEGENDARY));
    CHECK(static_cast<u8>(Rarity::COUNT)  == static_cast<u8>(Rarity::MYTHIC) + 1);
    // Diablo 2's unique tan, distinct from legendary gold.
    const Vec3 m = rarityColor(Rarity::MYTHIC), l = rarityColor(Rarity::LEGENDARY);
    CHECK(m.x == doctest::Approx(0.78f));
    CHECK(m.y == doctest::Approx(0.70f));
    CHECK(m.z == doctest::Approx(0.47f));
    CHECK((m.x != l.x || m.y != l.y || m.z != l.z));
}

// --- no legendary may grant a skill its SLOT cannot fire (2026-08-02) ----------------------------
// Two items shipped with `phase_dash` on a WEAPON: the weapon rail is an on-hit PROC whose switch
// has no PHASE_DASH case, so the Phase Saber and Shadow Stiletto rolled their proc, looked up the
// skill, fell through `default:` and did NOTHING — while the tooltip and the equip bar advertised
// "Teleports forward through enemies". Nothing caught it because the rails are switch statements:
// an unhandled SkillId is legal C++, silent at compile time and at load.
//
// This pin encodes each rail's ACTUAL capability set. When a rail learns a new skill, add it here;
// when an item is authored with a skill its slot can't fire, this fails instead of shipping.
namespace {

// Weapons dispatch by weaponType: melee/hitscan use the engine_combat.cpp proc switch, projectile
// uses the engine_init_callbacks.cpp one. They do NOT support the same set — that asymmetry is
// itself a trap (arc_fire is melee-only, shadow_ricochet was projectile-only until this pass).
// Weapons dispatch over THREE switches, not two — and the first version of this table modelled only
// two, which is how it certified VOID_ZONE as handled while the co-op remote twin had no case for it.
// A guard that is wrong in the safe-looking direction is worse than no guard, so each rail is
// enumerated separately and a melee/hitscan skill must satisfy BOTH of its rails:
//   * LOCAL melee/hitscan — engine_combat.cpp, the firing player's own hits
//   * REMOTE twin        — engine_combat.cpp, a GUEST's hits resolved on the host
//   * PROJECTILE         — engine_init_callbacks.cpp, the shared projectile-hit callback, which runs
//                          for every projectile regardless of owner, so it needs no guest twin
bool localMeleeHitscanHandles(SkillId id) {
    switch (id) {
        case SkillId::FROZEN_ORB:
        case SkillId::CHAIN_LIGHTNING:
        case SkillId::METEOR_STRIKE:
        case SkillId::BLOOD_NOVA:
        case SkillId::VOID_ZONE:
        case SkillId::ARC_FIRE:
        case SkillId::SHADOW_RICOCHET:
        case SkillId::PHASE_REND:
            return true;
        default:
            return false;
    }
}
bool remoteTwinHandles(SkillId id) {
    switch (id) {
        case SkillId::FROZEN_ORB:
        case SkillId::CHAIN_LIGHTNING:
        case SkillId::METEOR_STRIKE:
        case SkillId::BLOOD_NOVA:
        case SkillId::ARC_FIRE:
        case SkillId::SHADOW_RICOCHET:
        case SkillId::PHASE_REND:
        case SkillId::VOID_ZONE:      // added 2026-08-02 with the guest twin's own case
            return true;
        default:
            return false;
    }
}
bool projectileProcHandles(SkillId id) {
    switch (id) {
        case SkillId::VOID_ZONE:
        case SkillId::FROZEN_ORB:
        case SkillId::CHAIN_LIGHTNING:
        case SkillId::BLOOD_NOVA:
        case SkillId::METEOR_STRIKE:
        case SkillId::SHADOW_RICOCHET:
        case SkillId::PHASE_REND:
            return true;
        default:
            return false;   // NB: no ARC_FIRE on this rail
    }
}

bool weaponProcHandles(WeaponType wt, SkillId id) {
    if (id == SkillId::THROWAWAY) return true;     // out-of-band (reload throw), not a proc switch
    if (wt == WeaponType::PROJECTILE) return projectileProcHandles(id);
    // Melee/hitscan must work for the host AND for a guest, or the item is dead one seat over.
    return localMeleeHitscanHandles(id) && remoteTwinHandles(id);
}

bool railHandles(ItemSlot slot, WeaponType wt, SkillId id) {
    switch (slot) {
        case ItemSlot::WEAPON: return weaponProcHandles(wt, id);
        case ItemSlot::ARMOR:
            return id == SkillId::BLOOD_NOVA || id == SkillId::STATIC_CHARGE ||
                   id == SkillId::HEMOPHAGE  || id == SkillId::METEOR_STRIKE ||
                   id == SkillId::FROZEN_ORB || id == SkillId::CHAIN_LIGHTNING ||
                   id == SkillId::PHASE_DASH;
        case ItemSlot::RING:
            return id == SkillId::BERSERKER   || id == SkillId::LIFE_STEAL ||
                   id == SkillId::THORNS      || id == SkillId::GRAVITY_PULL ||
                   id == SkillId::SECOND_WIND || id == SkillId::DIVINE_JUDGMENT ||
                   id == SkillId::SOUL_HARVEST|| id == SkillId::PHASE_STRIKE ||
                   id == SkillId::VOID_KILL;
        case ItemSlot::GLOVES:  return id == SkillId::FRENZY;
        // The offhand switch has a `default:` that falls through to a generic freeze bash, so no
        // offhand skill is ever fully inert — every id is "handled" there by construction.
        case ItemSlot::OFFHAND: return true;
        // Boots (F) and helmet (G) go through SkillSystem::tryActivate.
        case ItemSlot::BOOTS:
        case ItemSlot::HELMET:
            return id == SkillId::PHASE_DASH || id == SkillId::BREAK_FREE ||
                   id == SkillId::CHAIN_LIGHTNING || id == SkillId::METEOR_STRIKE;
        default: return false;
    }
}

} // namespace

TEST_CASE("every legendary's granted skill is live on its slot's rail") {
    static ItemDef defs[MAX_ITEM_DEFS];
    u32 dc = 0;
    REQUIRE(ItemLoader::loadItemDefs(DUNGEON_REPO_ROOT "/assets/config/items.json", defs, dc));

    u32 checked = 0;
    for (u32 i = 0; i < dc; i++) {
        const ItemDef& d = defs[i];
        if (d.legendarySkillId == SkillId::NONE) continue;
        checked++;
        CHECK_MESSAGE(railHandles(d.slot, d.weaponType, d.legendarySkillId),
                      "DEAD LEGENDARY: '", doctest::String(d.name),
                      "' (defId ", i, ") grants a skill its slot cannot fire");
    }
    CHECK(checked > 40);   // the audit found 51 — guard against the loop silently matching nothing
}

// A def authored `minRarity: legendary` must NEVER be produced below that tier: such an item is a
// named unique with no granted skill (isLegendaryOrBetter is false), i.e. a dud wearing a famous
// name. Prompted by a soak line reading "AutoEquip[0]: Vampiric Blade [Magic]".
TEST_CASE("a legendary-only def never rolls below legendary") {
    static ItemDef defs[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    u32 dc = 0, ac = 0;
    REQUIRE(ItemLoader::loadItemDefs (DUNGEON_REPO_ROOT "/assets/config/items.json",   defs,    dc));
    REQUIRE(ItemLoader::loadAffixDefs(DUNGEON_REPO_ROOT "/assets/config/affixes.json", affixes, ac));

    ItemGen::init(1234);
    u32 violations = 0, checked = 0;
    for (u8 lvl = 1; lvl <= 60; lvl++) {
        for (u32 i = 0; i < 400; i++) {
            const ItemInstance it = ItemGen::rollItem(lvl, defs, dc, affixes, ac);
            if (it.defId == 0xFFFF || it.defId >= dc) continue;
            checked++;
            if (defs[it.defId].minRarity == Rarity::LEGENDARY &&
                !isLegendaryOrBetter(it.rarity)) {
                if (violations == 0)
                    MESSAGE("first violation: ", doctest::String(defs[it.defId].name),
                            " rolled at rarity ", (u32)it.rarity);
                violations++;
            }
        }
    }
    CHECK(checked > 1000);
    CHECK(violations == 0);
}
