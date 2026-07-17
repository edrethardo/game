#pragma once

#include "core/types.h"
#include "game/item.h"  // ItemSlot::COUNT — keeps the equipment-panel row count in lockstep

// Shared layout constants and hit-testing for inventory, equipment, and quickbar panels.
// Used by both HUD rendering (hud.cpp) and interaction logic (engine.cpp).

namespace InventoryUI {
    // Backpack grid (6 columns x 4 rows, right side of screen)
    static constexpr f32 BP_CELL = 32.0f;
    static constexpr f32 BP_GAP  = 4.0f;
    static constexpr u32 BP_COLS = 6;
    static constexpr u32 BP_ROWS = 4;

    // Equipment panel (left side)
    static constexpr f32 EQ_W   = 240.0f;
    static constexpr f32 EQ_H   = 32.0f;
    static constexpr f32 EQ_GAP = 5.0f;
    // Derived from the item-slot enum so adding a slot (e.g. GLOVES) automatically extends the
    // equipment panel's controller cursor range AND its mouse hit-test (was a hardcoded 6, which
    // left a newly-added 7th slot unreachable on controller-only platforms like Switch).
    static constexpr u32 EQ_SLOTS = static_cast<u32>(ItemSlot::COUNT);

    // Quickbar (bottom center). The slot COUNT is QUICKBAR_SLOTS from item.h — never redeclare it
    // here. A local QB_SLOTS=8 used to shadow it after the bar shrank to 4, so hitTest built rects
    // for 8 phantom slots and every consumer (which bounds-check against the real 4) rejected the
    // resulting index — silently killing drag-assign, reorder and remove at every resolution.
    static constexpr f32 QB_SIZE = 40.0f;
    static constexpr f32 QB_GAP  = 4.0f;

    struct SlotHit {
        enum Panel : u8 { NONE, BACKPACK, EQUIPMENT, QUICKBAR, STASH, STASH_TAB };
        Panel panel = NONE;
        u8    index = 0;
    };

    // ---- Account stash panel (drawn over the equipment area while the stash is open) ----
    // THE single source for the stash grid + page tabs: HUD::drawStashPanel and hitTestStash
    // both derive from stashLayout() — the quickbarLayout discipline (draw and hit-test that
    // each re-derive geometry WILL drift).
    static constexpr u32 STASH_COLS = 8;
    static constexpr u32 STASH_ROWS = 6;
    static constexpr u32 STASH_TABS = 5;
    struct StashRects {
        f32 x = 0.0f, startY = 0.0f;      // slot (col 0, row 0)'s left/bottom
        f32 cell = 0.0f, gap = 0.0f;      // scaled
        f32 tabX = 0.0f, tabY = 0.0f;     // tab 0's left/bottom
        f32 tabW = 0.0f, tabH = 0.0f, tabGap = 0.0f;
    };
    StashRects stashLayout(u32 sw, u32 sh);
    // Hit-test ONLY the stash panel (slots + tabs). The caller checks this before the regular
    // hitTest while the stash is open — the panel overlaps the (hidden) equipment area.
    SlotHit hitTestStash(u32 sw, u32 sh, s32 mx, s32 my);

    // Hit-test a mouse position (HUD coords, Y=0 at bottom) against all panels.
    SlotHit hitTest(u32 sw, u32 sh, s32 mx, s32 my);

    // Returns true if position is inside any inventory/quickbar panel.
    bool isInsideAnyPanel(u32 sw, u32 sh, s32 mx, s32 my);

    // ---- Skill bars (class + equipment) ----
    // The in-game HUD anchors these itself; the inventory screen re-draws the SAME bars in the SAME
    // place, and needs to hit-test their slots for hover tooltips. Rather than let the geometry
    // become a FIFTH copy (it already lives in inventory_ui, hud_inventory's draw + hover, and two
    // cursor->mouse blocks), both callers go through skillBarLayout() below.
    static constexpr f32 SKILL_SLOT = 64.0f;
    static constexpr f32 SKILL_GAP  = 4.0f;
    static constexpr u32 CLASS_SKILL_SLOTS     = 4;
    static constexpr u32 MAX_EQUIP_SKILL_SLOTS = 6;
    // Rightward nudge of the whole bottom action cluster (skill bars + quickbar) so it clears the
    // potion flask in the bottom-left gutter. Mirrored by engine_hud.cpp's kFlaskClusterShift —
    // this is the definition; that one derives from it.
    static constexpr f32 FLASK_CLUSTER_SHIFT = 100.0f;

    // Resolved quickbar geometry, in HUD coords (origin bottom-left, Y up). startX/baseY are the
    // LEFT/BOTTOM edge of slot 0; slot s spans [startX + s*(slot+gap), +slot] x [baseY, +slot].
    // THE single source for where the quickbar is: HUD::drawQuickbar, InventoryUI::hitTest and
    // skillBarLayout all derive from this. They previously each re-derived it and disagreed —
    // draw applied uiScale + FLASK_CLUSTER_SHIFT, hit-test applied neither — so the clickable
    // rects did not overlap the drawn bar at ANY resolution.
    struct QuickbarRects {
        f32 startX = 0.0f, baseY = 0.0f;
        f32 slot   = 0.0f, gap   = 0.0f;   // already scaled by uiScale
    };

    QuickbarRects quickbarLayout(u32 sw, u32 sh);

    // Resolved bar geometry, in HUD coords (origin bottom-left, Y up). x/y are the LEFT/BOTTOM edge
    // of each bar's first slot; slot s spans [x + s*(slot+gap), +slot] x [y, y+slot].
    // equipCount == 0 means no equipment skills are equipped: there is no equip bar to hit-test.
    struct SkillBarRects {
        f32 classX = 0.0f, classY = 0.0f;
        f32 equipX = 0.0f, equipY = 0.0f;
        f32 slot   = 0.0f, gap    = 0.0f;   // already scaled by uiScale
        u32 equipCount = 0;
    };

    // Mirrors the in-game anchor math (engine_hud.cpp renderSkillsHUD): the class bar sits to the
    // LEFT of the quickbar, the equip bar is centred 8px above it.
    SkillBarRects skillBarLayout(u32 sw, u32 sh, u32 equipCount);

    // Hit-test a HUD-coord position against the two bars. Returns false if it lands on neither
    // (including the gaps BETWEEN slots — a gap is not a slot).
    bool skillSlotAt(const SkillBarRects& r, s32 mx, s32 my, bool& outIsClassBar, u8& outIndex);
}
