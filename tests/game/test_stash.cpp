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
