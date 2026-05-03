#pragma once

#include "core/types.h"

// Shared layout constants and hit-testing for inventory, equipment, and quickbar panels.
// Used by both HUD rendering (hud.cpp) and interaction logic (engine.cpp).

namespace InventoryUI {
    // Backpack grid (6 columns x 4 rows, right side of screen)
    static constexpr f32 BP_CELL = 26.0f;
    static constexpr f32 BP_GAP  = 3.0f;
    static constexpr u32 BP_COLS = 6;
    static constexpr u32 BP_ROWS = 4;

    // Equipment panel (left side)
    static constexpr f32 EQ_W   = 140.0f;
    static constexpr f32 EQ_H   = 26.0f;
    static constexpr f32 EQ_GAP = 4.0f;
    static constexpr u32 EQ_SLOTS = 6;

    // Quickbar (bottom center)
    static constexpr f32 QB_SIZE = 40.0f;
    static constexpr f32 QB_GAP  = 4.0f;
    static constexpr u32 QB_SLOTS = 8;

    struct SlotHit {
        enum Panel : u8 { NONE, BACKPACK, EQUIPMENT, QUICKBAR };
        Panel panel = NONE;
        u8    index = 0;
    };

    // Hit-test a mouse position (HUD coords, Y=0 at bottom) against all panels.
    SlotHit hitTest(u32 sw, u32 sh, s32 mx, s32 my);

    // Returns true if position is inside any inventory/quickbar panel.
    bool isInsideAnyPanel(u32 sw, u32 sh, s32 mx, s32 my);
}
