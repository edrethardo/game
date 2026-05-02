#pragma once

#include "core/types.h"

struct ItemDef;
enum struct Rarity : u8;

// Procedural 16x16 pixel item icon atlas.
// Each weapon subtype and armor slot type gets a distinct silhouette.
// Icons are white silhouettes with alpha — rarity tint applied at draw time.

static constexpr u32 ICON_SIZE        = 16;
static constexpr u32 ICON_ATLAS_COLS  = 16;
static constexpr u32 ICON_ATLAS_ROWS  = 2;

namespace ItemIconSystem {
    // Generate icon atlas texture. Call once after OpenGL context is ready.
    void init();
    void shutdown();

    // Draw an item icon as a textured quad at (x,y) with given size.
    // Uses the unlit shader; tints by rarity color.
    void drawIcon(u32 screenWidth, u32 screenHeight,
                  f32 x, f32 y, f32 size,
                  const ItemDef& def, Rarity rarity);
}
