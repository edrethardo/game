#include "game/inventory_ui.h"

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
    {
        f32 totalW = QB_SLOTS * QB_SIZE + (QB_SLOTS - 1) * QB_GAP;
        f32 startX = (static_cast<f32>(sw) - totalW) * 0.5f;
        f32 baseY = 20.0f;

        for (u32 i = 0; i < QB_SLOTS; i++) {
            f32 x = startX + static_cast<f32>(i) * (QB_SIZE + QB_GAP);
            if (fmx >= x && fmx <= x + QB_SIZE && fmy >= baseY && fmy <= baseY + QB_SIZE) {
                result.panel = SlotHit::QUICKBAR;
                result.index = static_cast<u8>(i);
                return result;
            }
        }
    }

    return result; // NONE
}

bool InventoryUI::isInsideAnyPanel(u32 sw, u32 sh, s32 mx, s32 my) {
    return hitTest(sw, sh, mx, my).panel != SlotHit::NONE;
}
