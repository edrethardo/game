// hud_inventory.cpp — HUD inventory screen, item tooltips, and loot notification.
// Part of the HUD namespace split from hud.cpp. Name helpers (affixTypeName,
// rarityName, slotName, subtypeName, skillDisplayName, skillDescription) are
// local statics here since they are only consumed by this translation unit.
// Calls pushLine/pushQuad/flushHUD via hud_internal.h.

#include "renderer/hud.h"
#include "renderer/hud_internal.h"
#include "renderer/font.h"
#include "renderer/item_icons.h"
#include "game/item.h"
#include <cstdio>
#include <cstring>

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
        case AffixType::LIFESTEAL_PCT:      return "Lifesteal %";
        case AffixType::PROJECTILE_SPEED:   return "+Proj Speed";
        case AffixType::CONE_ANGLE:         return "+Swing Arc";
        case AffixType::DAMAGE_TO_FLYING:   return "+Dmg vs Flying";
        case AffixType::CLIP_SIZE_PCT:      return "+Clip Size %";
        case AffixType::RELOAD_SPEED_PCT:   return "+Reload Speed %";
        case AffixType::ENERGY_FLAT:        return "+Max Energy";
        case AffixType::ATTACK_SPEED_PCT:   return "+Attack Speed %";
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
        case ItemSlot::GLOVES:  return "Gloves";
        default:                return "";
    }
}

static const char* subtypeName(WeaponSubtype st) {
    switch (st) {
        case WeaponSubtype::SWORD:          return "Sword";
        case WeaponSubtype::DAGGER:         return "Dagger";
        case WeaponSubtype::AXE:            return "Axe";
        case WeaponSubtype::CLAYMORE:       return "Claymore";
        case WeaponSubtype::CLEAVER:        return "Cleaver";
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
        case SkillId::FRENZY:          return "Frenzy";
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
        case SkillId::SECOND_WIND:     return "Below 20% HP: heal 30% and\ngain 1.5s invulnerability. 60s cooldown.";
        case SkillId::SOUL_HARVEST:    return "Each kill: +5% speed, +3% damage\nfor 10s. Stacks up to 5 times.";
        case SkillId::GRAVITY_PULL:    return "Enemies within 5m are slowly\npulled toward you.";
        case SkillId::PHASE_STRIKE:    return "20% on kill: smoke bomb that\nblinds nearby enemies for 0.5s.";
        case SkillId::VOID_KILL:       return "15% on kill: void zone on corpse\ndealing 60% missing HP to nearby.";
        case SkillId::ARC_FIRE:        return "20% on hit: ignite the ground\nacross the full swing arc for 1.5s.";
        case SkillId::FRENZY:          return "Each hit: +5% attack speed for 4s.\nStacks up to 6 times (+30%).";
        default: return "";
    }
}

void HUD::drawLootNotification(u32 sw, u32 sh, Vec3 color, f32 alpha) {
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    // Center-top of screen
    f32 barW = 160.0f * uiScale;
    f32 barH = 8.0f * uiScale;
    f32 x0 = static_cast<f32>(sw) * 0.5f - barW * 0.5f;
    f32 y0 = static_cast<f32>(sh) - 60.0f * uiScale;

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

    flushHUD();
}

void HUD::drawInventoryScreen(u32 sw, u32 sh,
                               const PlayerInventory& inv,
                               const ItemDef* itemDefs,
                               u8 selectedSlot, bool selectedIsEquipped,
                               s32 mouseX, s32 mouseY)
{
    // Scale inventory layout relative to 720p reference height so it fits
    // in split-screen viewports (e.g. horizontal split = half height)
    f32 uiScale = static_cast<f32>(sh) / 720.0f;

    f32 centerY = static_cast<f32>(sh) * 0.5f;

    // --- Equipment panel (left side, raised to leave room for tooltips below) ---
    f32 eqX      = static_cast<f32>(sw) * 0.12f;
    f32 eqStartY = centerY + 220.0f * uiScale;
    f32 slotW    = 240.0f * uiScale;
    f32 slotH    = 32.0f * uiScale;
    f32 slotGap  = 5.0f * uiScale;

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
        flushHUD();
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
            flushHUD();
            // Icon on left side of slot
            ItemIconSystem::drawIcon(sw, sh, eqX + 3.0f * uiScale, y + 2.0f * uiScale, slotH - 4.0f * uiScale, def, item.rarity);
            // Item name to the right of icon
            FontSystem::drawText(sw, sh, eqX + slotH + 4.0f * uiScale, y + 9.0f * uiScale,
                                 def.name, rarityColor(item.rarity), 2);
        } else {
            // Slot type label for empty slots
            // Order MUST match the ItemSlot enum (GLOVES is appended after RING there).
            static const char* slotLabels[] = {"Weapon", "Offhand", "Helmet", "Armor", "Boots", "Ring", "Gloves"};
            Vec3 dimColor = {0.25f, 0.25f, 0.3f};
            FontSystem::drawText(sw, sh, eqX + 6.0f * uiScale, y + 8.0f * uiScale,
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

    // --- Backpack panel (closer to equipment, raised for tooltip space below) ---
    f32 bpX      = static_cast<f32>(sw) * 0.42f;
    f32 bpStartY = centerY + 180.0f * uiScale;
    f32 cellSize = 32.0f * uiScale;
    f32 cellGap  = 4.0f * uiScale;

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
        flushHUD();
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
            flushHUD();
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
            flushHUD();
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
                    const ItemDef& bpDef = itemDefs[inv.backpack[i].defId];
                    ItemSlot eqSlot = bpDef.slot;
                    u32 eqIdx = static_cast<u32>(eqSlot);

                    // Both tooltips below the inventory panels, side by side
                    // Left = equipped (matches equipment panel), Right = backpack item
                    f32 tooltipY = 80.0f * uiScale;
                    f32 leftTipX = eqX;
                    f32 rightTipX = eqX + 336.0f * uiScale;

                    // Left: currently equipped in matching slot
                    if (!isItemEmpty(inv.equipped[eqIdx])) {
                        drawItemTooltip(sw, sh, leftTipX, tooltipY,
                                        inv.equipped[eqIdx],
                                        itemDefs[inv.equipped[eqIdx].defId]);
                        // "EQUIPPED" label below the left tooltip
                        f32 boxW = 80.0f, boxH = 16.0f;
                        f32 boxX = leftTipX + (320.0f - boxW) * 0.5f;
                        f32 boxY = tooltipY - boxH - 4.0f;
                        Vec3 gold = {1.0f, 0.85f, 0.3f};
                        for (f32 fy = 0; fy < boxH; fy += 1.0f)
                            pushLine(boxX, boxY + fy, boxX + boxW, boxY + fy, {0.08f, 0.08f, 0.12f});
                        pushLine(boxX, boxY, boxX + boxW, boxY, gold * 0.6f);
                        pushLine(boxX, boxY + boxH, boxX + boxW, boxY + boxH, gold * 0.6f);
                        pushLine(boxX, boxY, boxX, boxY + boxH, gold * 0.6f);
                        pushLine(boxX + boxW, boxY, boxX + boxW, boxY + boxH, gold * 0.6f);
                        flushHUD();
                        f32 labelW = FontSystem::textWidth("EQUIPPED", 1);
                        FontSystem::drawText(sw, sh, boxX + (boxW - labelW) * 0.5f, boxY + 3.0f,
                                            "EQUIPPED", gold, 1);
                    } else {
                        char emptyLabel[32];
                        std::snprintf(emptyLabel, sizeof(emptyLabel),
                                      "Empty %s", slotName(eqSlot));
                        FontSystem::drawText(sw, sh, leftTipX, tooltipY,
                                            emptyLabel, {0.55f, 0.55f, 0.6f}, 1);
                    }

                    // Right: hovered backpack item
                    drawItemTooltip(sw, sh, rightTipX, tooltipY,
                                    inv.backpack[i], bpDef);
                }
                break;
            }
        }
    }

    flushHUD();
}

void HUD::drawItemTooltip(u32 sw, u32 sh, f32 tipX, f32 tipY,
                            const ItemInstance& item, const ItemDef& def)
{
    if (isItemEmpty(item)) return;

    Vec3 rColor = rarityColor(item.rarity);

    f32 nameScale = 2.5f;
    f32 bodyScale = 1.5f;
    f32 lineH = FontSystem::textHeight(bodyScale) + 3.0f;
    f32 nameH = FontSystem::textHeight(nameScale) + 6.0f;
    f32 padX = 10.0f;
    f32 padY = 8.0f;

    // Count lines for sizing
    u32 lineCount = 3; // rarity, slot, separator
    if (def.slot == ItemSlot::WEAPON) {
        lineCount += 1; // subtype
        lineCount += 3; // damage, cooldown, range
        if (def.weaponType == WeaponType::HITSCAN && def.baseClipSize > 0)
            lineCount += 1; // clip + reload
    } else {
        if (item.bonusHealth > 0.0f) lineCount += 1;
    }
    lineCount += item.affixCount;
    if (def.legendarySkillId != SkillId::NONE && item.rarity == Rarity::LEGENDARY) {
        lineCount += 6; // extra spacing + separator + skill name + 3 description lines
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

    flushHUD();

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
    flushHUD();
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

        // Clip size + reload time for hitscan weapons
        if (def.weaponType == WeaponType::HITSCAN && def.baseClipSize > 0) {
            std::snprintf(buf, sizeof(buf), "Clip: %u  Reload: %.1fs",
                          def.baseClipSize, def.baseReloadTime);
            FontSystem::drawText(sw, sh, textX, curY, buf, {0.9f, 0.7f, 0.9f}, bodyScale);
            curY -= lineH;
        }
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
        // Extra spacing before legendary section
        curY -= lineH;
        // Gold separator
        curY -= lineH * 0.3f;
        pushLine(textX, curY + lineH * 0.3f, tipX + tooltipW - padX, curY + lineH * 0.3f, {0.6f, 0.5f, 0.15f});
        flushHUD();
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
            case ItemSlot::GLOVES:  activationLabel = "On Hit"; activationColor = {0.9f, 0.6f, 0.2f}; break;
            default: break;
        }

        curY -= lineH; // extra line before skill text
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
