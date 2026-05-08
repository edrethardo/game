#include "game/inventory_ui.h"

InventoryUI::SlotHit InventoryUI::hitTest(u32 sw, u32 sh, s32 mx, s32 my) {
    SlotHit result;
    f32 fmx = static_cast<f32>(mx);
    f32 fmy = static_cast<f32>(my);

    // --- Backpack grid (right side) ---
    {
        f32 bpX = static_cast<f32>(sw) * 0.42f;
        f32 bpStartY = static_cast<f32>(sh) * 0.5f + 180.0f;

        for (u32 i = 0; i < BP_COLS * BP_ROWS; i++) {
            u32 col = i % BP_COLS;
            u32 row = i / BP_COLS;
            f32 x = bpX + static_cast<f32>(col) * (BP_CELL + BP_GAP);
            f32 y = bpStartY - static_cast<f32>(row) * (BP_CELL + BP_GAP);

            if (fmx >= x && fmx <= x + BP_CELL && fmy >= y && fmy <= y + BP_CELL) {
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
        f32 eqStartY = centerY + 220.0f;

        for (u32 i = 0; i < EQ_SLOTS; i++) {
            f32 y = eqStartY - static_cast<f32>(i) * (EQ_H + EQ_GAP);
            if (fmx >= eqX && fmx <= eqX + EQ_W && fmy >= y && fmy <= y + EQ_H) {
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
