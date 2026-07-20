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

    // Global UI scale multiplier (1.0 = default, 1.3 = 30% larger)
    void  setUIScale(f32 s);
    f32   getUIScale();

    // Draw text at pixel position (x,y = bottom-left of first glyph).
    // scale: pixel multiplier (1.0 = 5x7, 2.0 = 10x14, 1.5 = 7x10, etc.)
    void drawText(u32 screenWidth, u32 screenHeight,
                  f32 x, f32 y, const char* text,
                  Vec3 color, f32 scale = 1.0f);

    // Same as drawText but first stamps the glyphs offset in every direction in
    // outlineColor (default black), then the fill on top — a 1-glyph-pixel border
    // that keeps light HUD text (e.g. the white ammo readout) legible over bright
    // backgrounds like the town's sand floor. Costs 8 extra draw calls, so reserve
    // it for the few HUD strings that overlap variable-brightness terrain.
    void drawTextOutlined(u32 screenWidth, u32 screenHeight,
                          f32 x, f32 y, const char* text,
                          Vec3 color, f32 scale = 1.0f,
                          Vec3 outlineColor = Vec3{0.0f, 0.0f, 0.0f});

    // Returns width in pixels of the given text at the given scale
    f32 textWidth(const char* text, f32 scale = 1.0f);

    // Returns height in pixels at the given scale
    f32 textHeight(f32 scale = 1.0f);
}
