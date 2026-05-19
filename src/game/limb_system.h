#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "renderer/mesh.h"

// Limb-based procedural animation system.
// Each enemy type has a static LimbConfig defining attachment points and mesh sizes.
// Limb angles are computed per-frame from entity state (no stored per-limb data).
// Beyond LOD distance, entities render as a single body mesh (no limb overhead).

static constexpr u32 MAX_LIMBS = 12;
static constexpr f32 LIMB_LOD_DIST_SQ = 64.0f; // 8m — reduced for Switch perf

struct LimbDef {
    Vec3 pivotOffset;   // relative to entity feet position
    Vec3 meshHalfSize;  // box half-extents for this limb
    f32  restAngle;     // resting pose angle (radians)
    u8   pivotAxis;     // 0=X (pitch), 1=Y (yaw), 2=Z (roll)
    bool mirrored;      // negate computed angle for paired limbs
};

struct LimbConfig {
    u8      limbCount;
    LimbDef limbs[MAX_LIMBS];
};

// Shared mesh registry entry — matches Engine::MeshDef layout.
// Defined here so LimbSystem::init can operate on the engine's mesh array
// without requiring access to Engine's private inner struct.
struct MeshDef {
    Mesh mesh;
    AABB bounds;
    char name[32];
};

namespace LimbSystem {
    // Build limb box meshes and register them in the mesh registry.
    // Call once during Engine::init() after loading OBJ meshes.
    void init(MeshDef* meshDefs, u32& meshDefCount);

    // Override limb mesh IDs with OBJ-loaded meshes (call after init + OBJ loading)
    void setObjMeshIds(u8 armId, u8 legId, u8 wingId, u8 butcherArmId, u8 butcherLegId, u8 batFootId, u8 spiderLegPairId);
    void setPitFiendWingMeshId(u8 id);

    // Returns true if this mesh ID is an OBJ-loaded limb (not a procedural box)
    bool isObjLimbMesh(u8 meshId);

    // Get the static limb configuration for an enemy type.
    const LimbConfig& getConfig(EnemyType type);

    // Compute the animation angle for a specific limb on an entity.
    f32 computeAngle(const Entity& e, u32 limbIdx, EnemyType type);

    // Get the mesh index (in meshDefs) for a limb based on enemy type and limb index.
    u8 getLimbMeshId(EnemyType type, u32 limbIdx);

    // Boss-specific limb config (extra limbs on top of base skeleton rig).
    // configId: 1=Andariel, 2=Mephisto, 3=Diablo, 4=Reaper. 0=none.
    const LimbConfig& getBossConfig(u8 configId);
    u8 getBossLimbMeshId(u8 configId, u32 limbIdx);
}
