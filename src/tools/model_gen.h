#pragma once

#include "core/types.h"
#include "core/math.h"

// Agent-friendly procedural weapon mesh generation.
// Writes OBJ files composed of axis-aligned boxes, matching the format
// consumed by ObjLoader::load. Each box produces 8 vertices, 12 triangles,
// with 6 shared axis-aligned normals and 4 corner UVs.
//
// Usage (from code or CLI):
//   BoxDef boxes[] = { {{-0.03,-0.2,-0.01}, {0.03,0.5,0.01}}, ... };
//   WeaponModelGen::writeOBJ("assets/meshes/greatsword.obj", boxes, 2);

namespace WeaponModelGen {

    // A single axis-aligned box defined by min/max corners.
    struct BoxDef {
        Vec3 min;
        Vec3 max;
    };

    static constexpr u32 MAX_BOXES = 6;

    // A weapon model definition: name + array of boxes.
    struct WeaponDef {
        const char* name;         // e.g. "sword" — used for filename
        const char* comment;      // description line in OBJ header
        BoxDef boxes[MAX_BOXES];
        u32 boxCount;
    };

    // Write an OBJ file from a list of boxes.
    bool writeOBJ(const char* path, const BoxDef* boxes, u32 boxCount,
                  const char* comment = nullptr);

    // Generate a single weapon OBJ to <meshDir>/<name>.obj.
    // Only writes if file doesn't exist (unless force=true).
    bool generateWeapon(const char* meshDir, const WeaponDef& def, bool force = false);

    // Generate all built-in weapon OBJ files.
    // Returns number of files written.
    u32 generateAll(const char* meshDir = "assets/meshes", bool force = false);

    // Lookup a built-in definition by name. Returns nullptr if not found.
    const WeaponDef* getBuiltinDef(const char* name);

    // Get all built-in definitions.
    const WeaponDef* getAllDefs(u32& outCount);

    // ---- Icon bitmap generation ----

    // A 16x16 icon defined as 16 rows of u16 bitmasks (MSB = leftmost pixel).
    struct IconDef {
        const char* name;
        u16 rows[16];
    };

    // Get all built-in icon definitions (weapons + armor slots).
    const IconDef* getAllIcons(u32& outCount);

    // Print a single icon bitmap as ASCII art to stdout (for preview).
    void previewIcon(const IconDef& icon);

    // Write all icons as a C source snippet to the given file path.
    // The output can be copy-pasted into item_icons.cpp.
    bool writeIconSource(const char* path);
}
