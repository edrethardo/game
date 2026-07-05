// hud.cpp — HUD GL batch core: vertex buffer state, pushLine/pushQuad/flushHUD
// definitions (non-static; declared in hud_internal.h for split files to use),
// plus HUD::init/shutdown/flush and the small always-on-screen elements
// (crosshair, hit marker, health/energy bars, weapon indicator, menu options,
// profiler overlay, net stats, filled bar).
//
// Larger feature areas are split into separate translation units:
//   hud_input_glyphs.cpp  — keyboard/controller/mouse symbols
//   hud_skill_bar.cpp     — class/equip skill bars, radial cooldown
//   hud_status.cpp        — status icons, speech bubble, damage vignette/direction
//   hud_inventory.cpp     — inventory screen, item tooltips, loot notification
//   hud_portraits.cpp     — summon portrait, quickbar

#include "renderer/hud.h"
#include "renderer/hud_cooldown_util.h"
#include "renderer/shader.h"
#include "renderer/font.h"
#include "core/log.h"
#include "core/profiler.h"
#include <glad/glad.h>
#include <cmath>
#include <cstdio>

// Simple 2D line renderer for HUD elements (crosshair, hit markers).
// Uses a small dynamic VBO with position+color, drawn with the debug shader
// but with an orthographic VP matrix mapping pixels to NDC.

struct HudVertex {
    Vec3 pos;
    Vec3 color;
};

static constexpr u32 MAX_HUD_VERTS = 8192; // all HUD elements now batch into single flush

static u32    s_vao = 0;
static u32    s_vbo = 0;
static Shader s_shader;
static HudVertex s_verts[MAX_HUD_VERTS];
static u32 s_vertCount = 0;
static u32 s_screenW = 1280;  // cached for flushHUD (set by HUD::flush)
static u32 s_screenH = 720;

// Non-static so split TUs can call them (declared in hud_internal.h).
void pushLine(f32 x0, f32 y0, f32 x1, f32 y1, Vec3 color) {
    if (s_vertCount + 2 > MAX_HUD_VERTS) return;
    s_verts[s_vertCount++] = {{x0, y0, 0.0f}, color};
    s_verts[s_vertCount++] = {{x1, y1, 0.0f}, color};
}

void pushQuad(f32 x0, f32 y0, f32 x1, f32 y1, Vec3 color) {
    // Draw as 3 lines forming a filled-looking outline
    pushLine(x0, y0, x1, y0, color);
    pushLine(x0, y1, x1, y1, color);
    pushLine(x0, y0, x0, y1, color);
    pushLine(x1, y0, x1, y1, color);
}

void flushHUD() {
    if (s_vertCount == 0 || !s_vao) return;

    glDisable(GL_DEPTH_TEST);

    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, s_vertCount * sizeof(HudVertex), s_verts);

    glUseProgram(s_shader.program);

    // Build ortho projection: (0,0) = bottom-left, (w,h) = top-right
    Mat4 ortho;
    f32 w = static_cast<f32>(s_screenW);
    f32 h = static_cast<f32>(s_screenH);
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

void HUD::flush(u32 screenWidth, u32 screenHeight) {
    s_screenW = screenWidth;
    s_screenH = screenHeight;
    flushHUD();
}

void HUD::drawCrosshair(u32 screenWidth, u32 screenHeight, Vec3 color) {
    s_screenW = screenWidth; s_screenH = screenHeight;
    f32 uiScale = static_cast<f32>(screenHeight) / 720.0f;
    f32 cx = screenWidth  * 0.5f;
    f32 cy = screenHeight * 0.5f;
    f32 gap  = 4.0f * uiScale;
    f32 size = 12.0f * uiScale;

    // Four lines with a gap in the centre
    pushLine(cx - size, cy, cx - gap, cy, color);  // left
    pushLine(cx + gap,  cy, cx + size, cy, color);  // right
    pushLine(cx, cy - size, cx, cy - gap, color);   // bottom
    pushLine(cx, cy + gap,  cx, cy + size, color);  // top

    flushHUD();
}

void HUD::drawHitMarker(u32 screenWidth, u32 screenHeight, f32 alpha) {
    f32 uiScale = static_cast<f32>(screenHeight) / 720.0f;
    f32 cx = screenWidth  * 0.5f;
    f32 cy = screenHeight * 0.5f;
    f32 size = 8.0f * uiScale;
    f32 inner = 3.0f * uiScale;
    Vec3 color = {alpha, alpha, alpha};

    // X shape
    pushLine(cx - size, cy - size, cx - inner, cy - inner, color);
    pushLine(cx + inner,    cy + inner,    cx + size, cy + size, color);
    pushLine(cx + size, cy - size, cx + inner, cy - inner, color);
    pushLine(cx - inner,    cy + inner,    cx - size, cy + size, color);

    flushHUD();
}

void HUD::drawHealthBar(u32 screenWidth, u32 screenHeight,
                         f32 health, f32 maxHealth)
{
    s_screenW = screenWidth; s_screenH = screenHeight;
    f32 uiScale = static_cast<f32>(screenHeight) / 720.0f;
    f32 barW = 200.0f * uiScale;
    f32 barH = 16.0f * uiScale;
    f32 x0 = 20.0f * uiScale;
    f32 y0 = 20.0f * uiScale;

    f32 frac = (maxHealth > 0.0f) ? health / maxHealth : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    // Background outline
    pushQuad(x0, y0, x0 + barW, y0 + barH, {0.3f, 0.3f, 0.3f});

    // Filled portion (draw as horizontal lines to simulate fill)
    Vec3 barColor = (frac > 0.3f) ? Vec3{0.2f, 0.8f, 0.2f} : Vec3{0.9f, 0.2f, 0.2f};
    f32 fillW = barW * frac;
    f32 fillPad = 2.0f * uiScale;
    for (f32 y = y0 + fillPad; y < y0 + barH - fillPad; y += 2.0f * uiScale) {
        pushLine(x0 + fillPad, y, x0 + fillPad + fillW - fillPad * 2.0f, y, barColor);
    }

    flushHUD();
}

// Potion belt flask — a primitive-drawn flask (no asset) welded beside the health bar.
// States: cooling (radial sweep + seconds number, dimmed) | urgent (steady red pulse when
// low HP + ready) | ready. A green "ready" pop plays on the cooling->ready transition.
void HUD::drawPotionFlask(u32 sw, u32 sh, f32 x, f32 y, const PotionHudState& st) {
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    f32 w = 44.0f * uiScale;
    f32 h = 46.0f * uiScale;
    f32 cx = x + w * 0.5f;
    f32 cy = y + h * 0.5f;
    bool cooling = st.cooldownRemaining > 0.0f;

    // Gentle red breathing pulse (1 Hz, no strobe) when urgent. pulsePhase is the shared
    // HUD clock (m_statsTimer), which wraps by exactly 1.0 s each second — so we use an
    // angular frequency of 2*pi (an integer # of cycles per wrap) to keep the sine
    // CONTINUOUS across that wrap (no once-a-second jump). 1 Hz is well under any
    // photosensitivity threshold and shares the red + low-HP trigger of the screen vignette.
    f32 pulse = st.urgent ? (0.55f + 0.45f * std::sin(st.pulsePhase * 6.2832f)) : 0.0f;

    // Rim / border color per state.
    Vec3 rim = cooling ? Vec3{0.45f, 0.18f, 0.15f}
             : st.urgent ? Vec3{0.7f + 0.3f * pulse, 0.22f, 0.16f}
                         : Vec3{0.75f, 0.28f, 0.22f};

    // Glass body: rounded rect over the lower ~72% of the cell.
    f32 bx0 = x + 10.0f * uiScale, bx1 = x + w - 10.0f * uiScale;
    f32 by0 = y + 2.0f * uiScale,  by1 = y + h * 0.72f;
    pushQuad(bx0, by0, bx1, by1, rim);

    // Red liquid fill (horizontal lines), dim while cooling, brighter/pulsing when urgent.
    f32 lum = cooling ? 0.42f : (st.urgent ? 0.7f + 0.3f * pulse : 0.9f);
    Vec3 liquid = {0.85f * lum, 0.16f * lum + 0.04f, 0.12f * lum + 0.03f};
    for (f32 ly = by0 + 2.0f * uiScale; ly < by1 - 1.0f * uiScale; ly += 1.0f * uiScale) {
        pushLine(bx0 + 2.0f * uiScale, ly, bx1 - 2.0f * uiScale, ly, liquid);
    }

    // Neck + cork.
    f32 nk = 6.0f * uiScale;
    pushLine(cx - nk, by1, cx - nk, y + h - 8.0f * uiScale, rim);
    pushLine(cx + nk, by1, cx + nk, y + h - 8.0f * uiScale, rim);
    pushQuad(cx - nk - 1.0f * uiScale, y + h - 8.0f * uiScale,
             cx + nk + 1.0f * uiScale, y + h - 3.0f * uiScale, {0.55f, 0.4f, 0.25f});

    // Urgent glow: a red ring around the whole cell, breathing with the 1 Hz pulse.
    if (st.urgent) {
        Vec3 glow = Vec3{0.9f, 0.25f, 0.2f} * (0.4f + 0.6f * pulse);
        pushQuad(x + 1.0f * uiScale, y + 1.0f * uiScale,
                 x + w - 1.0f * uiScale, y + h - 1.0f * uiScale, glow);
    }
    flushHUD();

    // Cooling: radial sweep + centered seconds number (same language as skills).
    if (cooling) {
        f32 frac = (st.maxCooldown > 0.0f) ? st.cooldownRemaining / st.maxCooldown : 0.0f;
        drawRadialCooldown(cx, cy, w * 0.42f, frac, {0.05f, 0.04f, 0.05f}, {1.0f, 0.5f, 0.4f});
        flushHUD();
        if (HudCooldown::showCooldownNumber(st.cooldownRemaining)) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", HudCooldown::cooldownSeconds(st.cooldownRemaining));
            f32 tw = FontSystem::textWidth(buf, 2);
            FontSystem::drawText(sw, sh, cx - tw * 0.5f, cy - 7.0f * uiScale, buf, {1.0f, 1.0f, 1.0f}, 2);
        }
    }

    // Key label above the flask (highlighted when ready to drink).
    drawKeySymbol(sw, sh, x + 4.0f * uiScale, y + h + 2.0f * uiScale, st.keyLabel, !cooling);

    // Green "ready" pop (shared with skills) on the cooling->ready transition.
    if (st.readyFlash > 0.0f) {
        drawReadyPop(cx, cy, w * 0.5f, HudCooldown::readyPopT(st.readyFlash), uiScale,
                     {0.42f, 0.88f, 0.54f});
        flushHUD();
    }
}

void HUD::drawWeaponIndicator(u32 screenWidth, u32 screenHeight, u8 weaponSlot) {
    f32 uiScale = static_cast<f32>(screenHeight) / 720.0f;
    f32 x0 = static_cast<f32>(screenWidth) - 120.0f * uiScale;
    f32 y0 = 20.0f * uiScale;

    // Color per weapon type
    Vec3 colors[3] = {
        {0.7f, 0.7f, 0.7f}, // 0: melee (grey)
        {1.0f, 0.8f, 0.2f}, // 1: hitscan (gold)
        {0.3f, 0.5f, 1.0f}, // 2: projectile (blue)
    };

    Vec3 c = (weaponSlot < 3) ? colors[weaponSlot] : Vec3{1,1,1};
    pushQuad(x0, y0, x0 + 100.0f * uiScale, y0 + 16.0f * uiScale, c);

    flushHUD();
}

void HUD::drawMenuOption(u32 screenWidth, u32 screenHeight,
                          f32 y, f32 width, f32 height,
                          Vec3 color, bool selected)
{
    s_screenW = screenWidth; s_screenH = screenHeight;
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

    flushHUD();
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

    flushHUD();
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

    flushHUD();
}

void HUD::drawEnergyBar(u32 sw, u32 sh, f32 energy, f32 maxEnergy) {
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    f32 barW = 200.0f * uiScale;
    f32 barH = 10.0f * uiScale;
    f32 x0 = 20.0f * uiScale;
    // Health bar sits at y0=20 with barH=16, so place energy bar 22px above it
    f32 y0 = (20.0f + 16.0f + 6.0f) * uiScale; // = 42px from bottom at 720p

    f32 frac = (maxEnergy > 0.0f) ? energy / maxEnergy : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    // Background outline
    pushQuad(x0, y0, x0 + barW, y0 + barH, {0.3f, 0.3f, 0.3f});

    // Filled portion in blue
    Vec3 barColor = {0.2f, 0.4f, 1.0f};
    f32 fillW = barW * frac;
    f32 fillPad = 2.0f * uiScale;
    for (f32 y = y0 + fillPad; y < y0 + barH - fillPad; y += 2.0f * uiScale) {
        pushLine(x0 + fillPad, y, x0 + fillPad + fillW - fillPad * 2.0f, y, barColor);
    }

    flushHUD();
}

// Filled horizontal bar — draws a background rect then a filled portion up to pct.
// Coordinates are in HUD pixel space (origin bottom-left).
void HUD::drawFilledBar(u32 sw, u32 sh, f32 x, f32 y, f32 barW, f32 barH,
                        f32 pct, Vec3 bgColor, Vec3 fgColor) {
    s_screenW = sw; s_screenH = sh;
    // Background outline
    pushQuad(x, y, x + barW, y + barH, bgColor);
    // Filled portion as horizontal scan lines
    if (pct > 0.0f) {
        f32 fillW = barW * pct;
        f32 pad = 1.0f;
        for (f32 fy = y + pad; fy < y + barH - pad; fy += 1.0f) {
            pushLine(x + pad, fy, x + pad + fillW - pad * 2.0f, fy, fgColor);
        }
    }
    flushHUD();
}

// Expanding, fading ring for the "ability is back" pop. The HUD batch has no alpha,
// so the ring color is lerped toward black by the pop's remaining life (readyPopAlpha).
// Caller is responsible for flushHUD() afterward.
void HUD::drawReadyPop(f32 cx, f32 cy, f32 baseHalf, f32 t01, f32 scale, Vec3 color) {
    f32 r = HudCooldown::readyPopRadius(t01, baseHalf, scale);
    f32 a = HudCooldown::readyPopAlpha(t01);
    Vec3 c = color * a;                            // fade toward black as it grows
    pushQuad(cx - r, cy - r, cx + r, cy + r, c);   // pushQuad draws a hollow outline = ring
}
