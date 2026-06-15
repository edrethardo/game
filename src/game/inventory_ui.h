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
