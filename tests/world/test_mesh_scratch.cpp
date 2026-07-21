// tests/world/test_mesh_scratch.cpp
// Headroom tripwire for the level mesher's per-material scratch buckets (src/world/level_mesh.cpp).
//
// Phase 3 of the FOUR_STORY work makes the mesher emit a top+underside+rim per SLAB (up to
// MAX_PLATFORMS_PER_CELL of them) instead of one slab per cell, so a fully-slabbed same-material
// section is the densest bucket the builder can produce. This proves that worst case still fits the
// 12288/16384 vertex/index scratch, so the pushQuad overflow path (a one-shot LOG_WARN) is a genuine
// bug backstop — and we do NOT bump the scratch (doubling adds ~7 MB transient during buildAll,
// Switch static-init pressure). level_mesh.cpp needs a GL context (MeshSystem::create) so it is not
// linked here; this is the pure-arithmetic headroom check.

#include "doctest/doctest.h"
#include "core/types.h"
#include "world/level_grid.h"   // MAX_PLATFORMS_PER_CELL (level_grid.cpp already linked into the suite)

namespace {
// Mirror the mesher's private scratch caps + section size. Keep in sync with:
//   src/world/level_mesh.cpp: SCRATCH_VERTS / SCRATCH_INDICES
//   src/world/level_mesh.h:   SECTION_SIZE
constexpr u32 kScratchVerts   = 12288;
constexpr u32 kScratchIndices = 16384;
constexpr u32 kSectionSize    = 16;
}

TEST_CASE("Mesher: worst-case fully-slabbed same-material section fits scratch") {
    // Densest same-material bucket: every cell in the 16x16 section is an open FOUR_STORY interior
    // cell whose floor, all MAX_PLATFORMS_PER_CELL slab tops/undersides and rims share ONE material.
    const u32 cells = kSectionSize * kSectionSize;   // 256

    // Quads such a cell contributes to that single bucket:
    //   1        floor quad
    //   0        ceiling quad  — SKIPPED on a full MAX-slab stack (the CELL_CEILING skip, Task 3.4)
    //   2 * MAX  slab top + underside (one pair per slab, Task 3.3)
    //   0        rim   — interior cell: every neighbour shares the same bands -> both strips empty
    //   0        riser — flat L0, no lower neighbour
    const u32 quadsPerCell = 1u + 2u * MAX_PLATFORMS_PER_CELL;   // = 7 at MAX=3
    CHECK(quadsPerCell == 7u);

    const u32 worstVerts   = cells * quadsPerCell * 4u;   // 4 verts   / quad -> 7168
    const u32 worstIndices = cells * quadsPerCell * 6u;   // 6 indices / quad -> 10752

    CHECK(worstVerts   <= kScratchVerts);     // 7168  <= 12288
    CHECK(worstIndices <= kScratchIndices);   // 10752 <= 16384

    // Even WITHOUT the ceiling skip (8 quads/cell) the section still fits (8192 verts / 12288
    // indices) — the skip is a fill/overdraw win, not a budget necessity.
    const u32 withCeilingVerts   = cells * (quadsPerCell + 1u) * 4u;
    const u32 withCeilingIndices = cells * (quadsPerCell + 1u) * 6u;
    CHECK(withCeilingVerts   <= kScratchVerts);
    CHECK(withCeilingIndices <= kScratchIndices);
}
