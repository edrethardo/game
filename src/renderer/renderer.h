#pragma once

#include "core/types.h"
#include "core/math.h"
#include "renderer/frustum.h"

struct Shader;
struct Texture;
struct Mesh;
struct Camera;

namespace Renderer {
    void init();
    void shutdown();

    void beginFrame(const Camera& camera);
    void submit(const Shader& shader, const Texture& texture,
                const Mesh& mesh, const Mat4& modelMatrix,
                const AABB& bounds,
                Vec4 color = {1,1,1,1});
    void flush();

    void setDirectionalLight(Vec3 direction, Vec3 color, Vec3 ambient);
    void setPointLights(const Vec3* positions, const Vec3* colors, u32 count);
    // Distance fog for the lit shader. Default OFF (start/end huge); call per frame to enable.
    // start/end are forward view-space distances; lit color fades to `color` across [start,end].
    void setFog(Vec3 color, f32 start, f32 end);

    u32 getDrawCallCount();
    u32 getVisibleCount();
    u32 getTotalSubmitted();

    // Light state accessors (for instanced renderers that bypass the submit path)
    Vec3 getLightDir();
    Vec3 getLightColor();
    Vec3 getAmbientColor();
    u32  getPointLights(Vec3* outPos, Vec3* outCol); // returns count (max 4)
}
