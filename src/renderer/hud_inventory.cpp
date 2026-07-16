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
#include "game/skill.h"   // SkillSystem::findSkillDef — tier 2 of the description resolver
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
        case AffixType::ARMOR:              return "+Armor";
        case AffixType::HEALTH_REGEN:       return "+HP Regen";
        case AffixType::THORNS_PCT:         return "Thorns %";
        case AffixType::MANASTEAL_PCT:      return "Mana Steal %";
        case AffixType::MANA_ON_KILL:       return "+Mana on Kill";
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

// ---------------------------------------------------------------------------
// Skill name + description resolution.
//
// THREE sources, because no single one covers every skill:
//   1. a slot-specific override — for a skill that genuinely behaves differently depending on the
//      slot it rides in (BLOOD_NOVA: a FREE on-hit proc on a weapon, but a health-sacrificing
//      retaliation on armor/offhand). One shared string would have to lie about one of them.
//   2. SkillDef (skills.json) — the source of truth for anything that HAS a def, which is every
//      class skill and every castable legendary.
//   3. the legacy C++ tables below — the ONLY source for the ~10 legendary PASSIVES that have no
//      SkillDef at all (Thorns, Berserker, Second Wind, ...; the proc code notes as much:
//      "No def = this legendary isn't a weapon proc").
//
// Both the item tooltip and the skill-bar tooltip resolve through here, so the same skill can never
// describe itself two different ways in two different places.
// ---------------------------------------------------------------------------

// Fallback name table — legendary-only, and returns "Unknown" for every class skill, so it is a
// FALLBACK, not the primary: SkillDef.name is what actually covers the class skills.
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
        case SkillId::DIVINE_JUDGMENT: return "Divine Judgment";
        case SkillId::SHADOW_RICOCHET: return "Shadow Ricochet";
        case SkillId::STATIC_CHARGE:    return "Static Charge";
        case SkillId::HEMOPHAGE:        return "Hemophage";
        case SkillId::PROJECTILE_PARRY: return "Mirror Parry";
        default: return "Unknown";
    }
}

// Tier 1 + tier 3 of the resolver (see the block comment above): the slot-specific overrides, then
// the fallback table. Tier 2 (SkillDef.description, skills.json) is applied by
// resolveSkillDescription below, which reaches this only when the def has nothing to say.
//
// The table below therefore holds ONLY the legendary passives that have NO SkillDef — every skill
// that has one is described in skills.json instead. Do not re-add a case for a skill with a def:
// tier 2 wins, so the line would be unreachable, and unreachable text that looks authoritative is
// exactly how a tooltip rots away from what the code does.
static const char* skillDescription(SkillId id, ItemSlot slot) {
    // ---- Slot-specific overrides ----
    // Divine Judgment is TWO different things. Cast actively (the Paladin's 4th class skill) it
    // cleanses/heals/shields and calls pillars down. Worn as a RING (Phoenix Band) it is instead an
    // automatic below-25%-HP rescue (engine_update_skills.cpp tickArmorRingPassives). skills.json
    // describes the ACTIVE cast, so without this override the def would win and the Phoenix Band
    // would go back to describing something it does not do.
    if (id == SkillId::DIVINE_JUDGMENT && slot == ItemSlot::RING) {
        return "Below 25% HP: full heal, cleanse all\ndebuffs, stun nearby foes. 45s cooldown.";
    }
    // Thunderwall: chain_lightning on an OFFHAND is a perfect-block riposte, not a cast.
    // (chain_lightning HAS a skills.json def, so without this tier-1 override the def's
    // castable-skill text would win and the shield would describe something it doesn't do.)
    if (id == SkillId::CHAIN_LIGHTNING && slot == ItemSlot::OFFHAND) {
        return "On perfect block: arc lightning\nthrough your attacker.";
    }
    if (id == SkillId::BLOOD_NOVA) {
        switch (slot) {
            case ItemSlot::WEAPON:
                // The weapon proc costs no health — it is not a "sacrifice", and saying so was wrong.
                return "20% on hit: erupt in a ring of\nblood, savaging nearby enemies.";
            case ItemSlot::ARMOR:
                return "When struck: sacrifice 20% health to\nerupt in a blood nova.";
            case ItemSlot::OFFHAND:
                return "On perfect block: sacrifice 20% health\nto erupt in a blood nova.";
            default:
                break;  // active cast (right-click / quickbar) — the shared text below is accurate
        }
    }

    switch (id) {
        case SkillId::THROWAWAY:       return "On empty clip, throw weapon as\nan explosive projectile.";
        case SkillId::LIFE_STEAL:      return "Heal 5% of all damage dealt.";
        case SkillId::THORNS:          return "Reflect 20% of damage taken\nback to the nearest enemy.";
        case SkillId::BERSERKER:       return "+1% damage for each 1% of\nmissing health. Risk vs reward.";
        case SkillId::SECOND_WIND:     return "Below 20% HP: heal 30% and\ngain 1.5s invulnerability. 60s cooldown.";
        case SkillId::SOUL_HARVEST:    return "Each kill: +5% speed, +3% damage\nfor 10s. Stacks up to 5 times.";
        case SkillId::GRAVITY_PULL:    return "Enemies within 5m are slowly\npulled toward you.";
        case SkillId::PHASE_STRIKE:    return "20% on kill: smoke bomb that\nblinds nearby enemies for 0.5s.";
        case SkillId::VOID_KILL:       return "15% on kill: void zone on corpse\ndealing 60% missing HP to nearby.";
        case SkillId::ARC_FIRE:        return "20% on hit: ignite the ground\nacross the full swing arc for 1.5s.";
        case SkillId::STATIC_CHARGE:    return "Hits you take build charge. At 5\nstacks: discharge chain lightning.";
        case SkillId::HEMOPHAGE:        return "Enemies within 4m constantly\nbleed life to you.";
        case SkillId::PROJECTILE_PARRY: return "Perfectly blocked projectiles\nreflect back at double damage.";
        default: return "";
    }
}

// The full three-tier resolution. `slot` may be ItemSlot::COUNT for a skill that isn't riding in an
// item at all (a class skill), which simply means no slot override applies.
const char* HUD::resolveSkillDescription(SkillId id, ItemSlot slot,
                                         const SkillDef* skillDefs, u32 skillDefCount) {
    // 1. slot override wins outright — it exists precisely because the def's single description
    //    would be wrong for this slot. Both of these skills have a SkillDef, so without this gate
    //    tier 2 would silently overwrite the correct per-slot text with the generic one.
    if ((id == SkillId::BLOOD_NOVA &&
         (slot == ItemSlot::WEAPON || slot == ItemSlot::ARMOR || slot == ItemSlot::OFFHAND)) ||
        (id == SkillId::DIVINE_JUDGMENT && slot == ItemSlot::RING)) {
        return skillDescription(id, slot);
    }
    // 2. the def (skills.json).
    if (skillDefs) {
        const SkillDef* sd = SkillSystem::findSkillDef(skillDefs, skillDefCount, id);
        if (sd && sd->description[0] != '\0') return sd->description;
    }
    // 3. the def-less passives.
    return skillDescription(id, slot);
}

const char* HUD::resolveSkillName(SkillId id, const SkillDef* skillDefs, u32 skillDefCount) {
    if (skillDefs) {
        const SkillDef* sd = SkillSystem::findSkillDef(skillDefs, skillDefCount, id);
        if (sd && sd->name[0] != '\0') return sd->name;
    }
    return skillDisplayName(id);   // def-less passives (and the "Unknown" backstop)
}

// Skill tooltip — deliberately the same frame as drawItemTooltip below (same width, padding, fill,
// border, screen-clamp and \n-split body), so a skill reads like an item on this screen.
//
// The stats block is printed straight off the SkillDef and only for NON-ZERO fields, which is what
// makes it structurally unable to claim a cost the skill doesn't charge — the failure mode that
// produced the Phoenix Band / Blood armor tooltip bugs.
void HUD::drawSkillTooltip(u32 sw, u32 sh, f32 tipX, f32 tipY, const SkillTooltipInfo& info) {
    // Frame metrics scale by the same factor FontSystem bakes into every glyph (see drawItemTooltip
    // — this frame is contractually identical to that one, so it scales the same way).
    f32 ui        = FontSystem::getUIScale();
    f32 nameScale = 2.5f;
    f32 bodyScale = 1.5f;
    f32 lineH     = FontSystem::textHeight(bodyScale) + 3.0f * ui;
    f32 nameH     = FontSystem::textHeight(nameScale) + 4.0f * ui;
    f32 padX      = 10.0f * ui;
    f32 padY      = 8.0f * ui;

    // Count body lines up front so the frame can be sized before anything is drawn. Fractional
    // because the separators consume half a line each, matching drawItemTooltip's rule().
    f32 lineCount = 1.0f;                    // subtitle ("Class Skill" / "Legendary - Armor")
    const char* d = info.description;
    if (d && *d) {
        lineCount += 0.5f;                   // separator
        lineCount += 1.0f;                   // first description line
        for (const char* c = d; *c; c++) if (*c == '\n') lineCount += 1.0f;
    }
    u32 statLines = 0;
    if (info.def) {
        if (info.def->cooldown     > 0.0f) statLines++;
        if (info.def->energyCost   > 0.0f) statLines++;
        if (info.def->healthCostPct> 0.0f) statLines++;
        if (info.def->damage       > 0.0f) statLines++;
        if (info.def->radius       > 0.0f) statLines++;
        if (info.def->duration     > 0.0f) statLines++;
    }
    if (statLines > 0) lineCount += static_cast<f32>(statLines) + 0.5f;   // + separator
    if (info.unlockFloor > 0) lineCount += 1.0f;     // unlock / locked line
    if (info.upgraded)        lineCount += 1.0f;

    // Width: scaled 320 minimum (matches drawItemTooltip), widened to fit the longest measured
    // line. Only the name and the description lines can realistically exceed the minimum — the
    // stat/unlock rows are bounded snprintf formats, always narrower.
    f32 maxW = FontSystem::textWidth(info.name, nameScale);
    {
        f32 w = FontSystem::textWidth(info.subtitle, bodyScale);
        if (w > maxW) maxW = w;
    }
    if (d && *d) {
        const char* seg = d;
        while (*seg) {
            const char* eol = seg;
            while (*eol && *eol != '\n') eol++;
            char tmp[80];   // same truncation as the draw loop below, so we measure what is drawn
            u32 len = static_cast<u32>(eol - seg);
            if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
            std::memcpy(tmp, seg, len);
            tmp[len] = '\0';
            f32 w = FontSystem::textWidth(tmp, bodyScale);
            if (w > maxW) maxW = w;
            seg = (*eol == '\n') ? eol + 1 : eol;
        }
    }
    f32 tooltipW = 320.0f * ui;
    if (maxW + padX * 2.0f > tooltipW) tooltipW = maxW + padX * 2.0f;

    f32 tooltipH = nameH + lineCount * lineH + padY * 2.0f;

    // Clamp into the screen — the bars sit at the bottom-left, so an un-clamped tooltip would run
    // off both edges. (drawItemTooltip does the same.)
    if (tipX + tooltipW > static_cast<f32>(sw)) tipX = static_cast<f32>(sw) - tooltipW - 4.0f;
    if (tipX < 4.0f) tipX = 4.0f;
    if (tipY + tooltipH > static_cast<f32>(sh)) tipY = static_cast<f32>(sh) - tooltipH - 4.0f;
    if (tipY < 4.0f) tipY = 4.0f;

    // Frame: dark fill + border. Locked skills get a muted border so "you can't use this yet" reads
    // before any text does.
    // Same construction as drawItemTooltip: scanline fill, then pushQuad as the outline.
    const Vec3 bgColor = {0.06f, 0.06f, 0.10f};
    for (f32 y = tipY; y < tipY + tooltipH; y += 1.0f) {
        pushLine(tipX, y, tipX + tooltipW, y, bgColor);
    }
    const Vec3 border = info.unlocked ? Vec3{0.45f, 0.42f, 0.30f} : Vec3{0.35f, 0.20f, 0.20f};
    pushQuad(tipX, tipY, tipX + tooltipW, tipY + tooltipH, border);
    flushHUD();

    const f32 textX = tipX + padX;
    f32 curY = tipY + tooltipH - padY - nameH;

    FontSystem::drawText(sw, sh, textX, curY, info.name,
                         info.unlocked ? Vec3{1.0f, 0.9f, 0.5f} : Vec3{0.6f, 0.5f, 0.4f}, nameScale);
    curY -= lineH;

    char buf[96];
    FontSystem::drawText(sw, sh, textX, curY, info.subtitle, {0.55f, 0.55f, 0.65f}, bodyScale);
    curY -= lineH;

    if (d && *d) {
        // 0.3 offset / 0.5 consumption keep the rule clear of the neighbouring rows' glyphs — the
        // old 0.4/0.2 drew it through the ascenders of the first description line.
        pushLine(textX, curY + lineH * 0.3f, tipX + tooltipW - padX, curY + lineH * 0.3f,
                 {0.3f, 0.3f, 0.35f});
        flushHUD();
        curY -= lineH * 0.5f;
        const char* line = d;
        while (*line) {
            const char* eol = line;
            while (*eol && *eol != '\n') eol++;
            char descLine[80];
            u32 len = static_cast<u32>(eol - line);
            if (len >= sizeof(descLine)) len = sizeof(descLine) - 1;
            std::memcpy(descLine, line, len);
            descLine[len] = '\0';
            FontSystem::drawText(sw, sh, textX, curY, descLine, {0.75f, 0.75f, 0.80f}, bodyScale);
            curY -= lineH;
            line = (*eol == '\n') ? eol + 1 : eol;
        }
    }

    // Stats — only the fields this skill actually uses.
    if (statLines > 0) {
        // Same 0.3/0.5 separator geometry as above (and as drawItemTooltip's rule()).
        pushLine(textX, curY + lineH * 0.3f, tipX + tooltipW - padX, curY + lineH * 0.3f,
                 {0.3f, 0.3f, 0.35f});
        flushHUD();
        curY -= lineH * 0.5f;
        const Vec3 statCol = {0.65f, 0.75f, 0.85f};
        const SkillDef& sd = *info.def;
        if (sd.cooldown > 0.0f) {
            std::snprintf(buf, sizeof(buf), "Cooldown   %.1fs", static_cast<double>(sd.cooldown));
            FontSystem::drawText(sw, sh, textX, curY, buf, statCol, bodyScale); curY -= lineH;
        }
        if (sd.energyCost > 0.0f) {
            std::snprintf(buf, sizeof(buf), "Energy     %.0f", static_cast<double>(sd.energyCost));
            FontSystem::drawText(sw, sh, textX, curY, buf, statCol, bodyScale); curY -= lineH;
        }
        if (sd.healthCostPct > 0.0f) {
            std::snprintf(buf, sizeof(buf), "Health     %.0f%%",
                          static_cast<double>(sd.healthCostPct * 100.0f));
            FontSystem::drawText(sw, sh, textX, curY, buf, {0.85f, 0.45f, 0.45f}, bodyScale); curY -= lineH;
        }
        if (sd.damage > 0.0f) {
            std::snprintf(buf, sizeof(buf), "Damage     %.0f", static_cast<double>(sd.damage));
            FontSystem::drawText(sw, sh, textX, curY, buf, statCol, bodyScale); curY -= lineH;
        }
        if (sd.radius > 0.0f) {
            std::snprintf(buf, sizeof(buf), "Radius     %.1fm", static_cast<double>(sd.radius));
            FontSystem::drawText(sw, sh, textX, curY, buf, statCol, bodyScale); curY -= lineH;
        }
        if (sd.duration > 0.0f) {
            std::snprintf(buf, sizeof(buf), "Duration   %.1fs", static_cast<double>(sd.duration));
            FontSystem::drawText(sw, sh, textX, curY, buf, statCol, bodyScale); curY -= lineH;
        }
    }

    // Class-skill progression. A LOCKED skill must say what unlocks it — an empty tooltip on a
    // greyed slot tells the player nothing.
    if (info.unlockFloor > 0) {
        if (info.unlocked) {
            std::snprintf(buf, sizeof(buf), "Unlocked (floor %u)", info.unlockFloor);
            FontSystem::drawText(sw, sh, textX, curY, buf, {0.5f, 0.75f, 0.5f}, bodyScale);
        } else {
            std::snprintf(buf, sizeof(buf), "Locked - unlocks on floor %u", info.unlockFloor);
            FontSystem::drawText(sw, sh, textX, curY, buf, {0.85f, 0.45f, 0.45f}, bodyScale);
        }
        curY -= lineH;
    }
    if (info.upgraded) {
        std::snprintf(buf, sizeof(buf), "Upgraded (floor %u)", info.upgradeFloor);
        FontSystem::drawText(sw, sh, textX, curY, buf, {0.9f, 0.8f, 0.3f}, bodyScale);
        curY -= lineH;
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
                               const SkillDef* skillDefs, u32 skillDefCount,
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
                    drawItemTooltip(sw, sh, eqX + slotW + 8.0f * uiScale, y,
                                    inv.equipped[i], itemDefs[inv.equipped[i].defId],
                                    skillDefs, skillDefCount);
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

                    // Both tooltips below the inventory panels, side by side.
                    // Left = equipped (matches equipment panel), Right = backpack item.
                    // The right tooltip is placed off the LEFT frame's real (content-fitted,
                    // returned) width — the old fixed offset left a dead gap at 1080p and made the
                    // pair overlap in split-screen, where the frames are narrower than the offset.
                    f32 tooltipY = 80.0f * uiScale;
                    f32 leftTipX = eqX;
                    f32 leftW    = 320.0f * uiScale;   // frame minimum — stands in when slot empty

                    // Left: currently equipped in matching slot
                    if (!isItemEmpty(inv.equipped[eqIdx])) {
                        leftW = drawItemTooltip(sw, sh, leftTipX, tooltipY,
                                        inv.equipped[eqIdx],
                                        itemDefs[inv.equipped[eqIdx].defId],
                                        skillDefs, skillDefCount);
                        // "EQUIPPED" label below the left tooltip, centered under its real width
                        f32 boxW = 80.0f * uiScale, boxH = 16.0f * uiScale;
                        f32 boxX = leftTipX + (leftW - boxW) * 0.5f;
                        f32 boxY = tooltipY - boxH - 4.0f * uiScale;
                        Vec3 gold = {1.0f, 0.85f, 0.3f};
                        for (f32 fy = 0; fy < boxH; fy += 1.0f)
                            pushLine(boxX, boxY + fy, boxX + boxW, boxY + fy, {0.08f, 0.08f, 0.12f});
                        pushLine(boxX, boxY, boxX + boxW, boxY, gold * 0.6f);
                        pushLine(boxX, boxY + boxH, boxX + boxW, boxY + boxH, gold * 0.6f);
                        pushLine(boxX, boxY, boxX, boxY + boxH, gold * 0.6f);
                        pushLine(boxX + boxW, boxY, boxX + boxW, boxY + boxH, gold * 0.6f);
                        flushHUD();
                        f32 labelW = FontSystem::textWidth("EQUIPPED", 1);
                        FontSystem::drawText(sw, sh, boxX + (boxW - labelW) * 0.5f, boxY + 3.0f * uiScale,
                                            "EQUIPPED", gold, 1);
                    } else {
                        char emptyLabel[32];
                        std::snprintf(emptyLabel, sizeof(emptyLabel),
                                      "Empty %s", slotName(eqSlot));
                        FontSystem::drawText(sw, sh, leftTipX, tooltipY,
                                            emptyLabel, {0.55f, 0.55f, 0.6f}, 1);
                    }

                    // Right: hovered backpack item
                    drawItemTooltip(sw, sh, leftTipX + leftW + 16.0f * uiScale, tooltipY,
                                    inv.backpack[i], bpDef, skillDefs, skillDefCount);
                }
                break;
            }
        }
    }

    flushHUD();
}

f32 HUD::drawItemTooltip(u32 sw, u32 sh, f32 tipX, f32 tipY,
                            const ItemInstance& item, const ItemDef& def,
                            const SkillDef* skillDefs, u32 skillDefCount)
{
    if (isItemEmpty(item)) return 0.0f;

    Vec3 rColor = rarityColor(item.rarity);

    // Frame metrics scale by the SAME factor FontSystem bakes into every glyph (sh/720, set by
    // renderHUD before this runs). The frame used to be fixed raw pixels while the text inside
    // auto-scaled, so above 720p every long line ran straight through the right border.
    f32 ui = FontSystem::getUIScale();
    f32 nameScale = 2.5f;
    f32 bodyScale = 1.5f;
    f32 lineH = FontSystem::textHeight(bodyScale) + 3.0f * ui;
    f32 nameH = FontSystem::textHeight(nameScale) + 6.0f * ui;
    f32 padX = 10.0f * ui;
    f32 padY = 8.0f * ui;

    // The frame is sized by a dry run (pass 0) of the same emitters that later draw the content
    // (pass 1), so the box cannot disagree with what lands in it. The old hand-maintained line
    // count drifted from the draw code twice — the Endless Flight lines were never counted and the
    // weapon subtype was counted even when absent — and every such drift is text outside the frame.
    bool measuring = true;
    f32  used = 0.0f;    // vertical consumption in lineH units (separators take fractions)
    f32  maxW = 0.0f;    // widest line; textWidth already includes the UI scale
    f32  frameW = 0.0f;  // fixed between the passes; separators span it in pass 1
    f32  curY = 0.0f;

    auto line = [&](const char* s, Vec3 c, f32 scale) {
        if (measuring) {
            f32 w = FontSystem::textWidth(s, scale);
            if (w > maxW) maxW = w;
            used += 1.0f;
        } else {
            FontSystem::drawText(sw, sh, tipX + padX, curY, s, c, scale);
            curY -= lineH;
        }
    };
    auto gap = [&](f32 frac) {
        if (measuring) used += frac;
        else           curY -= lineH * frac;
    };
    // Separator rule. The 0.3 offset inside a 0.5 consumption is what keeps the line clear of both
    // the previous row's descenders and the next row's ascenders.
    auto rule = [&](Vec3 c) {
        if (measuring) { used += 0.5f; return; }
        pushLine(tipX + padX, curY + lineH * 0.3f, tipX + frameW - padX, curY + lineH * 0.3f, c);
        flushHUD();
        curY -= lineH * 0.5f;
    };

    auto emitBody = [&]() {
        char buf[80];

        line(rarityName(item.rarity), rColor, bodyScale);
        // A pet consumable is not really a "Ring" — its def only claims that slot to satisfy
        // the loader (see ItemDef.petSummon). Present it as what it is.
        line(def.petSummon ? "Companion" : slotName(def.slot), {0.7f, 0.7f, 0.75f}, bodyScale);

        if (def.slot == ItemSlot::WEAPON && def.weaponSubtype != WeaponSubtype::NONE) {
            line(subtypeName(def.weaponSubtype), {0.55f, 0.55f, 0.6f}, bodyScale);
        }

        rule({0.3f, 0.3f, 0.35f});

        // Pet consumable: no stats or affixes — say what using it does. ("Use" is the equip
        // action: double-click / A / a quickbar slot; Engine::tryUsePetItem intercepts them all.)
        if (def.petSummon) {
            line("Use: summon or dismiss your", {1.0f, 0.82f, 0.2f}, bodyScale);
            // Enemy-bound minis get the generic line — the item NAME already says which
            // creature; the goblin jackpot keeps its bespoke one.
            line(def.petEnemyIdx != 0xFF ? "miniature companion"
                                         : "mini goblin companion",
                 {1.0f, 0.82f, 0.2f}, bodyScale);
            line("Infinite uses", {0.55f, 0.55f, 0.6f}, bodyScale);
        }

        // Stats
        if (def.slot == ItemSlot::WEAPON) {
            std::snprintf(buf, sizeof(buf), "Damage: %.0f", item.damage);
            line(buf, {1.0f, 0.9f, 0.7f}, bodyScale);

            std::snprintf(buf, sizeof(buf), "Speed: %.2fs", def.baseCooldown);
            line(buf, {0.7f, 0.9f, 0.7f}, bodyScale);

            if (def.weaponType == WeaponType::MELEE) {
                std::snprintf(buf, sizeof(buf), "Range: %.1fm", def.baseRange);
            } else if (def.weaponType == WeaponType::HITSCAN) {
                std::snprintf(buf, sizeof(buf), "Range: %.0fm", def.baseRange);
            } else {
                std::snprintf(buf, sizeof(buf), "Proj Speed: %.0f", def.baseProjectileSpeed);
            }
            line(buf, {0.7f, 0.7f, 0.9f}, bodyScale);

            // Clip size + reload time for hitscan weapons
            if (def.weaponType == WeaponType::HITSCAN && def.baseClipSize > 0) {
                std::snprintf(buf, sizeof(buf), "Clip: %u  Reload: %.1fs",
                              def.baseClipSize, def.baseReloadTime);
                line(buf, {0.9f, 0.7f, 0.9f}, bodyScale);
            }
        } else {
            if (item.bonusHealth > 0.0f) {
                std::snprintf(buf, sizeof(buf), "+%.0f Health", item.bonusHealth);
                line(buf, {0.7f, 1.0f, 0.7f}, bodyScale);
            }
        }

        // Affixes
        for (u8 a = 0; a < item.affixCount; a++) {
            const Affix& affix = item.affixes[a];
            std::snprintf(buf, sizeof(buf), "%s: +%.1f", affixTypeName(affix.type), affix.value);
            line(buf, {0.4f, 0.85f, 1.0f}, bodyScale);
        }

        // Legendary skill — only shown on legendary-rarity items
        if (def.legendarySkillId != SkillId::NONE && item.rarity == Rarity::LEGENDARY) {
            gap(1.0f);                    // extra spacing before legendary section
            rule({0.6f, 0.5f, 0.15f});    // gold separator
            gap(1.0f);                    // extra line before skill text

            // Activation method depends on equipment slot
            const char* activationLabel = "Skill";
            Vec3 activationColor = {1.0f, 0.82f, 0.2f};
            switch (def.slot) {
                case ItemSlot::WEAPON:  activationLabel = "On Hit"; activationColor = {1.0f, 0.6f, 0.2f}; break;
                // Rings are PASSIVE, not right-clickable. Every ring legendary in items.json (Berserker,
                // Life Steal, Thorns, Phase Strike, Soul Harvest, Void Kill, Gravity Pull, Divine
                // Judgment) is consumed by m_ringPassive in tickArmorRingPassives / serverNetPost — none
                // is reachable from the right-click skill path, which fires class skills only. The old
                // "Right Click" label told the player to press a button that does nothing.
                case ItemSlot::RING:    activationLabel = "Passive"; break;
                case ItemSlot::BOOTS:   activationLabel = "Press F"; activationColor = {0.3f, 1.0f, 0.5f}; break;
                case ItemSlot::HELMET:  activationLabel = "Press G"; activationColor = {0.5f, 0.8f, 1.0f}; break;
                case ItemSlot::ARMOR:   activationLabel = "Passive Aura"; activationColor = {0.7f, 0.7f, 1.0f}; break;
                case ItemSlot::OFFHAND: activationLabel = "Perfect Block"; activationColor = {0.9f, 0.9f, 1.0f}; break;
                case ItemSlot::GLOVES:  activationLabel = "On Hit"; activationColor = {0.9f, 0.6f, 0.2f}; break;
                default: break;
            }

            // Resolve through the shared entry point so this text is IDENTICAL to what the skill-bar
            // tooltip shows for the same skill.
            const char* sName = resolveSkillName(def.legendarySkillId, skillDefs, skillDefCount);
            std::snprintf(buf, sizeof(buf), "[%s] %s", activationLabel, sName);
            line(buf, activationColor, bodyScale);

            // Skill description (split on \n)
            const char* desc = resolveSkillDescription(def.legendarySkillId, def.slot,
                                                      skillDefs, skillDefCount);
            const char* seg = desc;
            while (*seg) {
                const char* eol = seg;
                while (*eol && *eol != '\n') eol++;
                char descLine[80];
                u32 len = static_cast<u32>(eol - seg);
                if (len >= sizeof(descLine)) len = sizeof(descLine) - 1;
                std::memcpy(descLine, seg, len);
                descLine[len] = '\0';
                line(descLine, {0.9f, 0.75f, 0.3f}, bodyScale);
                seg = (*eol == '\n') ? eol + 1 : eol;
            }
        }

        // Infinity Chakram: it has no legendarySkill (the endless flight IS its gimmick), so give it
        // its own descriptor line. Shown whenever the def is flagged — the behavior is always active.
        if (def.infiniteFlight) {
            gap(1.0f);
            line("Endless Flight", {1.0f, 0.82f, 0.2f}, bodyScale);
            line("Bounces forever until it strikes a foe.", {0.9f, 0.75f, 0.3f}, bodyScale);
        }
    };

    // --- Pass 0: measure. The name header lives outside emitBody (it has its own row height), so
    // seed the width with it here.
    maxW = FontSystem::textWidth(def.name, nameScale);
    emitBody();

    frameW = 320.0f * ui;                    // minimum — keeps short tooltips from looking starved
    if (maxW + padX * 2.0f > frameW) frameW = maxW + padX * 2.0f;
    // The name header consumes TWO nameH rows in the draw path below (its baseline is offset by
    // nameH from the top pad, then curY steps down another nameH past it) — budget both. The last
    // body line needs no trailing advance, hence used-1: its baseline then lands exactly on the
    // bottom pad.
    f32 frameH = padY * 2.0f + nameH * 2.0f + (used - 1.0f) * lineH;

    // Clamp to screen
    if (tipX + frameW > static_cast<f32>(sw)) tipX = static_cast<f32>(sw) - frameW - 4.0f;
    if (tipY + frameH > static_cast<f32>(sh)) tipY = static_cast<f32>(sh) - frameH - 4.0f;
    if (tipX < 0) tipX = 4.0f;
    if (tipY < 0) tipY = 4.0f;

    // Dark background
    Vec3 bgColor = {0.06f, 0.06f, 0.10f};
    for (f32 y = tipY; y < tipY + frameH; y += 1.0f) {
        pushLine(tipX, y, tipX + frameW, y, bgColor);
    }

    // Border in rarity color (double border for legendaries)
    Vec3 borderColor = {rColor.x * 0.6f, rColor.y * 0.6f, rColor.z * 0.6f};
    pushQuad(tipX, tipY, tipX + frameW, tipY + frameH, borderColor);
    if (item.rarity == Rarity::LEGENDARY) {
        pushQuad(tipX + 1, tipY + 1, tipX + frameW - 1, tipY + frameH - 1, borderColor);
    }

    flushHUD();

    // --- Pass 1: draw. Same emitters, now with a real cursor.
    curY = tipY + frameH - padY - nameH;
    FontSystem::drawText(sw, sh, tipX + padX, curY, def.name, rColor, nameScale);
    curY -= nameH;
    measuring = false;
    emitBody();

    return frameW;
}
