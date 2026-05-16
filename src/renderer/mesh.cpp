#include "renderer/mesh.h"
#include "core/log.h"

#include <glad/glad.h>
#include <cstddef>

Mesh MeshSystem::create(const Vertex* vertices, u32 vertexCount,
                         const u32* indices, u32 indexCount) {
    Mesh mesh = Mesh{};
    mesh.indexCount = indexCount;

    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, vertexCount * sizeof(Vertex), vertices, GL_STATIC_DRAW);

    glGenBuffers(1, &mesh.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexCount * sizeof(u32), indices, GL_STATIC_DRAW);

    // Position (location 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, position));
    glEnableVertexAttribArray(0);

    // Normal (location 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(1);

    // UV (location 2)
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
    return mesh;
}

void MeshSystem::destroy(Mesh& mesh) {
    if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
    if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.ibo) glDeleteBuffers(1, &mesh.ibo);
    mesh = Mesh{};
}

void MeshSystem::draw(const Mesh& mesh) {
    glBindVertexArray(mesh.vao);
    glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, 0);
}

Mesh MeshSystem::createCube() {
    // 24 vertices (4 per face, unique normals)
    Vertex vertices[] = {
        // Front face (+Z)
        {{-0.5f, -0.5f,  0.5f}, { 0, 0, 1}, {0, 0}},
        {{ 0.5f, -0.5f,  0.5f}, { 0, 0, 1}, {1, 0}},
        {{ 0.5f,  0.5f,  0.5f}, { 0, 0, 1}, {1, 1}},
        {{-0.5f,  0.5f,  0.5f}, { 0, 0, 1}, {0, 1}},
        // Back face (-Z)
        {{ 0.5f, -0.5f, -0.5f}, { 0, 0,-1}, {0, 0}},
        {{-0.5f, -0.5f, -0.5f}, { 0, 0,-1}, {1, 0}},
        {{-0.5f,  0.5f, -0.5f}, { 0, 0,-1}, {1, 1}},
        {{ 0.5f,  0.5f, -0.5f}, { 0, 0,-1}, {0, 1}},
        // Right face (+X)
        {{ 0.5f, -0.5f,  0.5f}, { 1, 0, 0}, {0, 0}},
        {{ 0.5f, -0.5f, -0.5f}, { 1, 0, 0}, {1, 0}},
        {{ 0.5f,  0.5f, -0.5f}, { 1, 0, 0}, {1, 1}},
        {{ 0.5f,  0.5f,  0.5f}, { 1, 0, 0}, {0, 1}},
        // Left face (-X)
        {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {0, 0}},
        {{-0.5f, -0.5f,  0.5f}, {-1, 0, 0}, {1, 0}},
        {{-0.5f,  0.5f,  0.5f}, {-1, 0, 0}, {1, 1}},
        {{-0.5f,  0.5f, -0.5f}, {-1, 0, 0}, {0, 1}},
        // Top face (+Y)
        {{-0.5f,  0.5f,  0.5f}, { 0, 1, 0}, {0, 0}},
        {{ 0.5f,  0.5f,  0.5f}, { 0, 1, 0}, {1, 0}},
        {{ 0.5f,  0.5f, -0.5f}, { 0, 1, 0}, {1, 1}},
        {{-0.5f,  0.5f, -0.5f}, { 0, 1, 0}, {0, 1}},
        // Bottom face (-Y)
        {{-0.5f, -0.5f, -0.5f}, { 0,-1, 0}, {0, 0}},
        {{ 0.5f, -0.5f, -0.5f}, { 0,-1, 0}, {1, 0}},
        {{ 0.5f, -0.5f,  0.5f}, { 0,-1, 0}, {1, 1}},
        {{-0.5f, -0.5f,  0.5f}, { 0,-1, 0}, {0, 1}},
    };

    u32 indices[] = {
         0, 1, 2,  2, 3, 0,   // front
         4, 5, 6,  6, 7, 4,   // back
         8, 9,10, 10,11, 8,   // right
        12,13,14, 14,15,12,   // left
        16,17,18, 18,19,16,   // top
        20,21,22, 22,23,20,   // bottom
    };

    return create(vertices, 24, indices, 36);
}

Mesh MeshSystem::createQuad() {
    // Flat 1x1 quad in XY plane, facing +Z, centered at origin.
    // Used for billboard sprites (rarity discs, etc.)
    Vertex vertices[] = {
        {{-0.5f, -0.5f, 0.0f}, {0, 0, 1}, {0, 0}},
        {{ 0.5f, -0.5f, 0.0f}, {0, 0, 1}, {1, 0}},
        {{ 0.5f,  0.5f, 0.0f}, {0, 0, 1}, {1, 1}},
        {{-0.5f,  0.5f, 0.0f}, {0, 0, 1}, {0, 1}},
    };
    u32 indices[] = { 0, 1, 2,  2, 3, 0 };
    return create(vertices, 4, indices, 6);
}
