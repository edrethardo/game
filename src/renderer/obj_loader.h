#pragma once

#include "core/math.h"
#include "renderer/mesh.h"
#include "renderer/frustum.h"

// Per-region local-space bounding boxes of an upright humanoid body mesh, used to AUTO-FIT
// equipped armor (helmet/chest/boots/gloves) onto bodies of differing proportions without
// hand-tuned offsets. Computed from the parsed vertices at load (the only place CPU verts
// exist — the GPU Mesh keeps no vertex data). Regions are derived from proportional height
// bands; hands are the lateral arm clusters split by X sign. `valid` is false unless asked for.
struct BodyRegions {
    AABB head{};
    AABB torso{};   // chest band
    AABB feet{};
    AABB handL{};
    AABB handR{};
    f32  shoulderHalfW = 0.0f; // max |x| in the shoulder band — the body's "frame" half-width
    // Per-part validity: false when a band yielded no/degenerate geometry (e.g. a robed body with
    // no separate hands/feet) so the armor-fit can skip that piece instead of placing it wrong.
    bool headValid = false;
    bool torsoValid = false;
    bool feetValid = false;
    bool handsValid = false;
    bool valid = false;        // overall: body was measured at all
};

namespace ObjLoader {
    // Load an OBJ file and upload to GPU as a Mesh.
    // Supports v, vn, vt, f lines (triangles and quads).
    // Returns mesh with vao=0 on failure.
    // If outRegions is non-null, fills it with humanoid body-part AABBs (see BodyRegions);
    // only meaningful for upright player/humanoid body meshes.
    Mesh load(const char* path, AABB* outBounds = nullptr, BodyRegions* outRegions = nullptr);
}
