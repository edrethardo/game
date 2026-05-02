#include "game/item.h"
#include "core/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <json/nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================
//  ItemLoader
// ============================================================

static ItemSlot slotFromString(const std::string& s) {
    if (s == "weapon"  || s == "WEAPON")  return ItemSlot::WEAPON;
    if (s == "offhand" || s == "OFFHAND") return ItemSlot::OFFHAND;
    if (s == "helmet"  || s == "HELMET")  return ItemSlot::HELMET;
    if (s == "armor"   || s == "ARMOR")   return ItemSlot::ARMOR;
    if (s == "boots"   || s == "BOOTS")   return ItemSlot::BOOTS;
    if (s == "ring"    || s == "RING")    return ItemSlot::RING;
    return ItemSlot::WEAPON;
}

static Rarity rarityFromString(const std::string& s) {
    if (s == "common"    || s == "COMMON")    return Rarity::COMMON;
    if (s == "magic"     || s == "MAGIC")     return Rarity::MAGIC;
    if (s == "rare"      || s == "RARE")      return Rarity::RARE;
    if (s == "legendary" || s == "LEGENDARY") return Rarity::LEGENDARY;
    return Rarity::COMMON;
}

static WeaponType weaponTypeFromString(const std::string& s) {
    if (s == "melee"      || s == "MELEE")      return WeaponType::MELEE;
    if (s == "hitscan"    || s == "HITSCAN")    return WeaponType::HITSCAN;
    if (s == "projectile" || s == "PROJECTILE") return WeaponType::PROJECTILE;
    return WeaponType::MELEE;
}

static SkillId skillIdFromString(const std::string& s) {
    if (s == "frozen_orb"      || s == "FROZEN_ORB")      return SkillId::FROZEN_ORB;
    if (s == "chain_lightning" || s == "CHAIN_LIGHTNING") return SkillId::CHAIN_LIGHTNING;
    if (s == "meteor_strike"   || s == "METEOR_STRIKE")   return SkillId::METEOR_STRIKE;
    if (s == "blood_nova"      || s == "BLOOD_NOVA")      return SkillId::BLOOD_NOVA;
    if (s == "phase_dash"      || s == "PHASE_DASH")      return SkillId::PHASE_DASH;
    return SkillId::NONE;
}

static AffixType affixTypeFromString(const std::string& s) {
    if (s == "damage_flat"        || s == "DAMAGE_FLAT")        return AffixType::DAMAGE_FLAT;
    if (s == "health_flat"        || s == "HEALTH_FLAT")        return AffixType::HEALTH_FLAT;
    if (s == "move_speed_flat"    || s == "MOVE_SPEED_FLAT")    return AffixType::MOVE_SPEED_FLAT;
    if (s == "damage_pct"         || s == "DAMAGE_PCT")         return AffixType::DAMAGE_PCT;
    if (s == "cooldown_reduction" || s == "COOLDOWN_REDUCTION") return AffixType::COOLDOWN_REDUCTION;
    if (s == "health_pct"         || s == "HEALTH_PCT")         return AffixType::HEALTH_PCT;
    if (s == "life_on_hit"        || s == "LIFE_ON_HIT")        return AffixType::LIFE_ON_HIT;
    if (s == "projectile_speed"   || s == "PROJECTILE_SPEED")   return AffixType::PROJECTILE_SPEED;
    if (s == "cone_angle"         || s == "CONE_ANGLE")         return AffixType::CONE_ANGLE;
    if (s == "range_bonus"        || s == "RANGE_BONUS")        return AffixType::RANGE_BONUS;
    if (s == "damage_to_flying"   || s == "DAMAGE_TO_FLYING")   return AffixType::DAMAGE_TO_FLYING;
    return AffixType::DAMAGE_FLAT;
}

// Read an entire file into a malloc'd buffer (caller frees). Returns nullptr on failure.
static char* readFileToBuffer(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return nullptr;

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    char* buf = static_cast<char*>(std::malloc(size + 1));
    if (!buf) { std::fclose(f); return nullptr; }

    std::fread(buf, 1, size, f);
    buf[size] = '\0';
    std::fclose(f);
    return buf;
}

bool ItemLoader::loadItemDefs(const char* path, ItemDef* defs, u32& count) {
    char* buf = readFileToBuffer(path);
    if (!buf) {
        LOG_WARN("ItemLoader: cannot open %s", path);
        return false;
    }

    try {
        json doc = json::parse(buf);
        std::free(buf);

        if (!doc.contains("items") || !doc["items"].is_array()) {
            LOG_WARN("ItemLoader: missing 'items' array in %s", path);
            return false;
        }

        count = 0;
        for (auto& entry : doc["items"]) {
            if (count >= MAX_ITEM_DEFS) {
                LOG_WARN("ItemLoader: too many item defs (max %u), truncating", MAX_ITEM_DEFS);
                break;
            }

            ItemDef& def = defs[count];
            def = {};

            std::string name = entry.value("name", "unnamed");
            std::strncpy(def.name, name.c_str(), sizeof(def.name) - 1);

            std::string slotStr = entry.value("slot", "WEAPON");
            def.slot = slotFromString(slotStr);

            std::string maxRarityStr = entry.value("maxRarity", "COMMON");
            def.maxRarity = rarityFromString(maxRarityStr);

            std::string weaponTypeStr = entry.value("weaponType", "MELEE");
            def.weaponType = weaponTypeFromString(weaponTypeStr);

            def.baseDamage           = entry.value("baseDamage",           0.0f);
            def.baseRange            = entry.value("baseRange",            0.0f);
            def.baseCooldown         = entry.value("baseCooldown",         0.0f);
            def.baseConeAngle        = entry.value("baseConeAngle",        0.0f);
            def.baseProjectileSpeed  = entry.value("baseProjectileSpeed",  0.0f);
            def.baseProjectileRadius = entry.value("baseProjectileRadius", 0.0f);
            def.baseRecoil           = entry.value("baseRecoil",           0.0f);
            def.baseHealth           = entry.value("baseHealth",           0.0f);

            std::string legendaryStr = entry.value("legendarySkill", "NONE");
            def.legendarySkillId = skillIdFromString(legendaryStr);

            def.minLevel = static_cast<u8>(entry.value("minLevel", 1));
            def.maxLevel = static_cast<u8>(entry.value("maxLevel", 10));

            count++;
        }

        LOG_INFO("ItemLoader: loaded %u item defs from %s", count, path);
        return true;
    } catch (const json::exception& e) {
        std::free(buf);
        LOG_WARN("ItemLoader: JSON parse error in %s: %s", path, e.what());
        return false;
    }
}

bool ItemLoader::loadAffixDefs(const char* path, AffixDef* defs, u32& count) {
    char* buf = readFileToBuffer(path);
    if (!buf) {
        LOG_WARN("ItemLoader: cannot open %s", path);
        return false;
    }

    try {
        json doc = json::parse(buf);
        std::free(buf);

        if (!doc.contains("affixes") || !doc["affixes"].is_array()) {
            LOG_WARN("ItemLoader: missing 'affixes' array in %s", path);
            return false;
        }

        count = 0;
        for (auto& entry : doc["affixes"]) {
            if (count >= MAX_AFFIX_DEFS) {
                LOG_WARN("ItemLoader: too many affix defs (max %u), truncating", MAX_AFFIX_DEFS);
                break;
            }

            AffixDef& def = defs[count];
            def = {};

            std::string name = entry.value("name", "unnamed");
            std::strncpy(def.name, name.c_str(), sizeof(def.name) - 1);

            std::string typeStr = entry.value("type", "DAMAGE_FLAT");
            def.type = affixTypeFromString(typeStr);

            def.minValue = entry.value("minValue", 0.0f);
            def.maxValue = entry.value("maxValue", 0.0f);

            // Build slot bitmask from array of slot name strings
            // Bit 0 = WEAPON, bit 1 = OFFHAND, bit 2 = HELMET, bit 3 = ARMOR,
            // bit 4 = BOOTS, bit 5 = RING
            def.validSlots = 0;
            if (entry.contains("slots") && entry["slots"].is_array()) {
                for (auto& slotEntry : entry["slots"]) {
                    std::string slotStr = slotEntry.get<std::string>();
                    ItemSlot sl = slotFromString(slotStr);
                    def.validSlots |= static_cast<u8>(1u << static_cast<u32>(sl));
                }
            } else {
                // Default: valid for all slots
                def.validSlots = 0xFF;
            }

            count++;
        }

        LOG_INFO("ItemLoader: loaded %u affix defs from %s", count, path);
        return true;
    } catch (const json::exception& e) {
        std::free(buf);
        LOG_WARN("ItemLoader: JSON parse error in %s: %s", path, e.what());
        return false;
    }
}

bool ItemLoader::loadSkillDefs(const char* path, SkillDef* defs, u32& count) {
    char* buf = readFileToBuffer(path);
    if (!buf) {
        LOG_WARN("ItemLoader: cannot open %s", path);
        return false;
    }

    try {
        json doc = json::parse(buf);
        std::free(buf);

        if (!doc.contains("skills") || !doc["skills"].is_array()) {
            LOG_WARN("ItemLoader: missing 'skills' array in %s", path);
            return false;
        }

        count = 0;
        for (auto& entry : doc["skills"]) {
            if (count >= MAX_SKILL_DEFS) {
                LOG_WARN("ItemLoader: too many skill defs (max %u), truncating", MAX_SKILL_DEFS);
                break;
            }

            SkillDef& def = defs[count];
            def = {};

            std::string idStr = entry.value("id", "NONE");
            def.id = skillIdFromString(idStr);

            std::string name = entry.value("name", "unnamed");
            std::strncpy(def.name, name.c_str(), sizeof(def.name) - 1);

            def.cooldown        = entry.value("cooldown",        1.0f);
            def.energyCost      = entry.value("energyCost",      0.0f);
            def.damage          = entry.value("damage", entry.value("orbDamage", 0.0f));
            def.radius          = entry.value("radius", entry.value("orbRadius", 0.0f));
            def.duration        = entry.value("duration",        0.0f);
            def.projectileSpeed = entry.value("projectileSpeed", entry.value("orbSpeed", 0.0f));
            def.projectileCount = static_cast<u8>(entry.value("projectileCount", 0));

            // Frozen Orb specifics
            def.shardDamage   = entry.value("shardDamage",   0.0f);
            def.shardCount    = static_cast<u8>(entry.value("shardCount", 0));
            def.shardInterval = entry.value("shardInterval", 0.0f);
            def.shardSpeed    = entry.value("shardSpeed",    0.0f);
            def.shardRadius   = entry.value("shardRadius",   0.0f);
            def.angleStepDeg  = entry.value("angleStepDeg",  0.0f);

            // Chain Lightning specifics
            def.bounces      = static_cast<u8>(entry.value("bounces", 0));
            def.bounceRange  = entry.value("bounceRange",  0.0f);
            def.damageFalloff = entry.value("damageFalloff", 0.0f);

            // Blood Nova specifics
            def.healthCostPct = entry.value("healthCostPct", 0.0f);

            // Meteor Strike specifics
            def.delay = entry.value("delay", 0.0f);

            // Phase Dash specifics
            def.distance       = entry.value("distance",       0.0f);
            def.corridorWidth  = entry.value("corridorWidth",  0.0f);
            def.invulnDuration = entry.value("invulnDuration", 0.0f);

            count++;
        }

        LOG_INFO("ItemLoader: loaded %u skill defs from %s", count, path);
        return true;
    } catch (const json::exception& e) {
        std::free(buf);
        LOG_WARN("ItemLoader: JSON parse error in %s: %s", path, e.what());
        return false;
    }
}

// ============================================================
//  ItemGen
// ============================================================

static u32 s_rngState = 12345;
static u32 s_uidCounter = 1;

// LCG — fast, deterministic, small footprint (suitable for Switch)
static inline u32 lcgNext() {
    s_rngState = s_rngState * 1664525u + 1013904223u;
    return s_rngState;
}

// Returns a value in [0.0, 1.0)
static inline f32 randF01() {
    return static_cast<f32>(lcgNext() >> 8) / static_cast<f32>(1u << 24);
}

// Returns a value in [0, range)
static inline u32 randU32(u32 range) {
    if (range == 0) return 0;
    return lcgNext() % range;
}

void ItemGen::init(u32 seed) {
    s_rngState   = seed;
    s_uidCounter = 1;
    LOG_INFO("ItemGen: RNG seeded with %u", seed);
}

Rarity ItemGen::rollRarity(u8 enemyLevel) {
    // Base rates (%) — sum = 100
    f32 commonPct    = 60.0f;
    f32 magicPct     = 28.0f;
    f32 rarePct      = 10.0f;
    f32 legendaryPct =  2.0f;

    // Per level above 1: -1% common, +0.5% legendary, rest absorbed by magic/rare
    f32 levelsAbove1 = static_cast<f32>(enemyLevel > 1 ? enemyLevel - 1 : 0);
    commonPct    -= levelsAbove1 * 1.0f;
    legendaryPct += levelsAbove1 * 0.5f;

    // Clamp to sane ranges
    if (commonPct    < 0.0f)   commonPct    = 0.0f;
    if (legendaryPct > 50.0f)  legendaryPct = 50.0f;

    // Remaining budget split evenly between magic and rare to keep sum = 100
    f32 remaining = 100.0f - commonPct - legendaryPct;
    if (remaining < 0.0f) remaining = 0.0f;
    magicPct = remaining * (28.0f / 38.0f);
    rarePct  = remaining * (10.0f / 38.0f);

    f32 roll = randF01() * 100.0f;

    if (roll < legendaryPct)                          return Rarity::LEGENDARY;
    if (roll < legendaryPct + rarePct)                return Rarity::RARE;
    if (roll < legendaryPct + rarePct + magicPct)     return Rarity::MAGIC;
    return Rarity::COMMON;
}

void ItemGen::rollAffixes(ItemInstance& item, u8 itemLevel, ItemSlot slot,
                           const AffixDef* affixDefs, u32 affixDefCount) {
    // Determine how many affixes to roll based on rarity
    u8 minAffixes = 0;
    u8 maxAffixes = 0;
    switch (item.rarity) {
        case Rarity::COMMON:    minAffixes = 0; maxAffixes = 0; break;
        case Rarity::MAGIC:     minAffixes = 1; maxAffixes = 2; break;
        case Rarity::RARE:      minAffixes = 2; maxAffixes = 4; break;
        case Rarity::LEGENDARY: minAffixes = 2; maxAffixes = 3; break;
        default: break;
    }

    u8 affixCount = (maxAffixes > minAffixes)
        ? static_cast<u8>(minAffixes + randU32(maxAffixes - minAffixes + 1))
        : minAffixes;

    if (affixCount > MAX_AFFIXES_PER_ITEM)
        affixCount = MAX_AFFIXES_PER_ITEM;

    // Build list of valid affix candidates for this slot
    u32 slotBit = 1u << static_cast<u32>(slot);
    u32 candidateIndices[MAX_AFFIX_DEFS];
    u32 candidateCount = 0;
    for (u32 i = 0; i < affixDefCount; i++) {
        if (affixDefs[i].validSlots & slotBit)
            candidateIndices[candidateCount++] = i;
    }

    item.affixCount = 0;

    // Track which AffixTypes have already been assigned (no duplicate types)
    bool usedTypes[static_cast<u32>(AffixType::COUNT)] = {};

    f32 levelScale = 1.0f + 0.1f * static_cast<f32>(itemLevel);

    for (u8 a = 0; a < affixCount && item.affixCount < MAX_AFFIXES_PER_ITEM; a++) {
        // Shuffle-pick a random candidate that has an unused type
        // Collect remaining valid candidates
        u32 valid[MAX_AFFIX_DEFS];
        u32 validCount = 0;
        for (u32 c = 0; c < candidateCount; c++) {
            u32 idx = candidateIndices[c];
            u32 typeIdx = static_cast<u32>(affixDefs[idx].type);
            if (!usedTypes[typeIdx])
                valid[validCount++] = idx;
        }
        if (validCount == 0) break;

        u32 pick = valid[randU32(validCount)];
        const AffixDef& ad = affixDefs[pick];

        Affix affix;
        affix.type  = ad.type;
        affix.value = (ad.minValue + randF01() * (ad.maxValue - ad.minValue)) * levelScale;

        item.affixes[item.affixCount++] = affix;
        usedTypes[static_cast<u32>(ad.type)] = true;
    }
}

ItemInstance ItemGen::rollItem(u8 enemyLevel, const ItemDef* defs, u32 defCount,
                                const AffixDef* affixDefs, u32 affixDefCount) {
    // Collect valid defs for this enemy level
    u32 validIndices[MAX_ITEM_DEFS];
    u32 validCount = 0;
    for (u32 i = 0; i < defCount; i++) {
        if (enemyLevel >= defs[i].minLevel && enemyLevel <= defs[i].maxLevel)
            validIndices[validCount++] = i;
    }

    if (validCount == 0) {
        LOG_WARN("ItemGen: no valid item defs for enemy level %u", enemyLevel);
        return {};
    }

    u32 pick = validIndices[randU32(validCount)];
    const ItemDef& def = defs[pick];

    ItemInstance item;
    item.defId     = static_cast<u16>(pick);
    item.itemLevel = enemyLevel;
    item.uid       = s_uidCounter++;

    // Roll rarity, capped by the definition's maxRarity
    Rarity rolled = rollRarity(enemyLevel);
    if (rolled > def.maxRarity) rolled = def.maxRarity;
    item.rarity = rolled;

    // Scale base stats
    f32 levelMult = 1.0f + 0.15f * static_cast<f32>(enemyLevel);
    item.damage      = def.baseDamage  * levelMult;
    item.bonusHealth = def.baseHealth  * levelMult;

    // Roll affixes
    rollAffixes(item, enemyLevel, def.slot, affixDefs, affixDefCount);

    // Legendary items get their fixed skill affix appended (informational — stored separately)
    // The legendarySkillId is on the def; no affix slot consumed for it.

    return item;
}

// ============================================================
//  Inventory
// ============================================================

void Inventory::init(PlayerInventory& inv) {
    inv = {};
    for (u32 i = 0; i < static_cast<u32>(ItemSlot::COUNT); i++)
        inv.equipped[i].defId = 0xFFFF;
    for (u32 i = 0; i < MAX_INVENTORY_ITEMS; i++)
        inv.backpack[i].defId = 0xFFFF;
    inv.backpackCount = 0;
}

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
    inv.bonusRange              = 0.0f;
    inv.bonusDamageToFlying     = 0.0f;

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
                case AffixType::COOLDOWN_REDUCTION: inv.bonusCooldownReduction  += affix.value; break;
                case AffixType::HEALTH_PCT:         inv.bonusHealthPct          += affix.value; break;
                case AffixType::LIFE_ON_HIT:        inv.bonusLifeOnHit          += affix.value; break;
                case AffixType::PROJECTILE_SPEED:   inv.bonusProjectileSpeedPct += affix.value; break;
                case AffixType::CONE_ANGLE:         inv.bonusConeAngle          += affix.value; break;
                case AffixType::RANGE_BONUS:        inv.bonusRange              += affix.value; break;
                case AffixType::DAMAGE_TO_FLYING:   inv.bonusDamageToFlying     += affix.value; break;
                default: break;
            }
        }
    }

    // Cap cooldown reduction at 50%
    if (inv.bonusCooldownReduction > 0.5f)
        inv.bonusCooldownReduction = 0.5f;
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

WeaponDef Inventory::getEffectiveWeapon(const PlayerInventory& inv,
                                         const ItemDef* itemDefs,
                                         const WeaponDef& baseWeapon) {
    const ItemInstance& equipped = inv.equipped[static_cast<u32>(ItemSlot::WEAPON)];
    if (isItemEmpty(equipped)) return baseWeapon;

    const ItemDef& def = itemDefs[equipped.defId];

    WeaponDef wd;
    wd.name            = def.name;
    wd.type            = def.weaponType;

    // Base damage from the rolled item, plus flat bonus from affixes
    f32 rawDamage      = equipped.damage + inv.bonusDamageFlat;
    // Apply percentage bonus (stored as a raw multiplier addition, e.g. 10 = +10%)
    wd.damage          = rawDamage * (1.0f + inv.bonusDamagePct / 100.0f);

    // Cooldown reduced by cooldownReduction (0.0–0.5)
    wd.cooldown        = def.baseCooldown * (1.0f - inv.bonusCooldownReduction);
    if (wd.cooldown < 0.05f) wd.cooldown = 0.05f; // hard minimum

    wd.range           = def.baseRange + inv.bonusRange;
    wd.coneAngleDeg    = def.baseConeAngle + inv.bonusConeAngle;
    wd.projectileSpeed = def.baseProjectileSpeed * (1.0f + inv.bonusProjectileSpeedPct / 100.0f);
    wd.projectileRadius = def.baseProjectileRadius;
    wd.recoilKick      = def.baseRecoil;

    return wd;
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

// ============================================================
//  WorldItemSystem
// ============================================================

void WorldItemSystem::init(WorldItemPool& pool) {
    pool = {};
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        pool.items[i] = {};
        pool.items[i].active = false;
    }
    pool.activeCount = 0;
    pool.nextUid     = 1;
    LOG_INFO("WorldItemSystem: pool initialized (%u slots)", MAX_WORLD_ITEMS);
}

void WorldItemSystem::update(WorldItemPool& pool, f32 dt) {
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = pool.items[i];
        if (!wi.active) continue;

        wi.lifetime       -= dt;
        wi.bobTimer       += dt;
        wi.exclusiveTimer -= dt;

        if (wi.lifetime <= 0.0f) {
            wi.active = false;
            if (pool.activeCount > 0)
                pool.activeCount--;
        }
    }
}

bool WorldItemSystem::spawn(WorldItemPool& pool, const ItemInstance& item, Vec3 position) {
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = pool.items[i];
        if (wi.active) continue;

        wi.item          = item;
        wi.position      = position;
        wi.bobTimer      = 0.0f;
        wi.lifetime      = 60.0f;
        wi.exclusiveTimer = 3.0f;
        wi.ownerSlot     = 0xFF;
        wi.active        = true;
        pool.activeCount++;
        return true;
    }

    LOG_WARN("WorldItemSystem: pool full, cannot spawn item");
    return false;
}

bool WorldItemSystem::tryPickup(WorldItemPool& pool, Vec3 playerPos, u8 playerSlot,
                                  ItemInstance& outItem) {
    static constexpr f32 PICKUP_RADIUS = 2.0f;

    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = pool.items[i];
        if (!wi.active) continue;

        Vec3 delta = {
            playerPos.x - wi.position.x,
            playerPos.y - wi.position.y,
            playerPos.z - wi.position.z
        };
        f32 dist = sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (dist >= PICKUP_RADIUS) continue;

        // Check ownership: free-for-all, owned by this player, or exclusive timer expired
        bool canPickup = (wi.ownerSlot == 0xFF)
                      || (wi.ownerSlot == playerSlot)
                      || (wi.exclusiveTimer <= 0.0f);
        if (!canPickup) continue;

        outItem   = wi.item;
        wi.active = false;
        if (pool.activeCount > 0)
            pool.activeCount--;
        return true;
    }

    return false;
}
