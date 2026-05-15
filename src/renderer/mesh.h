#pragma once

#include "core/types.h"
#include "core/math.h"

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

struct Mesh {
    u32 vao = 0;
    u32 vbo = 0;
    u32 ibo = 0;
    u32 indexCount = 0;
};

namespace MeshSystem {
    Mesh create(const Vertex* vertices, u32 vertexCount,
                const u32* indices, u32 indexCount);
    void destroy(Mesh& mesh);
    void draw(const Mesh& mesh);

    Mesh createCube();
    Mesh createQuad();  // flat 1x1 quad in XY plane, centered at origin
}
