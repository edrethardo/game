// engine_render_entities.cpp — Engine::renderEntities: entity body + limb rendering.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
#include "platform/window.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "renderer/gl_context.h"
#include "renderer/renderer.h"
#include "renderer/debug_draw.h"
#include "renderer/hud.h"
#include "renderer/minimap.h"
#include "renderer/font.h"
#include "renderer/item_icons.h"
#include "renderer/material.h"
#include "renderer/projectile_renderer.h"
#include "renderer/obj_loader.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "world/level_loader.h"
#include "world/collision.h"
#include "world/combat_query.h"
#include "game/player.h"
#include "game/combat.h"
#include "game/enemy_ai.h"
#include "game/squad.h"
#include "game/limb_system.h"
#include "game/projectile.h"
#include "game/item.h"
#include "game/skill.h"
#include "game/inventory_ui.h"
#include "game/game_constants.h"
#include "net/net.h"
#include "net/server.h"
#include "net/client.h"
#include "net/snapshot.h"
#include "net/packet.h"
#include "core/log.h"
#include "core/math.h"
#include "core/frame_allocator.h"
#include "core/allocation_tracker.h"
#include "core/profiler.h"

#include <glad/glad.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// Shared statics defined in engine.cpp
// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;


// ---------------------------------------------------------------------------
// renderEntities — entity body + limb rendering loop with procedural animation.
// Submits to the shared Renderer batch; caller flushes (Renderer::flush) after
// all world geometry and effects have been submitted.
// ---------------------------------------------------------------------------
void Engine::renderEntities(u32 sw, u32 sh) {
    (void)sw; (void)sh; // sw/sh reserved for future use

    const EntityPool& entPool = (m_netRole == NetRole::CLIENT) ? m_renderInterp.entities : m_entities;
    const Texture& defaultTex = MaterialSystem::get(0)->texture;

    // Cache web material IDs once (avoid per-entity string lookups)
    static const u8 s_webMatIds[] = {
        MaterialSystem::getIdByName("prop_web"),
        MaterialSystem::getIdByName("prop_web_b"),
        MaterialSystem::getIdByName("prop_web_c"),
        MaterialSystem::getIdByName("prop_web_d"),
    };

    for (u32 a = 0; a < entPool.activeCount; a++) {
        u32 i = entPool.activeList[a];
        const Entity& e = entPool.entities[i];

        f32 scaleY = 1.0f;
        if (e.flags & ENT_DEAD) {
            scaleY = (e.deathTimer > 0.0f) ? e.deathTimer : 0.01f;
        }

        Vec3 renderHalf = e.halfExtents;
        renderHalf.y *= scaleY;
        Vec3 renderPos = e.position;
        if (e.flags & ENT_DEAD) {
            renderPos.y -= e.halfExtents.y * (1.0f - scaleY);
        }

        // Use mesh from registry if available
        u8 meshId = e.meshId;
        const Mesh& entMesh = (meshId < m_meshDefCount) ? m_meshDefs[meshId].mesh : m_cubeMesh;

        // --- Procedural animation ---
        f32 animBobY = 0.0f;
        f32 animLean = 0.0f;   // forward tilt (pitch) in radians
        f32 animScaleX = 1.0f; // wing flap for bats
        bool isMoving = (lengthSq(e.velocity) > 0.1f);
        bool isBat = (e.flags & ENT_FLYING) != 0;

        if (!(e.flags & ENT_DEAD)) {
            if (isBat) {
                // No body bob — bat body stays steady, only wings move
                // Lean into dive during flyby
                if (e.aiState == AIState::FLYBY) {
                    animLean = -0.5f;
                }
                // Attack: body lunges forward
                if (e.attackAnimT > 0.0f) {
                    f32 t = e.attackAnimT / 0.4f;
                    animLean = -0.6f * t;
                    animBobY += 0.12f * t;
                }
            } else if (isMoving) {
                // Ground enemies: walking bob
                animBobY = sinf(e.animTimer * 10.0f) * 0.04f;
            }

            // Attack lunge for non-bat enemies
            if (!isBat && e.attackAnimT > 0.0f) {
                f32 t = e.attackAnimT / 0.3f; // 0→1
                animLean = -0.3f * t; // lean forward
                animBobY += 0.05f * t; // slight hop
            }
        }

        Mat4 model;
        if (meshId > 0 && meshId < m_meshDefCount) {
            const AABB& meshBounds = m_meshDefs[meshId].bounds;
            f32 meshH = meshBounds.max.y - meshBounds.min.y;
            f32 targetH = e.halfExtents.y * 2.0f * scaleY;
            f32 uniformScale = (meshH > 0.001f) ? (targetH / meshH) : 1.0f;
            Vec3 basePos = renderPos - Vec3{0, e.halfExtents.y * scaleY, 0}
                         + Vec3{0, animBobY, 0};
            model = Mat4::translate(basePos)
                  * Mat4::rotateY(e.yaw)
                  * Mat4::rotateX(animLean)
                  * Mat4::scale({uniformScale * animScaleX, uniformScale, uniformScale});
        } else {
            model = Mat4::translate(renderPos + Vec3{0, animBobY, 0})
                  * Mat4::rotateY(e.yaw)
                  * Mat4::rotateX(animLean)
                  * Mat4::scale(renderHalf * 2.0f);
        }
        AABB bounds = {renderPos - renderHalf, renderPos + renderHalf};

        // Use entity's material texture if assigned, otherwise fallback
        const Material* entMat = MaterialSystem::get(e.materialId);
        const Texture& entTex = (e.materialId > 0) ? entMat->texture : defaultTex;

        // Resolve tint — friendly NPCs use their material skin tint
        Vec4 tint;
        if (e.flags & ENT_FRIENDLY) {
            tint = (e.materialId > 0) ? entMat->tint : Vec4{0.8f, 0.7f, 0.55f, 1.0f};
        } else if (e.enemyType == EnemyType::MIMIC) {
            // Dormant mimics look like normal chests; active ones turn red
            if (e.aiState == AIState::DORMANT) {
                tint = {0.6f, 0.4f, 0.2f, 1.0f}; // chest brown
            } else {
                tint = {0.9f, 0.3f, 0.2f, 1.0f}; // angry red
            }
        } else if (e.materialId > 0) {
            tint = entMat->tint;
        } else if (e.flags & ENT_FLYING) {
            tint = {0.4f, 0.5f, 1.0f, 1.0f};
        } else {
            tint = {0.8f, 0.5f, 0.3f, 1.0f};
        }
        // Aura-buffed enemies get a subtle red-orange tint shift
        if (e.hasAuraBuff && !(e.flags & ENT_FRIENDLY)) {
            tint.x = fminf(tint.x * 1.3f, 1.0f);
            tint.y *= 0.85f;
            tint.z *= 0.7f;
        }

        // Boss invuln/shield cue — STEADY (non-flashing) cool blue-white shift so the
        // player can read that the boss is currently un-killable. Driven purely by data
        // synced over the wire (bossPhase/minionShield), so host and clients match.
        // ENTOMBING = full invuln channel; SEALED/minionShield = 75% damage reduction.
        bool bossInvuln  = (e.bossPhase == BossPhase::ENTOMBING);
        bool bossShielded = e.minionShield ||
                            e.bossPhase == BossPhase::SEALED;
        if (bossInvuln || bossShielded) {
            f32 amt = bossInvuln ? 0.6f : 0.35f;  // invuln reads stronger than shielded
            tint.x = tint.x * (1.0f - amt) + 0.55f * amt;
            tint.y = tint.y * (1.0f - amt) + 0.75f * amt;
            tint.z = tint.z * (1.0f - amt) + 1.0f  * amt;
        }

        // Skip webs in main pass — rendered in a translucent second pass below
        if (e.materialId == s_webMatIds[0] || e.materialId == s_webMatIds[1] ||
            e.materialId == s_webMatIds[2] || e.materialId == s_webMatIds[3]) continue;

        if (e.flashTimer > 0.0f) {
            f32 flash = e.flashTimer / 0.12f;
            Vec4 flashColor = {1.0f, 0.3f * flash, 0.3f * flash, 1.0f};
            Renderer::submit(m_basicShader, entTex, entMesh, model, bounds, flashColor);
        } else {
            Renderer::submit(m_basicShader, entTex, entMesh, model, bounds, tint);
        }

        // Render articulated limbs (LOD: only when close enough to camera)
        // Cap limb count when many enemies are active to stay within draw call budget
        if (e.enemyType != EnemyType::GENERIC && !(e.flags & ENT_DEAD)) {
            Vec3 toCamera = m_camera.position - e.position;
            f32 limbLodSq = (e.enemyType == EnemyType::BAT) ? 225.0f : LIMB_LOD_DIST_SQ;
            if (lengthSq(toCamera) < limbLodSq) {
                const LimbConfig& limbCfg = (e.bossLimbConfig > 0)
                    ? LimbSystem::getBossConfig(e.bossLimbConfig)
                    : LimbSystem::getConfig(e.enemyType);

                u32 maxLimbs = limbCfg.limbCount;
#ifdef __SWITCH__
                // Switch: spiders get mandibles only (2/10), others capped at 4 when busy
                if (e.enemyType == EnemyType::SPIDER && e.bossDefIdx == 0xFF)
                    maxLimbs = 2; // mandibles only, skip 8 foot tips
                else if (entPool.activeCount > 15 && e.bossDefIdx == 0xFF)
                    maxLimbs = (maxLimbs > 4) ? 4 : maxLimbs;
#else
                if (entPool.activeCount > 15 && e.bossDefIdx == 0xFF) {
                    maxLimbs = (maxLimbs > 4) ? 4 : maxLimbs;
                }
#endif

                for (u32 li = 0; li < maxLimbs; li++) {
                    u8 limbMesh = (e.bossLimbConfig > 0)
                        ? LimbSystem::getBossLimbMeshId(e.bossLimbConfig, li)
                        : LimbSystem::getLimbMeshId(e.enemyType, li);
                    if (limbMesh == 0 || limbMesh >= m_meshDefCount) continue;

                    const LimbDef& ld = limbCfg.limbs[li];
                    f32 angle = LimbSystem::computeAngle(e, li, e.enemyType);
                    // Mirror negates angle for symmetric limbs (bat wings flap together).
                    // Skip for skeleton/boss arms+legs — phase offset handles alternation,
                    // mirroring the angle would make both sides swing in sync.
                    bool skipMirrorAngle = (e.enemyType == EnemyType::SKELETON ||
                                            e.enemyType == EnemyType::BOSS) && li < 2;
                    if (ld.mirrored && !skipMirrorAngle) angle = -angle;
                    angle += ld.restAngle;

                    // Build limb transform:
                    //   1. Entity feet position (world space)
                    //   2. Rotate by entity yaw (so limbs face same direction as body)
                    //   3. Apply local pivot offset (in entity-local space AFTER yaw)
                    //   4. Apply limb rotation (joint articulation)
                    //   5. Scale the limb mesh
                    Vec3 entBase = renderPos - Vec3{0, e.halfExtents.y * scaleY, 0}
                                 + Vec3{0, animBobY, 0};
                    // Pivot offset is in entity-local space, scaled to match body size.
                    // halfExtents.y defines the entity's half-height, so scale pivots
                    // relative to a reference height of 0.5 (standard skeleton halfExtents.y)
                    f32 pivotScale = e.halfExtents.y / 0.5f;
                    Vec3 localPivot = ld.pivotOffset * pivotScale;

                    Mat4 limbRot;
                    switch (ld.pivotAxis) {
                        case 0: limbRot = Mat4::rotateX(angle); break;
                        case 1: limbRot = Mat4::rotateY(angle); break;
                        case 2: limbRot = Mat4::rotateZ(angle); break;
                        default: limbRot = Mat4::identity(); break;
                    }

                    // Determine if this is an OBJ-loaded limb or a procedural box
                    const AABB& limbMeshBounds = m_meshDefs[limbMesh].bounds;
                    bool isObjLimb = LimbSystem::isObjLimbMesh(limbMesh);

                    // For OBJ limbs, the mesh origin needs to be shifted so the
                    // attachment end (top of arm = shoulder, top of leg = hip)
                    // is at the rotation origin. The mesh hangs DOWN from the pivot.
                    Vec3 meshOriginOffset = {0, 0, 0};
                    Vec3 limbScaleVec;
                    // Mirror the mesh on X for right-side limbs (wings, arms)
                    f32 mirrorX = ld.mirrored ? -1.0f : 1.0f;
                    if (isObjLimb) {
                        limbScaleVec = {pivotScale * mirrorX, pivotScale, pivotScale};
                        if (ld.pivotAxis == 0) {
                            // Arms: shift so top is at pivot, then tilt forward.
                            // Mesh center-Y becomes the rotation origin.
                            f32 meshMidY = (limbMeshBounds.max.y + limbMeshBounds.min.y) * 0.5f;
                            bool isArm = (li < 2) && (e.enemyType == EnemyType::SKELETON ||
                                                       e.enemyType == EnemyType::BOSS);
                            // Hellforge Smith hammer arm is limb index 2
                            if (e.enemyType == EnemyType::HELLFORGE_SMITH && li == 2) isArm = true;
                            if (isArm) {
                                // Arms held forward at ~45 degrees — ready to fight
                                meshOriginOffset = {0, -limbMeshBounds.max.y, 0};
                            } else {
                                // Legs hang straight down from hip
                                meshOriginOffset = {0, -limbMeshBounds.max.y, 0};
                            }
                        }
                    } else {
                        limbScaleVec = ld.meshHalfSize * 2.0f * pivotScale;
                        limbScaleVec.x *= mirrorX;
                    }

                    Mat4 limbModel = Mat4::translate(entBase)
                                   * Mat4::rotateY(e.yaw)
                                   * Mat4::translate(localPivot)
                                   * limbRot
                                   * Mat4::scale(limbScaleVec)
                                   * Mat4::translate(meshOriginOffset);

                    // Compute world-space limb position for culling bounds
                    f32 cy = cosf(e.yaw), sy2 = sinf(e.yaw);
                    Vec3 worldPivot = entBase + Vec3{
                        localPivot.x * cy + localPivot.z * sy2,
                        localPivot.y,
                        -localPivot.x * sy2 + localPivot.z * cy
                    };
                    AABB limbBounds = {worldPivot - Vec3{0.5f,0.5f,0.5f},
                                       worldPivot + Vec3{0.5f,0.5f,0.5f}};

                    // Special textures/tints per limb type
                    const Texture& limbTex = (e.enemyType == EnemyType::BAT && li < 2)
                        ? MaterialSystem::get(m_matIdBatWing)->texture
                        : entTex;

                    // Boss extra limbs (beyond base legs) get a dark spider/bone tint
                    Vec4 limbTint = tint;
                    if (e.flashTimer > 0.0f) {
                        limbTint = {1.0f, 0.3f * (e.flashTimer/0.12f), 0.3f * (e.flashTimer/0.12f), 1.0f};
                    } else if (e.bossLimbConfig > 0 && li >= 2) {
                        // Extra boss limbs: dark chitinous color
                        if (e.bossLimbConfig == 1) {
                            // Andariel spider legs: dark brown-black chitin
                            limbTint = {0.15f, 0.1f, 0.05f, 1.0f};
                        } else {
                            // Other bosses: dark grey-blue
                            limbTint = {0.2f, 0.2f, 0.25f, 1.0f};
                        }
                    }

                    Renderer::submit(m_basicShader, limbTex, m_meshDefs[limbMesh].mesh,
                                     limbModel, limbBounds, limbTint);
                }

                // Skeleton/Boss weapon: held in right hand (arms are part of body OBJ)
                if ((e.enemyType == EnemyType::SKELETON || e.enemyType == EnemyType::BOSS) &&
                    e.weaponMeshId > 0 && e.weaponMeshId < m_meshDefCount) {
                    // Weapon swings during attack, idle sway otherwise
                    f32 armAngle = 0.0f;
                    if (e.attackAnimT > 0.0f) {
                        f32 t = e.attackAnimT / 0.3f;
                        armAngle = 0.8f * sinf(t * 3.14159f);
                    } else {
                        armAngle = sinf(e.animTimer * 1.5f) * 0.1f;
                    }

                    Vec3 wEntBase = renderPos - Vec3{0, e.halfExtents.y * scaleY, 0}
                                  + Vec3{0, animBobY, 0};
                    f32 wPivotScale = e.halfExtents.y / 0.5f;

                    // Right hand position (matches where the OBJ arm ends)
                    Vec3 shoulder = {-0.22f * wPivotScale, 0.56f * wPivotScale, 0.0f};
                    f32 armLen = 0.44f * wPivotScale;
                    // Hand position = shoulder + arm rotated by armAngle around X
                    // Arm hangs down by default, swings with angle
                    f32 handY = shoulder.y - armLen * cosf(armAngle);
                    f32 handZ = -armLen * sinf(armAngle);

                    // Scale weapon to fit in hand
                    const AABB& wb = m_meshDefs[e.weaponMeshId].bounds;
                    f32 wH = wb.max.y - wb.min.y;
                    f32 wScale = (wH > 0.001f) ? (0.45f * wPivotScale / wH) : 0.3f;

                    // Weapon position: entity base + rotated hand offset
                    // The weapon's hilt (bottom) should be at the hand
                    // Weapon in entity-local space, then rotated by yaw to world
                    Vec3 localWeaponPos = {shoulder.x, handY, handZ};
                    f32 wcy = cosf(e.yaw), wsy = sinf(e.yaw);
                    Vec3 weaponPos = wEntBase + Vec3{
                        localWeaponPos.x * wcy + localWeaponPos.z * wsy,
                        localWeaponPos.y,
                        -localWeaponPos.x * wsy + localWeaponPos.z * wcy
                    };

                    Mat4 weaponModel = Mat4::translate(wEntBase)
                                     * Mat4::rotateY(e.yaw)
                                     * Mat4::translate(localWeaponPos)
                                     * Mat4::rotateX(armAngle)
                                     * Mat4::scale({wScale, wScale, wScale})
                                     * Mat4::translate({0, wH * 0.5f, 0});

                    AABB wBounds = {weaponPos - Vec3{0.5f,0.5f,0.5f},
                                    weaponPos + Vec3{0.5f,0.5f,0.5f}};

                    Renderer::submit(m_basicShader, entTex, m_meshDefs[e.weaponMeshId].mesh,
                                     weaponModel, wBounds,
                                     Vec4{0.7f, 0.7f, 0.8f, 1.0f});
                }
            }
        }
    }

    // Second pass: render translucent webs with alpha blending (batched, single state change).
    // Detect webs by content, not floor number. The previous gate was
    //   bool hasCavernWebs = (m_level.currentFloor >= 21 && currentFloor <= 30);
    // which assumed webs only spawned on the cavern tier — but spawnFloorDecorations
    // populates webs on every tier's prop list (engine_spawn.cpp:737-753, matWeb is the
    // first entry in dungeon/catacomb/cavern/hellforge/void lists), so on floors outside
    // 21-30 the main pass would `continue` over every web (web-matId skip above) and the
    // second pass never ran — web props became invisible solid obstacles the player could
    // walk into. The content scan preserves the original optimization (skip the GL_BLEND
    // state change when no webs are present) without baking floor numbers into the gate.
    {
        bool hasAnyWebs = false;
        for (u32 _a = 0; _a < entPool.activeCount && !hasAnyWebs; _a++) {
            u32 i = entPool.activeList[_a];
            const Entity& e = entPool.entities[i];
            if (e.materialId == s_webMatIds[0] || e.materialId == s_webMatIds[1] ||
                e.materialId == s_webMatIds[2] || e.materialId == s_webMatIds[3]) {
                hasAnyWebs = true;
            }
        }
        if (hasAnyWebs) {
        Renderer::flush();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (u32 _a = 0; _a < entPool.activeCount; _a++) { u32 i = entPool.activeList[_a];
            const Entity& e = entPool.entities[i];
            if (!(e.flags & ENT_ACTIVE)) continue;
            if (e.materialId != s_webMatIds[0] && e.materialId != s_webMatIds[1] &&
                e.materialId != s_webMatIds[2] && e.materialId != s_webMatIds[3]) continue;
            const Material* mat = MaterialSystem::get(e.materialId);
            const Texture& tex = (mat && mat->texture.handle) ? mat->texture : defaultTex;
            const Mesh& mesh = (e.meshId < m_meshDefCount) ? m_meshDefs[e.meshId].mesh : m_cubeMesh;
            // Ceiling webs (flat Y halfExtent) need 90° X rotation to lay horizontal
            bool isCeilingWeb = (e.halfExtents.y < 0.05f && e.halfExtents.x > 0.1f);
            Mat4 model;
            AABB bounds;
            if (isCeilingWeb) {
                // Scale as XZ slab directly (skip rotation — just swap Y/Z in scale)
                Vec3 ceilScale = {e.halfExtents.x * 2.0f, e.halfExtents.z * 2.0f, e.halfExtents.y * 2.0f};
                model = Mat4::translate(e.position) * Mat4::rotateY(e.yaw)
                      * Mat4::rotateX(1.5708f) * Mat4::scale(ceilScale);
                // Expand AABB so frustum culling doesn't clip the thin slab
                bounds = {e.position - Vec3{e.halfExtents.x, 0.2f, e.halfExtents.z},
                          e.position + Vec3{e.halfExtents.x, 0.2f, e.halfExtents.z}};
            } else {
                model = Mat4::translate(e.position) * Mat4::rotateY(e.yaw)
                      * Mat4::scale(e.halfExtents * 2.0f);
                bounds = entityAABB(e);
            }
            Renderer::submit(m_basicShader, tex, mesh, model, bounds, mat->tint);
        }
        Renderer::flush();
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        } // end cavern floor check
    }

    // Stun indicator — 3 spinning stars orbiting above stunned entity heads
    for (u32 a = 0; a < entPool.activeCount; a++) {
        u32 i = entPool.activeList[a];
        const Entity& e = entPool.entities[i];
        if (e.flags & ENT_DEAD) continue;
        if (e.stunTimer <= 0.0f) continue;

        Vec3 headPos = e.position + Vec3{0, e.halfExtents.y * 2.0f + 0.15f, 0};
        f32 t = e.animTimer * 4.0f; // spin speed
        f32 orbitR = 0.25f;

        for (u32 star = 0; star < 3; star++) {
            f32 angle = t + star * (6.28318f / 3.0f);
            Vec3 starPos = headPos + Vec3{cosf(angle) * orbitR, sinf(t * 2.0f + star) * 0.05f,
                                           sinf(angle) * orbitR};
            // Draw a small 4-pointed star burst
            f32 sz = 0.06f;
            DebugDraw::line(starPos + Vec3{-sz, 0, 0}, starPos + Vec3{sz, 0, 0}, {1.0f, 1.0f, 0.3f});
            DebugDraw::line(starPos + Vec3{0, -sz, 0}, starPos + Vec3{0, sz, 0}, {1.0f, 1.0f, 0.3f});
            DebugDraw::line(starPos + Vec3{0, 0, -sz}, starPos + Vec3{0, 0, sz}, {1.0f, 0.9f, 0.2f});
            // Diagonal crosses for sparkle
            f32 d = sz * 0.7f;
            DebugDraw::line(starPos + Vec3{-d, d, 0}, starPos + Vec3{d, -d, 0}, {1.0f, 0.8f, 0.1f});
            DebugDraw::line(starPos + Vec3{-d, -d, 0}, starPos + Vec3{d, d, 0}, {1.0f, 0.8f, 0.1f});
        }
    }

    // Enemy rim aura — subtle colored lines around the entity's feet so they
    // pop from the background.  Uses DebugDraw (pure color, no texture).
#ifndef __SWITCH__  // skip on Switch for performance
    for (u32 a = 0; a < entPool.activeCount; a++) {
        u32 i = entPool.activeList[a];
        const Entity& e = entPool.entities[i];
        if (e.flags & ENT_DEAD) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;

        f32 distSq = lengthSq(e.position - m_camera.position);
        if (distSq > 225.0f) continue;

        f32 fade = 1.0f - distSq / 225.0f;
        f32 pulse = 0.7f + 0.3f * sinf(e.animTimer * 3.0f + static_cast<f32>(i));

        Vec3 col;
        f32 r;
        if (e.enemyType == EnemyType::BOSS ||
            e.enemyType == EnemyType::PIT_FIEND ||
            e.enemyType == EnemyType::HELLFORGE_SMITH) {
            col = {0.8f * fade * pulse, 0.15f * fade, 0.05f * fade};
            r = e.halfExtents.x * 1.3f;
        } else {
            col = {0.6f * fade * pulse, 0.25f * fade, 0.08f * fade};
            r = e.halfExtents.x * 1.1f;
        }

        // Ground ring around the entity's feet
        Vec3 base = e.position - Vec3{0, e.halfExtents.y - 0.05f, 0};
        static constexpr u32 RING_SEGS = 8;
        for (u32 s = 0; s < RING_SEGS; s++) {
            f32 a0 = static_cast<f32>(s) * (6.2832f / RING_SEGS);
            f32 a1 = a0 + (6.2832f / RING_SEGS);
            Vec3 p0 = base + Vec3{cosf(a0) * r, 0, sinf(a0) * r};
            Vec3 p1 = base + Vec3{cosf(a1) * r, 0, sinf(a1) * r};
            DebugDraw::line(p0, p1, col);
        }
    }
#endif // !__SWITCH__

    for (u32 a = 0; a < entPool.activeCount; a++) {
        u32 i = entPool.activeList[a];
        const Entity& e = entPool.entities[i];
        if (e.flags & ENT_DEAD) continue;
        if (!(e.enemyRole & EnemyRole::AURA)) continue;

        Vec3 base = e.position - Vec3{0, e.halfExtents.y - 0.02f, 0};
        f32 t = e.animTimer * 1.5f;
        f32 pulse = 4.8f + 0.2f * sinf(t * 3.0f);
        Vec3 aurCol = {1.0f, 0.7f, 0.15f};

        // Outer ring — dashed, rotating clockwise
        static constexpr u32 AURA_SEGS = 16;
        for (u32 s = 0; s < AURA_SEGS; s++) {
            if (s % 2 != 0) continue;
            f32 a0 = t + static_cast<f32>(s) * (6.2832f / AURA_SEGS);
            f32 a1 = t + static_cast<f32>(s + 1) * (6.2832f / AURA_SEGS);
            Vec3 p0 = base + Vec3{cosf(a0) * pulse, 0, sinf(a0) * pulse};
            Vec3 p1 = base + Vec3{cosf(a1) * pulse, 0, sinf(a1) * pulse};
            DebugDraw::line(p0, p1, aurCol);
        }

        // Inner ring — solid, rotating counter-clockwise
        f32 innerR = pulse * 0.5f;
        Vec3 innerCol = {0.8f, 0.55f, 0.1f};
        for (u32 s = 0; s < AURA_SEGS; s++) {
            f32 a0 = -t * 0.7f + static_cast<f32>(s) * (6.2832f / AURA_SEGS);
            f32 a1 = -t * 0.7f + static_cast<f32>(s + 1) * (6.2832f / AURA_SEGS);
            Vec3 p0 = base + Vec3{cosf(a0) * innerR, 0, sinf(a0) * innerR};
            Vec3 p1 = base + Vec3{cosf(a1) * innerR, 0, sinf(a1) * innerR};
            DebugDraw::line(p0, p1, innerCol);
        }
    }

    // Enemy light source — starburst glow lines radiating from each entity's
    // center to simulate a small point light.  Pure DebugDraw lines (no texture).
#ifndef __SWITCH__  // skip on Switch for performance
    for (u32 a = 0; a < entPool.activeCount; a++) {
        u32 i = entPool.activeList[a];
        const Entity& e = entPool.entities[i];
        if (e.flags & ENT_DEAD) continue;
        if (e.enemyType == EnemyType::PROP) continue;

        Vec3 lightPos = e.position + Vec3{0, e.halfExtents.y * 0.5f, 0};

        f32 distSq = lengthSq(lightPos - m_camera.position);
        if (distSq > 400.0f) continue;
        f32 fade = 1.0f - distSq / 400.0f;
        f32 pulse = 0.7f + 0.3f * sinf(e.animTimer * 4.0f + static_cast<f32>(i));

        Vec3 col;
        f32 lightRadius;
        u32 rayCount;

        if (e.flags & ENT_FRIENDLY) {
            col = {0.1f * fade, 0.45f * fade * pulse, 0.15f * fade};
            lightRadius = 0.4f;
            rayCount = 4;
        } else if (e.enemyType == EnemyType::BOSS ||
                   e.enemyType == EnemyType::PIT_FIEND ||
                   e.enemyType == EnemyType::HELLFORGE_SMITH) {
            col = {0.9f * fade * pulse, 0.2f * fade, 0.05f * fade};
            lightRadius = 1.0f;
            rayCount = 8;
        } else {
            col = {0.65f * fade * pulse, 0.3f * fade, 0.08f * fade};
            lightRadius = 0.5f;
            rayCount = 6;
        }

        // Starburst: lines radiating outward in all directions from the light center
        for (u32 r = 0; r < rayCount; r++) {
            f32 angle = static_cast<f32>(r) * (6.2832f / static_cast<f32>(rayCount))
                      + e.animTimer * 1.5f; // slow rotation
            f32 dx = cosf(angle) * lightRadius;
            f32 dz = sinf(angle) * lightRadius;
            // Horizontal rays
            DebugDraw::line(lightPos, lightPos + Vec3{dx, 0, dz}, col);
            // Angled rays (upward and downward)
            DebugDraw::line(lightPos, lightPos + Vec3{dx * 0.7f, lightRadius * 0.5f, dz * 0.7f}, col * 0.7f);
            DebugDraw::line(lightPos, lightPos + Vec3{dx * 0.7f, -lightRadius * 0.3f, dz * 0.7f}, col * 0.5f);
        }

        // Vertical accent line (upward glow)
        DebugDraw::line(lightPos, lightPos + Vec3{0, lightRadius * 0.6f, 0}, col * 0.8f);
    }
#endif // !__SWITCH__
}
