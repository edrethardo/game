// test_journal_layout.cpp — the Journal panel's geometry.
//
// Draw and hit-test both derive from journalLayout(). This file pins that they agree: every other
// inventory panel in this codebase drifted apart at some point because each side re-derived its
// own rects (see the quickbar note in inventory_ui.h).
#include "../../external/doctest/doctest.h"
#include "game/inventory_ui.h"

TEST_CASE("journal layout scales with screen height and stays on screen") {
    for (u32 sh : {720u, 1080u, 480u}) {
        const u32 sw = sh * 16 / 9;
        const InventoryUI::JournalRects r = InventoryUI::journalLayout(sw, sh);
        CAPTURE(sh);
        REQUIRE(r.rowH > 0.0f);
        REQUIRE(r.listX >= 0.0f);
        REQUIRE(r.listX + r.listW <= static_cast<f32>(sw));
        REQUIRE(r.detailX > r.listX + r.listW);          // detail sits right of the list
        REQUIRE(r.detailX + r.detailW <= static_cast<f32>(sw));
        REQUIRE(r.listTopY <= static_cast<f32>(sh));
    }
}

TEST_CASE("clicking a quest row hit-tests to that row index") {
    const u32 sw = 1280, sh = 720;
    const InventoryUI::JournalRects r = InventoryUI::journalLayout(sw, sh);

    for (u8 row = 0; row < 4; row++) {
        // Row 0 is drawn at the TOP and rows descend, matching the build grid's convention.
        const s32 mx = static_cast<s32>(r.listX + r.listW * 0.5f);
        const s32 my = static_cast<s32>(r.listTopY - r.rowH * (static_cast<f32>(row) + 0.5f));
        const InventoryUI::SlotHit h = InventoryUI::hitTestJournal(sw, sh, mx, my);
        CAPTURE(row);
        REQUIRE(h.panel == InventoryUI::SlotHit::JOURNAL_ROW);
        REQUIRE(h.index == row);
    }
}

TEST_CASE("clicking an act tab hit-tests to that tab") {
    const u32 sw = 1280, sh = 720;
    const InventoryUI::JournalRects r = InventoryUI::journalLayout(sw, sh);
    for (u8 t = 0; t < 2; t++) {
        const s32 mx = static_cast<s32>(r.tabX + (r.tabW + r.tabGap) * t + r.tabW * 0.5f);
        const s32 my = static_cast<s32>(r.tabY + r.tabH * 0.5f);
        const InventoryUI::SlotHit h = InventoryUI::hitTestJournal(sw, sh, mx, my);
        CAPTURE(t);
        REQUIRE(h.panel == InventoryUI::SlotHit::JOURNAL_TAB);
        REQUIRE(h.index == t);
    }
}

TEST_CASE("a click outside the panel hits nothing") {
    const InventoryUI::SlotHit h = InventoryUI::hitTestJournal(1280, 720, 5, 5);
    REQUIRE(h.panel == InventoryUI::SlotHit::NONE);
}
