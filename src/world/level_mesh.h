#pragma once

#include "core/types.h"
#include "core/math.h"
#include "renderer/mesh.h"
#include "renderer/frustum.h"
#include "renderer/shader.h"
#include "renderer/texture.h"
#include "world/level_grid.h"

static constexpr u32 SECTION_SIZE = 16; // cells per section in X and Z
static constexpr u32 MAX_SUBMESHES_PER_SECTION = 8;

struct SectionSubmesh {
    Mesh mesh;
    u8   materialId = 0;
};

struct LevelSection {
    SectionSubmesh submeshes[MAX_SUBMESHES_PER_SECTION];
    u32  submeshCount = 0;
    AABB bounds;
    Mat4 model;   // identity — world-space already
    bool dirty = false;
};

namespace LevelMeshSystem {
    // Build all sections from grid. Allocates sections array (caller must destroyAll).
    // Returns number of sections written into outSections (max = ceil(w/16)*ceil(d/16)).
    u32 buildAll(const LevelGrid& grid, LevelSection* outSections, u32 maxSections);

    // Submit all sections to Renderer (frustum-culled via bounds).
    // Uses MaterialSystem to look up textures per submesh.
    void submitAll(const LevelSection* sections, u32 count,
                   const Shader& shader);

    // Destroy all GPU resources in sections array.
    void destroyAll(LevelSection* sections, u32 count);

    // Max sections for a grid (allocate at least this many LevelSection slots).
    inline u32 maxSections(const LevelGrid& grid) {
        u32 sx = (grid.width  + SECTION_SIZE - 1) / SECTION_SIZE;
        u32 sz = (grid.depth  + SECTION_SIZE - 1) / SECTION_SIZE;
        return sx * sz;
    }
}
