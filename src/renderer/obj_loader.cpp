#include "renderer/obj_loader.h"
#include "core/log.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

// newlib (Switch) doesn't have strtok_r — provide a simple implementation
#ifdef __SWITCH__
static char* strtok_r(char* str, const char* delim, char** saveptr) {
    if (!str) str = *saveptr;
    str += std::strspn(str, delim);
    if (*str == '\0') { *saveptr = str; return nullptr; }
    char* end = str + std::strcspn(str, delim);
    if (*end) { *end = '\0'; end++; }
    *saveptr = end;
    return str;
}
#endif

struct ObjVert {
    u32 posIdx, normIdx, uvIdx;
};

static bool parseface(const char* token, u32& posIdx, u32& uvIdx, u32& normIdx) {
    // Formats: v, v/vt, v/vt/vn, v//vn
    posIdx = uvIdx = normIdx = 0;
    int n = 0;
    if (std::strchr(token, '/')) {
        if (std::strstr(token, "//")) {
            n = std::sscanf(token, "%u//%u", &posIdx, &normIdx);
            uvIdx = 0;
        } else {
            n = std::sscanf(token, "%u/%u/%u", &posIdx, &uvIdx, &normIdx);
            if (n < 2) return false;
        }
    } else {
        n = std::sscanf(token, "%u", &posIdx);
    }
    return posIdx > 0;
}

Mesh ObjLoader::load(const char* path, AABB* outBounds) {
    FILE* f = std::fopen(path, "r");
    if (!f) {
        LOG_WARN("ObjLoader: could not open %s", path);
        return {};
    }

    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<Vertex> vertices;
    std::vector<u32> indices;

    // AABB tracking
    Vec3 bmin = { 1e30f,  1e30f,  1e30f};
    Vec3 bmax = {-1e30f, -1e30f, -1e30f};

    // Multi-material tracking: records the start index and name for each usemtl group.
    // materialGroupCount==0 after load means no usemtl was seen (legacy single-material path).
    Mesh resultMesh = {};
    char currentMaterial[32] = {};     // current usemtl name, empty = none seen yet
    u32  currentGroupIndexStart = 0;   // index offset where the current group begins
    bool seenUsemtl = false;

    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        if (line[0] == 'v' && line[1] == ' ') {
            Vec3 p;
            if (std::sscanf(line + 2, "%f %f %f", &p.x, &p.y, &p.z) == 3) {
                positions.push_back(p);
                if (p.x < bmin.x) bmin.x = p.x;
                if (p.y < bmin.y) bmin.y = p.y;
                if (p.z < bmin.z) bmin.z = p.z;
                if (p.x > bmax.x) bmax.x = p.x;
                if (p.y > bmax.y) bmax.y = p.y;
                if (p.z > bmax.z) bmax.z = p.z;
            }
        } else if (line[0] == 'v' && line[1] == 'n') {
            Vec3 n;
            if (std::sscanf(line + 3, "%f %f %f", &n.x, &n.y, &n.z) == 3) {
                normals.push_back(n);
            }
        } else if (line[0] == 'v' && line[1] == 't') {
            Vec2 uv;
            if (std::sscanf(line + 3, "%f %f", &uv.x, &uv.y) >= 2) {
                uvs.push_back(uv);
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            // Parse face — support triangles and quads
            char* ctx = nullptr;
            char* tok = strtok_r(line + 2, " \t\n\r", &ctx);
            u32 faceVerts[4];
            u32 faceCount = 0;

            while (tok && faceCount < 4) {
                u32 pi, ui, ni;
                if (!parseface(tok, pi, ui, ni)) break;

                Vertex v = {};
                if (pi > 0 && pi <= positions.size()) v.position = positions[pi - 1];
                if (ni > 0 && ni <= normals.size())   v.normal   = normals[ni - 1];
                if (ui > 0 && ui <= uvs.size())        v.uv       = uvs[ui - 1];

                faceVerts[faceCount] = static_cast<u32>(vertices.size());
                vertices.push_back(v);
                faceCount++;

                tok = strtok_r(nullptr, " \t\n\r", &ctx);
            }

            if (faceCount >= 3) {
                indices.push_back(faceVerts[0]);
                indices.push_back(faceVerts[1]);
                indices.push_back(faceVerts[2]);
            }
            if (faceCount == 4) {
                indices.push_back(faceVerts[0]);
                indices.push_back(faceVerts[2]);
                indices.push_back(faceVerts[3]);
            }
        } else if (std::strncmp(line, "usemtl ", 7) == 0) {
            // Finalize the in-progress group (if any faces were emitted into it)
            if (seenUsemtl && currentGroupIndexStart < indices.size()
                    && resultMesh.materialGroupCount < MAX_MESH_MATERIALS) {
                MeshMaterialGroup& grp = resultMesh.materials[resultMesh.materialGroupCount++];
                grp.indexStart = currentGroupIndexStart;
                grp.indexCount = static_cast<u32>(indices.size()) - currentGroupIndexStart;
                std::strncpy(grp.materialName, currentMaterial, 31);
                grp.materialName[31] = '\0';
            }
            // Start the new group: trim trailing newline from the material name
            char* nameStart = line + 7;
            u32 nameLen = static_cast<u32>(std::strlen(nameStart));
            while (nameLen > 0 && (nameStart[nameLen - 1] == '\n' || nameStart[nameLen - 1] == '\r'))
                nameLen--;
            std::strncpy(currentMaterial, nameStart, (nameLen < 31) ? nameLen : 31);
            currentMaterial[(nameLen < 31) ? nameLen : 31] = '\0';
            currentGroupIndexStart = static_cast<u32>(indices.size());
            seenUsemtl = true;
        }
        // Ignore mtllib, g, s, o, etc.
    }

    // Finalize the last material group (if the file used usemtl at all)
    if (seenUsemtl && currentGroupIndexStart < indices.size()
            && resultMesh.materialGroupCount < MAX_MESH_MATERIALS) {
        MeshMaterialGroup& grp = resultMesh.materials[resultMesh.materialGroupCount++];
        grp.indexStart = currentGroupIndexStart;
        grp.indexCount = static_cast<u32>(indices.size()) - currentGroupIndexStart;
        std::strncpy(grp.materialName, currentMaterial, 31);
        grp.materialName[31] = '\0';
    }

    std::fclose(f);

    if (vertices.empty() || indices.empty()) {
        LOG_WARN("ObjLoader: no geometry in %s", path);
        return {};
    }

    if (outBounds) {
        outBounds->min = bmin;
        outBounds->max = bmax;
    }

    // Upload geometry; copy material group metadata into the returned mesh.
    Mesh mesh = MeshSystem::create(vertices.data(), static_cast<u32>(vertices.size()),
                                   indices.data(), static_cast<u32>(indices.size()));
    mesh.materialGroupCount = resultMesh.materialGroupCount;
    for (u32 g = 0; g < resultMesh.materialGroupCount; g++)
        mesh.materials[g] = resultMesh.materials[g];

    if (mesh.materialGroupCount > 0) {
        LOG_INFO("ObjLoader: loaded %s (%u verts, %u indices, %u material groups)",
                 path, (u32)vertices.size(), (u32)indices.size(), (u32)mesh.materialGroupCount);
    } else {
        LOG_INFO("ObjLoader: loaded %s (%u verts, %u indices)",
                 path, (u32)vertices.size(), (u32)indices.size());
    }
    return mesh;
}
