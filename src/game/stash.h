#pragma once
// stash.h — the shared account stash: 5 pages × 48 slots (2× the 24-slot backpack), one file
// for ALL characters (gear transfer between heroes). Pure data + page/slot math live here so
// tests pin them without the engine; file I/O (stash.dat sidecar, atomic) is engine_stash.cpp.
//
// Persistence contract: stash.dat is a VERSIONED SIDECAR beside the saves (the stats_NN.dat /
// menagerie.dat precedent) — save_NN.dat itself is never touched. Layout: u32 STASH_VERSION,
// u32 slot count, then count × raw ItemInstance (empty = defId 0xFFFF). Any ItemInstance layout
// change already bumps SAVE_VERSION; bump STASH_VERSION alongside it and mirror-read the old one.

#include "core/types.h"
#include "game/item.h"

namespace Stash {

constexpr u32 PAGE_COUNT     = 5;
constexpr u32 PAGE_COLS      = 8;
constexpr u32 PAGE_ROWS      = 6;
constexpr u32 PAGE_SLOTS     = PAGE_COLS * PAGE_ROWS;          // 48 = 2x MAX_INVENTORY_ITEMS
constexpr u32 TOTAL_SLOTS    = PAGE_COUNT * PAGE_SLOTS;        // 240
constexpr u32 STASH_VERSION  = 1;

static_assert(PAGE_SLOTS == 2 * MAX_INVENTORY_ITEMS,
              "a stash page is by design exactly twice the backpack");

struct State {
    ItemInstance items[TOTAL_SLOTS];   // all empty (defId 0xFFFF) by default
    u8   page  = 0;                    // UI: currently shown page
    bool dirty = false;                // unsaved changes (engine writes stash.dat when set)
};

// Global slot index for (page, slotOnPage) — the single indexing rule.
inline u32 slotIndex(u32 page, u32 slotOnPage) { return page * PAGE_SLOTS + slotOnPage; }

// First free slot ON THE GIVEN PAGE (deposits never spill onto other pages — the player chose
// the page they're looking at). Returns PAGE_SLOTS if the page is full.
inline u32 firstFreeOnPage(const State& st, u32 page) {
    for (u32 s = 0; s < PAGE_SLOTS; s++)
        if (isItemEmpty(st.items[slotIndex(page, s)])) return s;
    return PAGE_SLOTS;
}

// Deposit into the given page. Returns false (stash untouched) when the page is full or the
// item is empty/sentinel — sentinels (globes, shrines, shards) must never enter storage.
inline bool deposit(State& st, u32 page, const ItemInstance& item) {
    if (isItemEmpty(item) || isSentinelItem(item)) return false;
    u32 s = firstFreeOnPage(st, page);
    if (s >= PAGE_SLOTS) return false;
    st.items[slotIndex(page, s)] = item;
    st.dirty = true;
    return true;
}

// Withdraw the item at (page, slot) into `out`. Returns false if the slot is empty.
inline bool withdraw(State& st, u32 page, u32 slotOnPage, ItemInstance& out) {
    ItemInstance& it = st.items[slotIndex(page, slotOnPage)];
    if (isItemEmpty(it)) return false;
    out = it;
    it = ItemInstance{};
    st.dirty = true;
    return true;
}

} // namespace Stash
