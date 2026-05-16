#include "renderer/material.h"
#include "core/log.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <json/nlohmann/json.hpp>

using json = nlohmann::json;

static Material s_materials[MAX_MATERIALS];
static u32 s_materialCount = 0;

static void createFallback() {
    // Material 0 is always a white fallback
    Material& m = s_materials[0];
    m.texture = TextureSystem::createWhite();
    m.tint = {1, 1, 1, 1};
    std::strncpy(m.name, "default", sizeof(m.name) - 1);
    s_materialCount = 1;
}

void MaterialSystem::init(const char* jsonPath) {
    createFallback();

    FILE* f = std::fopen(jsonPath, "r");
    if (!f) {
        LOG_WARN("MaterialSystem: no %s — using fallback white material", jsonPath);
        return;
    }

    // Read entire file
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    char* buf = static_cast<char*>(std::malloc(size + 1));
    (void)std::fread(buf, 1, size, f);
    buf[size] = '\0';
    std::fclose(f);

    try {
        json doc = json::parse(buf);
        std::free(buf);

        if (!doc.contains("materials") || !doc["materials"].is_array()) {
            LOG_WARN("MaterialSystem: invalid format in %s", jsonPath);
            return;
        }

        for (auto& entry : doc["materials"]) {
            u8 id = entry.value("id", (u8)s_materialCount);
            if (id >= MAX_MATERIALS) continue;

            Material& m = s_materials[id];

            // Name
            std::string name = entry.value("name", "unnamed");
            std::strncpy(m.name, name.c_str(), sizeof(m.name) - 1);

            // Texture
            if (entry.contains("texture")) {
                std::string texFile = entry["texture"];
                char texPath[256];
                std::snprintf(texPath, sizeof(texPath), ASSET_PATH("assets/textures/%s"), texFile.c_str());
                m.texture = TextureSystem::load(texPath);
            } else {
                m.texture = TextureSystem::createWhite();
            }

            // Tint
            if (entry.contains("tint") && entry["tint"].is_array() && entry["tint"].size() >= 3) {
                m.tint.x = entry["tint"][0].get<f32>();
                m.tint.y = entry["tint"][1].get<f32>();
                m.tint.z = entry["tint"][2].get<f32>();
                m.tint.w = (entry["tint"].size() >= 4) ? entry["tint"][3].get<f32>() : 1.0f;
            } else {
                m.tint = {1, 1, 1, 1};
            }

            if (id >= s_materialCount) s_materialCount = id + 1;
        }

        LOG_INFO("MaterialSystem: loaded %u materials from %s", s_materialCount, jsonPath);
    } catch (const json::exception& e) {
        std::free(buf);
        LOG_WARN("MaterialSystem: JSON parse error in %s: %s", jsonPath, e.what());
    }
}

void MaterialSystem::shutdown() {
    for (u32 i = 0; i < s_materialCount; i++) {
        TextureSystem::destroy(s_materials[i].texture);
        s_materials[i] = Material{};
    }
    s_materialCount = 0;
}

const Material* MaterialSystem::get(u8 materialId) {
    if (materialId >= s_materialCount) return &s_materials[0]; // fallback
    return &s_materials[materialId];
}

u8 MaterialSystem::getIdByName(const char* name) {
    for (u32 i = 0; i < s_materialCount; i++) {
        if (std::strcmp(s_materials[i].name, name) == 0) return static_cast<u8>(i);
    }
    return 0;
}

u32 MaterialSystem::count() {
    return s_materialCount;
}
