#include "renderer/hud.h"
#include "renderer/shader.h"
#include "renderer/font.h"
#include "core/log.h"
#include "core/profiler.h"
#include "game/item.h"
#include <glad/glad.h>
#include <cstdio>

// Simple 2D line renderer for HUD elements (crosshair, hit markers).
// Uses a small dynamic VBO with position+color, drawn with the debug shader
// but with an orthographic VP matrix mapping pixels to NDC.

struct HudVertex {
    Vec3 pos;
    Vec3 color;
};

static constexpr u32 MAX_HUD_VERTS = 1024;

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
    s_shader = ShaderSystem::load("assets/shaders/debug.vert", "assets/shaders/debug.frag");

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
    f32 eqX      = static_cast<f32>(sw) * 0.15f;
    f32 eqStartY = centerY + 100.0f;
    f32 slotW    = 140.0f;
    f32 slotH    = 26.0f;
    f32 slotGap  = 4.0f;

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

        // Power fill bar inside slot
        if (!isItemEmpty(item)) {
            f32 fillW = slotW * 0.9f * (item.damage / 80.0f);
            if (fillW > slotW * 0.9f) fillW = slotW * 0.9f;
            f32 barY = y + 4.0f;
            for (f32 line = 0; line < 4.0f; line += 1.0f) {
                pushLine(eqX + 4.0f, barY + line, eqX + 4.0f + fillW, barY + line, color);
            }
        }

        // Selection arrow
        if (selected) {
            Vec3 hi = {1.0f, 1.0f, 1.0f};
            pushLine(eqX - 10.0f, y + slotH * 0.5f, eqX - 4.0f, y + slotH * 0.5f, hi);
        }
    }

    // --- Backpack panel (right side, 6 columns x 4 rows) ---
    f32 bpX      = static_cast<f32>(sw) * 0.55f;
    f32 bpStartY = centerY + 60.0f;
    f32 cellSize = 26.0f;
    f32 cellGap  = 3.0f;

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

        // Fill filled slots
        if (!isItemEmpty(item)) {
            Vec3 fillColor = {color.x * 0.5f, color.y * 0.5f, color.z * 0.5f};
            for (f32 line = 2.0f; line < cellSize - 2.0f; line += 1.0f) {
                pushLine(x + 2.0f, y + line, x + cellSize - 2.0f, y + line, fillColor);
            }
        }

        // Selection highlight
        if (selected) {
            Vec3 hi = {1.0f, 1.0f, 1.0f};
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

void HUD::drawItemTooltip(u32 sw, u32 sh, f32 tipX, f32 tipY,
                            const ItemInstance& item, const ItemDef& def)
{
    if (isItemEmpty(item)) return;

    Vec3 rColor = rarityColor(item.rarity);

    // Calculate tooltip dimensions
    u32 nameScale = 2;
    u32 bodyScale = 1;
    f32 lineH = FontSystem::textHeight(bodyScale) + 3.0f;
    f32 nameH = FontSystem::textHeight(nameScale) + 4.0f;
    f32 padX = 8.0f;
    f32 padY = 6.0f;

    // Count lines for sizing
    u32 lineCount = 3; // rarity, slot, blank separator
    if (def.slot == ItemSlot::WEAPON) {
        lineCount += 1; // subtype
        lineCount += 3; // damage, cooldown, range
    } else {
        lineCount += 1; // health
    }
    lineCount += item.affixCount; // affix lines
    if (def.legendarySkillId != SkillId::NONE) lineCount += 1;

    f32 tooltipW = 180.0f;
    f32 tooltipH = padY * 2 + nameH + lineCount * lineH;

    // Clamp tooltip to screen bounds
    if (tipX + tooltipW > static_cast<f32>(sw)) tipX = static_cast<f32>(sw) - tooltipW - 4.0f;
    if (tipY + tooltipH > static_cast<f32>(sh)) tipY = static_cast<f32>(sh) - tooltipH - 4.0f;
    if (tipX < 0) tipX = 4.0f;
    if (tipY < 0) tipY = 4.0f;

    // Draw dark background
    Vec3 bgColor = {0.08f, 0.08f, 0.12f};
    for (f32 y = tipY; y < tipY + tooltipH; y += 1.0f) {
        pushLine(tipX, y, tipX + tooltipW, y, bgColor);
    }

    // Border in rarity color
    Vec3 borderColor = {rColor.x * 0.6f, rColor.y * 0.6f, rColor.z * 0.6f};
    pushQuad(tipX, tipY, tipX + tooltipW, tipY + tooltipH, borderColor);

    flushHUD(sw, sh);

    // Draw text content
    f32 textX = tipX + padX;
    f32 curY = tipY + tooltipH - padY - nameH; // start from top (y increases upward)

    // Item name
    FontSystem::drawText(sw, sh, textX, curY, def.name, rColor, nameScale);
    curY -= nameH;

    // Rarity
    FontSystem::drawText(sw, sh, textX, curY, rarityName(item.rarity), rColor, bodyScale);
    curY -= lineH;

    // Slot type
    FontSystem::drawText(sw, sh, textX, curY, slotName(def.slot), {0.8f, 0.8f, 0.8f}, bodyScale);
    curY -= lineH;

    // Weapon subtype
    if (def.slot == ItemSlot::WEAPON && def.weaponSubtype != WeaponSubtype::NONE) {
        FontSystem::drawText(sw, sh, textX, curY, subtypeName(def.weaponSubtype), {0.6f, 0.6f, 0.6f}, bodyScale);
        curY -= lineH;
    }

    // Separator line
    curY -= lineH * 0.5f;

    // Stats
    char buf[64];
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
        std::snprintf(buf, sizeof(buf), "%s: %.1f", name, affix.value);
        FontSystem::drawText(sw, sh, textX, curY, buf, {0.4f, 0.8f, 1.0f}, bodyScale);
        curY -= lineH;
    }

    // Legendary skill
    if (def.legendarySkillId != SkillId::NONE) {
        FontSystem::drawText(sw, sh, textX, curY, "* Legendary Power *", {1.0f, 0.5f, 0.0f}, bodyScale);
        curY -= lineH;
    }
}
