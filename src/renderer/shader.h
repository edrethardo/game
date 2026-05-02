#pragma once

#include "core/types.h"

struct Vec3;
struct Vec4;
struct Mat4;

struct Shader {
    u32 program = 0;

    // Cached uniform locations
    s32 loc_mvp = -1;
    s32 loc_model = -1;
    s32 loc_lightDir = -1;
    s32 loc_lightColor = -1;
    s32 loc_ambientColor = -1;
    s32 loc_texture0 = -1;
    s32 loc_color = -1;
    s32 loc_vp = -1;       // for debug/HUD shaders that use u_vp
};

namespace ShaderSystem {
    Shader load(const char* vertPath, const char* fragPath);
    void destroy(Shader& shader);
    void bind(const Shader& shader);

    void setMat4(s32 location, const Mat4& m);
    void setVec3(s32 location, const Vec3& v);
    void setVec4(s32 location, const Vec4& v);
    void setFloat(s32 location, f32 v);
    void setInt(s32 location, s32 v);
}
