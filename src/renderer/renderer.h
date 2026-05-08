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

    u32 getDrawCallCount();
    u32 getVisibleCount();
    u32 getTotalSubmitted();
}
