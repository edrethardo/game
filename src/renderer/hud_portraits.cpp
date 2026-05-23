// hud_portraits.cpp — HUD summon portrait and quickbar drawing. Part of the
// HUD namespace split from hud.cpp. Calls pushLine/pushQuad/flushHUD via
// hud_internal.h.

#include "renderer/hud.h"
#include "renderer/hud_internal.h"
#include "renderer/font.h"
#include "renderer/item_icons.h"
#include "game/item.h"
#include <cstdio>
#include <cstring>

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
        flushHUD();
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
        flushHUD();
    }
}

void HUD::drawQuickbar(u32 sw, u32 sh,
                        const QuickbarState& qb,
                        const PlayerInventory& inv,
                        const ItemDef* itemDefs,
                        f32 cooldownPct) {
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    f32 SLOT_SIZE = 40.0f * uiScale;
    f32 SLOT_GAP  = 4.0f * uiScale;
    // Total width of all 8 slots plus gaps between them
    f32 TOTAL_W   = QUICKBAR_SLOTS * SLOT_SIZE + (QUICKBAR_SLOTS - 1) * SLOT_GAP;
    f32 Y_OFFSET  = 20.0f * uiScale; // distance from bottom edge

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
        FontSystem::drawText(sw, sh, x0 + 2.0f * uiScale, y1 - 10.0f * uiScale, numStr,
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
            FontSystem::drawText(sw, sh, textX, y0 - 10.0f * uiScale, abbrev, rc, 1);

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

    flushHUD();
}
