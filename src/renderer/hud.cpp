#include "renderer/hud.h"
#include "renderer/shader.h"
#include "renderer/font.h"
#include "renderer/item_icons.h"
#include "renderer/material.h"
#include "platform/input.h"
#include "core/log.h"
#include "core/profiler.h"
#include "game/item.h"
#include <glad/glad.h>
#include <cstdio>
#include <cstring>

// Simple 2D line renderer for HUD elements (crosshair, hit markers).
// Uses a small dynamic VBO with position+color, drawn with the debug shader
// but with an orthographic VP matrix mapping pixels to NDC.

struct HudVertex {
    Vec3 pos;
    Vec3 color;
};

static constexpr u32 MAX_HUD_VERTS = 2048;

static u32    s_vao = 0;
static u32    s_vbo = 0;
static Shader s_shader;
static HudVertex s_verts[MAX_HUD_VERTS];
static u32 s_vertCount = 0;

static void pushLine(f32 x0, f32 y0, f32 x1, f32 y1, Vec3 color) {
    if (s_vertCount + 2 > MAX_HUD_VERTS) return;
    s_verts[s_vertCount++] = {{x0, y0, 0.0f}, color};
    s_verts[s_vertCount++] = {{x1, y1, 0.0f}, color};
}

static void pushQuad(f32 x0, f32 y0, f32 x1, f32 y1, Vec3 color) {
    // Draw as 3 lines forming a filled-looking outline
    pushLine(x0, y0, x1, y0, color);
    pushLine(x0, y1, x1, y1, color);
    pushLine(x0, y0, x0, y1, color);
    pushLine(x1, y0, x1, y1, color);
}

static void flushHUD(u32 screenWidth, u32 screenHeight) {
    if (s_vertCount == 0 || !s_vao) return;

    glDisable(GL_DEPTH_TEST);

    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, s_vertCount * sizeof(HudVertex), s_verts);

    glUseProgram(s_shader.program);

    // Build ortho projection: (0,0) = bottom-left, (w,h) = top-right
    Mat4 ortho;
    f32 w = static_cast<f32>(screenWidth);
    f32 h = static_cast<f32>(screenHeight);
    // Simple orthographic: map [0,w] x [0,h] to [-1,1] x [-1,1]
    ortho = Mat4::identity();
    ortho.m[0]  =  2.0f / w;
    ortho.m[5]  =  2.0f / h;
    ortho.m[10] = -1.0f;
    ortho.m[12] = -1.0f;
    ortho.m[13] = -1.0f;

    if (s_shader.loc_vp >= 0) glUniformMatrix4fv(s_shader.loc_vp, 1, GL_FALSE, ortho.ptr());

    glBindVertexArray(s_vao);
    glLineWidth(2.0f);
    glDrawArrays(GL_LINES, 0, s_vertCount);
    glLineWidth(1.0f);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);

    s_vertCount = 0;
}

void HUD::init() {
    s_shader = ShaderSystem::load(ASSET_PATH("assets/shaders/debug.vert"), ASSET_PATH("assets/shaders/debug.frag"));

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);

    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_verts), nullptr, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(HudVertex), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(HudVertex), (void*)(sizeof(Vec3)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    LOG_INFO("HUD initialized");
}

void HUD::shutdown() {
    if (s_vao) { glDeleteVertexArrays(1, &s_vao); s_vao = 0; }
    if (s_vbo) { glDeleteBuffers(1, &s_vbo); s_vbo = 0; }
    ShaderSystem::destroy(s_shader);
}

void HUD::drawCrosshair(u32 screenWidth, u32 screenHeight, Vec3 color) {
    f32 cx = screenWidth  * 0.5f;
    f32 cy = screenHeight * 0.5f;
    f32 gap  = 4.0f;
    f32 size = 12.0f;

    // Four lines with a gap in the centre
    pushLine(cx - size, cy, cx - gap, cy, color);  // left
    pushLine(cx + gap,  cy, cx + size, cy, color);  // right
    pushLine(cx, cy - size, cx, cy - gap, color);   // bottom
    pushLine(cx, cy + gap,  cx, cy + size, color);  // top

    flushHUD(screenWidth, screenHeight);
}

void HUD::drawHitMarker(u32 screenWidth, u32 screenHeight, f32 alpha) {
    f32 cx = screenWidth  * 0.5f;
    f32 cy = screenHeight * 0.5f;
    f32 size = 8.0f;
    Vec3 color = {alpha, alpha, alpha};

    // X shape
    pushLine(cx - size, cy - size, cx - 3, cy - 3, color);
    pushLine(cx + 3,    cy + 3,    cx + size, cy + size, color);
    pushLine(cx + size, cy - size, cx + 3, cy - 3, color);
    pushLine(cx - 3,    cy + 3,    cx - size, cy + size, color);

    flushHUD(screenWidth, screenHeight);
}

void HUD::drawHealthBar(u32 screenWidth, u32 screenHeight,
                         f32 health, f32 maxHealth)
{
    f32 barW = 200.0f;
    f32 barH = 16.0f;
    f32 x0 = 20.0f;
    f32 y0 = 20.0f;

    f32 frac = (maxHealth > 0.0f) ? health / maxHealth : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    // Background outline
    pushQuad(x0, y0, x0 + barW, y0 + barH, {0.3f, 0.3f, 0.3f});

    // Filled portion (draw as horizontal lines to simulate fill)
    Vec3 barColor = (frac > 0.3f) ? Vec3{0.2f, 0.8f, 0.2f} : Vec3{0.9f, 0.2f, 0.2f};
    f32 fillW = barW * frac;
    for (f32 y = y0 + 2; y < y0 + barH - 2; y += 2.0f) {
        pushLine(x0 + 2, y, x0 + 2 + fillW - 4, y, barColor);
    }

    flushHUD(screenWidth, screenHeight);
}

void HUD::drawWeaponIndicator(u32 screenWidth, u32 screenHeight, u8 weaponSlot) {
    f32 x0 = static_cast<f32>(screenWidth) - 120.0f;
    f32 y0 = 20.0f;

    // Color per weapon type
    Vec3 colors[3] = {
        {0.7f, 0.7f, 0.7f}, // 0: melee (grey)
        {1.0f, 0.8f, 0.2f}, // 1: hitscan (gold)
        {0.3f, 0.5f, 1.0f}, // 2: projectile (blue)
    };

    Vec3 c = (weaponSlot < 3) ? colors[weaponSlot] : Vec3{1,1,1};
    pushQuad(x0, y0, x0 + 100, y0 + 16, c);

    flushHUD(screenWidth, screenHeight);
}

void HUD::drawMenuOption(u32 screenWidth, u32 screenHeight,
                          f32 y, f32 width, f32 height,
                          Vec3 color, bool selected)
{
    f32 cx = screenWidth * 0.5f;
    f32 x0 = cx - width * 0.5f;
    f32 x1 = cx + width * 0.5f;

    // Outer box
    pushQuad(x0, y, x1, y + height, color);

    if (selected) {
        // Fill with horizontal lines to show selection
        for (f32 fy = y + 2; fy < y + height - 2; fy += 2.0f) {
            pushLine(x0 + 2, fy, x1 - 2, fy, color);
        }
        // Selection arrows
        pushLine(x0 - 15, y + height * 0.5f, x0 - 5, y + height * 0.3f, color);
        pushLine(x0 - 15, y + height * 0.5f, x0 - 5, y + height * 0.7f, color);
        pushLine(x1 + 15, y + height * 0.5f, x1 + 5, y + height * 0.3f, color);
        pushLine(x1 + 15, y + height * 0.5f, x1 + 5, y + height * 0.7f, color);
    }

    flushHUD(screenWidth, screenHeight);
}

void HUD::drawProfiler(u32 screenWidth, u32 screenHeight) {
    Profiler& prof = getProfiler();
    if (!prof.enabled) return;

    f32 x0 = 20.0f;
    f32 y0 = static_cast<f32>(screenHeight) - 30.0f;

    // Frame time summary bar
    f32 barMaxMs = 33.3f; // 30 FPS reference
    f32 barW = 300.0f;
    f32 barH = 12.0f;

    // Background bar (full budget = 16.67ms reference)
    pushQuad(x0, y0, x0 + barW, y0 + barH, {0.2f, 0.2f, 0.2f});

    // 16.67ms marker line (60 FPS target)
    f32 targetX = x0 + (16.67f / barMaxMs) * barW;
    pushLine(targetX, y0 - 2, targetX, y0 + barH + 2, {0.0f, 0.8f, 0.0f});

    // Per-scope timing bars (stacked)
    f32 xCur = x0;
    Vec3 scopeColors[] = {
        {0.2f, 0.5f, 1.0f}, // Update (blue)
        {1.0f, 0.4f, 0.2f}, // AI (red-orange)
        {0.8f, 0.2f, 1.0f}, // Projectiles (purple)
        {0.2f, 0.8f, 0.4f}, // Render (green)
        {1.0f, 0.8f, 0.2f}, // Flush (yellow)
        {0.6f, 0.6f, 0.6f}, // Other (grey)
        {0.4f, 0.8f, 0.8f}, // Scope 6
        {0.8f, 0.4f, 0.4f}, // Scope 7
    };

    for (u32 i = 0; i < prof.scopeCount && i < 8; i++) {
        f32 w = static_cast<f32>(prof.scopes[i].elapsedMs / barMaxMs) * barW;
        if (w < 1.0f) w = 1.0f;
        Vec3 c = scopeColors[i];

        // Draw filled bar segment
        for (f32 y = y0 + 2; y < y0 + barH - 2; y += 2.0f) {
            pushLine(xCur, y, xCur + w, y, c);
        }
        xCur += w;
    }

    // Scope legend below the bar
    f32 legendY = y0 - 18.0f;
    f32 legendX = x0;
    for (u32 i = 0; i < prof.scopeCount && i < 8; i++) {
        if (!prof.scopes[i].name) continue;
        Vec3 c = scopeColors[i];
        // Small color indicator
        pushLine(legendX, legendY, legendX + 8, legendY, c);
        pushLine(legendX, legendY + 2, legendX + 8, legendY + 2, c);
        legendX += 50.0f;
    }

    // Frame time text-like indicators
    f32 infoY = y0 - 38.0f;
    // Avg frame time bar (scaled)
    f32 avgW = static_cast<f32>(prof.frameTimeAvg / barMaxMs) * barW;
    pushLine(x0, infoY, x0 + avgW, infoY, {0.8f, 0.8f, 0.8f});
    // Min bar
    f32 minW = static_cast<f32>(prof.frameTimeMin / barMaxMs) * barW;
    pushLine(x0, infoY - 4, x0 + minW, infoY - 4, {0.2f, 0.8f, 0.2f});
    // Max bar
    f32 maxW = static_cast<f32>(prof.frameTimeMax / barMaxMs) * barW;
    pushLine(x0, infoY - 8, x0 + maxW, infoY - 8, {0.8f, 0.2f, 0.2f});

    flushHUD(screenWidth, screenHeight);
}

void HUD::drawNetStats(u32 screenWidth, u32 screenHeight,
                        u32 playerCount, u32 ping, const char* role)
{
    (void)role;
    // Top-right corner: small bars indicating player count and ping
    f32 x0 = static_cast<f32>(screenWidth) - 150.0f;
    f32 y0 = static_cast<f32>(screenHeight) - 40.0f;

    // Player count indicator (bars for each player)
    for (u32 i = 0; i < playerCount && i < 4; i++) {
        f32 bx = x0 + i * 15.0f;
        Vec3 c = {0.2f, 0.8f, 0.2f};
        pushQuad(bx, y0, bx + 10, y0 + 20, c);
    }

    // Ping indicator (bar height proportional to ping)
    f32 pingH = (ping < 200) ? (ping / 200.0f) * 20.0f : 20.0f;
    Vec3 pingColor = (ping < 50) ? Vec3{0.2f, 0.8f, 0.2f}
                   : (ping < 100) ? Vec3{0.8f, 0.8f, 0.2f}
                   : Vec3{0.8f, 0.2f, 0.2f};
    pushLine(x0 + 80, y0, x0 + 80, y0 + pingH, pingColor);
    pushLine(x0 + 82, y0, x0 + 82, y0 + pingH, pingColor);

    flushHUD(screenWidth, screenHeight);
}

void HUD::drawEnergyBar(u32 sw, u32 sh, f32 energy, f32 maxEnergy) {
    f32 barW = 200.0f;
    f32 barH = 10.0f;
    f32 x0 = 20.0f;
    // Health bar sits at y0=20 with barH=16, so place energy bar 22px above it
    f32 y0 = 20.0f + 16.0f + 6.0f; // = 42px from bottom

    f32 frac = (maxEnergy > 0.0f) ? energy / maxEnergy : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    // Background outline
    pushQuad(x0, y0, x0 + barW, y0 + barH, {0.3f, 0.3f, 0.3f});

    // Filled portion in blue
    Vec3 barColor = {0.2f, 0.4f, 1.0f};
    f32 fillW = barW * frac;
    for (f32 y = y0 + 2; y < y0 + barH - 2; y += 2.0f) {
        pushLine(x0 + 2, y, x0 + 2 + fillW - 4, y, barColor);
    }

    flushHUD(sw, sh);
}

// Check if a label is a known controller button name
static bool isControllerLabel(const char* label) {
    static const char* kNames[] = {
        "A", "B", "X", "Y", "L", "R", "ZL", "ZR",
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
        flushHUD(sw, sh);
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
        flushHUD(sw, sh);
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
        flushHUD(sw, sh);
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
        flushHUD(sw, sh);
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

    flushHUD(sw, sh);

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

    flushHUD(sw, sh);
}

void HUD::drawClassSkillBar(u32 sw, u32 sh, f32 x, f32 y,
                              u8 activeSlot, u32 currentFloor,
                              const u8* unlockFloors, const u8* upgradeFloors,
                              const f32* cooldownTimers, const f32* maxCooldowns)
{
    f32 slotW = 32.0f, slotH = 32.0f, gap = 3.0f;

    for (u8 s = 0; s < 4; s++) {
        f32 sx = x + s * (slotW + gap);
        bool unlocked = (currentFloor >= unlockFloors[s]);
        bool selected = (s == activeSlot);
        bool upgraded = (currentFloor >= upgradeFloors[s]);

        // Background fill
        Vec3 bgCol = unlocked ? Vec3{0.12f, 0.12f, 0.18f} : Vec3{0.06f, 0.06f, 0.08f};
        if (selected && unlocked) bgCol = {0.16f, 0.2f, 0.3f};
        for (f32 fy = 0; fy < slotH; fy += 1.0f) {
            pushLine(sx, y + fy, sx + slotW, y + fy, bgCol);
        }

        // Border
        Vec3 borderCol = selected ? Vec3{0.4f, 0.9f, 0.5f} : Vec3{0.25f, 0.25f, 0.35f};
        if (!unlocked) borderCol = {0.12f, 0.12f, 0.18f};
        if (upgraded) borderCol = {0.9f, 0.8f, 0.3f};
        pushQuad(sx, y, sx + slotW, y + slotH, borderCol);

        // Cooldown overlay — dark from top, shrinks downward as skill recharges
        if (unlocked && cooldownTimers[s] > 0.0f) {
            f32 maxCD = (maxCooldowns && maxCooldowns[s] > 0.0f) ? maxCooldowns[s] : 1.0f;
            f32 cdFrac = cooldownTimers[s] / maxCD;
            if (cdFrac > 1.0f) cdFrac = 1.0f;
            f32 cdH = slotH * cdFrac;
            Vec3 cdCol = {0.08f, 0.08f, 0.12f};
            // Draw from top of slot downward
            for (f32 fy = 0; fy < cdH; fy += 1.0f) {
                f32 ly = y + slotH - 1 - fy;
                pushLine(sx + 1, ly, sx + slotW - 1, ly, cdCol);
            }
        }

        flushHUD(sw, sh);

        // Key symbol — show D-pad directions on controller, number keys on keyboard
        const char* skillLabel;
        if (Input::isGamepadConnected(0)) {
            static const char* dpadLabels[] = {"Up", "Rt", "Dn", "Lt"};
            skillLabel = dpadLabels[s];
        } else {
            static char numBuf[4][2] = {{'1',0}, {'2',0}, {'3',0}, {'4',0}};
            skillLabel = numBuf[s];
        }
        drawKeySymbol(sw, sh, sx + 7.0f, y + 8.0f, skillLabel, selected && unlocked);

        // Locked text
        if (!unlocked) {
            char lockTxt[8];
            std::snprintf(lockTxt, sizeof(lockTxt), "F%u", unlockFloors[s]);
            FontSystem::drawText(sw, sh, sx + 6.0f, y + 2.0f, lockTxt, {0.35f, 0.25f, 0.25f}, 1);
        }
    }
}

// 8x8 pixel-art icons for legendary equipment skills.
// 0=transparent, 1=primary, 2=secondary, 3=detail, 4=highlight
static const u8 kIconFrozenOrb[8][8] = {
    {0,0,0,2,2,0,0,0},
    {0,0,2,4,4,2,0,0},
    {0,2,4,1,1,4,2,0},
    {2,4,1,1,1,1,4,2},
    {2,4,1,1,1,1,4,2},
    {0,2,4,1,1,4,2,0},
    {0,0,2,4,4,2,0,0},
    {0,0,0,2,2,0,0,0},
};
static const u8 kIconChainLightning[8][8] = {
    {0,0,0,0,4,0,0,0},
    {0,0,0,4,1,0,0,0},
    {0,0,4,1,0,0,0,0},
    {0,0,1,4,4,0,0,0},
    {0,0,0,0,1,4,0,0},
    {0,0,0,4,1,0,0,0},
    {0,0,4,1,0,0,0,0},
    {0,0,1,0,0,0,0,0},
};
static const u8 kIconMeteorStrike[8][8] = {
    {0,0,0,0,0,3,4,0},
    {0,0,0,0,3,4,3,0},
    {0,0,0,3,4,3,0,0},
    {0,0,3,4,3,0,0,0},
    {0,0,1,2,0,0,0,0},
    {0,1,2,2,1,0,0,0},
    {1,2,4,4,2,1,0,0},
    {0,1,2,2,1,0,0,0},
};
static const u8 kIconBloodNova[8][8] = {
    {0,0,0,4,4,0,0,0},
    {0,3,0,0,0,0,3,0},
    {0,0,1,0,0,1,0,0},
    {4,0,0,2,2,0,0,4},
    {4,0,0,2,2,0,0,4},
    {0,0,1,0,0,1,0,0},
    {0,3,0,0,0,0,3,0},
    {0,0,0,4,4,0,0,0},
};
static const u8 kIconPhaseDash[8][8] = {
    {0,0,0,0,0,0,0,4},
    {0,0,0,0,0,0,4,1},
    {3,0,0,0,0,4,1,0},
    {0,3,2,2,4,1,0,0},
    {0,0,3,2,4,1,0,0},
    {0,0,0,4,1,0,0,0},
    {0,0,4,1,0,0,0,0},
    {0,4,0,0,0,0,0,0},
};

// Blazing Arc: horizontal fire sweep icon
static const u8 kIconArcFire[8][8] = {
    {0,0,0,4,4,0,0,0},
    {0,0,4,1,1,4,0,0},
    {0,4,1,0,0,1,4,0},
    {4,1,0,0,0,0,1,4},
    {2,2,0,0,0,0,2,2},
    {1,4,1,0,0,1,4,1},
    {0,1,4,1,1,4,1,0},
    {0,0,1,4,4,1,0,0},
};

// Color palettes per skill icon
static void getSkillIconColors(u8 skillId, Vec3 cols[5]) {
    cols[0] = {0,0,0}; // transparent (unused)
    switch (static_cast<SkillId>(skillId)) {
        case SkillId::FROZEN_ORB:
            cols[1] = {0.5f, 0.8f, 1.0f}; cols[2] = {0.2f, 0.4f, 0.7f};
            cols[3] = {0.3f, 0.5f, 0.8f}; cols[4] = {0.9f, 0.95f, 1.0f};
            break;
        case SkillId::CHAIN_LIGHTNING:
            cols[1] = {0.4f, 0.6f, 1.0f}; cols[2] = {0.2f, 0.3f, 0.6f};
            cols[3] = {0.3f, 0.5f, 0.9f}; cols[4] = {1.0f, 1.0f, 0.6f};
            break;
        case SkillId::METEOR_STRIKE:
            cols[1] = {1.0f, 0.5f, 0.1f}; cols[2] = {0.8f, 0.3f, 0.1f};
            cols[3] = {0.6f, 0.3f, 0.1f}; cols[4] = {1.0f, 0.9f, 0.3f};
            break;
        case SkillId::BLOOD_NOVA:
            cols[1] = {0.8f, 0.1f, 0.1f}; cols[2] = {0.5f, 0.05f, 0.05f};
            cols[3] = {0.6f, 0.15f, 0.1f}; cols[4] = {1.0f, 0.3f, 0.2f};
            break;
        case SkillId::PHASE_DASH:
            cols[1] = {0.3f, 0.8f, 0.5f}; cols[2] = {0.15f, 0.5f, 0.3f};
            cols[3] = {0.2f, 0.6f, 0.4f}; cols[4] = {0.6f, 1.0f, 0.7f};
            break;
        case SkillId::ARC_FIRE:
            cols[1] = {1.0f, 0.5f, 0.1f}; cols[2] = {0.9f, 0.3f, 0.05f};
            cols[3] = {0.7f, 0.2f, 0.05f}; cols[4] = {1.0f, 0.85f, 0.2f};
            break;
        default:
            cols[1] = {0.6f, 0.6f, 0.6f}; cols[2] = {0.3f, 0.3f, 0.3f};
            cols[3] = {0.4f, 0.4f, 0.4f}; cols[4] = {0.9f, 0.9f, 0.9f};
            break;
    }
}

static const u8* getSkillIcon(u8 skillId) {
    switch (static_cast<SkillId>(skillId)) {
        case SkillId::FROZEN_ORB:       return &kIconFrozenOrb[0][0];
        case SkillId::CHAIN_LIGHTNING:  return &kIconChainLightning[0][0];
        case SkillId::METEOR_STRIKE:    return &kIconMeteorStrike[0][0];
        case SkillId::BLOOD_NOVA:       return &kIconBloodNova[0][0];
        case SkillId::PHASE_DASH:       return &kIconPhaseDash[0][0];
        case SkillId::ARC_FIRE:         return &kIconArcFire[0][0];
        default:                        return nullptr;
    }
}

void HUD::drawEquipSkillBar(u32 sw, u32 sh, f32 x, f32 y,
                              const EquipSkillSlot* slots, u32 slotCount)
{
    f32 slotW = 32.0f, slotH = 32.0f, gap = 3.0f;

    for (u32 s = 0; s < slotCount; s++) {
        const EquipSkillSlot& slot = slots[s];
        f32 sx = x + s * (slotW + gap);
        bool ready = (slot.cooldownTimer <= 0.0f);

        // Background fill
        Vec3 bgCol = ready ? Vec3{0.1f, 0.08f, 0.15f} : Vec3{0.06f, 0.06f, 0.08f};
        for (f32 fy = 0; fy < slotH; fy += 1.0f) {
            pushLine(sx, y + fy, sx + slotW, y + fy, bgCol);
        }

        // Border — gold for legendary feel
        Vec3 borderCol = ready ? Vec3{0.7f, 0.55f, 0.2f} : Vec3{0.3f, 0.25f, 0.15f};
        if (slot.isPassive) borderCol = ready ? Vec3{0.5f, 0.4f, 0.7f} : Vec3{0.25f, 0.2f, 0.35f};
        pushQuad(sx, y, sx + slotW, y + slotH, borderCol);

        // Cooldown overlay — dark from top, shrinks downward as skill recharges
        if (slot.cooldownTimer > 0.0f) {
            f32 maxCD = (slot.maxCooldown > 0.0f) ? slot.maxCooldown : 1.0f;
            f32 cdFrac = slot.cooldownTimer / maxCD;
            if (cdFrac > 1.0f) cdFrac = 1.0f;
            f32 cdH = slotH * cdFrac;
            Vec3 cdCol = {0.05f, 0.05f, 0.08f};
            for (f32 fy = 0; fy < cdH; fy += 1.0f) {
                f32 ly = y + slotH - 1 - fy;
                pushLine(sx + 1, ly, sx + slotW - 1, ly, cdCol);
            }
        }

        // Draw 8x8 skill icon scaled to 16x16, centered in the slot
        const u8* icon = getSkillIcon(slot.skillId);
        if (icon) {
            Vec3 cols[5];
            getSkillIconColors(slot.skillId, cols);
            f32 iconX = sx + 8.0f;  // center 16px icon in 32px slot
            f32 iconY = y + 8.0f;
            f32 px = 2.0f; // pixel scale
            for (u32 iy = 0; iy < 8; iy++) {
                for (u32 ix = 0; ix < 8; ix++) {
                    u8 c = icon[iy * 8 + ix];
                    if (c == 0) continue;
                    f32 pxX = iconX + ix * px;
                    f32 pxY = iconY + (7 - iy) * px; // flip Y
                    for (f32 fy = 0; fy < px; fy += 1.0f) {
                        pushLine(pxX, pxY + fy, pxX + px, pxY + fy,
                                 ready ? cols[c] : cols[c] * 0.4f);
                    }
                }
            }
        }

        flushHUD(sw, sh);

        // Key label or "Auto" for passives
        if (slot.isPassive) {
            FontSystem::drawText(sw, sh, sx + 4.0f, y + 2.0f, "auto",
                                 {0.5f, 0.4f, 0.7f}, 1);
        } else {
            drawKeySymbol(sw, sh, sx + 7.0f, y - 20.0f, slot.keyLabel, ready);
        }
    }
}

void HUD::drawSummonPortrait(u32 sw, u32 sh, f32 x, f32 y,
                              const char* name, Vec3 iconColor,
                              f32 healthFrac, u32 count, u8 iconMatId)
{
    f32 boxW = 110.0f;
    f32 boxH = 24.0f;
    f32 iconSz = 18.0f;

    // Background fill
    Vec3 bg = {0.06f, 0.06f, 0.10f};
    for (f32 fy = 0; fy < boxH; fy += 1.0f) {
        pushLine(x, y + fy, x + boxW, y + fy, bg);
    }

    // Border
    pushQuad(x, y, x + boxW, y + boxH, {0.3f, 0.3f, 0.4f});

    // Icon: render pixel-art portrait matching the in-game entity.
    // Embedded 8x8 pixel patterns for drone/swarm/turret icons.
    // Colors: 0=bg, 1=body, 2=body2, 3=leg/detail, 4=eye
    {
        // Determine which icon to draw based on material ID
        // 88=drone, 89=swarm, 90=turret (from materials.json)
        static const u8 droneIcon[8][8] = {
            {0,0,0,0,0,0,3,0}, // row 0 (bottom)
            {0,3,0,0,0,0,0,3},
            {0,0,1,2,2,1,0,0},
            {3,3,1,2,2,1,3,3},
            {0,0,1,1,1,1,0,0},
            {0,3,1,4,4,1,3,0},
            {3,0,0,0,0,0,0,3},
            {0,0,0,0,0,0,0,0},
        };
        static const u8 swarmIcon[8][8] = {
            {0,0,0,0,0,0,0,0},
            {0,0,0,0,0,0,0,0},
            {0,0,1,1,1,1,0,0},
            {0,1,1,1,1,1,1,0},
            {0,0,1,4,4,1,0,0},
            {0,0,3,1,1,3,0,0},
            {0,3,0,0,0,0,3,0},
            {0,0,0,0,0,0,0,0},
        };
        static const u8 turretIcon[8][8] = {
            {0,0,0,0,0,0,0,0},
            {0,0,0,0,0,0,0,0},
            {0,0,0,1,1,0,0,0},
            {0,0,1,2,2,1,0,0},
            {0,0,1,2,2,1,0,0},
            {0,0,0,3,1,0,0,0},
            {0,0,0,3,0,0,0,0},
            {0,0,0,4,0,0,0,0},
        };

        // Pick icon and colors based on iconMatId
        const u8 (*icon)[8] = droneIcon;
        Vec3 colors[5];
        if (iconMatId == 88) { // drone
            icon = droneIcon;
            colors[0] = {0.06f, 0.06f, 0.08f}; // bg
            colors[1] = {0.35f, 0.33f, 0.40f}; // body
            colors[2] = {0.27f, 0.25f, 0.32f}; // body2
            colors[3] = {0.24f, 0.22f, 0.28f}; // legs
            colors[4] = {0.86f, 0.16f, 0.12f}; // red eyes
        } else if (iconMatId == 89) { // swarm
            icon = swarmIcon;
            colors[0] = {0.06f, 0.06f, 0.08f};
            colors[1] = {0.31f, 0.29f, 0.35f};
            colors[2] = {0.27f, 0.25f, 0.32f};
            colors[3] = {0.24f, 0.22f, 0.28f};
            colors[4] = {0.59f, 0.71f, 0.90f}; // pale blue eye
        } else if (iconMatId == 90) { // turret
            icon = turretIcon;
            colors[0] = {0.06f, 0.06f, 0.08f};
            colors[1] = {0.33f, 0.31f, 0.37f};
            colors[2] = {0.27f, 0.25f, 0.32f};
            colors[3] = {0.39f, 0.39f, 0.45f}; // barrel
            colors[4] = {0.86f, 0.20f, 0.12f}; // red dot
        } else {
            // Fallback: solid color square
            icon = nullptr;
        }

        f32 ix = x + 3, iy = y + 3;
        f32 pxSz = iconSz / 8.0f; // pixel size in screen units

        if (icon) {
            for (u32 py = 0; py < 8; py++) {
                for (u32 px = 0; px < 8; px++) {
                    u8 ci = icon[py][px];
                    if (ci == 0) continue; // skip background pixels
                    Vec3 c = colors[ci];
                    f32 px0 = ix + px * pxSz;
                    f32 py0 = iy + py * pxSz;
                    for (f32 fy = 0; fy < pxSz; fy += 1.0f) {
                        pushLine(px0, py0 + fy, px0 + pxSz, py0 + fy, c);
                    }
                }
            }
        } else {
            for (f32 fy = 0; fy < iconSz; fy += 1.0f) {
                pushLine(ix, iy + fy, ix + iconSz, iy + fy, iconColor);
            }
        }
        flushHUD(sw, sh);
    }

    // Name text
    f32 textX = x + iconSz + 6;
    char label[32];
    if (count > 1) {
        std::snprintf(label, sizeof(label), "%s x%u", name, count);
    } else {
        std::snprintf(label, sizeof(label), "%s", name);
    }
    FontSystem::drawText(sw, sh, textX, y + 12, label, {0.8f, 0.8f, 0.9f}, 1);

    // Health bar (if requested)
    if (healthFrac >= 0.0f) {
        f32 barX = textX;
        f32 barW = boxW - iconSz - 12;
        f32 barH = 4.0f;
        f32 barY = y + 3;
        if (healthFrac > 1.0f) healthFrac = 1.0f;
        Vec3 hpBg = {0.15f, 0.15f, 0.2f};
        Vec3 hpCol = (healthFrac > 0.5f) ? Vec3{0.2f, 0.8f, 0.3f} : Vec3{0.9f, 0.3f, 0.1f};
        for (f32 fy = 0; fy < barH; fy += 1.0f) {
            pushLine(barX, barY + fy, barX + barW, barY + fy, hpBg);
        }
        for (f32 fy = 0; fy < barH; fy += 1.0f) {
            pushLine(barX, barY + fy, barX + barW * healthFrac, barY + fy, hpCol);
        }
        flushHUD(sw, sh);
    }
}

void HUD::drawSkillCooldown(u32 sw, u32 sh, f32 cooldownPct) {
    // Weapon indicator is at (sw - 120, 20) with size 100x16.
    // Place the skill cooldown square just below it.
    f32 x0 = static_cast<f32>(sw) - 120.0f;
    f32 y0 = 20.0f + 16.0f + 6.0f; // below weapon indicator
    f32 size = 16.0f;

    if (cooldownPct < 0.0f) cooldownPct = 0.0f;
    if (cooldownPct > 1.0f) cooldownPct = 1.0f;

    // Outline
    Vec3 outlineColor = (cooldownPct == 0.0f) ? Vec3{0.0f, 1.0f, 1.0f} : Vec3{0.2f, 0.2f, 0.3f};
    pushQuad(x0, y0, x0 + size, y0 + size, outlineColor);

    // Fill from bottom to top proportional to (1 - cooldownPct)
    f32 fillAmount = 1.0f - cooldownPct;
    f32 fillH = size * fillAmount;
    Vec3 fillColor = (cooldownPct == 0.0f) ? Vec3{0.0f, 1.0f, 1.0f} : Vec3{0.2f, 0.2f, 0.3f};
    for (f32 y = y0 + 2; y < y0 + 2 + fillH - 2; y += 1.0f) {
        pushLine(x0 + 2, y, x0 + size - 2, y, fillColor);
    }

    flushHUD(sw, sh);
}

void HUD::drawQuickbar(u32 sw, u32 sh,
                        const QuickbarState& qb,
                        const PlayerInventory& inv,
                        const ItemDef* itemDefs,
                        f32 cooldownPct) {
    static constexpr f32 SLOT_SIZE = 40.0f;
    static constexpr f32 SLOT_GAP  = 4.0f;
    // Total width of all 8 slots plus gaps between them
    static constexpr f32 TOTAL_W   = QUICKBAR_SLOTS * SLOT_SIZE + (QUICKBAR_SLOTS - 1) * SLOT_GAP;
    static constexpr f32 Y_OFFSET  = 20.0f; // distance from bottom edge

    f32 startX = (static_cast<f32>(sw) - TOTAL_W) * 0.5f;
    f32 baseY  = Y_OFFSET;

    for (u32 i = 0; i < QUICKBAR_SLOTS; i++) {
        f32 x0 = startX + static_cast<f32>(i) * (SLOT_SIZE + SLOT_GAP);
        f32 y0 = baseY;
        f32 x1 = x0 + SLOT_SIZE;
        f32 y1 = y0 + SLOT_SIZE;

        bool active = (i == qb.activeSlot);

        // Slot background — dark fill, warmer tint for active slot
        Vec3 bgColor = active ? Vec3{0.25f, 0.22f, 0.15f} : Vec3{0.1f, 0.1f, 0.12f};
        for (f32 fy = y0 + 1; fy < y1 - 1; fy += 2.0f) {
            pushLine(x0 + 1, fy, x1 - 1, fy, bgColor);
        }

        // Border — gold for active, grey for inactive
        Vec3 borderColor = active ? Vec3{1.0f, 0.85f, 0.3f} : Vec3{0.35f, 0.35f, 0.4f};
        pushQuad(x0, y0, x1, y1, borderColor);

        // Slot number label (top-left corner of slot, 1-indexed)
        char numStr[4];
        std::snprintf(numStr, sizeof(numStr), "%u", i + 1);
        FontSystem::drawText(sw, sh, x0 + 2, y1 - 10, numStr,
                             active ? Vec3{1.0f, 0.9f, 0.5f} : Vec3{0.5f, 0.5f, 0.5f}, 1);

        // Resolve the item currently assigned to this slot
        const ItemInstance* item = Quickbar::resolveSlot(qb, inv, static_cast<u8>(i));
        if (item && !isItemEmpty(*item)) {
            const ItemDef& def = itemDefs[item->defId];
            Vec3 rc = rarityColor(item->rarity);

            // Rarity-colored fill in the inner area (above the number label)
            for (f32 fy = y0 + 4; fy < y1 - 12; fy += 2.0f) {
                pushLine(x0 + 4, fy, x1 - 4, fy, rc * 0.5f);
            }

            // Item icon centered within the slot, inset by 4px on each side
            ItemIconSystem::drawIcon(sw, sh, x0 + 4, y0 + 4, SLOT_SIZE - 8, def, item->rarity);

            // Abbreviated item name below the slot (first 5 chars)
            char abbrev[6] = {};
            std::strncpy(abbrev, def.name, 5);
            f32 textW = FontSystem::textWidth(abbrev, 1);
            f32 textX = x0 + (SLOT_SIZE - textW) * 0.5f;
            FontSystem::drawText(sw, sh, textX, y0 - 10, abbrev, rc, 1);

            // Hand marker on the currently equipped weapon's slot (bottom-right corner)
            // Check if this item matches the equipped weapon by UID
            const ItemInstance& eqWpn = inv.equipped[static_cast<u32>(ItemSlot::WEAPON)];
            if (!isItemEmpty(eqWpn) && item->uid == eqWpn.uid) {
                Vec3 hc = {1.0f, 0.9f, 0.6f}; // warm white
                f32 hx = x1 - 12.0f;
                f32 hy = y0 + 2.0f;
                // Palm
                pushLine(hx + 2, hy,     hx + 8, hy,     hc);
                pushLine(hx + 2, hy + 1, hx + 8, hy + 1, hc);
                pushLine(hx + 2, hy + 2, hx + 8, hy + 2, hc);
                pushLine(hx + 1, hy + 3, hx + 9, hy + 3, hc);
                // Fingers
                pushLine(hx + 2, hy + 4, hx + 3, hy + 4, hc);
                pushLine(hx + 4, hy + 4, hx + 5, hy + 4, hc);
                pushLine(hx + 6, hy + 4, hx + 7, hy + 4, hc);
                pushLine(hx + 8, hy + 4, hx + 9, hy + 4, hc);
                pushLine(hx + 2, hy + 5, hx + 3, hy + 5, hc);
                pushLine(hx + 4, hy + 5, hx + 5, hy + 5, hc);
                pushLine(hx + 6, hy + 5, hx + 7, hy + 5, hc);
                pushLine(hx + 8, hy + 5, hx + 9, hy + 5, hc);
                // Thumb
                pushLine(hx,     hy + 1, hx + 1, hy + 1, hc);
                pushLine(hx,     hy + 2, hx + 1, hy + 2, hc);
            }

        }
    }

    // Attack cooldown bar — vertical bar to the left of the quickbar
    if (cooldownPct > 0.0f) {
        f32 barW = 6.0f;
        f32 barH = SLOT_SIZE;
        f32 barX = startX - barW - 8.0f;  // 8px gap left of first slot
        f32 barY = baseY;

        // Background
        pushQuad(barX, barY, barX + barW, barY + barH, {0.15f, 0.15f, 0.2f});
        // Fill from bottom up (remaining cooldown)
        f32 fillH = barH * (1.0f - cooldownPct);
        Vec3 fillColor = {0.8f, 0.6f, 0.1f}; // gold
        for (f32 fy = barY + 1; fy < barY + 1 + fillH; fy += 1.0f) {
            pushLine(barX + 1, fy, barX + barW - 1, fy, fillColor);
        }
    }

    flushHUD(sw, sh);
}

void HUD::drawLootNotification(u32 sw, u32 sh, Vec3 color, f32 alpha) {
    // Center-top of screen
    f32 barW = 160.0f;
    f32 barH = 8.0f;
    f32 x0 = static_cast<f32>(sw) * 0.5f - barW * 0.5f;
    f32 y0 = static_cast<f32>(sh) - 60.0f;

    Vec3 c = {color.x * alpha, color.y * alpha, color.z * alpha};

    // Outline
    pushLine(x0,        y0,        x0 + barW, y0,        c);
    pushLine(x0 + barW, y0,        x0 + barW, y0 + barH, c);
    pushLine(x0 + barW, y0 + barH, x0,        y0 + barH, c);
    pushLine(x0,        y0 + barH, x0,        y0,        c);

    // Fill
    for (f32 y = y0 + 1; y < y0 + barH; y += 1.0f) {
        pushLine(x0 + 1, y, x0 + barW - 1, y, c);
    }

    flushHUD(sw, sh);
}

void HUD::drawInventoryScreen(u32 sw, u32 sh,
                               const PlayerInventory& inv,
                               const ItemDef* itemDefs,
                               u8 selectedSlot, bool selectedIsEquipped,
                               s32 mouseX, s32 mouseY)
{

    f32 centerY = static_cast<f32>(sh) * 0.5f;

    // --- Equipment panel (left side) ---
    f32 eqX      = static_cast<f32>(sw) * 0.12f;
    f32 eqStartY = centerY + 130.0f;
    f32 slotW    = 240.0f;
    f32 slotH    = 32.0f;
    f32 slotGap  = 5.0f;

    // Dark background behind equipment panel
    {
        u32 slotCount = static_cast<u32>(ItemSlot::COUNT);
        f32 topY = eqStartY + slotH;
        f32 botY = eqStartY - static_cast<f32>(slotCount - 1) * (slotH + slotGap);
        f32 pad = 8.0f;
        Vec3 bg = {0.05f, 0.05f, 0.08f};
        for (f32 fy = botY - pad; fy < topY + pad; fy += 1.0f) {
            pushLine(eqX - pad, fy, eqX + slotW + pad, fy, bg);
        }
        flushHUD(sw, sh);
    }

    for (u32 i = 0; i < static_cast<u32>(ItemSlot::COUNT); i++) {
        f32 y = eqStartY - static_cast<f32>(i) * (slotH + slotGap);
        const ItemInstance& item = inv.equipped[i];
        bool selected = selectedIsEquipped && (selectedSlot == static_cast<u8>(i));

        Vec3 color = {0.3f, 0.3f, 0.3f};
        if (!isItemEmpty(item)) {
            color = rarityColor(item.rarity);
        }

        if (selected) {
            color.x = color.x * 1.5f; if (color.x > 1.0f) color.x = 1.0f;
            color.y = color.y * 1.5f; if (color.y > 1.0f) color.y = 1.0f;
            color.z = color.z * 1.5f; if (color.z > 1.0f) color.z = 1.0f;
        }

        // Slot outline
        pushLine(eqX,          y,         eqX + slotW, y,         color);
        pushLine(eqX + slotW,  y,         eqX + slotW, y + slotH, color);
        pushLine(eqX + slotW,  y + slotH, eqX,         y + slotH, color);
        pushLine(eqX,          y + slotH, eqX,         y,         color);

        // Item icon and name inside equipment slot
        if (!isItemEmpty(item)) {
            const ItemDef& def = itemDefs[item.defId];
            // Dark fill behind icon
            Vec3 fillColor = {color.x * 0.2f + 0.04f, color.y * 0.2f + 0.04f, color.z * 0.2f + 0.04f};
            for (f32 line = 2.0f; line < slotH - 2.0f; line += 1.0f) {
                pushLine(eqX + 2.0f, y + line, eqX + slotW - 2.0f, y + line, fillColor);
            }
            flushHUD(sw, sh);
            // Icon on left side of slot
            ItemIconSystem::drawIcon(sw, sh, eqX + 3.0f, y + 2.0f, slotH - 4.0f, def, item.rarity);
            // Item name to the right of icon
            FontSystem::drawText(sw, sh, eqX + slotH + 4.0f, y + 9.0f,
                                 def.name, rarityColor(item.rarity), 2);
        } else {
            // Slot type label for empty slots
            static const char* slotLabels[] = {"Weapon", "Offhand", "Helmet", "Armor", "Boots", "Ring"};
            Vec3 dimColor = {0.25f, 0.25f, 0.3f};
            FontSystem::drawText(sw, sh, eqX + 6.0f, y + 8.0f,
                                 slotLabels[i], dimColor, 1);
        }

        // Selection highlight — golden border + arrow
        if (selected) {
            Vec3 hi = {1.0f, 0.9f, 0.4f};
            // Golden border (2px)
            pushLine(eqX - 2,         y - 2,          eqX + slotW + 2, y - 2,          hi);
            pushLine(eqX + slotW + 2, y - 2,          eqX + slotW + 2, y + slotH + 2, hi);
            pushLine(eqX + slotW + 2, y + slotH + 2, eqX - 2,          y + slotH + 2, hi);
            pushLine(eqX - 2,         y + slotH + 2, eqX - 2,          y - 2,          hi);
            pushLine(eqX - 1,         y - 1,          eqX + slotW + 1, y - 1,          hi);
            pushLine(eqX + slotW + 1, y - 1,          eqX + slotW + 1, y + slotH + 1, hi);
            pushLine(eqX + slotW + 1, y + slotH + 1, eqX - 1,          y + slotH + 1, hi);
            pushLine(eqX - 1,         y + slotH + 1, eqX - 1,          y - 1,          hi);
            // Arrow
            pushLine(eqX - 12.0f, y + slotH * 0.5f, eqX - 4.0f, y + slotH * 0.5f, hi);
            pushLine(eqX - 7.0f, y + slotH * 0.5f + 3.0f, eqX - 4.0f, y + slotH * 0.5f, hi);
            pushLine(eqX - 7.0f, y + slotH * 0.5f - 3.0f, eqX - 4.0f, y + slotH * 0.5f, hi);
        }
    }

    // --- Backpack panel (right side, 6 columns x 4 rows) ---
    f32 bpX      = static_cast<f32>(sw) * 0.52f;
    f32 bpStartY = centerY + 90.0f;
    f32 cellSize = 32.0f;
    f32 cellGap  = 4.0f;

    // Dark background behind backpack panel
    {
        f32 panelW = 6.0f * (cellSize + cellGap) - cellGap;
        // Rows go from bpStartY (top of row 0) down to row 3 bottom
        f32 topY = bpStartY + cellSize;
        f32 botY = bpStartY - 3.0f * (cellSize + cellGap);
        f32 pad = 8.0f;
        Vec3 bg = {0.05f, 0.05f, 0.08f};
        for (f32 fy = botY - pad; fy < topY + pad; fy += 1.0f) {
            pushLine(bpX - pad, fy, bpX + panelW + pad, fy, bg);
        }
        flushHUD(sw, sh);
    }

    for (u32 i = 0; i < MAX_INVENTORY_ITEMS; i++) {
        u32 col = i % 6;
        u32 row = i / 6;
        f32 x = bpX + static_cast<f32>(col) * (cellSize + cellGap);
        f32 y = bpStartY - static_cast<f32>(row) * (cellSize + cellGap);

        const ItemInstance& item = inv.backpack[i];
        bool selected = !selectedIsEquipped && (selectedSlot == static_cast<u8>(i));

        Vec3 color = {0.2f, 0.2f, 0.2f};
        if (!isItemEmpty(item)) {
            color = rarityColor(item.rarity);
        }

        // Slot outline
        pushLine(x,           y,           x + cellSize, y,           color);
        pushLine(x + cellSize, y,           x + cellSize, y + cellSize, color);
        pushLine(x + cellSize, y + cellSize, x,           y + cellSize, color);
        pushLine(x,           y + cellSize, x,           y,           color);

        // Fill + icon for occupied slots
        if (!isItemEmpty(item)) {
            Vec3 fillColor = {color.x * 0.2f + 0.04f, color.y * 0.2f + 0.04f, color.z * 0.2f + 0.04f};
            for (f32 line = 2.0f; line < cellSize - 2.0f; line += 1.0f) {
                pushLine(x + 2.0f, y + line, x + cellSize - 2.0f, y + line, fillColor);
            }
            flushHUD(sw, sh);
            const ItemDef& def = itemDefs[item.defId];
            ItemIconSystem::drawIcon(sw, sh, x + 3.0f, y + 3.0f, cellSize - 6.0f, def, item.rarity);
        }

        // Selection highlight — bright border + golden glow fill
        if (selected) {
            // Golden glow fill behind the slot
            Vec3 glow = {0.4f, 0.35f, 0.1f};
            for (f32 line = 1.0f; line < cellSize - 1.0f; line += 1.0f) {
                pushLine(x + 1.0f, y + line, x + cellSize - 1.0f, y + line, glow);
            }
            flushHUD(sw, sh);
            // Re-draw icon on top of glow
            if (!isItemEmpty(item)) {
                const ItemDef& def2 = itemDefs[item.defId];
                ItemIconSystem::drawIcon(sw, sh, x + 3.0f, y + 3.0f, cellSize - 6.0f, def2, item.rarity);
            }
            // White border (2px thick)
            Vec3 hi = {1.0f, 0.9f, 0.4f};
            pushLine(x - 2, y - 2,           x + cellSize + 2, y - 2,           hi);
            pushLine(x + cellSize + 2, y - 2, x + cellSize + 2, y + cellSize + 2, hi);
            pushLine(x + cellSize + 2, y + cellSize + 2, x - 2, y + cellSize + 2, hi);
            pushLine(x - 2, y + cellSize + 2, x - 2, y - 2,                       hi);
            pushLine(x - 1, y - 1,           x + cellSize + 1, y - 1,           hi);
            pushLine(x + cellSize + 1, y - 1, x + cellSize + 1, y + cellSize + 1, hi);
            pushLine(x + cellSize + 1, y + cellSize + 1, x - 1, y + cellSize + 1, hi);
            pushLine(x - 1, y + cellSize + 1, x - 1, y - 1,                       hi);
        }
    }

    // --- Tooltip on hover ---
    if (mouseX >= 0 && mouseY >= 0) {
        f32 mx = static_cast<f32>(mouseX);
        f32 my = static_cast<f32>(mouseY);

        // Check equipment slots
        for (u32 i = 0; i < static_cast<u32>(ItemSlot::COUNT); i++) {
            f32 y = eqStartY - static_cast<f32>(i) * (slotH + slotGap);
            if (mx >= eqX && mx <= eqX + slotW && my >= y && my <= y + slotH) {
                if (!isItemEmpty(inv.equipped[i])) {
                    drawItemTooltip(sw, sh, eqX + slotW + 8.0f, y,
                                    inv.equipped[i], itemDefs[inv.equipped[i].defId]);
                }
                break;
            }
        }

        // Check backpack slots
        for (u32 i = 0; i < MAX_INVENTORY_ITEMS; i++) {
            u32 col = i % 6;
            u32 row = i / 6;
            f32 x = bpX + static_cast<f32>(col) * (cellSize + cellGap);
            f32 y = bpStartY - static_cast<f32>(row) * (cellSize + cellGap);

            if (mx >= x && mx <= x + cellSize && my >= y && my <= y + cellSize) {
                if (!isItemEmpty(inv.backpack[i])) {
                    drawItemTooltip(sw, sh, x + cellSize + 8.0f, y,
                                    inv.backpack[i], itemDefs[inv.backpack[i].defId]);
                }
                break;
            }
        }
    }

    flushHUD(sw, sh);
}

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

static const u8* getStatusIcon(u32 idx) {
    static const u8* icons[] = {
        &kIconPoison[0][0], &kIconBurn[0][0], &kIconFreeze[0][0],
        &kIconSlow[0][0], &kIconInvuln[0][0]
    };
    return (idx < 5) ? icons[idx] : nullptr;
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
        default:
            cols[1] = cols[2] = cols[3] = cols[4] = {0.5f, 0.5f, 0.5f};
            break;
    }
}

void HUD::drawStatusIcons(u32 sw, u32 sh, f32 x, f32 y,
                            const StatusEffect* effects, u32 count)
{
    f32 iconSize = 28.0f;
    f32 gap = 5.0f;
    f32 cx = x;

    for (u32 i = 0; i < count; i++) {
        if (effects[i].timer <= 0.0f) continue;

        // Pulsing brightness when timer is low (<2s)
        f32 pulse = (effects[i].timer < 2.0f)
            ? 0.5f + 0.5f * sinf(effects[i].timer * 8.0f)
            : 1.0f;

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
            f32 px = 3.0f; // pixel scale
            f32 iconX = cx + 2.0f;
            f32 iconY = y + 2.0f;
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

        flushHUD(sw, sh);

        // Timer text above icon
        char timeTxt[8];
        std::snprintf(timeTxt, sizeof(timeTxt), "%.0f", effects[i].timer);
        f32 tw = FontSystem::textWidth(timeTxt, 1);
        FontSystem::drawText(sw, sh, cx + (iconSize - tw) * 0.5f, y + iconSize + 2.0f,
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

    flushHUD(sw, sh);

    // Text centered inside the bubble
    Vec3 tc = {textColor.x * alpha, textColor.y * alpha, textColor.z * alpha};
    FontSystem::drawText(sw, sh, x - textW * 0.5f, y, text, tc, 1);
}

// Helper: get string name for affix type
static const char* affixTypeName(AffixType type) {
    switch (type) {
        case AffixType::DAMAGE_FLAT:        return "+Damage";
        case AffixType::HEALTH_FLAT:        return "+Health";
        case AffixType::MOVE_SPEED_FLAT:    return "+Move Speed";
        case AffixType::DAMAGE_PCT:         return "+Damage %";
        case AffixType::COOLDOWN_REDUCTION: return "Cooldown Reduction";
        case AffixType::HEALTH_PCT:         return "+Health %";
        case AffixType::LIFE_ON_HIT:        return "Life on Hit";
        case AffixType::PROJECTILE_SPEED:   return "+Proj Speed";
        case AffixType::CONE_ANGLE:         return "+Swing Arc";
        case AffixType::RANGE_BONUS:        return "+Range";
        case AffixType::DAMAGE_TO_FLYING:   return "+Dmg vs Flying";
        case AffixType::CLIP_SIZE_PCT:      return "+Clip Size %";
        case AffixType::RELOAD_SPEED_PCT:   return "+Reload Speed %";
        default:                            return "Unknown";
    }
}

static const char* rarityName(Rarity r) {
    switch (r) {
        case Rarity::COMMON:    return "Common";
        case Rarity::MAGIC:     return "Magic";
        case Rarity::RARE:      return "Rare";
        case Rarity::LEGENDARY: return "Legendary";
        default:                return "";
    }
}

static const char* slotName(ItemSlot slot) {
    switch (slot) {
        case ItemSlot::WEAPON:  return "Weapon";
        case ItemSlot::OFFHAND: return "Offhand";
        case ItemSlot::HELMET:  return "Helmet";
        case ItemSlot::ARMOR:   return "Armor";
        case ItemSlot::BOOTS:   return "Boots";
        case ItemSlot::RING:    return "Ring";
        default:                return "";
    }
}

static const char* subtypeName(WeaponSubtype st) {
    switch (st) {
        case WeaponSubtype::SWORD:          return "Sword";
        case WeaponSubtype::DAGGER:         return "Dagger";
        case WeaponSubtype::AXE:            return "Axe";
        case WeaponSubtype::PISTOL:         return "Pistol";
        case WeaponSubtype::SMG:            return "SMG";
        case WeaponSubtype::CARBINE:        return "Carbine";
        case WeaponSubtype::REVOLVER:       return "Revolver";
        case WeaponSubtype::BOW:            return "Bow";
        case WeaponSubtype::CROSSBOW:       return "Crossbow";
        case WeaponSubtype::THROWING_KNIFE: return "Throwing Knife";
        case WeaponSubtype::MOLOTOV:        return "Molotov";
        default:                            return "";
    }
}

// Skill name + description for legendary tooltip display
static const char* skillDisplayName(SkillId id) {
    switch (id) {
        case SkillId::FROZEN_ORB:      return "Frozen Orb";
        case SkillId::CHAIN_LIGHTNING: return "Chain Lightning";
        case SkillId::METEOR_STRIKE:   return "Meteor Strike";
        case SkillId::BLOOD_NOVA:      return "Blood Nova";
        case SkillId::PHASE_DASH:      return "Phase Dash";
        case SkillId::THROWAWAY:       return "Throwaway";
        case SkillId::VOID_ZONE:       return "Void Zone";
        case SkillId::LIFE_STEAL:      return "Life Steal";
        case SkillId::THORNS:          return "Thorns";
        case SkillId::BERSERKER:       return "Berserker";
        case SkillId::SECOND_WIND:     return "Second Wind";
        case SkillId::SOUL_HARVEST:    return "Soul Harvest";
        case SkillId::GRAVITY_PULL:    return "Gravity Pull";
        case SkillId::PHASE_STRIKE:    return "Phase Strike";
        case SkillId::VOID_KILL:       return "Void Kill";
        case SkillId::ARC_FIRE:        return "Blazing Arc";
        default: return "Unknown";
    }
}
static const char* skillDescription(SkillId id) {
    switch (id) {
        case SkillId::FROZEN_ORB:      return "Launches an icy orb that spirals\nout frost shards in all directions.";
        case SkillId::CHAIN_LIGHTNING: return "Fires a bolt of lightning that\nbounces between nearby enemies.";
        case SkillId::METEOR_STRIKE:   return "Calls down a massive meteor that\nscorches the ground on impact.";
        case SkillId::BLOOD_NOVA:      return "Sacrifices health to unleash a\ndevastating ring of blood energy.";
        case SkillId::PHASE_DASH:      return "Teleports forward through enemies,\ndamaging all in the corridor.";
        case SkillId::THROWAWAY:       return "On empty clip, throw weapon as\nan explosive projectile.";
        case SkillId::VOID_ZONE:       return "5% on hit: dark void zone dealing\nflat damage + 60% missing HP.";
        case SkillId::LIFE_STEAL:      return "Heal 5% of all damage dealt.";
        case SkillId::THORNS:          return "Reflect 20% of damage taken\nback to the nearest enemy.";
        case SkillId::BERSERKER:       return "+1% damage for each 1% of\nmissing health. Risk vs reward.";
        case SkillId::SECOND_WIND:     return "Below 20% HP: heal 30% and\ngain 2s invulnerability. 60s cooldown.";
        case SkillId::SOUL_HARVEST:    return "Each kill: +5% speed, +3% damage\nfor 10s. Stacks up to 5 times.";
        case SkillId::GRAVITY_PULL:    return "Enemies within 5m are slowly\npulled toward you.";
        case SkillId::PHASE_STRIKE:    return "10% on hit: teleport behind\nthe target.";
        case SkillId::VOID_KILL:       return "15% on kill: void zone on corpse\ndealing 60% missing HP to nearby.";
        case SkillId::ARC_FIRE:        return "20% on hit: ignite the ground\nacross the full swing arc for 1.5s.";
        default: return "";
    }
}

void HUD::drawItemTooltip(u32 sw, u32 sh, f32 tipX, f32 tipY,
                            const ItemInstance& item, const ItemDef& def)
{
    if (isItemEmpty(item)) return;

    Vec3 rColor = rarityColor(item.rarity);

    u32 nameScale = 3;
    u32 bodyScale = 2;
    f32 lineH = FontSystem::textHeight(bodyScale) + 3.0f;
    f32 nameH = FontSystem::textHeight(nameScale) + 6.0f;
    f32 padX = 10.0f;
    f32 padY = 8.0f;

    // Count lines for sizing
    u32 lineCount = 3; // rarity, slot, separator
    if (def.slot == ItemSlot::WEAPON) {
        lineCount += 1; // subtype
        lineCount += 3; // damage, cooldown, range
    } else {
        if (item.bonusHealth > 0.0f) lineCount += 1;
    }
    lineCount += item.affixCount;
    if (def.legendarySkillId != SkillId::NONE && item.rarity == Rarity::LEGENDARY) {
        lineCount += 4; // separator + skill name + 2 description lines
    }

    f32 tooltipW = 320.0f;
    f32 tooltipH = padY * 2 + nameH + lineCount * lineH;

    // Clamp to screen
    if (tipX + tooltipW > static_cast<f32>(sw)) tipX = static_cast<f32>(sw) - tooltipW - 4.0f;
    if (tipY + tooltipH > static_cast<f32>(sh)) tipY = static_cast<f32>(sh) - tooltipH - 4.0f;
    if (tipX < 0) tipX = 4.0f;
    if (tipY < 0) tipY = 4.0f;

    // Dark background
    Vec3 bgColor = {0.06f, 0.06f, 0.10f};
    for (f32 y = tipY; y < tipY + tooltipH; y += 1.0f) {
        pushLine(tipX, y, tipX + tooltipW, y, bgColor);
    }

    // Border in rarity color (double border for legendaries)
    Vec3 borderColor = {rColor.x * 0.6f, rColor.y * 0.6f, rColor.z * 0.6f};
    pushQuad(tipX, tipY, tipX + tooltipW, tipY + tooltipH, borderColor);
    if (item.rarity == Rarity::LEGENDARY) {
        pushQuad(tipX + 1, tipY + 1, tipX + tooltipW - 1, tipY + tooltipH - 1, borderColor);
    }

    flushHUD(sw, sh);

    f32 textX = tipX + padX;
    f32 curY = tipY + tooltipH - padY - nameH;

    // Item name (large)
    FontSystem::drawText(sw, sh, textX, curY, def.name, rColor, nameScale);
    curY -= nameH;

    // Rarity
    FontSystem::drawText(sw, sh, textX, curY, rarityName(item.rarity), rColor, bodyScale);
    curY -= lineH;

    // Slot type
    FontSystem::drawText(sw, sh, textX, curY, slotName(def.slot), {0.7f, 0.7f, 0.75f}, bodyScale);
    curY -= lineH;

    // Weapon subtype
    if (def.slot == ItemSlot::WEAPON && def.weaponSubtype != WeaponSubtype::NONE) {
        FontSystem::drawText(sw, sh, textX, curY, subtypeName(def.weaponSubtype), {0.55f, 0.55f, 0.6f}, bodyScale);
        curY -= lineH;
    }

    // Separator
    pushLine(textX, curY + lineH * 0.3f, tipX + tooltipW - padX, curY + lineH * 0.3f, {0.3f, 0.3f, 0.35f});
    flushHUD(sw, sh);
    curY -= lineH * 0.5f;

    // Stats
    char buf[80];
    if (def.slot == ItemSlot::WEAPON) {
        std::snprintf(buf, sizeof(buf), "Damage: %.0f", item.damage);
        FontSystem::drawText(sw, sh, textX, curY, buf, {1.0f, 0.9f, 0.7f}, bodyScale);
        curY -= lineH;

        std::snprintf(buf, sizeof(buf), "Speed: %.2fs", def.baseCooldown);
        FontSystem::drawText(sw, sh, textX, curY, buf, {0.7f, 0.9f, 0.7f}, bodyScale);
        curY -= lineH;

        if (def.weaponType == WeaponType::MELEE) {
            std::snprintf(buf, sizeof(buf), "Range: %.1fm", def.baseRange);
        } else if (def.weaponType == WeaponType::HITSCAN) {
            std::snprintf(buf, sizeof(buf), "Range: %.0fm", def.baseRange);
        } else {
            std::snprintf(buf, sizeof(buf), "Proj Speed: %.0f", def.baseProjectileSpeed);
        }
        FontSystem::drawText(sw, sh, textX, curY, buf, {0.7f, 0.7f, 0.9f}, bodyScale);
        curY -= lineH;
    } else {
        if (item.bonusHealth > 0.0f) {
            std::snprintf(buf, sizeof(buf), "+%.0f Health", item.bonusHealth);
            FontSystem::drawText(sw, sh, textX, curY, buf, {0.7f, 1.0f, 0.7f}, bodyScale);
            curY -= lineH;
        }
    }

    // Affixes
    for (u8 a = 0; a < item.affixCount; a++) {
        const Affix& affix = item.affixes[a];
        const char* name = affixTypeName(affix.type);
        std::snprintf(buf, sizeof(buf), "%s: +%.1f", name, affix.value);
        FontSystem::drawText(sw, sh, textX, curY, buf, {0.4f, 0.85f, 1.0f}, bodyScale);
        curY -= lineH;
    }

    // Legendary skill — only shown on legendary-rarity items
    if (def.legendarySkillId != SkillId::NONE && item.rarity == Rarity::LEGENDARY) {
        // Gold separator
        curY -= lineH * 0.3f;
        pushLine(textX, curY + lineH * 0.3f, tipX + tooltipW - padX, curY + lineH * 0.3f, {0.6f, 0.5f, 0.15f});
        flushHUD(sw, sh);
        curY -= lineH * 0.2f;

        // Activation method depends on equipment slot
        const char* activationLabel = "Skill";
        Vec3 activationColor = {1.0f, 0.82f, 0.2f};
        switch (def.slot) {
            case ItemSlot::WEAPON:  activationLabel = "On Hit"; activationColor = {1.0f, 0.6f, 0.2f}; break;
            case ItemSlot::RING:    activationLabel = "Right Click"; break;
            case ItemSlot::BOOTS:   activationLabel = "Press F"; activationColor = {0.3f, 1.0f, 0.5f}; break;
            case ItemSlot::HELMET:  activationLabel = "Press G"; activationColor = {0.5f, 0.8f, 1.0f}; break;
            case ItemSlot::ARMOR:   activationLabel = "Passive Aura"; activationColor = {0.7f, 0.7f, 1.0f}; break;
            case ItemSlot::OFFHAND: activationLabel = "Perfect Block"; activationColor = {0.9f, 0.9f, 1.0f}; break;
            default: break;
        }

        const char* sName = skillDisplayName(def.legendarySkillId);
        std::snprintf(buf, sizeof(buf), "[%s] %s", activationLabel, sName);
        FontSystem::drawText(sw, sh, textX, curY, buf, activationColor, bodyScale);
        curY -= lineH;

        // Skill description (split on \n)
        const char* desc = skillDescription(def.legendarySkillId);
        const char* line = desc;
        while (*line) {
            // Find end of line
            const char* eol = line;
            while (*eol && *eol != '\n') eol++;
            char descLine[80];
            u32 len = static_cast<u32>(eol - line);
            if (len >= sizeof(descLine)) len = sizeof(descLine) - 1;
            std::memcpy(descLine, line, len);
            descLine[len] = '\0';
            FontSystem::drawText(sw, sh, textX, curY, descLine, {0.9f, 0.75f, 0.3f}, bodyScale);
            curY -= lineH;
            line = (*eol == '\n') ? eol + 1 : eol;
        }
    }
}
