// test_stash.cpp — the shared account stash: page math, deposit/withdraw rules, and the
// design contract that a page is exactly twice the backpack. The file round-trip itself is
// engine I/O (engine_stash.cpp, atomic sidecar) and is exercised by the game; these tests pin
// the pure logic every UI/transfer path rides on.

#include <doctest/doctest.h>
#include "game/stash.h"

static ItemInstance mkItem(u16 defId) {
    ItemInstance it{};
    it.defId = defId;
    it.uid   = 1000u + defId;
    return it;
}

TEST_CASE("Stash: geometry — 5 pages, each exactly twice the backpack") {
    CHECK(Stash::PAGE_COUNT == 5);
    CHECK(Stash::PAGE_SLOTS == 2 * MAX_INVENTORY_ITEMS);
    CHECK(Stash::TOTAL_SLOTS == 240);
    CHECK(Stash::slotIndex(0, 0) == 0);
    CHECK(Stash::slotIndex(1, 0) == Stash::PAGE_SLOTS);
    CHECK(Stash::slotIndex(4, Stash::PAGE_SLOTS - 1) == Stash::TOTAL_SLOTS - 1);
}

TEST_CASE("Stash: deposit fills the current page only, in slot order") {
    Stash::State st;
    CHECK(Stash::deposit(st, 2, mkItem(5)));
    CHECK(st.items[Stash::slotIndex(2, 0)].defId == 5);
    CHECK(Stash::deposit(st, 2, mkItem(7)));
    CHECK(st.items[Stash::slotIndex(2, 1)].defId == 7);
    CHECK(st.dirty);
    // Other pages untouched.
    CHECK(isItemEmpty(st.items[Stash::slotIndex(0, 0)]));
}

TEST_CASE("Stash: a full page refuses — deposits never spill to another page") {
    Stash::State st;
    for (u32 s = 0; s < Stash::PAGE_SLOTS; s++)
        CHECK(Stash::deposit(st, 0, mkItem(static_cast<u16>(s))));
    CHECK_FALSE(Stash::deposit(st, 0, mkItem(999)));       // page 0 full
    CHECK(isItemEmpty(st.items[Stash::slotIndex(1, 0)]));  // page 1 still untouched
    CHECK(Stash::deposit(st, 1, mkItem(999)));             // explicit page switch works
}

TEST_CASE("Stash: sentinels and empties never enter storage") {
    Stash::State st;
    ItemInstance globe{};
    globe.defId = GLOBE_HEALTH_ID;
    CHECK_FALSE(Stash::deposit(st, 0, globe));
    CHECK_FALSE(Stash::deposit(st, 0, ItemInstance{}));
    CHECK_FALSE(st.dirty);
}

TEST_CASE("Stash: withdraw empties the slot and round-trips the item") {
    Stash::State st;
    ItemInstance in = mkItem(42);
    in.rarity = Rarity::LEGENDARY;
    in.affixCount = 2;
    REQUIRE(Stash::deposit(st, 3, in));
    ItemInstance out{};
    CHECK(Stash::withdraw(st, 3, 0, out));
    CHECK(out.defId == 42);
    CHECK(out.uid == in.uid);
    CHECK(out.rarity == Rarity::LEGENDARY);
    CHECK(isItemEmpty(st.items[Stash::slotIndex(3, 0)]));
    CHECK_FALSE(Stash::withdraw(st, 3, 0, out));   // already empty
}

// withdraw clears the vacated slot with `it = ItemInstance{}`. Pin that the WHOLE slot resets,
// not just defId: the cheap-looking "optimization" of stamping defId = 0xFFFF passes every
// isItemEmpty check while leaving stale affixes and damage behind for the next item deposited
// into that slot to inherit.
TEST_CASE("Stash: withdraw fully resets the vacated slot, not just defId") {
    Stash::State st;
    ItemInstance in = mkItem(7);
    in.affixCount = MAX_AFFIXES_PER_ITEM;
    for (u32 a = 0; a < MAX_AFFIXES_PER_ITEM; a++) {
        in.affixes[a].type  = AffixType::HEALTH_FLAT;
        in.affixes[a].value = 99.0f;
    }
    in.damage = 12.5f;
    REQUIRE(Stash::deposit(st, 0, in));

    ItemInstance out{};
    REQUIRE(Stash::withdraw(st, 0, 0, out));
    CHECK(out.affixCount == MAX_AFFIXES_PER_ITEM);          // the item itself round-trips intact
    CHECK(out.affixes[0].value == doctest::Approx(99.0f));

    const ItemInstance& slot = st.items[Stash::slotIndex(0, 0)];
    CHECK(slot.defId == 0xFFFF);
    CHECK(slot.affixCount == 0);
    CHECK(slot.damage == doctest::Approx(0.0f));
    CHECK(slot.uid == 0);
    for (u32 a = 0; a < MAX_AFFIXES_PER_ITEM; a++) {
        CHECK(slot.affixes[a].type  == AffixType::DAMAGE_FLAT);
        CHECK(slot.affixes[a].value == doctest::Approx(0.0f));
    }
}

// Stash::State declares `ItemInstance items[TOTAL_SLOTS];` and documents them as "all empty by
// default" — that contract is pure default member initializers, and the affix array deliberately
// carries no `= {}` of its own (it ICEs GCC 13; see item.h). So default-init alone must produce an
// empty slot, which leans entirely on Affix's DMIs. Value-init would zero the affixes either way;
// the default-init half is the one that fails if Affix's initializers are ever dropped.
TEST_CASE("ItemInstance: a default-constructed instance is an empty, zeroed slot") {
    ItemInstance defaultInit;
    ItemInstance valueInit{};
    for (u32 a = 0; a < MAX_AFFIXES_PER_ITEM; a++) {
        CHECK(defaultInit.affixes[a].type  == AffixType::DAMAGE_FLAT);
        CHECK(defaultInit.affixes[a].value == doctest::Approx(0.0f));
        CHECK(valueInit.affixes[a].type    == AffixType::DAMAGE_FLAT);
        CHECK(valueInit.affixes[a].value   == doctest::Approx(0.0f));
    }
    CHECK(defaultInit.defId == 0xFFFF);
    CHECK(valueInit.defId   == 0xFFFF);
}
