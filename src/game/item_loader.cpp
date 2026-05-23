// item_loader.cpp — JSON loaders for ItemDef, AffixDef, SkillDef, and visual resolution.
#include "game/item.h"
#include "core/log.h"
#include "renderer/material.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <json/nlohmann/json.hpp>

using json = nlohmann::json;

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

static WeaponSubtype weaponSubtypeFromString(const std::string& s) {
    if (s == "sword"          || s == "SWORD")          return WeaponSubtype::SWORD;
    if (s == "dagger"         || s == "DAGGER")         return WeaponSubtype::DAGGER;
    if (s == "axe"            || s == "AXE")            return WeaponSubtype::AXE;
    if (s == "claymore"       || s == "CLAYMORE")       return WeaponSubtype::CLAYMORE;
    if (s == "cleaver"        || s == "CLEAVER")        return WeaponSubtype::CLEAVER;
    if (s == "pistol"         || s == "PISTOL")         return WeaponSubtype::PISTOL;
    if (s == "smg"            || s == "SMG")            return WeaponSubtype::SMG;
    if (s == "carbine"        || s == "CARBINE")        return WeaponSubtype::CARBINE;
    if (s == "revolver"       || s == "REVOLVER")       return WeaponSubtype::REVOLVER;
    if (s == "bow"            || s == "BOW")            return WeaponSubtype::BOW;
    if (s == "crossbow"       || s == "CROSSBOW")       return WeaponSubtype::CROSSBOW;
    if (s == "throwing_knife" || s == "THROWING_KNIFE") return WeaponSubtype::THROWING_KNIFE;
    if (s == "molotov"        || s == "MOLOTOV")        return WeaponSubtype::MOLOTOV;
    if (s == "wand"           || s == "WAND")           return WeaponSubtype::WAND;
    return WeaponSubtype::NONE;
}

SkillId skillIdFromString(const std::string& s) {
    // Legacy legendary skills
    if (s == "frozen_orb")      return SkillId::FROZEN_ORB;
    if (s == "chain_lightning")  return SkillId::CHAIN_LIGHTNING;
    if (s == "meteor_strike")   return SkillId::METEOR_STRIKE;
    if (s == "blood_nova")      return SkillId::BLOOD_NOVA;
    if (s == "phase_dash")      return SkillId::PHASE_DASH;
    // Warrior
    if (s == "cleave")          return SkillId::CLEAVE;
    if (s == "war_cry")         return SkillId::WAR_CRY;
    if (s == "thunderclap")     return SkillId::THUNDERCLAP;
    if (s == "whirlwind")       return SkillId::WHIRLWIND;
    if (s == "earthquake")      return SkillId::EARTHQUAKE;
    // Ranger
    if (s == "multi_shot")      return SkillId::MULTI_SHOT;
    if (s == "rain_of_arrows")  return SkillId::RAIN_OF_ARROWS;
    if (s == "poison_arrow")    return SkillId::POISON_ARROW;
    if (s == "shadow_shot")     return SkillId::SHADOW_SHOT;
    if (s == "volley")          return SkillId::VOLLEY;
    if (s == "piercing_shot")   return SkillId::PIERCING_SHOT;
    if (s == "barrage")         return SkillId::BARRAGE;
    if (s == "mark_prey")       return SkillId::MARK_PREY;
    // Sorcerer
    if (s == "fireball")        return SkillId::FIREBALL;
    // Rogue
    if (s == "knife_burst")     return SkillId::KNIFE_BURST;
    if (s == "poison_cloud")    return SkillId::POISON_CLOUD;
    if (s == "shadow_strike")   return SkillId::SHADOW_STRIKE;
    if (s == "fan_of_knives")   return SkillId::FAN_OF_KNIVES;
    if (s == "shadow_step")     return SkillId::SHADOW_STEP;
    if (s == "shadow_dance")    return SkillId::SHADOW_DANCE;
    // Paladin
    if (s == "holy_smite")        return SkillId::HOLY_SMITE;
    if (s == "consecration")      return SkillId::CONSECRATION;
    if (s == "divine_shield")     return SkillId::DIVINE_SHIELD;
    if (s == "holy_bombardment")  return SkillId::HOLY_BOMBARDMENT;
    if (s == "holy_nova")         return SkillId::HOLY_NOVA;
    if (s == "divine_judgment")   return SkillId::DIVINE_JUDGMENT;
    // Combat Engineer
    if (s == "shock_bolt")      return SkillId::SHOCK_BOLT;
    if (s == "deploy_turret")   return SkillId::DEPLOY_TURRET;
    if (s == "tesla_coil")      return SkillId::TESLA_COIL;
    if (s == "mech_overdrive")  return SkillId::MECH_OVERDRIVE;
    // Marksman
    if (s == "aimed_shot")           return SkillId::AIMED_SHOT;
    if (s == "explosive_round")      return SkillId::EXPLOSIVE_ROUND;
    if (s == "rapid_fire")           return SkillId::RAPID_FIRE;
    if (s == "overcharged_magazine") return SkillId::OVERCHARGED_MAGAZINE;
    if (s == "headshot")             return SkillId::HEADSHOT;
    // Tinkerer
    if (s == "combat_drone")    return SkillId::COMBAT_DRONE;
    if (s == "swarm_deploy")    return SkillId::SWARM_DEPLOY;
    if (s == "overclock")       return SkillId::OVERCLOCK;
    if (s == "detonate_swarm")  return SkillId::DETONATE_SWARM;
    if (s == "swarm_queen")     return SkillId::SWARM_QUEEN;
    if (s == "swarm_drones")    return SkillId::SWARM_DRONES;
    if (s == "stun_grenade")    return SkillId::STUN_GRENADE;
    if (s == "throwaway")       return SkillId::THROWAWAY;
    if (s == "void_zone")       return SkillId::VOID_ZONE;
    if (s == "shadow_ricochet") return SkillId::SHADOW_RICOCHET;
    if (s == "life_steal")      return SkillId::LIFE_STEAL;
    if (s == "thorns")          return SkillId::THORNS;
    if (s == "berserker")       return SkillId::BERSERKER;
    if (s == "second_wind")     return SkillId::SECOND_WIND;
    if (s == "soul_harvest")    return SkillId::SOUL_HARVEST;
    if (s == "gravity_pull")    return SkillId::GRAVITY_PULL;
    if (s == "phase_strike")    return SkillId::PHASE_STRIKE;
    if (s == "void_kill")       return SkillId::VOID_KILL;
    if (s == "arc_fire")        return SkillId::ARC_FIRE;
    // Wanderer
    if (s == "deflect")           return SkillId::DEFLECT;
    if (s == "exploit_weakness")  return SkillId::EXPLOIT_WEAKNESS;
    if (s == "adrenaline_surge")  return SkillId::ADRENALINE_SURGE;
    if (s == "deaths_dance")      return SkillId::DEATHS_DANCE;
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
    if (s == "damage_to_flying"   || s == "DAMAGE_TO_FLYING")   return AffixType::DAMAGE_TO_FLYING;
    if (s == "clip_size_pct"      || s == "CLIP_SIZE_PCT")      return AffixType::CLIP_SIZE_PCT;
    if (s == "reload_speed_pct"   || s == "RELOAD_SPEED_PCT")   return AffixType::RELOAD_SPEED_PCT;
    if (s == "energy_flat"        || s == "ENERGY_FLAT")        return AffixType::ENERGY_FLAT;
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

    (void)std::fread(buf, 1, size, f);
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
            def = ItemDef{};

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
            def.baseClipSize         = static_cast<u8>(entry.value("baseClipSize", 0));
            def.baseReloadTime       = entry.value("baseReloadTime",       0.0f);
            def.baseHealth           = entry.value("baseHealth",           0.0f);

            std::string legendaryStr = entry.value("legendarySkill", "NONE");
            def.legendarySkillId = skillIdFromString(legendaryStr);

            def.minLevel = static_cast<u8>(entry.value("minLevel", 1));
            def.maxLevel = static_cast<u8>(entry.value("maxLevel", 10));

            std::string subtypeStr = entry.value("weaponSubtype", "NONE");
            def.weaponSubtype = weaponSubtypeFromString(subtypeStr);

            def.dropWeight = entry.value("dropWeight", 1.0f);

            // Store mesh/material names for deferred resolution
            std::string meshStr = entry.value("mesh", "");
            std::strncpy(def.meshName, meshStr.c_str(), sizeof(def.meshName) - 1);

            std::string matStr = entry.value("material", "");
            std::strncpy(def.materialName, matStr.c_str(), sizeof(def.materialName) - 1);

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
            def = AffixDef{};

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
            def = SkillDef{};

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

            // Holy Nova specifics
            def.secondaryDamage = entry.value("secondaryDamage", 0.0f);
            def.allyHealPct     = entry.value("allyHealPct",     0.0f);

            // Wanderer: Deflect
            if (entry.contains("activeWindow"))    def.activeWindow    = entry.value("activeWindow",    0.0f);
            if (entry.contains("stunDuration"))    def.stunDuration    = entry.value("stunDuration",    0.0f);

            // Wanderer: Exploit Weakness
            if (entry.contains("markDuration"))    def.markDuration    = entry.value("markDuration",    0.0f);
            if (entry.contains("damageMultiplier"))def.damageMultiplier= entry.value("damageMultiplier",0.0f);

            // Wanderer: Death's Dance
            if (entry.contains("slashRadius"))     def.slashRadius     = entry.value("slashRadius",     0.0f);
            if (entry.contains("slashDamageMult")) def.slashDamageMult = entry.value("slashDamageMult", 0.0f);

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
//  ItemLoader::resolveVisuals
// ============================================================

// Forward declaration — Engine provides mesh name lookup
// Called from Engine::init() after meshes and materials are loaded
void ItemLoader::resolveVisuals(ItemDef* defs, u32 count) {
    for (u32 i = 0; i < count; i++) {
        ItemDef& def = defs[i];

        // Resolve material name to ID
        if (def.materialName[0] != '\0') {
            s32 matId = MaterialSystem::getIdByName(def.materialName);
            if (matId >= 0) {
                def.materialId = static_cast<u8>(matId);
            } else {
                LOG_WARN("ItemLoader: material '%s' not found for item '%s'", def.materialName, def.name);
            }
        }

        // meshId resolution requires Engine's mesh registry — handled by Engine::init()
        // after calling this function, engine resolves meshName -> meshId separately
    }
    LOG_INFO("ItemLoader: resolved visuals for %u item defs", count);
}
