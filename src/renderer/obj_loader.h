#pragma once

#include "core/math.h"
#include "renderer/mesh.h"
#include "renderer/frustum.h"

namespace ObjLoader {
    // Load an OBJ file and upload to GPU as a Mesh.
    // Supports v, vn, vt, f lines (triangles and quads).
    // Returns mesh with vao=0 on failure.
    Mesh load(const char* path, AABB* outBounds = nullptr);
}
