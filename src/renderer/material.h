#pragma once

#include "core/types.h"
#include "core/math.h"
#include "renderer/texture.h"

static constexpr u32 MAX_MATERIALS = 64;

struct Material {
    Texture texture;
    Vec4    tint = {1.0f, 1.0f, 1.0f, 1.0f};
    char    name[32] = {};
};

namespace MaterialSystem {
    // Load materials from JSON. Creates white fallback for material 0 if file missing.
    void init(const char* jsonPath);
    void shutdown();

    const Material* get(u8 materialId);
    u8   getIdByName(const char* name);
    u32  count();
}
