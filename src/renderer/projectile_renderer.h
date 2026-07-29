#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/projectile.h"
#include "renderer/mesh.h"
#include "renderer/shader.h"
#include "game/limb_system.h"  // MeshDef

// Instanced projectile renderer — batches all projectiles by mesh type into
// single glDrawElementsInstanced calls. Supports thousands of projectiles with
// only ~8 draw calls (one per unique mesh). Uses per-instance vertex attributes
// for model matrix + color instead of per-draw uniforms.

namespace ProjectileRenderer {
    void init();
    void shutdown();

    // Render all active projectiles in the pool. Groups by meshId, uploads
    // instance data, and issues one instanced draw per mesh type.
    // meshDefs/meshDefCount: the engine's mesh registry for VAO/index lookup.
    // meleeMeshFlags: optional meshDefCount-sized table marking meshes that belong to a MELEE
    // weapon. A thrown weapon is drawn at a readable size and given a heavier spin instead of being
    // normalised to the same 0.4 m as a knife or a molotov. Pass nullptr to keep the old behaviour.
    void render(const ProjectilePool& pool, const Mat4& vp,
                const MeshDef* meshDefs, u32 meshDefCount,
                u8 arrowMeshId, u8 boltMeshId,
                const bool* meleeMeshFlags = nullptr);
}
