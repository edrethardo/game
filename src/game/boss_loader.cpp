// Boss definition loader — parses assets/config/bosses.json into BossDefTable.
// Each boss entry defines stats, roles, personality, skills, projectile, and loot.
// See boss_def.h for struct definitions and the design spec for schema docs.

#include "game/boss_loader.h"
#include "game/entity.h"
#include "game/item.h"  // for SkillId, skillIdFromString
#include "renderer/material.h"
#include "core/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <json/nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
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

static u8 parseRoles(const json& rolesArr) {
    u8 mask = EnemyRole::NORMAL;
    for (auto& r : rolesArr) {
        std::string s = r.get<std::string>();
        if      (s == "ambush")        mask |= EnemyRole::AMBUSH;
        else if (s == "summoner")      mask |= EnemyRole::SUMMONER;
        else if (s == "healer")        mask |= EnemyRole::HEALER;
        else if (s == "aura")          mask |= EnemyRole::AURA;
        else if (s == "ranged_caster") mask |= EnemyRole::RANGED_CASTER;
        else if (s == "charger")       mask |= EnemyRole::CHARGER;
        else if (s == "bomber")        mask |= EnemyRole::BOMBER;
        else if (s == "shield_bearer") mask |= EnemyRole::SHIELD_BEARER;
    }
    return mask;
}

static BossPersonality parsePersonality(const std::string& s) {
    if (s == "berserker")  return BossPersonality::BERSERKER;
    if (s == "kiter")      return BossPersonality::KITER;
    if (s == "teleporter") return BossPersonality::TELEPORTER;
    if (s == "duelist")    return BossPersonality::DUELIST;
    LOG_WARN("BossLoader: unknown personality '%s', defaulting to BERSERKER", s.c_str());
    return BossPersonality::BERSERKER;
}

static u8 parseRarity(const std::string& s) {
    if (s == "magic")     return 1;
    if (s == "rare")      return 2;
    if (s == "legendary") return 3;
    return 0;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
bool BossLoader::load(const char* path, BossDefTable& table) {
    char* buf = readFileToBuffer(path);
    if (!buf) {
        LOG_WARN("BossLoader: cannot open %s", path);
        return false;
    }

    try {
        json doc = json::parse(buf);
        std::free(buf);

        if (!doc.contains("bosses") || !doc["bosses"].is_array()) {
            LOG_WARN("BossLoader: missing 'bosses' array in %s", path);
            return false;
        }

        table.count = 0;
        for (auto& entry : doc["bosses"]) {
            if (table.count >= MAX_BOSS_DEFS) {
                LOG_WARN("BossLoader: too many boss defs (max %u)", MAX_BOSS_DEFS);
                break;
            }

            BossDef& def = table.defs[table.count];
            def = BossDef{};

            def.floor          = static_cast<u8>(entry.value("floor", 0));
            def.isMajor        = entry.value("isMajor", false);

            std::string name   = entry.value("name", "Unknown Boss");
            std::strncpy(def.name, name.c_str(), sizeof(def.name) - 1);

            std::string speech = entry.value("speech", "");
            std::strncpy(def.speech, speech.c_str(), sizeof(def.speech) - 1);

            def.baseHp         = entry.value("baseHp", 500.0f);
            def.baseDmg        = entry.value("baseDmg", 30.0f);
            def.speed          = entry.value("speed", 3.0f);
            def.atkRange       = entry.value("atkRange", 3.5f);
            def.atkCooldown    = entry.value("atkCooldown", 1.0f);
            def.detectionRange = entry.value("detectionRange", 40.0f);

            if (entry.contains("halfExtents") && entry["halfExtents"].is_array()) {
                auto& he = entry["halfExtents"];
                def.halfExtents = {he[0].get<f32>(), he[1].get<f32>(), he[2].get<f32>()};
            }

            std::string mesh = entry.value("meshName", "skeleton");
            std::strncpy(def.meshName, mesh.c_str(), sizeof(def.meshName) - 1);

            std::string mat = entry.value("matName", "");
            std::strncpy(def.matName, mat.c_str(), sizeof(def.matName) - 1);

            std::string wpn = entry.value("weaponName", "");
            std::strncpy(def.weaponName, wpn.c_str(), sizeof(def.weaponName) - 1);

            // Roles bitmask
            if (entry.contains("roles") && entry["roles"].is_array()) {
                def.roles = parseRoles(entry["roles"]);
            }

            // AI personality
            def.personality = parsePersonality(entry.value("personality", "berserker"));

            // Skill (resolved to SkillId enum)
            std::string skillStr = entry.value("skillId", "");
            if (!skillStr.empty()) {
                def.skillId = static_cast<u8>(skillIdFromString(skillStr));
            }

            def.enrageFactor = entry.value("enrageFactor", 0.3f);
            def.minionShield = entry.value("minionShield", false);

            // Melee on-hit effect
            def.onHitEffect   = static_cast<u8>(entry.value("onHitEffect", 0));
            def.onHitDuration = entry.value("onHitDuration", 0.0f);
            def.onHitDps      = entry.value("onHitDps", 0.0f);

            // Projectile sub-object
            if (entry.contains("projectile") && entry["projectile"].is_object()) {
                auto& p = entry["projectile"];
                def.projectile.enabled        = p.value("enabled", false);
                def.projectile.usesWeaponMesh = p.value("usesWeaponMesh", false);
                def.projectile.speed          = p.value("speed", 18.0f);
                def.projectile.radius         = p.value("radius", 0.15f);
                def.projectile.cooldown       = p.value("cooldown", 4.0f);
                def.projectile.onHitEffect    = static_cast<u8>(p.value("onHitEffect", 0));
                def.projectile.onHitDuration  = p.value("onHitDuration", 0.0f);
            }

            // Loot
            def.lootGuarantee = parseRarity(entry.value("lootGuarantee", "rare"));
            def.bonusDrops    = static_cast<u8>(entry.value("bonusDrops", 0));

            def.limbConfig    = static_cast<u8>(entry.value("limbConfig", 0));

            table.count++;
            LOG_INFO("BossLoader: loaded '%s' (floor %u)", def.name, def.floor);
        }

        return true;
    } catch (const json::exception& e) {
        LOG_WARN("BossLoader: JSON parse error in %s: %s", path, e.what());
        std::free(buf);
        return false;
    }
}

// ---------------------------------------------------------------------------
// Resolve visual IDs after mesh/material systems init
// ---------------------------------------------------------------------------
void BossLoader::resolveVisuals(BossDefTable& table) {
    for (u32 i = 0; i < table.count; i++) {
        BossDef& def = table.defs[i];
        def.materialId = MaterialSystem::getIdByName(def.matName);
        // meshId and weaponMeshId are resolved by Engine::init
        // since the mesh registry is Engine-local
    }
}
