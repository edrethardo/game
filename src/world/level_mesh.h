#pragma once

#include "core/types.h"
#include "core/math.h"
#include "renderer/mesh.h"
#include "renderer/frustum.h"
#include "renderer/shader.h"
#include "renderer/texture.h"
#include "world/level_grid.h"

static constexpr u32 SECTION_SIZE = 16; // cells per section in X and Z
// Upper bound on sections the occlusion cull keeps hysteresis state for. Sized above the engine's
// MAX_LEVEL_SECTIONS rather than including engine.h — the mesher must not depend on the engine.
// A section past this bound simply skips the cull and is always drawn, which is the safe direction.
static constexpr u32 MAX_TRACKED_SECTIONS = 128;
// One submesh per distinct material in a section. Bumped 8→12 to leave room for the extra
// prop materials (iron/bone/wood) that scatter decorations bake into floor sections on top
// of the floor/wall/ceiling materials already present. See LevelMeshSystem::addPropMesh.
static constexpr u32 MAX_SUBMESHES_PER_SECTION = 12;

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
    // --- Decoration props ---------------------------------------------------
    // Register a small prop mesh (rubble, bones, rock, mushroom, …) to be scattered onto open
    // floor cells and BAKED into the section's material buckets during buildAll — so any number
    // of props costs only +1 submesh per prop-material per section, not one draw call each.
    // The CPU geometry is copied, so the caller's buffers need not outlive the call. Call
    // addPropMesh once per prop at init (after loading the OBJ CPU verts), before buildAll.
    // `radius` is the prop's horizontal half-extent (unused for now; reserved for clearance).
    void addPropMesh(const Vertex* verts, u32 vertCount,
                     const u32* indices, u32 indexCount,
                     u8 materialId, f32 radius);
    // Drop all registered props (e.g. before re-registering). buildAll works with zero props.
    void clearPropMeshes();

    // Build all sections from grid. Allocates sections array (caller must destroyAll).
    // Returns number of sections written into outSections (max = ceil(w/16)*ceil(d/16)).
    u32 buildAll(const LevelGrid& grid, u32 seed, LevelSection* outSections, u32 maxSections);

    // Submit all sections to Renderer (frustum-culled via bounds).
    // Uses MaterialSystem to look up textures per submesh.
    // `grid`/`eye` enable the OCCLUSION cull: a section neither of whose sample points can be seen
    // from the eye is skipped entirely, along with all of its submeshes. Pass nullptr for the
    // frustum-only behaviour (town, arena, anything without a meaningful occluder set).
    void submitAll(const LevelSection* sections, u32 count,
                   const Shader& shader,
                   const LevelGrid* grid = nullptr, Vec3 eye = {0, 0, 0});

    // Destroy all GPU resources in sections array.
    void destroyAll(LevelSection* sections, u32 count);

    // Max sections for a grid (allocate at least this many LevelSection slots).
    inline u32 maxSections(const LevelGrid& grid) {
        u32 sx = (grid.width  + SECTION_SIZE - 1) / SECTION_SIZE;
        u32 sz = (grid.depth  + SECTION_SIZE - 1) / SECTION_SIZE;
        return sx * sz;
    }
}
