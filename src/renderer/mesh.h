#pragma once

#include "core/types.h"
#include "core/math.h"

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

// Describes a contiguous range of indices that share one material.
// Populated by ObjLoader when the OBJ file contains usemtl directives.
struct MeshMaterialGroup {
    u32  indexStart  = 0;
    u32  indexCount  = 0;
    u8   materialId  = 0;             // resolved from materialName after MaterialSystem::init
    char materialName[32] = {};       // raw name from usemtl; kept for deferred resolution
};

static constexpr u32 MAX_MESH_MATERIALS = 4;

struct Mesh {
    u32 vao = 0;
    u32 vbo = 0;
    u32 ibo = 0;
    u32 indexCount = 0;

    // Multi-material support: if materialGroupCount > 0 the mesh uses per-group
    // material overrides; otherwise the single material passed at submission is used.
    MeshMaterialGroup materials[MAX_MESH_MATERIALS] = {};
    u8  materialGroupCount = 0;
};

namespace MeshSystem {
    Mesh create(const Vertex* vertices, u32 vertexCount,
                const u32* indices, u32 indexCount);
    void destroy(Mesh& mesh);
    void draw(const Mesh& mesh);

    Mesh createCube();
    Mesh createQuad();  // flat 1x1 quad in XY plane, centered at origin
}
