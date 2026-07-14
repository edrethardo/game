// test_max_health.cpp — max HP is DERIVED: class base (grown per floor) + gear + active buff.
//
// The bug this pins: Inventory::getEffectiveMaxHealth existed, was correct, and was called by
// NOTHING. Every HEALTH_FLAT roll, every HEALTH_PCT roll and every item's base health contributed
// exactly zero to the player — max HP was only ever class base x floor growth. The entire defensive
// half of the loot system was inert, and nothing about that fails to compile or crashes.
//
// The second bug: because maxHealth was a free-running accumulator that anything could permanently
// nudge (and that was written straight to the save), a leaked Vitality shrine compounded across
// sessions to 44,922 on a real character whose legitimate value was ~1,195. Deriving maxHealth
// instead of accumulating it is what makes that structurally impossible.

#include <doctest/doctest.h>
#include "game/item.h"

namespace {

struct FakePlayer {
    f32 health            = 130.0f;
    f32 maxHealth         = 130.0f;
    f32 baseMaxHealth     = 130.0f;   // paladin class base
    f32 shrineHealthBonus = 0.0f;
};

ItemInstance healthItem(f32 flatBase, f32 pctAffix = 0.0f) {
    ItemInstance it;
    it.defId       = 0;
    it.rarity      = Rarity::RARE;
    it.bonusHealth = flatBase;
    if (pctAffix > 0.0f) {
        it.affixCount = 1;
        it.affixes[0] = {AffixType::HEALTH_PCT, pctAffix};
    }
    return it;
}

} // namespace

TEST_CASE("MaxHealth: a naked character is exactly their class base") {
    PlayerInventory inv;
    FakePlayer p;
    Inventory::refreshMaxHealth(p, inv);
    CHECK(p.maxHealth == doctest::Approx(130.0f));
}

TEST_CASE("MaxHealth: an item's flat health reaches the player") {
    // This is the whole bug: before the fix, this stayed at 130.
    PlayerInventory inv;
    inv.equipped[static_cast<u32>(ItemSlot::ARMOR)] = healthItem(1000.0f);
    Inventory::recalculateStats(inv);

    FakePlayer p;
    Inventory::refreshMaxHealth(p, inv);
    CHECK(p.maxHealth == doctest::Approx(1130.0f));   // 130 base + 1000 item
}

TEST_CASE("MaxHealth: HEALTH_PCT scales the whole pool, base included") {
    PlayerInventory inv;
    inv.equipped[static_cast<u32>(ItemSlot::ARMOR)] = healthItem(1000.0f, 50.0f);
    Inventory::recalculateStats(inv);

    FakePlayer p;
    Inventory::refreshMaxHealth(p, inv);
    CHECK(p.maxHealth == doctest::Approx((130.0f + 1000.0f) * 1.5f));   // 1695 — the runtime-verified value
}

TEST_CASE("MaxHealth: HEALTH_PCT from several slots adds, it does not compound") {
    PlayerInventory inv;
    inv.equipped[static_cast<u32>(ItemSlot::ARMOR)]  = healthItem(0.0f, 20.0f);
    inv.equipped[static_cast<u32>(ItemSlot::HELMET)] = healthItem(0.0f, 30.0f);
    Inventory::recalculateStats(inv);

    FakePlayer p;
    Inventory::refreshMaxHealth(p, inv);
    CHECK(p.maxHealth == doctest::Approx(130.0f * 1.5f));   // +20% and +30% => +50%, not 1.2*1.3
}

TEST_CASE("MaxHealth: floor growth applies to the BASE, gear stacks on top") {
    // The chosen model: (base x growth + gearFlat) x (1 + gearPct). Growth must not multiply gear,
    // or 150 floors of 1.5% would turn late-game gear into an unkillable character.
    PlayerInventory inv;
    inv.equipped[static_cast<u32>(ItemSlot::ARMOR)] = healthItem(1000.0f);
    Inventory::recalculateStats(inv);

    FakePlayer p;
    p.baseMaxHealth = 130.0f * 9.19f;      // ~149 descents of +1.5%
    Inventory::refreshMaxHealth(p, inv);

    CHECK(p.maxHealth == doctest::Approx(130.0f * 9.19f + 1000.0f));
    CHECK(p.maxHealth < 130.0f * 9.19f * 2.0f);   // gear was NOT scaled by the growth
}

TEST_CASE("MaxHealth: un-equipping drops max HP and clamps current HP under it") {
    PlayerInventory inv;
    inv.equipped[static_cast<u32>(ItemSlot::ARMOR)] = healthItem(1000.0f);
    Inventory::recalculateStats(inv);

    FakePlayer p;
    Inventory::refreshMaxHealth(p, inv);
    p.health = p.maxHealth;                        // topped up at 1130

    inv.equipped[static_cast<u32>(ItemSlot::ARMOR)] = ItemInstance{};   // strip it
    Inventory::recalculateStats(inv);
    Inventory::refreshMaxHealth(p, inv);

    CHECK(p.maxHealth == doctest::Approx(130.0f));
    CHECK(p.health <= p.maxHealth);                // must never sit above the cap
}

TEST_CASE("MaxHealth: an active shrine buff sits on top of the derived value") {
    PlayerInventory inv;
    inv.equipped[static_cast<u32>(ItemSlot::ARMOR)] = healthItem(1000.0f);
    Inventory::recalculateStats(inv);

    FakePlayer p;
    p.shrineHealthBonus = 452.0f;                  // 40% of 1130, as Shrine::apply would grant
    Inventory::refreshMaxHealth(p, inv);
    CHECK(p.maxHealth == doctest::Approx(1130.0f + 452.0f));

    p.shrineHealthBonus = 0.0f;                    // buff expires
    Inventory::refreshMaxHealth(p, inv);
    CHECK(p.maxHealth == doctest::Approx(1130.0f));   // and leaves nothing behind
}

TEST_CASE("MaxHealth: recomputing repeatedly is stable (it derives, it does not accumulate)") {
    // The structural fix. maxHealth used to be an accumulator, so anything that nudged it did so
    // permanently — which is exactly how the shrine leak compounded into the save file.
    PlayerInventory inv;
    inv.equipped[static_cast<u32>(ItemSlot::ARMOR)] = healthItem(1000.0f, 25.0f);
    Inventory::recalculateStats(inv);

    FakePlayer p;
    for (int i = 0; i < 1000; i++) Inventory::refreshMaxHealth(p, inv);   // a thousand ticks
    CHECK(p.maxHealth == doctest::Approx((130.0f + 1000.0f) * 1.25f));    // unchanged
}

TEST_CASE("MaxHealth: never drops below 1") {
    PlayerInventory inv;
    FakePlayer p;
    p.baseMaxHealth = 0.0f;
    Inventory::refreshMaxHealth(p, inv);
    CHECK(p.maxHealth >= 1.0f);
}
