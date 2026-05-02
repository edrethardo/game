#pragma once

#include "core/types.h"
#include "core/math.h"

// Simple bitmap font for HUD text rendering.
// Glyphs are 5x7 pixels, packed into a texture atlas.
// Supports ASCII 32-126 (printable characters).

static constexpr u32 GLYPH_W = 5;
static constexpr u32 GLYPH_H = 7;
static constexpr u32 GLYPH_SPACING = 1;  // pixels between chars
static constexpr u32 FONT_FIRST_CHAR = 32;
static constexpr u32 FONT_LAST_CHAR = 126;
static constexpr u32 FONT_CHAR_COUNT = FONT_LAST_CHAR - FONT_FIRST_CHAR + 1;

namespace FontSystem {
    // Initialize font: generates glyph atlas texture. Call once at startup.
    void init();
    void shutdown();

    // Draw text at pixel position (x,y = bottom-left of first glyph).
    // scale: pixel multiplier (1 = 5x7, 2 = 10x14, etc.)
    void drawText(u32 screenWidth, u32 screenHeight,
                  f32 x, f32 y, const char* text,
                  Vec3 color, u32 scale = 1);

    // Returns width in pixels of the given text at the given scale
    f32 textWidth(const char* text, u32 scale = 1);

    // Returns height in pixels at the given scale
    f32 textHeight(u32 scale = 1);
}
