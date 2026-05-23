// inventory.cpp — Player and NPC inventory management: equip/unequip, stat recalculation, effective weapon/health.
#include "game/item.h"
#include "core/log.h"
#include "renderer/material.h"

#include <cmath>

// ============================================================
//  Inventory
// ============================================================

void Inventory::init(PlayerInventory& inv) {
    inv = PlayerInventory{};
    for (u32 i = 0; i < static_cast<u32>(ItemSlot::COUNT); i++)
        inv.equipped[i].defId = 0xFFFF;
    for (u32 i = 0; i < MAX_INVENTORY_ITEMS; i++)
        inv.backpack[i].defId = 0xFFFF;
    inv.backpackCount = 0;
}

static Inventory::StatsChangedCallback s_statsChangedCb = nullptr;
void Inventory::setStatsChangedCallback(StatsChangedCallback cb) { s_statsChangedCb = cb; }

void Inventory::recalculateStats(PlayerInventory& inv) {
    inv.bonusDamageFlat         = 0.0f;
    inv.bonusDamagePct          = 0.0f;
    inv.bonusHealthFlat         = 0.0f;
    inv.bonusHealthPct          = 0.0f;
    inv.bonusMoveSpeed          = 0.0f;
    inv.bonusCooldownReduction  = 0.0f;
    inv.bonusLifeOnHit          = 0.0f;
    inv.bonusProjectileSpeedPct = 0.0f;
    inv.bonusConeAngle          = 0.0f;
    inv.bonusDamageToFlying     = 0.0f;
    inv.bonusClipSizePct        = 0.0f;
    inv.bonusReloadSpeedPct     = 0.0f;
    inv.bonusEnergyFlat         = 0.0f;  // was missing — accumulated across every recalc

    for (u32 s = 0; s < static_cast<u32>(ItemSlot::COUNT); s++) {
        const ItemInstance& equipped = inv.equipped[s];
        if (isItemEmpty(equipped)) continue;

        for (u8 a = 0; a < equipped.affixCount; a++) {
            const Affix& affix = equipped.affixes[a];
            switch (affix.type) {
                case AffixType::DAMAGE_FLAT:        inv.bonusDamageFlat         += affix.value; break;
                case AffixType::HEALTH_FLAT:        inv.bonusHealthFlat         += affix.value; break;
                case AffixType::MOVE_SPEED_FLAT:    inv.bonusMoveSpeed          += affix.value; break;
                case AffixType::DAMAGE_PCT:         inv.bonusDamagePct          += affix.value; break;
                case AffixType::COOLDOWN_REDUCTION: {
                    // Multiplicative stacking: (1-a)*(1-b) gives diminishing returns
                    // affix.value is a percentage (e.g. 10.0 = 10%), convert to fraction
                    f32 frac = affix.value / 100.0f;
                    inv.bonusCooldownReduction = 1.0f - (1.0f - inv.bonusCooldownReduction) * (1.0f - frac);
                } break;
                case AffixType::HEALTH_PCT:         inv.bonusHealthPct          += affix.value; break;
                case AffixType::LIFE_ON_HIT:        inv.bonusLifeOnHit          += affix.value; break;
                case AffixType::PROJECTILE_SPEED:   inv.bonusProjectileSpeedPct += affix.value; break;
                case AffixType::CONE_ANGLE:         inv.bonusConeAngle          += affix.value; break;
                case AffixType::DAMAGE_TO_FLYING:   inv.bonusDamageToFlying     += affix.value; break;
                case AffixType::CLIP_SIZE_PCT:      inv.bonusClipSizePct        += affix.value; break;
                case AffixType::RELOAD_SPEED_PCT:   inv.bonusReloadSpeedPct     += affix.value; break;
                case AffixType::ENERGY_FLAT:        inv.bonusEnergyFlat         += affix.value; break;
                default: break;
            }
        }
    }

    // Cap CDR at 92% (minimum cooldown = 8% of base)
    if (inv.bonusCooldownReduction > 0.92f)
        inv.bonusCooldownReduction = 0.92f;
    // Cap reload speed at 60%
    if (inv.bonusReloadSpeedPct > 60.0f)
        inv.bonusReloadSpeedPct = 60.0f;

    if (s_statsChangedCb) s_statsChangedCb(inv);
}

void Inventory::recalculateNpcStats(NpcEquipment& equip) {
    // Same logic as recalculateStats but operates on the NPC equipment struct
    equip.bonusDamageFlat         = 0.0f;
    equip.bonusDamagePct          = 0.0f;
    equip.bonusHealthFlat         = 0.0f;
    equip.bonusHealthPct          = 0.0f;
    equip.bonusMoveSpeed          = 0.0f;
    equip.bonusCooldownReduction  = 0.0f;
    equip.bonusLifeOnHit          = 0.0f;
    equip.bonusProjectileSpeedPct = 0.0f;
    equip.bonusConeAngle          = 0.0f;
    equip.bonusDamageToFlying     = 0.0f;

    for (u32 s = 0; s < static_cast<u32>(ItemSlot::COUNT); s++) {
        const ItemInstance& equipped = equip.equipped[s];
        if (isItemEmpty(equipped)) continue;

        // Add health from armor items
        equip.bonusHealthFlat += equipped.bonusHealth;

        for (u8 a = 0; a < equipped.affixCount; a++) {
            const Affix& affix = equipped.affixes[a];
            switch (affix.type) {
                case AffixType::DAMAGE_FLAT:        equip.bonusDamageFlat         += affix.value; break;
                case AffixType::HEALTH_FLAT:        equip.bonusHealthFlat         += affix.value; break;
                case AffixType::MOVE_SPEED_FLAT:    equip.bonusMoveSpeed          += affix.value; break;
                case AffixType::DAMAGE_PCT:         equip.bonusDamagePct          += affix.value; break;
                case AffixType::COOLDOWN_REDUCTION: {
                    f32 frac = affix.value / 100.0f;
                    equip.bonusCooldownReduction = 1.0f - (1.0f - equip.bonusCooldownReduction) * (1.0f - frac);
                } break;
                case AffixType::HEALTH_PCT:         equip.bonusHealthPct          += affix.value; break;
                case AffixType::LIFE_ON_HIT:        equip.bonusLifeOnHit          += affix.value; break;
                case AffixType::PROJECTILE_SPEED:   equip.bonusProjectileSpeedPct += affix.value; break;
                case AffixType::CONE_ANGLE:         equip.bonusConeAngle          += affix.value; break;
                case AffixType::DAMAGE_TO_FLYING:   equip.bonusDamageToFlying     += affix.value; break;
                default: break;
            }
        }
    }
    // Cap CDR at 92% (minimum cooldown = 8% of base)
    if (equip.bonusCooldownReduction > 0.92f)
        equip.bonusCooldownReduction = 0.92f;
}

bool Inventory::addToBackpack(PlayerInventory& inv, const ItemInstance& item) {
    // First look for an empty slot among already-used slots
    for (u8 i = 0; i < MAX_INVENTORY_ITEMS; i++) {
        if (inv.backpack[i].defId == 0xFFFF) {
            inv.backpack[i] = item;
            if (i >= inv.backpackCount)
                inv.backpackCount = static_cast<u8>(i + 1);
            return true;
        }
    }
    return false;
}

void Inventory::equip(PlayerInventory& inv, u8 backpackIndex, const ItemDef* itemDefs) {
    if (backpackIndex >= MAX_INVENTORY_ITEMS) return;

    ItemInstance& bpItem = inv.backpack[backpackIndex];
    if (isItemEmpty(bpItem)) return;

    // Resolve slot from the item definition
    u32 slotIdx = static_cast<u32>(itemDefs[bpItem.defId].slot);

    ItemInstance& equippedSlot = inv.equipped[slotIdx];
    ItemInstance  oldEquipped  = equippedSlot;

    // Place backpack item into the equipped slot
    equippedSlot = bpItem;

    // If there was something equipped, put it in the backpack slot we just freed
    if (!isItemEmpty(oldEquipped))
        bpItem = oldEquipped;
    else
        bpItem.defId = 0xFFFF;

    recalculateStats(inv);
}

bool Inventory::unequip(PlayerInventory& inv, ItemSlot slot) {
    u32 slotIdx = static_cast<u32>(slot);
    if (slotIdx >= static_cast<u32>(ItemSlot::COUNT)) return false;

    ItemInstance& equippedSlot = inv.equipped[slotIdx];
    if (isItemEmpty(equippedSlot)) return false;

    // Find first empty backpack slot
    for (u8 i = 0; i < MAX_INVENTORY_ITEMS; i++) {
        if (inv.backpack[i].defId == 0xFFFF) {
            inv.backpack[i] = equippedSlot;
            if (i >= inv.backpackCount)
                inv.backpackCount = static_cast<u8>(i + 1);
            equippedSlot.defId = 0xFFFF;
            recalculateStats(inv);
            return true;
        }
    }

    LOG_WARN("Inventory: cannot unequip — backpack is full");
    return false;
}

ItemInstance Inventory::dropFromBackpack(PlayerInventory& inv, u8 backpackIndex) {
    if (backpackIndex >= MAX_INVENTORY_ITEMS) {
        ItemInstance empty;
        empty.defId = 0xFFFF;
        return empty;
    }

    ItemInstance copy = inv.backpack[backpackIndex];
    inv.backpack[backpackIndex].defId = 0xFFFF;

    // Shrink backpackCount if this was the last occupied slot
    if (backpackIndex + 1 == inv.backpackCount) {
        // Walk backwards to find the new last occupied index
        while (inv.backpackCount > 0 &&
               inv.backpack[inv.backpackCount - 1].defId == 0xFFFF) {
            inv.backpackCount--;
        }
    }

    return copy;
}

// Shared core: builds a WeaponDef from a definition + inventory bonuses + a pre-rolled damage value.
// Both getEffectiveWeapon and getWeaponFromItem delegate here to avoid duplicate logic.
static WeaponDef buildWeaponDef(const ItemDef& def, const PlayerInventory& inv, f32 baseDamage) {
    WeaponDef wd;
    wd.name            = def.name;
    wd.type            = def.weaponType;

    // Flat damage scaled by fire rate — sqrt falloff for fast weapons (gentler
    // than linear), linear for slow weapons (full ratio, no cap).
    f32 cdRef = 0.4f;
    f32 ratio = (def.baseCooldown > 0.05f) ? (def.baseCooldown / cdRef) : 1.0f;
    f32 flatScale = (ratio <= 1.0f) ? sqrtf(ratio) : (ratio * ratio);
    f32 rawDamage      = baseDamage + inv.bonusDamageFlat * flatScale;
    // Apply percentage bonus (stored as a raw multiplier addition, e.g. 10 = +10%)
    wd.damage          = rawDamage * (1.0f + inv.bonusDamagePct / 100.0f);

    // Cooldown reduced by cooldownReduction (0.0–0.5)
    wd.cooldown        = def.baseCooldown * (1.0f - inv.bonusCooldownReduction);
    if (wd.cooldown < 0.05f) wd.cooldown = 0.05f; // hard minimum to prevent division by zero

    wd.range           = def.baseRange;
    wd.coneAngleDeg    = def.baseConeAngle + inv.bonusConeAngle;
    wd.projectileSpeed = def.baseProjectileSpeed * (1.0f + inv.bonusProjectileSpeedPct / 100.0f);
    wd.projectileRadius = def.baseProjectileRadius;
    wd.recoilKick      = def.baseRecoil;

    // Clip size: base + percentage bonus from affixes
    if (def.baseClipSize > 0) {
        wd.clipSize = static_cast<u8>(def.baseClipSize * (1.0f + inv.bonusClipSizePct / 100.0f));
        if (wd.clipSize < 1) wd.clipSize = 1;
    } else {
        wd.clipSize = 0;
    }
    // Reload time: base reduced by reload speed bonus
    wd.reloadTime = def.baseReloadTime * (1.0f - inv.bonusReloadSpeedPct / 100.0f);
    if (def.baseReloadTime > 0.0f && wd.reloadTime < 0.2f) wd.reloadTime = 0.2f;

    // Crit: daggers crit far more often; every other weapon has a small baseline.
    // Tune these here — single source of truth for per-subtype crit chance/mult.
    if (def.weaponSubtype == WeaponSubtype::DAGGER) {
        wd.critChance = 0.20f;  // 20% crit chance for daggers
        wd.critMult   = 2.5f;   // 2.5× damage on dagger crits
    } else {
        wd.critChance = 0.05f;  // 5% baseline for all other weapons
        wd.critMult   = 2.0f;   // 2× damage on standard crits
    }

    return wd;
}

WeaponDef Inventory::getEffectiveWeapon(const PlayerInventory& inv,
                                         const ItemDef* itemDefs,
                                         const WeaponDef& baseWeapon) {
    const ItemInstance& equipped = inv.equipped[static_cast<u32>(ItemSlot::WEAPON)];
    if (isItemEmpty(equipped)) return baseWeapon;
    return buildWeaponDef(itemDefs[equipped.defId], inv, equipped.damage);
}

WeaponDef Inventory::getWeaponFromItem(const PlayerInventory& inv,
                                       const ItemDef* itemDefs,
                                       const ItemInstance& item) {
    return buildWeaponDef(itemDefs[item.defId], inv, item.damage);
}

f32 Inventory::getEffectiveMaxHealth(const PlayerInventory& inv, f32 baseMaxHealth) {
    // Include bonusHealth contributions from each equipped item's rolled base stat
    f32 totalHealthFlat = inv.bonusHealthFlat;
    for (u32 s = 0; s < static_cast<u32>(ItemSlot::COUNT); s++) {
        const ItemInstance& eq = inv.equipped[s];
        if (!isItemEmpty(eq))
            totalHealthFlat += eq.bonusHealth;
    }
    return (baseMaxHealth + totalHealthFlat) * (1.0f + inv.bonusHealthPct / 100.0f);
}

ItemInstance Inventory::dropFromEquipment(PlayerInventory& inv, ItemSlot slot) {
    u32 slotIdx = static_cast<u32>(slot);
    if (slotIdx >= static_cast<u32>(ItemSlot::COUNT)) {
        ItemInstance empty;
        empty.defId = 0xFFFF;
        return empty;
    }

    ItemInstance copy = inv.equipped[slotIdx];
    inv.equipped[slotIdx].defId = 0xFFFF;
    recalculateStats(inv);
    return copy;
}
