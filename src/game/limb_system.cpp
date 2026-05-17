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
// Arms are part of the body OBJ mesh (static). Only legs are animated limbs.
// Limb indices: 0=L_leg, 1=R_leg
static const LimbConfig s_skeletonConfig = {
    2,
    {
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

// Spider: legs are static in body OBJ. Mandibles + 8 foot tips animated.
// Foot tip positions match where each leg ends (gx=±7, gy=0, gz=-2..-1..0..1).
// In pivotScale space (÷0.6): foot X = 7*0.12/0.6 = 1.40, Z = sz*0.12/0.6
static const LimbConfig s_spiderConfig = {
    10,
    {
        // Mandibles (0-1)
        {{ 0.10f, 0.08f, -0.85f}, {0.04f, 0.05f, 0.02f}, 0.0f, 1, false},
        {{-0.10f, 0.08f, -0.85f}, {0.04f, 0.05f, 0.02f}, 0.0f, 1, true},
        // Left foot tips (2-5) at each leg end
        {{-1.00f, 0.0f, -0.29f}, {0.02f, 0.03f, 0.02f}, 0.0f, 0, false},
        {{-1.00f, 0.0f, -0.14f}, {0.02f, 0.03f, 0.02f}, 0.0f, 0, false},
        {{-1.00f, 0.0f,  0.00f}, {0.02f, 0.03f, 0.02f}, 0.0f, 0, false},
        {{-1.00f, 0.0f,  0.14f}, {0.02f, 0.03f, 0.02f}, 0.0f, 0, false},
        // Right foot tips (6-9)
        {{ 1.00f, 0.0f, -0.29f}, {0.02f, 0.03f, 0.02f}, 0.0f, 0, true},
        {{ 1.00f, 0.0f, -0.14f}, {0.02f, 0.03f, 0.02f}, 0.0f, 0, true},
        {{ 1.00f, 0.0f,  0.00f}, {0.02f, 0.03f, 0.02f}, 0.0f, 0, true},
        {{ 1.00f, 0.0f,  0.14f}, {0.02f, 0.03f, 0.02f}, 0.0f, 0, true},
    }
};

// Hellhound: 4 legs (quadruped gallop animation). Body mesh has no limbs.
// Leg positions at each corner of the body (front-left, front-right, rear-left, rear-right)
static const LimbConfig s_hellhoundConfig = {
    4,
    {
        // Front legs — forward pivot
        {{ 0.20f, 0.15f, -0.30f}, {0.08f, 0.20f, 0.08f}, 0.0f, 0, false},  // front-left
        {{-0.20f, 0.15f, -0.30f}, {0.08f, 0.20f, 0.08f}, 0.0f, 0, true},   // front-right
        // Rear legs — backward pivot
        {{ 0.20f, 0.15f,  0.30f}, {0.08f, 0.20f, 0.08f}, 0.0f, 0, false},  // rear-left
        {{-0.20f, 0.15f,  0.30f}, {0.08f, 0.20f, 0.08f}, 0.0f, 0, true},   // rear-right
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

void LimbSystem::setObjMeshIds(u8 armId, u8 legId, u8 wingId, u8 butcherArmId, u8 butcherLegId, u8 batFootId, u8 spiderLegPairId) {
    if (armId > 0)           s_armMeshId = armId;
    if (legId > 0)           s_legMeshId = legId;
    if (wingId > 0)          s_wingMeshId = wingId;
    if (butcherArmId > 0)    s_butcherArmMeshId = butcherArmId;
    if (butcherLegId > 0)    s_butcherLegMeshId = butcherLegId;
    if (batFootId > 0)       s_clawMeshId = batFootId;
    if (spiderLegPairId > 0) s_spiderLegMeshId = spiderLegPairId;
    LOG_INFO("LimbSystem: OBJ overrides arm=%u leg=%u wing=%u bArm=%u bLeg=%u claw=%u spider=%u",
             s_armMeshId, s_legMeshId, s_wingMeshId, s_butcherArmMeshId, s_butcherLegMeshId, s_clawMeshId, s_spiderLegMeshId);
}

bool LimbSystem::isObjLimbMesh(u8 meshId) {
    return meshId == s_armMeshId || meshId == s_legMeshId ||
           meshId == s_wingMeshId || meshId == s_butcherArmMeshId ||
           meshId == s_butcherLegMeshId || meshId == s_clawMeshId ||
           meshId == s_spiderLegMeshId;
}

// ============================================================
//  Config lookup
// ============================================================
const LimbConfig& LimbSystem::getConfig(EnemyType type) {
    switch (type) {
        case EnemyType::SKELETON: return s_skeletonConfig;
        case EnemyType::BOSS:    return s_skeletonConfig; // boss uses same limb rig
        case EnemyType::BAT:     return s_batConfig;
        case EnemyType::SPIDER:    return s_spiderConfig;
        case EnemyType::HELLHOUND: return s_hellhoundConfig;
        default:                   return s_genericConfig;
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
            // 0-1 = legs only (arms are in the body OBJ)
            return s_legMeshId;
        case EnemyType::BAT:
            // 0-1 = wings, 2-3 = claws
            return (limbIdx < 2) ? s_wingMeshId : s_clawMeshId;
        case EnemyType::SPIDER:
            // 0-1 = mandibles, 2-9 = foot tips
            return (limbIdx < 2) ? s_mandibleMeshId : s_clawMeshId;
        case EnemyType::HELLHOUND:
            // 0-3 = legs (all use leg mesh)
            return s_legMeshId;
        default:
            return 0;
    }
}

// ============================================================
//  Boss-specific limb configs (extra limbs beyond skeleton base)
// ============================================================

// Boss configs reuse the 4-limb skeleton base, plus extra limbs at index 4+.
// Base limbs for bosses: legs only (arms are in the body OBJ)
#define SKEL_BASE_LIMBS \
    {{ 0.10f, 0.25f, 0.0f}, {0.09f, 0.18f, 0.09f}, 0.0f, 0, false}, \
    {{-0.10f, 0.25f, 0.0f}, {0.09f, 0.18f, 0.09f}, 0.0f, 0, true}

// Andariel: base legs (2) + 4 spider legs above shoulders curving forward toward player.
// restAngle negative = tip curves forward (-Z = toward player).
// Legs rise from behind shoulders then arc forward with fangs pointing at target.
static const LimbConfig s_bossAndarielConfig = {
    6,
    {
        SKEL_BASE_LIMBS,
        // Upper pair — high above shoulders, arcing forward over head
        {{ 0.18f, 0.85f, -0.05f}, {0.06f, 0.38f, 0.06f}, -0.9f, 0, false},
        {{-0.18f, 0.85f, -0.05f}, {0.06f, 0.38f, 0.06f}, -0.9f, 0, true},
        // Lower pair — shoulder height, wider, curving forward
        {{ 0.30f, 0.78f, -0.05f}, {0.06f, 0.34f, 0.06f}, -0.7f, 0, false},
        {{-0.30f, 0.78f, -0.05f}, {0.06f, 0.34f, 0.06f}, -0.7f, 0, true},
    }
};

// Mephisto: base legs (2) + 2 ghostly tentacles above shoulders
static const LimbConfig s_bossMephistoConfig = {
    4,
    {
        SKEL_BASE_LIMBS,
        {{ 0.28f, 0.70f, -0.08f}, {0.05f, 0.30f, 0.05f}, -0.5f, 0, false},
        {{-0.28f, 0.70f, -0.08f}, {0.05f, 0.30f, 0.05f}, -0.5f, 0, true},
    }
};

// Diablo: base legs (2) + 2 back spikes
static const LimbConfig s_bossDiabloConfig = {
    4,
    {
        SKEL_BASE_LIMBS,
        {{ 0.18f, 0.72f, -0.12f}, {0.05f, 0.35f, 0.05f}, -0.3f, 0, false},
        {{-0.18f, 0.72f, -0.12f}, {0.05f, 0.35f, 0.05f}, -0.3f, 0, true},
    }
};

// Grim Reaper: base legs (2) + 2 scythe-blade appendages from back
static const LimbConfig s_bossReaperConfig = {
    4,
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
    // 0-1 = legs (same as skeleton)
    if (limbIdx < 2) return s_legMeshId;
    // Extra limbs (2+): spider legs for Andariel, arm mesh for others
    if (configId == 1) return s_spiderLegMeshId;
    return s_armMeshId; // tentacles, spikes, blades
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
            // 2-limb layout: 0=L_leg, 1=R_leg. Arms are in the body OBJ.
            // Boss extra limbs at 2+.
            f32 walkPhase = e.animTimer * 8.0f;
            bool isRight = (limbIdx == 1);
            f32 phase = isRight ? (walkPhase + 3.14159f) : walkPhase;

            if (limbIdx < 2) {
                // Legs — forward/back swing
                return sinf(phase) * 0.6f * speed01;
            } else {
                // Extra boss limbs (2+): slow menacing sway
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
            if (limbIdx < 2) {
                // Mandibles: gentle idle chew, rapid snap on attack
                f32 angle = sinf(e.animTimer * 3.0f) * 0.12f;
                if (e.attackAnimT > 0.0f) {
                    f32 t = e.attackAnimT / 0.3f;
                    angle = 0.4f * sinf(t * 3.14159f * 3.0f);
                }
                return angle;
            } else {
                // Foot tips: subtle tapping/scratching, each leg slightly offset
                f32 phase = e.animTimer * 6.0f + limbIdx * 0.8f;
                f32 tap = sinf(phase) * 0.2f * (speed01 * 0.8f + 0.2f);
                return tap;
            }
        }

        case EnemyType::HELLHOUND: {
            // Quadruped gallop: diagonal pairs move together (0+3, 1+2)
            // Front-left(0) syncs with rear-right(3), front-right(1) with rear-left(2)
            f32 freq = 8.0f * speed01 + 2.0f; // faster gallop when running
            f32 amplitude = 0.6f * speed01 + 0.1f; // bigger strides when fast
            bool diagonalA = (limbIdx == 0 || limbIdx == 3);
            f32 phase = diagonalA ? 0.0f : 3.14159f; // offset by half cycle
            f32 angle = sinf(e.animTimer * freq + phase) * amplitude;
            // Attack lunge: all legs extend forward briefly
            if (e.attackAnimT > 0.0f) {
                f32 t = e.attackAnimT / 0.3f;
                angle += (limbIdx < 2 ? -0.5f : 0.3f) * sinf(t * 3.14159f);
            }
            return angle;
        }

        default:
            return 0.0f;
    }
}
