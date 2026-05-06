#include "game/limb_system.h"
#include "renderer/mesh.h"
#include "core/log.h"
#include <cmath>
#include <cstring>

// ============================================================
//  Limb mesh indices (assigned during init)
// ============================================================
static u8 s_armMeshId       = 0;
static u8 s_legMeshId       = 0;
static u8 s_spiderLegMeshId = 0;
static u8 s_mandibleMeshId  = 0;
static u8 s_wingMeshId      = 0;
static u8 s_clawMeshId      = 0;
static u8 s_butcherArmMeshId = 0;
static u8 s_butcherLegMeshId = 0;

// ============================================================
//  Box mesh builder (same pattern as engine.cpp hand mesh)
// ============================================================
static Mesh buildBoxMesh(Vec3 halfSize) {
    Vec3 mn = {-halfSize.x, -halfSize.y, -halfSize.z};
    Vec3 mx = { halfSize.x,  halfSize.y,  halfSize.z};

    Vec3 corners[8] = {
        {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z},
        {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
        {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z},
        {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z},
    };
    Vec3 normals[6] = {
        {0,0,-1}, {0,0,1}, {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}
    };
    u32 faceIdx[6][4] = {
        {0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {4,5,1,0}, {3,2,6,7}
    };
    // Per-face UV coordinates — each face maps [0,0] to [1,1]
    Vec2 faceUVs[4] = {{0,0}, {1,0}, {1,1}, {0,1}};

    Vertex verts[24];
    u32 indices[36];
    u32 vc = 0, ic = 0;

    for (u32 f = 0; f < 6; f++) {
        for (u32 v = 0; v < 4; v++) {
            verts[vc++] = {corners[faceIdx[f][v]], normals[f], faceUVs[v]};
        }
        u32 b = f * 4;
        indices[ic++] = b;     indices[ic++] = b + 1; indices[ic++] = b + 2;
        indices[ic++] = b;     indices[ic++] = b + 2; indices[ic++] = b + 3;
    }

    return MeshSystem::create(verts, vc, indices, ic);
}

// ============================================================
//  Limb configurations per enemy type
// ============================================================

// Skeleton: 8 limbs (4 arm segments + 4 leg segments)
// Limb indices: 0=L_arm, 1=R_arm, 2=L_leg, 3=R_leg
// Each is ONE full OBJ mesh (upper + lower + hand/foot).
// Pivot at shoulder/hip. Scaled by pivotScale at render time.
static const LimbConfig s_skeletonConfig = {
    4,
    {
        // Left arm — pivot at shoulder, swings from shoulder joint
        {{ 0.22f, 0.56f, 0.0f}, {0.07f, 0.16f, 0.07f}, 0.0f, 0, false},
        // Right arm
        {{-0.22f, 0.56f, 0.0f}, {0.07f, 0.16f, 0.07f}, 0.0f, 0, true},
        // Left leg — pivot at hip
        {{ 0.10f, 0.25f, 0.0f}, {0.09f, 0.18f, 0.09f}, 0.0f, 0, false},
        // Right leg
        {{-0.10f, 0.25f, 0.0f}, {0.09f, 0.18f, 0.09f}, 0.0f, 0, true},
    }
};

// Bat: 4 limbs (2 wings + 2 claws)
// Limb indices: 0=L_wing, 1=R_wing, 2=L_claw, 3=R_claw
static const LimbConfig s_batConfig = {
    4,
    {
        {{0.15f, 0.18f, 0.0f},  {0.45f, 0.03f, 0.28f}, 0.0f, 2, false},  // left wing — big, prominent
        {{-0.15f, 0.18f, 0.0f},{0.45f, 0.03f, 0.28f}, 0.0f, 2, true},   // right wing
        {{0.08f, -0.15f, 0.0f}, {0.03f, 0.10f, 0.03f}, 0.0f, 0, false},  // left claw
        {{-0.08f, -0.15f, 0.0f},{0.03f, 0.10f, 0.03f}, 0.0f, 0, true},   // right claw
    }
};

// Spider: 10 limbs (8 legs + 2 mandibles)
// Limb indices: 0-7 = legs (radial), 8=L_mandible, 9=R_mandible
static const LimbConfig s_spiderConfig = {
    10,
    {
        // 8 legs arranged around body at 45-degree intervals
        // Front-right, front-left, mid-right, mid-left, etc.
        {{ 0.35f, 0.08f,  0.35f}, {0.03f, 0.22f, 0.03f}, 0.3f, 0, false},  // leg 0 FR
        {{-0.35f, 0.08f,  0.35f}, {0.03f, 0.22f, 0.03f}, 0.3f, 0, true},   // leg 1 FL
        {{ 0.45f, 0.08f,  0.10f}, {0.03f, 0.22f, 0.03f}, 0.3f, 0, false},  // leg 2 MR
        {{-0.45f, 0.08f,  0.10f}, {0.03f, 0.22f, 0.03f}, 0.3f, 0, true},   // leg 3 ML
        {{ 0.45f, 0.08f, -0.10f}, {0.03f, 0.22f, 0.03f}, 0.3f, 0, false},  // leg 4 BR-mid
        {{-0.45f, 0.08f, -0.10f}, {0.03f, 0.22f, 0.03f}, 0.3f, 0, true},   // leg 5 BL-mid
        {{ 0.35f, 0.08f, -0.35f}, {0.03f, 0.22f, 0.03f}, 0.3f, 0, false},  // leg 6 BR
        {{-0.35f, 0.08f, -0.35f}, {0.03f, 0.22f, 0.03f}, 0.3f, 0, true},   // leg 7 BL
        // Mandibles
        {{ 0.06f, 0.05f,  0.45f}, {0.04f, 0.05f, 0.02f}, 0.0f, 1, false},  // left mandible (Y-axis)
        {{-0.06f, 0.05f,  0.45f}, {0.04f, 0.05f, 0.02f}, 0.0f, 1, true},   // right mandible
    }
};

static const LimbConfig s_genericConfig = {0, {}};

// ============================================================
//  Init: build limb meshes
// ============================================================
void LimbSystem::init(MeshDef* meshDefs, u32& meshDefCount) {
    // Helper lambda: build a box mesh and register it in the shared mesh array
    auto registerMesh = [&](const char* name, Vec3 halfSize) -> u8 {
        if (meshDefCount >= 64) return 0;
        Mesh m = buildBoxMesh(halfSize);
        if (m.vao == 0) return 0;
        MeshDef& def = meshDefs[meshDefCount];
        std::strncpy(def.name, name, sizeof(def.name) - 1);
        def.mesh = m;
        def.bounds = {
            {-halfSize.x, -halfSize.y, -halfSize.z},
            { halfSize.x,  halfSize.y,  halfSize.z}
        };
        u8 id = static_cast<u8>(meshDefCount);
        meshDefCount++;
        return id;
    };

    // Note: skeleton/human/butcher OBJ meshes already include arms and legs,
    // so arm/leg limb boxes are only used for boss EXTRA limbs and spider mandibles.
    // They need to be large enough to be visible outside the body.
    // Limb meshes — sized to match body proportions when body OBJ has no arms/legs.
    // These are the SOLE source of appendages now.
    s_armMeshId       = registerMesh("limb_arm",        {0.07f, 0.16f, 0.07f});
    s_legMeshId       = registerMesh("limb_leg",        {0.09f, 0.22f, 0.09f});
    s_spiderLegMeshId = registerMesh("limb_spider_leg", {0.05f, 0.28f, 0.05f});
    s_mandibleMeshId  = registerMesh("limb_mandible",   {0.06f, 0.08f, 0.03f});
    s_wingMeshId      = registerMesh("limb_wing",       {0.45f, 0.03f, 0.28f});
    s_clawMeshId      = registerMesh("limb_claw",       {0.04f, 0.10f, 0.04f});

    LOG_INFO("LimbSystem: registered %u limb meshes (arm=%u leg=%u spider=%u mand=%u wing=%u claw=%u)",
             6u, s_armMeshId, s_legMeshId, s_spiderLegMeshId, s_mandibleMeshId, s_wingMeshId, s_clawMeshId);
}

void LimbSystem::setObjMeshIds(u8 armId, u8 legId, u8 wingId, u8 butcherArmId, u8 butcherLegId, u8 batFootId) {
    if (armId > 0)        s_armMeshId = armId;
    if (legId > 0)        s_legMeshId = legId;
    if (wingId > 0)       s_wingMeshId = wingId;
    if (butcherArmId > 0) s_butcherArmMeshId = butcherArmId;
    if (butcherLegId > 0) s_butcherLegMeshId = butcherLegId;
    if (batFootId > 0)    s_clawMeshId = batFootId;
    LOG_INFO("LimbSystem: OBJ overrides arm=%u leg=%u wing=%u bArm=%u bLeg=%u claw=%u",
             s_armMeshId, s_legMeshId, s_wingMeshId, s_butcherArmMeshId, s_butcherLegMeshId, s_clawMeshId);
}

bool LimbSystem::isObjLimbMesh(u8 meshId) {
    return meshId == s_armMeshId || meshId == s_legMeshId ||
           meshId == s_wingMeshId || meshId == s_butcherArmMeshId ||
           meshId == s_butcherLegMeshId || meshId == s_clawMeshId;
}

// ============================================================
//  Config lookup
// ============================================================
const LimbConfig& LimbSystem::getConfig(EnemyType type) {
    switch (type) {
        case EnemyType::SKELETON: return s_skeletonConfig;
        case EnemyType::BOSS:    return s_skeletonConfig; // boss uses same limb rig
        case EnemyType::BAT:     return s_batConfig;
        case EnemyType::SPIDER:  return s_spiderConfig;
        default:                 return s_genericConfig;
    }
}

// ============================================================
//  Limb mesh ID lookup
// ============================================================
u8 LimbSystem::getLimbMeshId(EnemyType type, u32 limbIdx) {
    switch (type) {
        case EnemyType::SKELETON:
        case EnemyType::BOSS:
            // 0-3 = arms, 4-7 = legs
            // 0-1 = arms, 2-3 = legs
            return (limbIdx < 2) ? s_armMeshId : s_legMeshId;
        case EnemyType::BAT:
            // 0-1 = wings, 2-3 = claws
            return (limbIdx < 2) ? s_wingMeshId : s_clawMeshId;
        case EnemyType::SPIDER:
            // 0-7 = legs, 8-9 = mandibles
            return (limbIdx < 8) ? s_spiderLegMeshId : s_mandibleMeshId;
        default:
            return 0;
    }
}

// ============================================================
//  Boss-specific limb configs (extra limbs beyond skeleton base)
// ============================================================

// Boss configs reuse the 4-limb skeleton base, plus extra limbs at index 4+.
#define SKEL_BASE_LIMBS \
    {{ 0.22f, 0.56f, 0.0f}, {0.07f, 0.16f, 0.07f}, 0.0f, 0, false}, \
    {{-0.22f, 0.56f, 0.0f}, {0.07f, 0.16f, 0.07f}, 0.0f, 0, true},  \
    {{ 0.10f, 0.25f, 0.0f}, {0.09f, 0.18f, 0.09f}, 0.0f, 0, false}, \
    {{-0.10f, 0.25f, 0.0f}, {0.09f, 0.18f, 0.09f}, 0.0f, 0, true}

// Andariel: base limbs + 4 spider legs from torso sides
static const LimbConfig s_bossAndarielConfig = {
    8,
    {
        SKEL_BASE_LIMBS,
        // Extra: 4 spider legs extending outward and forward
        {{ 0.35f, 0.50f,  0.15f}, {0.05f, 0.30f, 0.05f}, 0.6f, 0, false},
        {{-0.35f, 0.50f,  0.15f}, {0.05f, 0.30f, 0.05f}, 0.6f, 0, true},
        {{ 0.38f, 0.40f,  0.05f}, {0.05f, 0.28f, 0.05f}, 0.5f, 0, false},
        {{-0.38f, 0.40f,  0.05f}, {0.05f, 0.28f, 0.05f}, 0.5f, 0, true},
    }
};

// Mephisto: base limbs + 2 ghostly tentacles above shoulders
static const LimbConfig s_bossMephistoConfig = {
    6,
    {
        SKEL_BASE_LIMBS,
        {{ 0.28f, 0.70f, -0.08f}, {0.05f, 0.30f, 0.05f}, -0.5f, 0, false},
        {{-0.28f, 0.70f, -0.08f}, {0.05f, 0.30f, 0.05f}, -0.5f, 0, true},
    }
};

// Diablo: base limbs + 2 back spikes
static const LimbConfig s_bossDiabloConfig = {
    6,
    {
        SKEL_BASE_LIMBS,
        {{ 0.18f, 0.72f, -0.12f}, {0.05f, 0.35f, 0.05f}, -0.3f, 0, false},
        {{-0.18f, 0.72f, -0.12f}, {0.05f, 0.35f, 0.05f}, -0.3f, 0, true},
    }
};

// Grim Reaper: base limbs + 2 scythe-blade appendages from back
static const LimbConfig s_bossReaperConfig = {
    6,
    {
        SKEL_BASE_LIMBS,
        {{ 0.30f, 0.65f, -0.10f}, {0.04f, 0.35f, 0.07f}, -0.6f, 0, false},
        {{-0.30f, 0.65f, -0.10f}, {0.04f, 0.35f, 0.07f}, -0.6f, 0, true},
    }
};

#undef SKEL_BASE_LIMBS

const LimbConfig& LimbSystem::getBossConfig(u8 configId) {
    switch (configId) {
        case 1: return s_bossAndarielConfig;
        case 2: return s_bossMephistoConfig;
        case 3: return s_bossDiabloConfig;
        case 4: return s_bossReaperConfig;
        default: return s_skeletonConfig;
    }
}

u8 LimbSystem::getBossLimbMeshId(u8 configId, u32 limbIdx) {
    // 0-1 = arms, 2-3 = legs (same as skeleton)
    if (limbIdx < 2) return s_armMeshId;
    if (limbIdx < 4) return s_legMeshId;
    // Extra limbs (4+): use spider leg mesh for Andariel, arm mesh for others
    if (configId == 1) return s_spiderLegMeshId; // Andariel's spider legs
    return s_armMeshId; // tentacles, spikes, blades use arm mesh
}

// ============================================================
//  Animation: compute per-limb angle
// ============================================================
f32 LimbSystem::computeAngle(const Entity& e, u32 limbIdx, EnemyType type) {
    // Normalize horizontal speed to [0, 1] for animation scaling
    f32 speed01 = length(Vec3{e.velocity.x, 0, e.velocity.z}) / (e.moveSpeed + 0.001f);
    if (speed01 > 1.0f) speed01 = 1.0f;
    bool isMoving = speed01 > 0.05f;

    switch (type) {
        case EnemyType::SKELETON:
        case EnemyType::BOSS: {
            // 4-limb layout: 0=L_arm, 1=R_arm, 2=L_leg, 3=R_leg
            // Extra boss limbs at 4+
            f32 walkPhase = e.animTimer * 8.0f;
            bool isRight = (limbIdx == 1 || limbIdx == 3);
            f32 phase = isRight ? (walkPhase + 3.14159f) : walkPhase;

            if (limbIdx < 2) {
                // Arms — swing opposite to legs
                // Attack override for right arm (index 1)
                if (e.attackAnimT > 0.0f && limbIdx == 1) {
                    f32 t = e.attackAnimT / 0.3f;
                    return -1.2f * sinf(t * 3.14159f);
                }
                f32 armPhase = phase + 3.14159f;
                f32 idleSway = sinf(e.animTimer * 2.0f) * 0.08f;
                return sinf(armPhase) * 0.5f * speed01 + idleSway;
            } else if (limbIdx < 4) {
                // Legs — forward/back swing
                return sinf(phase) * 0.6f * speed01;
            } else {
                // Extra boss limbs (4+): slow menacing sway
                f32 extraPhase = e.animTimer * 1.5f + limbIdx * 1.2f;
                f32 sway = sinf(extraPhase) * 0.25f;
                if (e.attackAnimT > 0.0f) {
                    sway += -0.4f * sinf(e.attackAnimT / 0.3f * 3.14159f);
                }
                return sway;
            }
        }

        case EnemyType::BAT: {
            f32 flapSpeed = isMoving ? 8.0f : 4.0f;

            if (limbIdx < 2) {
                // Wings: asymmetric flap — fast powerful downstroke, slow recovery upstroke
                // like real bat wings
                f32 phase = fmodf(e.animTimer * flapSpeed, 6.2832f);
                f32 angle;
                if (phase < 2.0f) {
                    // Downstroke (fast, powerful) — sweeps down to -0.7 radians
                    angle = -sinf(phase * 1.57f) * 0.7f;
                } else {
                    // Upstroke (slow, graceful recovery)
                    f32 t = (phase - 2.0f) / 4.2832f;
                    angle = -0.7f * (1.0f - t * t); // ease-out curve
                }

                if (e.attackAnimT > 0.0f) {
                    // Attack: wings sweep wide and aggressive
                    f32 t = e.attackAnimT / 0.4f;
                    angle = -0.5f + sinf(t * 3.14159f) * 1.4f;
                }
                return angle;
            } else {
                // Claws: dangle + reach forward when chasing
                f32 angle = sinf(e.animTimer * 2.0f) * 0.15f;
                if (isMoving) angle -= 0.25f;
                if (e.attackAnimT > 0.0f) {
                    f32 t = e.attackAnimT / 0.4f;
                    angle = -0.9f * sinf(t * 3.14159f);
                }
                return angle;
            }
        }

        case EnemyType::SPIDER: {
            if (limbIdx < 8) {
                // Legs: alternating gait with per-leg phase offset (8 legs = π/4 each)
                f32 walkPhase = e.animTimer * 10.0f;
                f32 phaseOff = static_cast<f32>(limbIdx) * (3.14159f / 4.0f);
                // +0.3 offset keeps legs spread outward at rest (added to restAngle in LimbDef)
                return sinf(walkPhase + phaseOff) * 0.5f * speed01 + 0.3f;
            } else {
                // Mandibles: idle chew cycle, rapid snapping on attack
                f32 angle = sinf(e.animTimer * 4.0f) * 0.15f;
                if (e.attackAnimT > 0.0f) {
                    f32 t = e.attackAnimT / 0.3f;
                    angle = 0.5f * sinf(t * 3.14159f * 3.0f); // rapid triple-snap
                }
                return angle;
            }
        }

        default:
            return 0.0f;
    }
}
