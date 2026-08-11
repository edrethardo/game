// test_menu_frame.cpp — the tabbed character menu's frame and page tabs.
//
// The menu is ONE framed panel hosting three pages (Inventory / Character / Quests). Its geometry
// lives in InventoryUI::menuFrameLayout, and both the chrome renderer (hud_menu_frame.cpp) and the
// click handler (engine_inventory.cpp) derive from it. This file pins that they agree, and that
// every page has a content box to lay out inside.
//
// The specific failure it exists to catch: a tab strip whose drawn plates and clickable rects
// drift apart. Every panel in this HUD has done that at least once — the quickbar's rects did not
// overlap its drawn bar at ANY resolution — which is why the layout split exists at all.
#include "../../external/doctest/doctest.h"
#include "game/inventory_ui.h"

TEST_CASE("the menu frame is on screen and leaves a content box at every resolution") {
    // 480p is the floor a Switch handheld-ish window reaches; 1440p is the widest desktop tested.
    for (u32 sh : {480u, 720u, 1080u, 1440u}) {
        const u32 sw = sh * 16 / 9;
        const InventoryUI::MenuFrameRects r = InventoryUI::menuFrameLayout(sw, sh);
        CAPTURE(sh);

        REQUIRE(r.x >= 0.0f);
        REQUIRE(r.y >= 0.0f);
        REQUIRE(r.x + r.w <= static_cast<f32>(sw));
        REQUIRE(r.y + r.h <= static_cast<f32>(sh));

        // The content box must be strictly INSIDE the frame, or a page draws over its own border.
        REQUIRE(r.contentX >= r.x);
        REQUIRE(r.contentY >= r.y);
        REQUIRE(r.contentX + r.contentW <= r.x + r.w);
        REQUIRE(r.contentY + r.contentH <= r.y + r.h);
        // ...and it must be a real box. A zero or negative height here is not a cosmetic bug: the
        // character sheet divides by it to size its rows.
        REQUIRE(r.contentW > 0.0f);
        REQUIRE(r.contentH > 0.0f);

        // The tab strip sits above the content, never over it.
        REQUIRE(r.tabY >= r.contentY + r.contentH);
        REQUIRE(r.tabY + r.tabH <= r.y + r.h);
    }
}

TEST_CASE("every page tab is clickable across its whole drawn plate") {
    // Sampled at the plate's EDGES, not just its centre, and that is the point of the case. A
    // centre-only check passes against a hit-test offset by up to half a tab width — verified by
    // sabotage: shifting hitTestMenuTabs by +40 px left a centre click still inside the wrong
    // rect, and the first version of this test happily agreed. The edges are where draw and
    // hit-test actually have to meet.
    //
    // The x0/width expression below mirrors hud_menu_frame.cpp's draw loop exactly; both read
    // menuFrameLayout, so this pins that the two derive the SAME rect from it.
    for (u32 sh : {480u, 720u, 1080u}) {
        const u32 sw = sh * 16 / 9;
        const InventoryUI::MenuFrameRects r = InventoryUI::menuFrameLayout(sw, sh);
        const s32 my = static_cast<s32>(r.tabY + r.tabH * 0.5f);

        for (u8 t = 0; t < InventoryUI::MENU_TABS; t++) {
            const f32 x0 = r.tabX + (r.tabW + r.tabGap) * static_cast<f32>(t);   // plate's left
            const f32 x1 = x0 + r.tabW;                                          // ...and right
            // Just inside each edge, and the middle.
            const s32 probes[3] = { static_cast<s32>(x0 + 1.0f),
                                    static_cast<s32>(x0 + r.tabW * 0.5f),
                                    static_cast<s32>(x1 - 2.0f) };
            for (u32 p = 0; p < 3; p++) {
                const InventoryUI::SlotHit h = InventoryUI::hitTestMenuTabs(sw, sh, probes[p], my);
                CAPTURE(sh); CAPTURE(t); CAPTURE(p);
                REQUIRE(h.panel == InventoryUI::SlotHit::MENU_TAB);
                REQUIRE(h.index == t);
            }
        }

        // ...and just OUTSIDE the first tab's left edge is no tab at all — the other half of the
        // same agreement, which an "is it inside something" test would miss.
        const InventoryUI::SlotHit before =
            InventoryUI::hitTestMenuTabs(sw, sh, static_cast<s32>(r.tabX - 2.0f), my);
        CAPTURE(sh);
        REQUIRE(before.panel == InventoryUI::SlotHit::NONE);
    }
}

TEST_CASE("the tab strip fits inside the frame and its tabs do not overlap") {
    const u32 sw = 1280, sh = 720;
    const InventoryUI::MenuFrameRects r = InventoryUI::menuFrameLayout(sw, sh);
    // Last tab's right edge stays inside the panel — an overhanging tab is drawn but unclickable
    // where it leaves the frame, which reads as a dead button.
    const f32 lastRight = r.tabX + (r.tabW + r.tabGap) * (InventoryUI::MENU_TABS - 1) + r.tabW;
    REQUIRE(lastRight <= r.x + r.w);
    REQUIRE(r.tabGap >= 0.0f);   // a negative gap would overlap neighbouring tabs
}

TEST_CASE("a click off the tab strip is not a tab") {
    const u32 sw = 1280, sh = 720;
    const InventoryUI::MenuFrameRects r = InventoryUI::menuFrameLayout(sw, sh);

    // Directly BELOW the strip — this is page content, and treating it as a tab would swallow
    // every click the pages care about.
    {
        const s32 mx = static_cast<s32>(r.tabX + r.tabW * 0.5f);
        const s32 my = static_cast<s32>(r.tabY - 4.0f);
        REQUIRE(InventoryUI::hitTestMenuTabs(sw, sh, mx, my).panel == InventoryUI::SlotHit::NONE);
    }
    // In the GAP between two tabs: a gap is not a tab, the same rule skillSlotAt follows.
    if (r.tabGap >= 2.0f) {
        const s32 mx = static_cast<s32>(r.tabX + r.tabW + r.tabGap * 0.5f);
        const s32 my = static_cast<s32>(r.tabY + r.tabH * 0.5f);
        REQUIRE(InventoryUI::hitTestMenuTabs(sw, sh, mx, my).panel == InventoryUI::SlotHit::NONE);
    }
    // Far right of the strip, past the last tab.
    {
        const s32 mx = static_cast<s32>(r.x + r.w - 4.0f);
        const s32 my = static_cast<s32>(r.tabY + r.tabH * 0.5f);
        REQUIRE(InventoryUI::hitTestMenuTabs(sw, sh, mx, my).panel == InventoryUI::SlotHit::NONE);
    }
}

TEST_CASE("the quest log lays out inside the menu frame's content box") {
    // The quest log used to be a fixed 520-logical-px box floated in the middle of the screen,
    // which overlapped the equipment column on one side and left the right third empty. It is a
    // PAGE now, so it must live entirely within the frame that hosts it.
    for (u32 sh : {480u, 720u, 1080u}) {
        const u32 sw = sh * 16 / 9;
        const InventoryUI::MenuFrameRects m = InventoryUI::menuFrameLayout(sw, sh);
        const InventoryUI::JournalRects   j = InventoryUI::journalLayout(sw, sh);
        CAPTURE(sh);
        REQUIRE(j.listX >= m.contentX);
        REQUIRE(j.detailX + j.detailW <= m.contentX + m.contentW);
        REQUIRE(j.tabY + j.tabH <= m.contentY + m.contentH);   // act tabs below the page tabs
        REQUIRE(j.listTopY <= j.tabY);                          // list starts under the act tabs
        REQUIRE(j.detailW > 0.0f);
    }
}

TEST_CASE("the act tabs never collide with the page tabs") {
    // Two rows of tabs, one above the other. If they overlapped, a click meant for ACT II would
    // land on the CHARACTER page and the player would be thrown off the quest log entirely.
    const u32 sw = 1280, sh = 720;
    const InventoryUI::MenuFrameRects m = InventoryUI::menuFrameLayout(sw, sh);
    const InventoryUI::JournalRects   j = InventoryUI::journalLayout(sw, sh);

    const s32 mx = static_cast<s32>(j.tabX + j.tabW * 0.5f);
    const s32 my = static_cast<s32>(j.tabY + j.tabH * 0.5f);
    REQUIRE(InventoryUI::hitTestMenuTabs(sw, sh, mx, my).panel == InventoryUI::SlotHit::NONE);
    REQUIRE(j.tabY + j.tabH <= m.tabY);
}
