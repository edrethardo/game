// test_quickbar.cpp — quickbar geometry + slot bookkeeping.
//
// The quickbar had no test coverage at all, and that is precisely why its worst bug survived: the
// clickable rects (InventoryUI::hitTest) and the drawn bar (HUD::drawQuickbar) each derived their
// own geometry and disagreed — the hit-test iterated a stale 8 slots and applied neither the UI
// scale nor the flask offset. Every visible slot therefore hit-tested to an index >= 4, which every
// Quickbar:: entry point bounds-rejects, so drag-assign / reorder / remove were silent no-ops at
// EVERY resolution. A single "click the centre of the drawn slot, get that slot back" assertion
// would have caught it on day one. That assertion is the first test below.

#include <doctest/doctest.h>
#include "game/inventory_ui.h"
#include "game/item.h"

// Centre of drawn quickbar slot s, in HUD coords — derived from the SAME layout the renderer uses.
static void qbSlotCentre(const InventoryUI::QuickbarRects& r, u32 s, s32& x, s32& y) {
    x = static_cast<s32>(r.startX + s * (r.slot + r.gap) + r.slot * 0.5f);
    y = static_cast<s32>(r.baseY + r.slot * 0.5f);
}

// Build a backpack item with a known uid. defId 0 is a valid def; 0xFFFF is the empty sentinel.
static ItemInstance makeItem(u32 uid) {
    ItemInstance it{};
    it.defId = 0;
    it.uid   = uid;
    return it;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

TEST_CASE("Quickbar: the centre of every drawn slot hit-tests to that same slot") {
    // Across resolutions, because the old bug's symptom changed with uiScale: at 720p every drawn
    // slot mapped to a phantom index 4-7, and at 1080p+ two of the four mapped to nothing at all.
    const u32 res[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}};
    for (const auto& rr : res) {
        const u32 sw = rr[0], sh = rr[1];
        const auto qb = InventoryUI::quickbarLayout(sw, sh);
        for (u32 s = 0; s < QUICKBAR_SLOTS; s++) {
            s32 x, y; qbSlotCentre(qb, s, x, y);
            const auto hit = InventoryUI::hitTest(sw, sh, x, y);
            REQUIRE(hit.panel == InventoryUI::SlotHit::QUICKBAR);
            CHECK(hit.index == s);
        }
    }
}

TEST_CASE("Quickbar: hit-test never returns an index the Quickbar API would reject") {
    // The old QB_SLOTS=8 handed out indices 4..7, which every Quickbar:: function bounds-rejects —
    // turning each interaction into a silent no-op instead of a visible error.
    const auto qb = InventoryUI::quickbarLayout(1280, 720);
    for (s32 x = 0; x < 1280; x += 3) {
        for (s32 y = 0; y < 120; y += 3) {   // sweep the whole bottom strip the bar lives in
            const auto hit = InventoryUI::hitTest(1280, 720, x, y);
            if (hit.panel == InventoryUI::SlotHit::QUICKBAR)
                CHECK(hit.index < QUICKBAR_SLOTS);
        }
    }
    (void)qb;
}

TEST_CASE("Quickbar: the gaps between slots are not slots") {
    const auto qb = InventoryUI::quickbarLayout(1280, 720);
    for (u32 s = 0; s + 1 < QUICKBAR_SLOTS; s++) {
        // Midpoint of the gap between slot s and s+1.
        s32 x = static_cast<s32>(qb.startX + s * (qb.slot + qb.gap) + qb.slot + qb.gap * 0.5f);
        s32 y = static_cast<s32>(qb.baseY + qb.slot * 0.5f);
        CHECK(InventoryUI::hitTest(1280, 720, x, y).panel != InventoryUI::SlotHit::QUICKBAR);
    }
}

TEST_CASE("Quickbar: the class skill bar does not hit-test as quickbar") {
    // The phantom rects sat directly on top of the class skill bar, and hitTest runs BEFORE any
    // skill-bar handling — so clicks meant for a class skill were swallowed as quickbar clicks.
    const u32 sw = 1280, sh = 720;
    const auto sb = InventoryUI::skillBarLayout(sw, sh, 0);
    for (u32 s = 0; s < InventoryUI::CLASS_SKILL_SLOTS; s++) {
        s32 x = static_cast<s32>(sb.classX + s * (sb.slot + sb.gap) + sb.slot * 0.5f);
        s32 y = static_cast<s32>(sb.classY + sb.slot * 0.5f);
        CHECK(InventoryUI::hitTest(sw, sh, x, y).panel != InventoryUI::SlotHit::QUICKBAR);
    }
}

TEST_CASE("Quickbar: the class bar sits flush to the left of the quickbar") {
    // skillBarLayout anchors off quickbarLayout; pin that they agree so the two bars can't drift.
    for (u32 sh : {720u, 1080u}) {
        const u32 sw = (sh * 16) / 9;
        const auto qb = InventoryUI::quickbarLayout(sw, sh);
        const auto sb = InventoryUI::skillBarLayout(sw, sh, 0);
        const f32 classW = InventoryUI::CLASS_SKILL_SLOTS * sb.slot +
                           (InventoryUI::CLASS_SKILL_SLOTS - 1) * sb.gap;
        CHECK(sb.classX + classW < qb.startX);            // strictly left of the bar
        CHECK(qb.startX - (sb.classX + classW) < 20.0f * (static_cast<f32>(sh) / 720.0f));  // and flush
    }
}

// ---------------------------------------------------------------------------
// Slot bookkeeping
// ---------------------------------------------------------------------------

TEST_CASE("Quickbar: syncWeaponSlot reclaims a BACKPACK_REF whose item is gone") {
    // A dropped item left its slot resolving to nullptr (so it DREW blank) while still holding
    // type == BACKPACK_REF — and the free-slot scan tests `type == EMPTY`. So a blank-looking slot
    // permanently blocked assignment, and with 4 slots the bar jammed for the rest of the run.
    PlayerInventory inv{};
    QuickbarState   qb{};

    inv.backpack[0] = makeItem(101);
    Quickbar::assignItem(qb, inv, 0);
    REQUIRE(qb.slots[0].type == QuickbarSlot::BACKPACK_REF);
    REQUIRE(Quickbar::resolveSlot(qb, inv, 0) != nullptr);

    // Drop it: the backpack slot empties, but the quickbar still points at it.
    inv.backpack[0].defId = 0xFFFF;
    CHECK(Quickbar::resolveSlot(qb, inv, 0) == nullptr);   // already drew blank

    Quickbar::syncWeaponSlot(qb, inv);
    CHECK(qb.slots[0].type == QuickbarSlot::EMPTY);        // now actually reclaimed

    // ...and the freed slot accepts a new item.
    inv.backpack[1] = makeItem(202);
    Quickbar::assignItem(qb, inv, 1);
    CHECK(qb.slots[0].type == QuickbarSlot::BACKPACK_REF);
    CHECK(qb.slots[0].itemUid == 202);
}

TEST_CASE("Quickbar: a live BACKPACK_REF survives syncWeaponSlot") {
    // The reclaim pass must not evict valid refs. The backpack never compacts (removeFromBackpack
    // leaves holes), so a still-valid sourceIndex stays valid.
    PlayerInventory inv{};
    QuickbarState   qb{};
    inv.backpack[3] = makeItem(303);
    Quickbar::assignItem(qb, inv, 3);
    REQUIRE(qb.slots[0].itemUid == 303);

    Quickbar::syncWeaponSlot(qb, inv);
    CHECK(qb.slots[0].type == QuickbarSlot::BACKPACK_REF);
    CHECK(qb.slots[0].itemUid == 303);
    CHECK(Quickbar::resolveSlot(qb, inv, 0) != nullptr);
}

TEST_CASE("Quickbar: assignToSlot targets an exact slot and never duplicates a uid") {
    PlayerInventory inv{};
    QuickbarState   qb{};
    inv.backpack[0] = makeItem(11);
    inv.backpack[1] = makeItem(22);

    Quickbar::assignToSlot(qb, inv, 2, DragSource::BACKPACK, 0);
    CHECK(qb.slots[2].type == QuickbarSlot::BACKPACK_REF);
    CHECK(qb.slots[2].itemUid == 11);

    // Re-assigning the same item elsewhere must MOVE it, not clone it.
    Quickbar::assignToSlot(qb, inv, 3, DragSource::BACKPACK, 0);
    CHECK(qb.slots[3].itemUid == 11);
    CHECK(qb.slots[2].type == QuickbarSlot::EMPTY);

    // And a second, different item coexists.
    Quickbar::assignToSlot(qb, inv, 1, DragSource::BACKPACK, 1);
    CHECK(qb.slots[1].itemUid == 22);
    CHECK(qb.slots[3].itemUid == 11);
}

TEST_CASE("Quickbar: swapSlots round-trips") {
    PlayerInventory inv{};
    QuickbarState   qb{};
    inv.backpack[0] = makeItem(11);
    inv.backpack[1] = makeItem(22);
    Quickbar::assignToSlot(qb, inv, 0, DragSource::BACKPACK, 0);
    Quickbar::assignToSlot(qb, inv, 1, DragSource::BACKPACK, 1);

    Quickbar::swapSlots(qb, 0, 1);
    CHECK(qb.slots[0].itemUid == 22);
    CHECK(qb.slots[1].itemUid == 11);

    Quickbar::swapSlots(qb, 0, 1);
    CHECK(qb.slots[0].itemUid == 11);
    CHECK(qb.slots[1].itemUid == 22);
}

TEST_CASE("Quickbar: out-of-range slot indices are rejected, not written") {
    PlayerInventory inv{};
    QuickbarState   qb{};
    inv.backpack[0] = makeItem(11);

    Quickbar::assignToSlot(qb, inv, QUICKBAR_SLOTS, DragSource::BACKPACK, 0);  // past the end
    for (u32 i = 0; i < QUICKBAR_SLOTS; i++) CHECK(qb.slots[i].type == QuickbarSlot::EMPTY);

    Quickbar::removeItem(qb, QUICKBAR_SLOTS);        // must not write past the array
    Quickbar::swapSlots(qb, 0, QUICKBAR_SLOTS);
    CHECK(Quickbar::resolveSlot(qb, inv, QUICKBAR_SLOTS) == nullptr);
    CHECK(qb.activeSlot == 0);                       // the byte that sits right after slots[]
}
