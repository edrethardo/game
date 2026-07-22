#include "game/inventory_ui.h"

InventoryUI::StashRects InventoryUI::stashLayout(u32 sw, u32 sh) {
    StashRects r;
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    r.cell   = BP_CELL * uiScale;
    r.gap    = BP_GAP * uiScale;
    r.x      = static_cast<f32>(sw) * 0.06f;
    r.startY = static_cast<f32>(sh) * 0.5f + 180.0f * uiScale;   // row 0 aligned with the backpack
    r.tabW   = 40.0f * uiScale;
    r.tabH   = 22.0f * uiScale;
    r.tabGap = 6.0f * uiScale;
    r.tabX   = r.x;
    r.tabY   = r.startY + r.cell + 14.0f * uiScale;
    return r;
}

InventoryUI::BuildGridRects InventoryUI::buildGridLayout(u32 sw, u32 sh) {
    // Right column, below the equipment paper-doll area: 3 cells across fits comfortably where
    // tooltips do not reach. Row 0 (Tanky) is drawn at the TOP, so cell (row,col) sits at
    // y = gridY + (2 - row) * (cell + gap) — hit-test and draw both use this via the rects.
    BuildGridRects r;
    const f32 uiScale = static_cast<f32>(sh) / 720.0f;
    r.cell    = 54.0f * uiScale;
    r.gap     = 8.0f  * uiScale;
    r.gridX   = static_cast<f32>(sw) * 0.72f;
    r.gridY   = static_cast<f32>(sh) * 0.5f - 200.0f * uiScale;
    r.toggleW = 3.0f * r.cell + 2.0f * r.gap;
    r.toggleH = 26.0f * uiScale;
    r.toggleX = r.gridX;
    r.toggleY = r.gridY + 3.0f * (r.cell + r.gap) + 6.0f * uiScale;
    return r;
}

InventoryUI::SlotHit InventoryUI::hitTestBuildGrid(u32 sw, u32 sh, s32 mx, s32 my) {
    SlotHit result;
    const BuildGridRects r = buildGridLayout(sw, sh);
    const f32 fmx = static_cast<f32>(mx), fmy = static_cast<f32>(my);
    if (fmx >= r.toggleX && fmx <= r.toggleX + r.toggleW &&
        fmy >= r.toggleY && fmy <= r.toggleY + r.toggleH) {
        result.panel = SlotHit::BUILD_TOGGLE;
        return result;
    }
    for (u8 row = 0; row < 3; row++)
        for (u8 col = 0; col < 3; col++) {
            const f32 x = r.gridX + col * (r.cell + r.gap);
            const f32 y = r.gridY + (2 - row) * (r.cell + r.gap);
            if (fmx >= x && fmx <= x + r.cell && fmy >= y && fmy <= y + r.cell) {
                result.panel = SlotHit::BUILD_CELL;
                result.index = static_cast<u8>(row * 3 + col);
                return result;
            }
        }
    return result;
}

InventoryUI::SlotHit InventoryUI::hitTestStash(u32 sw, u32 sh, s32 mx, s32 my) {
    SlotHit result;
    const StashRects r = stashLayout(sw, sh);
    f32 fmx = static_cast<f32>(mx), fmy = static_cast<f32>(my);
    for (u32 t = 0; t < STASH_TABS; t++) {
        f32 x = r.tabX + static_cast<f32>(t) * (r.tabW + r.tabGap);
        if (fmx >= x && fmx <= x + r.tabW && fmy >= r.tabY && fmy <= r.tabY + r.tabH) {
            result.panel = SlotHit::STASH_TAB;
            result.index = static_cast<u8>(t);
            return result;
        }
    }
    for (u32 i = 0; i < STASH_COLS * STASH_ROWS; i++) {
        u32 col = i % STASH_COLS, row = i / STASH_COLS;
        f32 x = r.x + static_cast<f32>(col) * (r.cell + r.gap);
        f32 y = r.startY - static_cast<f32>(row) * (r.cell + r.gap);
        if (fmx >= x && fmx <= x + r.cell && fmy >= y && fmy <= y + r.cell) {
            result.panel = SlotHit::STASH;
            result.index = static_cast<u8>(i);
            return result;
        }
    }
    return result;
}

InventoryUI::SlotHit InventoryUI::hitTest(u32 sw, u32 sh, s32 mx, s32 my) {
    SlotHit result;
    f32 fmx = static_cast<f32>(mx);
    f32 fmy = static_cast<f32>(my);

    // Scale relative to 720p reference so split-screen viewports work
    f32 uiScale = static_cast<f32>(sh) / 720.0f;

    // --- Backpack grid (right side) ---
    {
        f32 bpX = static_cast<f32>(sw) * 0.42f;
        f32 bpStartY = static_cast<f32>(sh) * 0.5f + 180.0f * uiScale;
        f32 cell = BP_CELL * uiScale;
        f32 gap  = BP_GAP * uiScale;

        for (u32 i = 0; i < BP_COLS * BP_ROWS; i++) {
            u32 col = i % BP_COLS;
            u32 row = i / BP_COLS;
            f32 x = bpX + static_cast<f32>(col) * (cell + gap);
            f32 y = bpStartY - static_cast<f32>(row) * (cell + gap);

            if (fmx >= x && fmx <= x + cell && fmy >= y && fmy <= y + cell) {
                result.panel = SlotHit::BACKPACK;
                result.index = static_cast<u8>(i);
                return result;
            }
        }
    }

    // --- Equipment panel (left side) ---
    {
        f32 eqX = static_cast<f32>(sw) * 0.12f;
        f32 centerY = static_cast<f32>(sh) * 0.5f;
        f32 eqStartY = centerY + 220.0f * uiScale;
        f32 eqW = EQ_W * uiScale;
        f32 eqH = EQ_H * uiScale;
        f32 eqGap = EQ_GAP * uiScale;

        for (u32 i = 0; i < EQ_SLOTS; i++) {
            f32 y = eqStartY - static_cast<f32>(i) * (eqH + eqGap);
            if (fmx >= eqX && fmx <= eqX + eqW && fmy >= y && fmy <= y + eqH) {
                result.panel = SlotHit::EQUIPMENT;
                result.index = static_cast<u8>(i);
                return result;
            }
        }
    }

    // --- Quickbar (bottom center) ---
    // Geometry comes from the shared quickbarLayout() so these rects land exactly on the drawn
    // slots. Iterates QUICKBAR_SLOTS, so the returned index is always one the Quickbar:: API will
    // accept — which is what makes drag-assign / reorder / remove work at all.
    {
        const QuickbarRects qb = quickbarLayout(sw, sh);
        for (u32 i = 0; i < QUICKBAR_SLOTS; i++) {
            f32 x = qb.startX + static_cast<f32>(i) * (qb.slot + qb.gap);
            if (fmx >= x && fmx <= x + qb.slot && fmy >= qb.baseY && fmy <= qb.baseY + qb.slot) {
                result.panel = SlotHit::QUICKBAR;
                result.index = static_cast<u8>(i);
                return result;
            }
        }
    }

    return result; // NONE
}

// The one definition of where the quickbar sits. Mirrors HUD::drawQuickbar's anchor: the bar is
// centred horizontally, then nudged right by FLASK_CLUSTER_SHIFT to clear the potion flask.
InventoryUI::QuickbarRects InventoryUI::quickbarLayout(u32 sw, u32 sh) {
    QuickbarRects r;
    const f32 uiScale = static_cast<f32>(sh) / 720.0f;
    r.slot = QB_SIZE * uiScale;
    r.gap  = QB_GAP  * uiScale;

    const f32 totalW = QUICKBAR_SLOTS * r.slot + (QUICKBAR_SLOTS - 1) * r.gap;
    r.startX = (static_cast<f32>(sw) - totalW) * 0.5f + FLASK_CLUSTER_SHIFT * uiScale;
    r.baseY  = 20.0f * uiScale;
    return r;
}

bool InventoryUI::isInsideAnyPanel(u32 sw, u32 sh, s32 mx, s32 my) {
    return hitTest(sw, sh, mx, my).panel != SlotHit::NONE;
}

// Anchor math for the two skill bars, kept here (pure, testable) instead of copied into the HUD and
// the inventory screen. Mirrors renderSkillsHUD: the class bar sits immediately LEFT of the
// quickbar, and the equip bar is centred 8px above it. The whole cluster is nudged right by
// FLASK_CLUSTER_SHIFT to clear the potion flask.
InventoryUI::SkillBarRects InventoryUI::skillBarLayout(u32 sw, u32 sh, u32 equipCount) {
    SkillBarRects r;
    const f32 uiScale = static_cast<f32>(sh) / 720.0f;
    r.slot = SKILL_SLOT * uiScale;
    r.gap  = SKILL_GAP  * uiScale;
    if (equipCount > MAX_EQUIP_SKILL_SLOTS) equipCount = MAX_EQUIP_SKILL_SLOTS;
    r.equipCount = equipCount;

    // Anchor off the shared quickbar layout rather than re-deriving it, so the class bar can never
    // drift away from the left edge of the bar it is supposed to sit flush against.
    const f32 qbX = quickbarLayout(sw, sh).startX;

    const f32 classW = CLASS_SKILL_SLOTS * r.slot + (CLASS_SKILL_SLOTS - 1) * r.gap;
    r.classX = qbX - classW - 12.0f * uiScale;
    r.classY = 14.0f * uiScale;

    if (equipCount > 0) {
        const f32 equipW = equipCount * r.slot + (equipCount - 1) * r.gap;
        r.equipX = r.classX + (classW - equipW) * 0.5f;   // centred over the class bar
        r.equipY = r.classY + r.slot + 8.0f * uiScale;    // one slot + 8px above
    }
    return r;
}

bool InventoryUI::skillSlotAt(const SkillBarRects& r, s32 mx, s32 my,
                              bool& outIsClassBar, u8& outIndex) {
    const f32 fmx = static_cast<f32>(mx);
    const f32 fmy = static_cast<f32>(my);
    const f32 stride = r.slot + r.gap;

    // Class bar (always present).
    if (fmy >= r.classY && fmy <= r.classY + r.slot) {
        for (u32 s = 0; s < CLASS_SKILL_SLOTS; s++) {
            const f32 sx = r.classX + s * stride;
            // Upper bound is sx + slot, NOT sx + stride — the gap between two slots belongs to
            // neither, so hovering it must show no tooltip rather than the slot to its left.
            if (fmx >= sx && fmx <= sx + r.slot) {
                outIsClassBar = true;
                outIndex = static_cast<u8>(s);
                return true;
            }
        }
    }
    // Equip bar (only when something is equipped that grants a skill).
    if (r.equipCount > 0 && fmy >= r.equipY && fmy <= r.equipY + r.slot) {
        for (u32 s = 0; s < r.equipCount; s++) {
            const f32 sx = r.equipX + s * stride;
            if (fmx >= sx && fmx <= sx + r.slot) {
                outIsClassBar = false;
                outIndex = static_cast<u8>(s);
                return true;
            }
        }
    }
    return false;
}
