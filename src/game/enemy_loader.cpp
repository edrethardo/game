// Enemy definition loader — parses assets/config/enemies.json into EnemyDefTable.
// Maps role strings to EnemyRole bitmask, aiPreference strings to AIState enum,
// and meshName to EnemyType. Visual IDs resolved separately after init.

#include "game/enemy_loader.h"
#include "renderer/material.h"
#include "core/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <json/nlohmann/json.hpp>

using json = nlohmann::json;

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

static u8 parseRole(const std::string& s) {
    if (s == "normal")        return EnemyRole::NORMAL;
    if (s == "ambush")        return EnemyRole::AMBUSH;
    if (s == "summoner")      return EnemyRole::SUMMONER;
    if (s == "healer")        return EnemyRole::HEALER;
    if (s == "aura")          return EnemyRole::AURA;
    if (s == "ranged_caster") return EnemyRole::RANGED_CASTER;
    if (s == "charger")       return EnemyRole::CHARGER;
    if (s == "bomber")        return EnemyRole::BOMBER;
    if (s == "shield_bearer") return EnemyRole::SHIELD_BEARER;
    return EnemyRole::NORMAL;
}

static u8 parseAIPreference(const std::string& s) {
    if (s == "idle")     return static_cast<u8>(AIState::IDLE);
    if (s == "chase")    return static_cast<u8>(AIState::CHASE);
    if (s == "strafe")   return static_cast<u8>(AIState::STRAFE);
    if (s == "flyby")    return static_cast<u8>(AIState::FLYBY);
    if (s == "dormant")  return static_cast<u8>(AIState::DORMANT);
    if (s == "flank")    return static_cast<u8>(AIState::FLANK);
    if (s == "retreat")  return static_cast<u8>(AIState::RETREAT);
    if (s == "surround") return static_cast<u8>(AIState::SURROUND);
    return static_cast<u8>(AIState::IDLE);
}

// Infer EnemyType from mesh name for limb system compatibility
static EnemyType inferEnemyType(const char* meshName) {
    if (std::strcmp(meshName, "bat") == 0)    return EnemyType::BAT;
    if (std::strcmp(meshName, "spider") == 0) return EnemyType::SPIDER;
    if (std::strcmp(meshName, "butcher") == 0) return EnemyType::BOSS;
    // Hellhound has its own quadruped rig with galloping animation
    if (std::strcmp(meshName, "hellhound") == 0) return EnemyType::HELLHOUND;
    // Sentinel has its own armored rig with shield arm
    if (std::strcmp(meshName, "sentinel") == 0) return EnemyType::SENTINEL;
    // Succubus: harpy-style with bat wings + dangling talons
    if (std::strcmp(meshName, "succubus") == 0) return EnemyType::SUCCUBUS;
    // Pit Fiend: own winged-demon rig (wings + bipedal legs)
    if (std::strcmp(meshName, "pit_fiend") == 0)      return EnemyType::PIT_FIEND;
    // Hellforge Smith: hunched blacksmith rig (legs + hammer arm)
    if (std::strcmp(meshName, "hellforge_smith") == 0) return EnemyType::HELLFORGE_SMITH;
    // Butcher-rig enemies use BOSS type for limb config (large skeleton rig)
    if (std::strcmp(meshName, "cave_troll") == 0)     return EnemyType::BOSS;
    if (std::strcmp(meshName, "abyssal_titan") == 0)  return EnemyType::BOSS;
    // Default: skeleton rig (humanoid, wraith, sentinel, succubus, entropy_weaver, etc.)
    return EnemyType::SKELETON;
}

bool EnemyLoader::load(const char* path, EnemyDefTable& table) {
    char* buf = readFileToBuffer(path);
    if (!buf) {
        LOG_WARN("EnemyLoader: cannot open %s", path);
        return false;
    }

    try {
        json doc = json::parse(buf);
        std::free(buf);

        if (!doc.contains("enemies") || !doc["enemies"].is_array()) {
            LOG_WARN("EnemyLoader: missing 'enemies' array in %s", path);
            return false;
        }

        table.count = 0;
        for (auto& entry : doc["enemies"]) {
            if (table.count >= MAX_ENEMY_DEFS) {
                LOG_WARN("EnemyLoader: too many enemy defs (max %u)", MAX_ENEMY_DEFS);
                break;
            }

            EnemyDef& def = table.defs[table.count];
            def = EnemyDef{};

            std::string name = entry.value("name", "Unknown");
            std::strncpy(def.name, name.c_str(), sizeof(def.name) - 1);

            def.tier = static_cast<u8>(entry.value("tier", 1));

            std::string mesh = entry.value("meshName", "skeleton");
            std::strncpy(def.meshName, mesh.c_str(), sizeof(def.meshName) - 1);

            std::string mat = entry.value("materialName", "skeleton_skin");
            std::strncpy(def.matName, mat.c_str(), sizeof(def.matName) - 1);

            def.health         = entry.value("health", 50.0f);
            def.moveSpeed      = entry.value("moveSpeed", 3.0f);
            def.detectionRange = entry.value("detectionRange", 15.0f);
            def.attackRange    = entry.value("attackRange", 2.5f);
            def.attackCooldown = entry.value("attackCooldown", 1.0f);
            def.damage         = entry.value("damage", 10.0f);
            def.flying         = entry.value("flying", false);

            if (entry.contains("halfExtents") && entry["halfExtents"].is_array()) {
                auto& he = entry["halfExtents"];
                def.halfExtents = {he[0].get<f32>(), he[1].get<f32>(), he[2].get<f32>()};
            }

            // Role: supports single string or array of strings for combined roles
            if (entry.contains("role") && entry["role"].is_array()) {
                u8 mask = EnemyRole::NORMAL;
                for (auto& r : entry["role"]) mask |= parseRole(r.get<std::string>());
                def.role = mask;
            } else {
                def.role = parseRole(entry.value("role", "normal"));
            }
            def.aiPreference  = parseAIPreference(entry.value("aiPreference", "chase"));
            def.onHitEffect   = static_cast<u8>(entry.value("onHitEffect", 0));
            def.onHitDuration = entry.value("onHitDuration", 0.0f);
            def.onHitDps      = entry.value("onHitDps", 0.0f);
            def.dropWeight    = entry.value("dropWeight", 1.0f);

            def.enemyType = inferEnemyType(def.meshName);

            table.count++;
        }

        LOG_INFO("EnemyLoader: loaded %u enemy defs from %s", table.count, path);
        return true;
    } catch (const json::exception& e) {
        LOG_WARN("EnemyLoader: JSON parse error in %s: %s", path, e.what());
        std::free(buf);
        return false;
    }
}

void EnemyLoader::resolveVisuals(EnemyDefTable& table) {
    for (u32 i = 0; i < table.count; i++) {
        EnemyDef& def = table.defs[i];
        def.materialId = MaterialSystem::getIdByName(def.matName);
        // meshId resolved by Engine (mesh registry is Engine-local)
    }
}
