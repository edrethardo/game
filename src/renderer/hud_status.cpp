// hud_status.cpp — HUD status effect icons, speech bubbles, damage vignette,
// and damage direction indicator. Part of the HUD namespace split from hud.cpp.
// Calls pushLine/pushQuad/flushHUD via hud_internal.h.

#include "renderer/hud.h"
#include "renderer/hud_internal.h"
#include "renderer/font.h"
#include <cmath>
#include <cstdio>

// 8x8 pixel-art icons for each status effect
// 0=transparent, 1=primary, 2=secondary, 3=detail, 4=highlight
static const u8 kIconPoison[8][8] = {
    {0,0,0,0,0,0,0,0},
    {0,0,0,1,1,0,0,0},
    {0,0,1,4,4,1,0,0},
    {0,1,4,2,2,4,1,0},
    {0,1,2,1,1,2,1,0},
    {0,0,1,3,3,1,0,0},
    {0,0,0,3,3,0,0,0},
    {0,0,0,0,3,0,0,0},
};
static const u8 kIconBurn[8][8] = {
    {0,0,0,0,0,0,0,0},
    {0,0,0,4,0,0,0,0},
    {0,0,4,4,4,0,0,0},
    {0,0,4,1,4,0,0,0},
    {0,4,1,1,1,4,0,0},
    {0,4,1,2,1,4,0,0},
    {0,3,4,2,4,3,0,0},
    {0,0,3,3,3,0,0,0},
};
static const u8 kIconFreeze[8][8] = {
    {0,0,0,4,4,0,0,0},
    {0,0,3,0,0,3,0,0},
    {0,3,0,1,1,0,3,0},
    {4,0,1,4,4,1,0,4},
    {4,0,1,4,4,1,0,4},
    {0,3,0,1,1,0,3,0},
    {0,0,3,0,0,3,0,0},
    {0,0,0,4,4,0,0,0},
};
static const u8 kIconSlow[8][8] = {
    {0,0,0,0,0,0,0,0},
    {0,0,1,1,1,1,0,0},
    {0,1,4,4,4,4,1,0},
    {0,1,4,2,3,0,1,0},
    {0,1,4,3,0,0,1,0},
    {0,1,4,0,0,0,1,0},
    {0,0,1,1,1,1,0,0},
    {0,0,0,0,0,0,0,0},
};
static const u8 kIconInvuln[8][8] = {
    {0,0,0,0,0,0,0,0},
    {0,1,1,1,1,1,1,0},
    {0,1,4,4,4,4,1,0},
    {0,1,4,2,2,4,1,0},
    {0,1,4,2,2,4,1,0},
    {0,0,1,4,4,1,0,0},
    {0,0,0,1,1,0,0,0},
    {0,0,0,0,0,0,0,0},
};
// Soul Harvest — skull icon
static const u8 kIconSoulHarvest[8][8] = {
    {0,0,1,1,1,1,0,0},
    {0,1,4,4,4,4,1,0},
    {0,1,2,4,4,2,1,0},
    {0,1,4,4,4,4,1,0},
    {0,0,1,2,2,1,0,0},
    {0,0,0,1,1,0,0,0},
    {0,0,1,3,3,1,0,0},
    {0,0,0,1,1,0,0,0},
};
// Adrenaline (Wanderer) — electric lightning bolt; stack count shown as displayValue
static const u8 kIconAdrenaline[8][8] = {
    {0,0,0,0,1,4,0,0},
    {0,0,0,1,4,2,0,0},
    {0,0,1,4,2,0,0,0},
    {0,1,4,4,4,2,2,0},
    {0,2,2,4,4,1,0,0},
    {0,0,0,4,1,0,0,0},
    {0,0,1,4,0,0,0,0},
    {0,1,4,0,0,0,0,0},
};

static const u8* getStatusIcon(u32 idx) {
    static const u8* icons[] = {
        &kIconPoison[0][0], &kIconBurn[0][0], &kIconFreeze[0][0],
        &kIconSlow[0][0], &kIconInvuln[0][0], &kIconSoulHarvest[0][0],
        &kIconAdrenaline[0][0]
    };
    return (idx < 7) ? icons[idx] : nullptr;
}

// Status-specific color palettes: [0]=unused, [1]=primary, [2]=secondary, [3]=detail, [4]=highlight
static void getStatusColors(u32 idx, Vec3 cols[5]) {
    cols[0] = {0,0,0};
    switch (idx) {
        case 0: // Poison
            cols[1] = {0.15f, 0.55f, 0.15f}; cols[2] = {0.1f, 0.35f, 0.1f};
            cols[3] = {0.2f, 0.4f, 0.1f};    cols[4] = {0.3f, 0.9f, 0.3f};
            break;
        case 1: // Burn
            cols[1] = {0.9f, 0.45f, 0.05f};  cols[2] = {0.7f, 0.2f, 0.05f};
            cols[3] = {0.5f, 0.15f, 0.05f};  cols[4] = {1.0f, 0.85f, 0.2f};
            break;
        case 2: // Freeze
            cols[1] = {0.3f, 0.6f, 0.9f};    cols[2] = {0.15f, 0.35f, 0.6f};
            cols[3] = {0.2f, 0.45f, 0.7f};   cols[4] = {0.8f, 0.92f, 1.0f};
            break;
        case 3: // Slow
            cols[1] = {0.5f, 0.25f, 0.7f};   cols[2] = {0.3f, 0.1f, 0.5f};
            cols[3] = {0.4f, 0.2f, 0.6f};    cols[4] = {0.75f, 0.5f, 1.0f};
            break;
        case 4: // Invulnerable
            cols[1] = {0.8f, 0.65f, 0.2f};   cols[2] = {0.6f, 0.5f, 0.15f};
            cols[3] = {0.5f, 0.4f, 0.1f};    cols[4] = {1.0f, 0.95f, 0.5f};
            break;
        case 5: // Soul Harvest
            cols[1] = {0.7f, 0.3f, 0.1f};    cols[2] = {0.5f, 0.15f, 0.05f};
            cols[3] = {0.4f, 0.2f, 0.1f};    cols[4] = {1.0f, 0.6f, 0.2f};
            break;
        case 6: // Adrenaline (electric yellow)
            cols[1] = {0.95f, 0.8f, 0.1f};   cols[2] = {0.7f, 0.45f, 0.05f};
            cols[3] = {0.8f, 0.6f, 0.1f};    cols[4] = {1.0f, 1.0f, 0.6f};
            break;
        default:
            cols[1] = cols[2] = cols[3] = cols[4] = {0.5f, 0.5f, 0.5f};
            break;
    }
}

void HUD::drawStatusIcons(u32 sw, u32 sh, f32 x, f32 y,
                            const StatusEffect* effects, u32 count)
{
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    f32 iconSize = 28.0f * uiScale;
    f32 gap = 5.0f * uiScale;
    f32 cx = x;

    for (u32 i = 0; i < count; i++) {
        if (effects[i].timer <= 0.0f) continue;

        // Pulsing brightness when timer is low (<2s)
        // Blink when effect is about to wear off
        f32 pulse = 1.0f;
        f32 t = effects[i].timer;
        if (t < 1.0f) {
            pulse = 0.1f + 0.9f * (0.5f + 0.5f * sinf(t * 12.0f)); // urgent fast blink, 10-100%
        } else if (t < 3.0f) {
            pulse = 0.3f + 0.7f * (0.5f + 0.5f * sinf(t * 6.0f));  // moderate blink, 30-100%
        } else if (t < 5.0f) {
            pulse = 0.3f + 0.7f * (0.5f + 0.5f * sinf(t * 3.0f));  // slow blink, 30-100%
        }

        // Background fill
        Vec3 bg = effects[i].color * 0.15f;
        for (f32 fy = 0; fy < iconSize; fy += 1.0f) {
            pushLine(cx, y + fy, cx + iconSize, y + fy, bg);
        }

        // Border in status color
        Vec3 borderCol = effects[i].color * (0.5f + 0.5f * pulse);
        pushQuad(cx, y, cx + iconSize, y + iconSize, borderCol);

        // Draw 8x8 pixel-art icon scaled to 24x24 (3px per pixel), centered
        const u8* icon = getStatusIcon(i);
        if (icon) {
            Vec3 cols[5];
            getStatusColors(i, cols);
            f32 px = 3.0f * uiScale; // pixel scale
            f32 iconX = cx + 2.0f * uiScale;
            f32 iconY = y + 2.0f * uiScale;
            for (u32 iy = 0; iy < 8; iy++) {
                for (u32 ix = 0; ix < 8; ix++) {
                    u8 c = icon[iy * 8 + ix];
                    if (c == 0) continue;
                    f32 pxX = iconX + ix * px;
                    f32 pxY = iconY + (7 - iy) * px; // flip Y
                    Vec3 col = cols[c] * pulse;
                    for (f32 fy = 0; fy < px; fy += 1.0f) {
                        pushLine(pxX, pxY + fy, pxX + px, pxY + fy, col);
                    }
                }
            }
        }

        flushHUD();

        // Timer or display value text above icon
        char timeTxt[8];
        f32 showVal = (effects[i].displayValue >= 0.0f) ? effects[i].displayValue : effects[i].timer;
        std::snprintf(timeTxt, sizeof(timeTxt), "%.0f", showVal);
        f32 tw = FontSystem::textWidth(timeTxt, 1);
        FontSystem::drawText(sw, sh, cx + (iconSize - tw) * 0.5f, y + iconSize + 2.0f * uiScale,
                             timeTxt, effects[i].color * pulse, 1);

        cx += iconSize + gap;
    }
}

void HUD::drawSpeechBubble(u32 sw, u32 sh, f32 x, f32 y,
                            const char* text, Vec3 textColor, f32 alpha) {
    if (!text || alpha <= 0.0f) return;

    f32 textW = FontSystem::textWidth(text, 1);
    f32 textH = FontSystem::textHeight(1);
    f32 padX = 6.0f, padY = 4.0f;
    f32 bgX0 = x - textW * 0.5f - padX;
    f32 bgY0 = y - padY;
    f32 bgX1 = x + textW * 0.5f + padX;
    f32 bgY1 = y + textH + padY;

    // Semi-transparent dark background drawn as filled horizontal lines
    Vec3 bgColor = {0.06f * alpha, 0.06f * alpha, 0.1f * alpha};
    for (f32 fy = bgY0; fy < bgY1; fy += 1.0f) {
        pushLine(bgX0, fy, bgX1, fy, bgColor);
    }

    // Border in a dimmed version of the text color
    Vec3 borderColor = {textColor.x * 0.5f * alpha,
                        textColor.y * 0.5f * alpha,
                        textColor.z * 0.5f * alpha};
    pushQuad(bgX0, bgY0, bgX1, bgY1, borderColor);

    // Small triangle pointer below bubble pointing toward the entity head
    f32 triX = x;
    f32 triY = bgY0 - 4.0f;
    pushLine(triX - 3.0f, bgY0, triX,        triY, borderColor);
    pushLine(triX,        triY, triX + 3.0f, bgY0, borderColor);

    flushHUD();

    // Text centered inside the bubble
    Vec3 tc = {textColor.x * alpha, textColor.y * alpha, textColor.z * alpha};
    FontSystem::drawText(sw, sh, x - textW * 0.5f, y, text, tc, 1);
}

// Red vignette overlay — draws red gradient lines along all four screen edges.
// Intensity 0..1 controls opacity; used when the player takes damage.
void HUD::drawDamageVignette(u32 sw, u32 sh, f32 intensity) {
    if (intensity <= 0.0f) return;
    if (intensity > 1.0f) intensity = 1.0f;

    f32 depth = static_cast<f32>(sh) * 0.12f;
    f32 fW = static_cast<f32>(sw);
    f32 fH = static_cast<f32>(sh);

    // Top edge
    for (f32 d = 0; d < depth; d += 1.0f) {
        f32 lineAlpha = (1.0f - d / depth) * intensity;
        Vec3 c = {0.8f * lineAlpha, 0.0f, 0.0f};
        pushLine(0, fH - d, fW, fH - d, c);
    }
    // Bottom edge
    for (f32 d = 0; d < depth; d += 1.0f) {
        f32 lineAlpha = (1.0f - d / depth) * intensity;
        Vec3 c = {0.8f * lineAlpha, 0.0f, 0.0f};
        pushLine(0, d, fW, d, c);
    }
    // Left edge
    for (f32 d = 0; d < depth; d += 1.0f) {
        f32 lineAlpha = (1.0f - d / depth) * intensity;
        Vec3 c = {0.8f * lineAlpha, 0.0f, 0.0f};
        pushLine(d, 0, d, fH, c);
    }
    // Right edge
    for (f32 d = 0; d < depth; d += 1.0f) {
        f32 lineAlpha = (1.0f - d / depth) * intensity;
        Vec3 c = {0.8f * lineAlpha, 0.0f, 0.0f};
        pushLine(fW - d, 0, fW - d, fH, c);
    }
    flushHUD();
}

void HUD::drawDamageDirection(u32 sw, u32 sh, f32 angle, f32 alpha) {
    if (alpha <= 0.0f) return;
    f32 cx = static_cast<f32>(sw) * 0.5f;
    f32 cy = static_cast<f32>(sh) * 0.5f;
    f32 r = 60.0f;
    f32 arcHalf = 0.35f; // ~20° half-width
    Vec3 c = {0.9f * alpha, 0.15f * alpha, 0.1f * alpha};
    // 3 line segments forming a short arc around crosshair
    for (int s = -1; s <= 1; s++) {
        f32 a0 = angle + s * arcHalf * 0.67f;
        f32 a1 = angle + (s + 1) * arcHalf * 0.67f;
        f32 x0 = cx + sinf(a0) * r, y0 = cy - cosf(a0) * r;
        f32 x1 = cx + sinf(a1) * r, y1 = cy - cosf(a1) * r;
        pushLine(x0, y0, x1, y1, c);
    }
    flushHUD();
}
