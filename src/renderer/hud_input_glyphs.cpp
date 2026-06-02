// hud_input_glyphs.cpp — HUD input-device symbol drawing (keyboard keys,
// controller buttons, mouse buttons). Part of the HUD namespace split from
// hud.cpp. Calls pushLine/pushQuad/flushHUD from hud.cpp via hud_internal.h.

#include "renderer/hud.h"
#include "renderer/hud_internal.h"
#include "renderer/font.h"
#include "platform/input.h"
#include <cstring>
#include <cmath>

// Check if a label is a known controller button name
static bool isControllerLabel(const char* label) {
    static const char* kNames[] = {
        "A", "B", "X", "Y", "L", "R", "ZL", "ZR",
        "L3", "R3",                           // stick clicks — drawn as a small gray circle
        "+", "-", "Up", "Rt", "Dn", "Lt",
        "L+A", "L+B", nullptr
    };
    for (const char** p = kNames; *p; p++) {
        if (std::strcmp(label, *p) == 0) return true;
    }
    return false;
}

// On Switch, input is already swapped at the reading level (swapSdlButtonForSwitch),
// so SDL "A" corresponds to the physical A button. No display swap needed.
static const char* switchButtonLabel(const char* sdlLabel) {
    return sdlLabel;
}

static Vec3 faceButtonColor(char c) {
    // Colors match the Nintendo physical label after swap
    switch (c) {
        case 'A': return {0.3f, 0.75f, 0.4f};   // green accent
        case 'B': return {0.8f, 0.3f, 0.3f};     // red accent
        case 'X': return {0.3f, 0.5f, 0.85f};    // blue accent
        case 'Y': return {0.85f, 0.75f, 0.2f};   // yellow accent
        default:  return {0.45f, 0.45f, 0.5f};
    }
}

void HUD::drawControllerButton(u32 sw, u32 sh, f32 x, f32 y,
                                 const char* label, bool highlighted)
{
    f32 dim = highlighted ? 1.0f : 0.55f;

    // Face buttons (A/B/X/Y) — filled circle with letter (swap for Nintendo layout)
    if (label[0] >= 'A' && label[0] <= 'Y' && label[1] == '\0') {
        const char* displayLabel = switchButtonLabel(label);
        f32 r = 9.0f;
        f32 cx = x + r;
        f32 cy = y + r;
        Vec3 col = faceButtonColor(displayLabel[0]) * dim;
        // Fill circle with scanlines
        for (f32 dy = -r; dy <= r; dy += 1.0f) {
            f32 hw = sqrtf(r * r - dy * dy);
            pushLine(cx - hw, cy + dy, cx + hw, cy + dy, col);
        }
        // Darker border ring
        Vec3 border = col * 0.6f;
        for (f32 a = 0; a < 6.28f; a += 0.15f) {
            f32 px = cx + cosf(a) * r;
            f32 py = cy + sinf(a) * r;
            pushLine(px, py, px + 0.5f, py, border);
        }
        flushHUD();
        // Letter centered (using Nintendo-swapped label)
        Vec3 tc = {1.0f * dim, 1.0f * dim, 1.0f * dim};
        f32 tw = FontSystem::textWidth(displayLabel, 1);
        FontSystem::drawText(sw, sh, cx - tw * 0.5f, cy - 3.0f, displayLabel, tc, 1);
        return;
    }

    // Combo buttons (L+A, L+B) — pill + face circle
    if (label[0] == 'L' && label[1] == '+') {
        // Draw L pill
        drawControllerButton(sw, sh, x, y, "L", highlighted);
        // Small + text
        FontSystem::drawText(sw, sh, x + 22.0f, y + 5.0f, "+",
                             {0.6f * dim, 0.6f * dim, 0.6f * dim}, 1);
        // Draw face button circle (drawControllerButton handles the swap internally)
        char face[2] = {label[2], 0};
        drawControllerButton(sw, sh, x + 28.0f, y, face, highlighted);
        return;
    }

    // Stick click (L3/R3) — small unsaturated circle with the label, evoking a
    // top-down view of an analog stick that's been pressed in. Smaller than face
    // buttons so the visual weight reads as "auxiliary action" rather than "primary".
    if ((label[0] == 'L' || label[0] == 'R') && label[1] == '3' && label[2] == '\0') {
        f32 r = 7.0f;
        f32 cx = x + r;
        f32 cy = y + r;
        Vec3 col = Vec3{0.42f, 0.42f, 0.48f} * dim;
        // Filled disc — scanline fill matches the face-button look.
        for (f32 dy = -r; dy <= r; dy += 1.0f) {
            f32 hw = sqrtf(r * r - dy * dy);
            pushLine(cx - hw, cy + dy, cx + hw, cy + dy, col);
        }
        // Outline ring — slightly darker than the fill, same step-angle pattern as A/B/X/Y.
        Vec3 border = col * 0.55f;
        for (f32 a = 0; a < 6.28f; a += 0.18f) {
            f32 px = cx + cosf(a) * r;
            f32 py = cy + sinf(a) * r;
            pushLine(px, py, px + 0.5f, py, border);
        }
        flushHUD();
        Vec3 tc = {0.92f * dim, 0.92f * dim, 0.95f * dim};
        f32 tw = FontSystem::textWidth(label, 1);
        FontSystem::drawText(sw, sh, cx - tw * 0.5f, cy - 3.0f, label, tc, 1);
        return;
    }

    // Shoulder/trigger (L/R/ZL/ZR) — rounded pill shape
    if ((label[0] == 'L' || label[0] == 'R') && (label[1] == '\0' || label[1] == 'L' || label[1] == 'R') ||
        (label[0] == 'Z')) {
        bool isTrigger = (label[0] == 'Z');
        Vec3 col = isTrigger ? Vec3{0.3f, 0.3f, 0.38f} * dim : Vec3{0.4f, 0.42f, 0.48f} * dim;
        f32 pw = FontSystem::textWidth(label, 1) + 10.0f;
        f32 ph = 16.0f;
        // Rounded rect fill
        for (f32 fy = 2; fy < ph - 2; fy += 1.0f)
            pushLine(x + 2, y + fy, x + pw - 2, y + fy, col);
        // Border
        Vec3 border = col * 1.4f;
        pushLine(x + 2, y + ph, x + pw - 2, y + ph, border);
        pushLine(x + 2, y, x + pw - 2, y, border);
        pushLine(x, y + 2, x, y + ph - 2, border);
        pushLine(x + pw, y + 2, x + pw, y + ph - 2, border);
        flushHUD();
        Vec3 tc = {0.9f * dim, 0.9f * dim, 0.95f * dim};
        f32 tw = FontSystem::textWidth(label, 1);
        FontSystem::drawText(sw, sh, x + (pw - tw) * 0.5f, y + 4.0f, label, tc, 1);
        return;
    }

    // D-pad directions (Up/Rt/Dn/Lt) — cross shape with highlighted arm
    if (std::strcmp(label, "Up") == 0 || std::strcmp(label, "Dn") == 0 ||
        std::strcmp(label, "Lt") == 0 || std::strcmp(label, "Rt") == 0) {
        f32 cx = x + 9.0f, cy = y + 9.0f;
        f32 arm = 6.0f, w = 4.0f;
        Vec3 base = {0.3f, 0.3f, 0.35f};
        Vec3 hi   = {0.7f, 0.75f, 0.8f};
        // Draw cross arms
        bool isUp = (label[0] == 'U'), isDn = (label[0] == 'D');
        bool isLt = (label[0] == 'L'), isRt = (label[0] == 'R');
        // Vertical arm (Y increases upward: positive fy = up on screen)
        for (f32 fy = -arm; fy <= arm; fy += 1.0f)
            pushLine(cx - w*0.5f, cy + fy, cx + w*0.5f, cy + fy,
                     (fy > 0 && isUp) || (fy < 0 && isDn) ? hi * dim : base * dim);
        // Horizontal arm
        for (f32 fy = -w*0.5f; fy <= w*0.5f; fy += 1.0f)
            pushLine(cx - arm, cy + fy, cx + arm, cy + fy,
                     (isLt || isRt) ? base * dim : base * dim);
        // Highlight specific direction
        if (isLt) for (f32 fy = -w*0.5f; fy <= w*0.5f; fy++) pushLine(cx - arm, cy+fy, cx - 1, cy+fy, hi*dim);
        if (isRt) for (f32 fy = -w*0.5f; fy <= w*0.5f; fy++) pushLine(cx + 1, cy+fy, cx + arm, cy+fy, hi*dim);
        flushHUD();
        return;
    }

    // System buttons (+/-) — small rounded rect
    {
        Vec3 col = {0.35f, 0.35f, 0.4f};
        f32 pw = 14.0f, ph = 14.0f;
        for (f32 fy = 2; fy < ph - 2; fy += 1.0f)
            pushLine(x + 2, y + fy, x + pw - 2, y + fy, col * dim);
        Vec3 border = {0.55f * dim, 0.55f * dim, 0.6f * dim};
        pushLine(x + 2, y + ph, x + pw - 2, y + ph, border);
        pushLine(x + 2, y, x + pw - 2, y, border);
        pushLine(x, y + 2, x, y + ph - 2, border);
        pushLine(x + pw, y + 2, x + pw, y + ph - 2, border);
        flushHUD();
        Vec3 tc = {0.9f * dim, 0.9f * dim, 0.9f * dim};
        f32 tw = FontSystem::textWidth(label, 1);
        FontSystem::drawText(sw, sh, x + (pw - tw) * 0.5f, y + 3.0f, label, tc, 1);
    }
}

void HUD::drawKeySymbol(u32 sw, u32 sh, f32 x, f32 y,
                          const char* label, bool highlighted)
{
    // Auto-detect controller button labels and draw proper symbols
    if (Input::isGamepadConnected(0) && isControllerLabel(label)) {
        drawControllerButton(sw, sh, x, y, label, highlighted);
        return;
    }

    f32 kw = 18.0f, kh = 18.0f;

    // Key background — darker when not highlighted
    Vec3 bg = highlighted ? Vec3{0.2f, 0.22f, 0.28f} : Vec3{0.12f, 0.12f, 0.16f};
    for (f32 fy = 1; fy < kh - 1; fy += 1.0f) {
        pushLine(x + 1, y + fy, x + kw - 1, y + fy, bg);
    }

    // Key border — raised edge look (lighter top/left, darker bottom/right)
    Vec3 hi = highlighted ? Vec3{0.8f, 0.85f, 0.9f} : Vec3{0.45f, 0.45f, 0.5f};
    Vec3 lo = highlighted ? Vec3{0.4f, 0.42f, 0.48f} : Vec3{0.25f, 0.25f, 0.3f};
    pushLine(x, y + kh, x + kw, y + kh, hi);       // top
    pushLine(x, y, x, y + kh, hi);                   // left
    pushLine(x + kw, y, x + kw, y + kh, lo);         // right
    pushLine(x, y, x + kw, y, lo);                    // bottom

    flushHUD();

    // Label text centered
    f32 tw = FontSystem::textWidth(label, 1);
    Vec3 tc = highlighted ? Vec3{1.0f, 1.0f, 1.0f} : Vec3{0.7f, 0.7f, 0.75f};
    FontSystem::drawText(sw, sh, x + (kw - tw) * 0.5f, y + 5.0f, label, tc, 1);
}

void HUD::drawMouseButton(u32 sw, u32 sh, f32 x, f32 y,
                            u8 button, bool highlighted)
{
    f32 mw = 16.0f, mh = 22.0f;

    // Mouse body outline
    Vec3 outline = highlighted ? Vec3{0.6f, 0.6f, 0.7f} : Vec3{0.35f, 0.35f, 0.4f};

    // Body fill
    Vec3 bodyBg = {0.1f, 0.1f, 0.14f};
    for (f32 fy = 2; fy < mh - 2; fy += 1.0f) {
        pushLine(x + 2, y + fy, x + mw - 2, y + fy, bodyBg);
    }

    // Outline — rounded-ish mouse shape
    pushLine(x + 2, y, x + mw - 2, y, outline);               // bottom
    pushLine(x + 2, y + mh, x + mw - 2, y + mh, outline);     // top (rounded)
    pushLine(x, y + 2, x, y + mh - 2, outline);                 // left
    pushLine(x + mw, y + 2, x + mw, y + mh - 2, outline);      // right
    // Round corners
    pushLine(x + 1, y + 1, x + 2, y, outline);
    pushLine(x + mw - 1, y + 1, x + mw - 2, y, outline);
    pushLine(x + 1, y + mh - 1, x + 2, y + mh, outline);
    pushLine(x + mw - 1, y + mh - 1, x + mw - 2, y + mh, outline);

    // Divider line between buttons (at 60% height)
    f32 divY = y + mh * 0.6f;
    pushLine(x + 2, divY, x + mw - 2, divY, outline);
    // Center divider between left and right buttons
    f32 midX = x + mw * 0.5f;
    pushLine(midX, divY, midX, y + mh - 2, outline);

    // Highlight the active button
    Vec3 activeCol = highlighted ? Vec3{0.4f, 0.9f, 0.5f} : Vec3{0.3f, 0.6f, 0.35f};
    if (button == 0) {
        // Left button — left half above divider
        for (f32 fy = divY + 1; fy < y + mh - 2; fy += 1.0f)
            pushLine(x + 2, fy, midX - 1, fy, activeCol);
    } else if (button == 1) {
        // Right button — right half above divider
        for (f32 fy = divY + 1; fy < y + mh - 2; fy += 1.0f)
            pushLine(midX + 1, fy, x + mw - 2, fy, activeCol);
    } else if (button == 2) {
        // Middle — small dot between buttons at the divider
        pushLine(midX - 1, divY - 2, midX + 1, divY - 2, activeCol);
        pushLine(midX - 1, divY - 1, midX + 1, divY - 1, activeCol);
    }

    // Scroll wheel indicator (small notch at top-center)
    pushLine(midX, y + mh - 4, midX, y + mh - 2, outline);

    flushHUD();
}
