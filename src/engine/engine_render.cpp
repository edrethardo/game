// Engine module — see engine.h for class definition.
// Split from engine.cpp for manageability. All methods are Engine:: members.

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
void Engine::renderViewmodel() {
    if (m_inventoryOpen) return;
    if (m_gameState != GameState::IN_GAME) return;

    // Resolve equipped weapon mesh — show fist if unarmed
    const ItemInstance& equipped = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
    bool hasWeapon = !isItemEmpty(equipped) &&
                     m_itemDefs[equipped.defId].meshId > 0 &&
                     m_itemDefs[equipped.defId].meshId < m_meshDefCount;

    u8 weaponMeshId = hasWeapon ? m_itemDefs[equipped.defId].meshId : 0;
    // Use a dummy ItemDef for unarmed (melee type)
    ItemDef unarmedDef = {};
    unarmedDef.weaponType = WeaponType::MELEE;
    unarmedDef.weaponSubtype = WeaponSubtype::NONE;
    const ItemDef& def = hasWeapon ? m_itemDefs[equipped.defId] : unarmedDef;

    // Clear depth so viewmodel renders on top of everything
    glClear(GL_DEPTH_BUFFER_BIT);

    // Use viewport dimensions (split-screen halves), not full window
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    f32 aspect = static_cast<f32>(vp[2]) / static_cast<f32>(vp[3]);

    // Wide FOV for viewmodel so arm/hand are visible in peripheral vision
    Mat4 proj = Mat4::perspective(85.0f * (3.14159f / 180.0f), aspect, 0.01f, 10.0f);

    // Weapon bob: figure-8 pattern — X at base freq, Y at 2× (lying 8 shape).
    // Slower, heavier cadence than vanilla Doom for a more powerful stride feel.
    f32 vxR = m_localPlayer.velocity.x;
    f32 vzR = m_localPlayer.velocity.z;
    f32 speedSqR = vxR * vxR + vzR * vzR;
    f32 bobR = speedSqR * 0.028f;
    if (bobR > 1.0f) bobR = 1.0f;

    f32 weaponAngle = m_viewmodelState.bobTimer * 5.5f; // match view bob frequency
    f32 bobX = bobR * 0.035f * sinf(weaponAngle);             // wide lateral swing
    f32 bobY = bobR * 0.025f * sinf(weaponAngle * 2.0f);      // 2× freq = figure-8 vertical
    // Add look sway — weapon trails behind camera rotation
    bobX += m_viewmodelState.swayYaw;
    bobY += m_viewmodelState.swayPitch;

    // Viewmodel-only recoil (doesn't affect camera)
    f32 recoilPitch = -m_viewmodelState.recoilKick * 0.12f;

    // Attack animation — per-subtype melee, generic recoil for ranged
    f32 attackPitch = 0.0f;  // X rotation (pitch forward/back)
    f32 attackYaw   = 0.0f;  // Y rotation (swing left/right)
    f32 attackRoll  = 0.0f;  // Z rotation (roll — horizontal swing in view plane)
    f32 attackZ     = 0.0f;  // Z offset (thrust forward/back)
    f32 attackY     = 0.0f;  // Y offset (drop down during reload)

    if (m_viewmodelState.attackAnimT > 0.0f) {
        if (def.weaponType == WeaponType::MELEE) {
            f32 t = m_viewmodelState.attackAnimT / 0.3f; // normalized 1→0
            f32 swing = sinf(t * 3.14159f); // smooth arc, peaks at t=0.5
            switch (def.weaponSubtype) {
                case WeaponSubtype::DAGGER:
                case WeaponSubtype::THROWING_KNIFE:
                    // Fast stab forward with slight upward arc
                    attackZ = -0.6f * swing;
                    attackPitch = -0.4f * swing;
                    attackY = 0.05f * swing;
                    break;
                case WeaponSubtype::AXE:
                    // Heavy overhead chop — big pitch rotation + downward drop
                    attackPitch = -1.4f * swing;
                    attackY = -0.08f * swing;
                    attackZ = -0.15f * swing;
                    break;
                case WeaponSubtype::CLAYMORE: {
                    // Claymore: hand sweeps from right to left across the body.
                    // t goes 1→0; cosine for smooth start/end, fast middle.
                    f32 sweepT = t;
                    f32 arc = sinf(sweepT * 3.14159f); // peaks at mid-swing
                    // X translation: weapon moves from far right to far left
                    bobX += 0.35f * cosf(sweepT * 3.14159f);
                    // Roll follows the lateral motion — blade tilts into the cut
                    attackRoll = -0.6f * cosf(sweepT * 3.14159f);
                    // Tip blade forward so it reads as a horizontal sweep
                    attackPitch = -0.7f;
                    // Forward reach at mid-swing
                    attackZ = -0.2f * arc;
                    // Slight drop at mid-swing from the weight
                    attackY = -0.06f * arc;
                } break;
                case WeaponSubtype::SWORD:
                default:
                    // Wide lateral slash with follow-through
                    attackYaw = -1.3f * swing;
                    attackPitch = -0.25f * swing;
                    attackZ = -0.12f * swing;
                    break;
            }
        } else if (def.weaponType == WeaponType::PROJECTILE) {
            f32 t = m_viewmodelState.attackAnimT / 0.3f;
            f32 swing = sinf(t * 3.14159f);
            switch (def.weaponSubtype) {
                case WeaponSubtype::MOLOTOV:
                    // Lob: arm swings up and forward in an arc
                    attackPitch = -1.0f * swing;
                    attackZ = -0.5f * swing;
                    attackY = 0.15f * swing;
                    break;
                case WeaponSubtype::THROWING_KNIFE:
                    // Quick flick forward
                    attackZ = -0.7f * swing;
                    attackPitch = -0.3f * swing;
                    break;
                case WeaponSubtype::BOW:
                    // Draw and release — pull back then snap forward
                    attackZ = 0.08f * swing;
                    attackPitch = 0.15f * t;
                    break;
                case WeaponSubtype::CROSSBOW:
                    // Short sharp recoil kick
                    attackPitch = 0.2f * t;
                    attackZ = 0.05f * t;
                    break;
                case WeaponSubtype::WAND:
                    // Magic pulse — slight forward thrust with upward flick
                    attackPitch = 0.12f * swing;
                    attackZ = -0.06f * swing;
                    break;
                default:
                    // Other projectiles — subtle recoil
                    attackPitch = 0.15f * t;
                    attackZ = 0.04f * t;
                    break;
            }
        } else if (def.weaponType == WeaponType::HITSCAN) {
            f32 t = m_viewmodelState.attackAnimT / 0.2f; // faster snap-back
            switch (def.weaponSubtype) {
                case WeaponSubtype::PISTOL:
                    // Quick upward kick, snaps back
                    attackPitch = 0.25f * t;
                    attackZ = 0.04f * t; // slight pushback
                    break;
                case WeaponSubtype::SMG:
                    // Rapid small jitter — high frequency, low amplitude
                    attackPitch = 0.12f * t + sinf(t * 40.0f) * 0.03f;
                    attackYaw = sinf(t * 30.0f) * 0.02f;
                    break;
                case WeaponSubtype::CARBINE:
                    // Heavy shoulder kick — big pitch, slow return
                    attackPitch = 0.4f * t;
                    attackZ = 0.08f * t;
                    break;
                case WeaponSubtype::REVOLVER:
                    // Strong upward flip with yaw torque
                    attackPitch = 0.35f * t;
                    attackYaw = 0.1f * t;
                    attackZ = 0.06f * t;
                    break;
                default:
                    attackPitch = 0.2f * t;
                    break;
            }
        }
    }

    // Throwaway legendary: weapon is gone during reload (it was thrown)
    WeaponState& vmWs = m_players[m_localPlayerIndex].weaponState;
    if (hasWeapon && m_itemDefs[equipped.defId].legendarySkillId == SkillId::THROWAWAY && vmWs.reloading) {
        return; // weapon is flying through the air — nothing to render
    }

    // Reload animation — weapon tilts down and to the side during reload
    f32 reloadAnim = 0.0f; // 0 = not reloading, 1 = mid-reload
    if (vmWs.reloading && def.weaponType == WeaponType::HITSCAN) {
        // Build effective weapon to get reload time for progress calc
        WeaponDef vmWpn;
        if (hasWeapon) {
            vmWpn = Inventory::getWeaponFromItem(m_inventories[m_localPlayerIndex],
                                                  m_itemDefs, equipped);
        } else {
            vmWpn = m_weaponDefs[vmWs.currentWeapon];
        }
        f32 maxReload = (vmWpn.reloadTime > 0.0f) ? vmWpn.reloadTime : 1.0f;
        f32 progress = 1.0f - vmWs.reloadTimer / maxReload; // 0→1

        // Snappy curve: fast drop (first 20%), hold (20-80%), fast snap back (last 20%)
        if (progress < 0.2f) {
            reloadAnim = sinf(progress / 0.2f * 1.5708f); // smooth ease-in
        } else if (progress > 0.8f) {
            reloadAnim = sinf((1.0f - progress) / 0.2f * 1.5708f); // smooth ease-out
        } else {
            reloadAnim = 1.0f; // full tilt in middle
        }

        // Weapon drops down, tilts, and rotates during reload
        attackPitch = -0.5f * reloadAnim;   // tilt weapon nose-down
        attackYaw   = 0.6f * reloadAnim;    // rotate to the right
        attackY     = -0.12f * reloadAnim;  // drop weapon downward
        attackZ     = 0.1f * reloadAnim;    // slight push forward
    }

    // Per-weapon-type positioning
    Vec3 offset;
    f32 holdYaw = 0.0f;
    f32 holdPitch = 0.0f;
    switch (def.weaponType) {
        case WeaponType::MELEE:
            offset = {0.35f + bobX, -0.35f + bobY + attackY, -0.45f + attackZ};
            if (def.weaponSubtype == WeaponSubtype::DAGGER ||
                def.weaponSubtype == WeaponSubtype::THROWING_KNIFE) {
                // Dagger: held forward for stabbing, blade pointing at target
                holdYaw = 0.1f;
                holdPitch = -0.5f; // angled forward like an icepick grip
            } else if (def.weaponSubtype == WeaponSubtype::AXE) {
                // Axe blade extends in -X; rotate -90° so it faces forward (away from player)
                holdYaw = 0.4f - 1.5708f;
                holdPitch = -0.2f;
            } else {
                holdYaw = 0.4f;
                holdPitch = -0.2f;
            }
            break;
        case WeaponType::HITSCAN:
            // Grip at hand, barrel forward — offset places grip at lower-right of screen
            offset = {0.35f + bobX, -0.40f + bobY + attackY, -0.45f + attackZ};
            holdYaw = 3.24159f; // π + slight offset — barrel faces forward (away from player)
            holdPitch = 0.05f;  // slight upward tilt so barrel aims at crosshair
            break;
        case WeaponType::PROJECTILE:
            offset = {0.30f + bobX, -0.35f + bobY + attackY, -0.50f};
            if (def.weaponSubtype == WeaponSubtype::CROSSBOW) {
                // Crossbow stock extends along +Z in mesh space — rotate π so it points away
                holdYaw = 3.14159f;
                holdPitch = 0.05f;
            } else {
                holdYaw = 0.2f;
                holdPitch = -0.1f;
            }
            break;
    }

    // Rapid vibration while firing ranged weapons
    if (m_viewmodelState.fireShakeTimer > 0.0f) {
        f32 intensity = m_viewmodelState.fireShakeTimer / 0.15f;
        f32 phase = m_viewmodelState.fireShakeTimer * 60.0f;
        offset.x += sinf(phase * 7.3f) * 0.003f * intensity;
        offset.y += sinf(phase * 11.1f) * 0.002f * intensity;
    }

    // Scale weapon mesh to fill viewmodel area (~0.8 units)
    const AABB& wb = m_meshDefs[weaponMeshId].bounds;
    f32 meshH = wb.max.y - wb.min.y;
    f32 meshW = wb.max.x - wb.min.x;
    f32 meshD = wb.max.z - wb.min.z;
    f32 maxDim = meshH;
    if (meshW > maxDim) maxDim = meshW;
    if (meshD > maxDim) maxDim = meshD;
    f32 weaponScale = (maxDim > 0.001f) ? (0.8f / maxDim) : 0.8f;

    // Center the mesh at origin before scaling (offset by mesh center)
    Vec3 meshCenter = {
        (wb.min.x + wb.max.x) * 0.5f,
        (wb.min.y + wb.max.y) * 0.5f,
        (wb.min.z + wb.max.z) * 0.5f
    };

    // Pivot point: rotate around the grip (bottom of mesh) for melee swings
    // and hitscan weapons so the barrel extends forward from the hand.
    Vec3 gripPivot   = {-meshCenter.x, -wb.min.y, -meshCenter.z}; // pivot at grip (bottom)
    Vec3 centerPivot = {-meshCenter.x, -meshCenter.y, -meshCenter.z}; // pivot at center
    bool useGripPivot = (attackRoll != 0.0f) || (def.weaponType == WeaponType::HITSCAN);

    Mat4 weaponModel = Mat4::translate(offset)
                     * Mat4::rotateZ(attackRoll)
                     * Mat4::rotateX(recoilPitch + attackPitch + holdPitch)
                     * Mat4::rotateY(holdYaw + attackYaw)
                     * Mat4::scale({weaponScale, weaponScale, weaponScale})
                     * Mat4::translate(useGripPivot ? gripPivot : centerPivot);

    Mat4 weaponMVP = proj * weaponModel;

    // Draw weapon mesh with material tint — fade to 30% alpha during stealth
    bool stealthed = (m_localPlayer.smokeTimer > 0.0f);
    if (stealthed) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    glUseProgram(m_unlitShader.program);
    glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, weaponMVP.m);

    const Material* wpnMat = MaterialSystem::get(def.materialId);
    Vec4 wpnTint = wpnMat ? wpnMat->tint : Vec4{0.7f, 0.7f, 0.7f, 1.0f};
    f32 stealthAlpha = stealthed ? 0.3f : 1.0f;
    glUniform4f(m_unlitShader.loc_color, wpnTint.x, wpnTint.y, wpnTint.z, wpnTint.w * stealthAlpha);

    glActiveTexture(GL_TEXTURE0);
    if (wpnMat) {
        glBindTexture(GL_TEXTURE_2D, wpnMat->texture.handle);
    } else {
        const Material* fallback = MaterialSystem::get(0);
        if (fallback) glBindTexture(GL_TEXTURE_2D, fallback->texture.handle);
    }
    glUniform1i(m_unlitShader.loc_texture0, 0);

    if (hasWeapon) {
        const Mesh& wpnMesh = m_meshDefs[weaponMeshId].mesh;
        if (wpnMesh.materialGroupCount > 0) {
            // Multi-material viewmodel: bind VAO once, draw each group with its own texture/tint.
            glBindVertexArray(wpnMesh.vao);
            for (u8 g = 0; g < wpnMesh.materialGroupCount; g++) {
                const MeshMaterialGroup& grp = wpnMesh.materials[g];
                const Material* mat = MaterialSystem::get(grp.materialId);
                if (mat) {
                    glBindTexture(GL_TEXTURE_2D, mat->texture.handle);
                    glUniform4f(m_unlitShader.loc_color,
                                mat->tint.x, mat->tint.y, mat->tint.z, mat->tint.w * stealthAlpha);
                }
                glDrawElements(GL_TRIANGLES, grp.indexCount, GL_UNSIGNED_INT,
                               reinterpret_cast<void*>(
                                   static_cast<uintptr_t>(grp.indexStart * sizeof(u32))));
            }
        } else {
            MeshSystem::draw(wpnMesh);
        }
    }

    // Draw hand gripping the weapon (or fist if unarmed)
    // Hand sits at the weapon's base, rotated to wrap around the grip
    {
        const Material* skinMat = MaterialSystem::get(MaterialSystem::getIdByName("human_skin"));
        Vec4 skinTint = {0.85f, 0.70f, 0.55f, 1.0f};
        glUniform4f(m_unlitShader.loc_color, skinTint.x, skinTint.y, skinTint.z, skinTint.w * stealthAlpha);
        if (skinMat) {
            glBindTexture(GL_TEXTURE_2D, skinMat->texture.handle);
        }

        // Hand at weapon grip — positioning and rotation depends on weapon type
        Vec3 handOff = {0.0f, -0.12f, 0.05f};
        Vec3 armOff  = {0.02f, -0.18f, 0.25f};
        f32  handRotX = 0.0f; // extra hand rotation to curl fingers around grip
        if (def.weaponType == WeaponType::HITSCAN) {
            // Pistol grip: fingers curl down around the grip, hand behind receiver
            handOff = {0.0f, -0.04f, -0.02f};
            armOff  = {0.02f, -0.14f, -0.20f};
            handRotX = -1.5708f; // -90° X rotation: fingers point down to wrap grip
        }
        Mat4 handModel = Mat4::translate(offset)
                       * Mat4::rotateX(recoilPitch + attackPitch + holdPitch)
                       * Mat4::rotateY(holdYaw + attackYaw)
                       * Mat4::translate(handOff)
                       * Mat4::rotateX(handRotX)
                       * Mat4::scale({1.2f, 1.2f, 1.2f});
        Mat4 handMVP = proj * handModel;
        glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, handMVP.m);
        MeshSystem::draw(m_handMesh);

        // Forearm extending back from the hand toward the camera
        Mat4 armModel = Mat4::translate(offset)
                      * Mat4::rotateX(recoilPitch + attackPitch + holdPitch)
                      * Mat4::rotateY(holdYaw + attackYaw)
                      * Mat4::translate(armOff)
                      * Mat4::rotateX(0.15f)
                      * Mat4::scale({0.08f, 0.07f, 0.30f});
        Mat4 armMVP = proj * armModel;
        glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, armMVP.m);
        MeshSystem::draw(m_cubeMesh);
    }

    // Shield on left side — lowered at rest, raised when blocking
    const ItemInstance& shieldItem = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::OFFHAND)];
    if (!isItemEmpty(shieldItem) && m_itemDefs[shieldItem.defId].slot == ItemSlot::OFFHAND) {
        u8 shieldMeshId = m_itemDefs[shieldItem.defId].meshId;
        if (shieldMeshId > 0 && shieldMeshId < m_meshDefCount) {
            // Block animation: smoothly raise shield from hip to face level
            f32 blockT = 0.0f; // 0 = resting (lowered), 1 = fully raised (blocking)
            if (m_localPlayer.blocking) {
                // Quick raise: reach full block in 0.15s
                blockT = fminf(m_localPlayer.blockTimer / 0.15f, 1.0f);
            }

            // Rest position: low and to the left (out of view center)
            // Block position: raised to cover center-left, angled to face forward
            f32 shieldY = -0.55f + blockT * 0.30f;  // raise from hip to chest
            f32 shieldX = -0.45f + blockT * 0.10f;   // move slightly inward when blocking
            f32 shieldZ = -0.45f - blockT * 0.10f;   // push slightly forward when blocking
            f32 shieldPitch = -0.3f + blockT * 0.3f;  // tilt upright when blocking
            f32 shieldYaw = -0.3f + blockT * 0.15f;   // face more forward when blocking

            Vec3 shieldOff = {shieldX + bobX * 0.3f, shieldY + bobY * 0.3f, shieldZ};

            // Scale shield mesh
            const AABB& sb = m_meshDefs[shieldMeshId].bounds;
            f32 sMaxDim = sb.max.y - sb.min.y;
            if (sb.max.x - sb.min.x > sMaxDim) sMaxDim = sb.max.x - sb.min.x;
            if (sb.max.z - sb.min.z > sMaxDim) sMaxDim = sb.max.z - sb.min.z;
            f32 shieldScale = (sMaxDim > 0.001f) ? (0.7f / sMaxDim) : 0.7f;
            Vec3 sCtr = {(sb.min.x+sb.max.x)*0.5f, (sb.min.y+sb.max.y)*0.5f, (sb.min.z+sb.max.z)*0.5f};

            Mat4 shieldModel = Mat4::translate(shieldOff)
                             * Mat4::rotateX(shieldPitch + recoilPitch * 0.2f)
                             * Mat4::rotateY(shieldYaw)
                             * Mat4::scale({shieldScale, shieldScale, shieldScale})
                             * Mat4::translate({-sCtr.x, -sCtr.y, -sCtr.z});
            Mat4 shieldMVP = proj * shieldModel;

            // Draw shield with its material
            const Material* shMat = MaterialSystem::get(m_itemDefs[shieldItem.defId].materialId);
            Vec4 shTint = shMat ? shMat->tint : Vec4{0.7f, 0.7f, 0.7f, 1.0f};
            glUniform4f(m_unlitShader.loc_color, shTint.x, shTint.y, shTint.z, shTint.w * stealthAlpha);
            if (shMat) glBindTexture(GL_TEXTURE_2D, shMat->texture.handle);
            glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, shieldMVP.m);
            MeshSystem::draw(m_meshDefs[shieldMeshId].mesh);

            // Left hand holding the shield
            const Material* skinMat2 = MaterialSystem::get(MaterialSystem::getIdByName("human_skin"));
            Vec4 skin2 = {0.85f, 0.70f, 0.55f, stealthAlpha};
            glUniform4f(m_unlitShader.loc_color, skin2.x, skin2.y, skin2.z, skin2.w);
            if (skinMat2) glBindTexture(GL_TEXTURE_2D, skinMat2->texture.handle);

            Mat4 lHandModel = Mat4::translate(shieldOff)
                            * Mat4::translate({0.0f, -0.08f, 0.05f})
                            * Mat4::scale({1.2f, 1.2f, 1.2f});
            Mat4 lHandMVP = proj * lHandModel;
            glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, lHandMVP.m);
            MeshSystem::draw(m_handMesh);

            // Left forearm
            Mat4 lArmModel = Mat4::translate(shieldOff)
                           * Mat4::translate({-0.02f, -0.14f, 0.25f})
                           * Mat4::rotateX(0.15f)
                           * Mat4::scale({0.08f, 0.07f, 0.30f});
            Mat4 lArmMVP = proj * lArmModel;
            glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, lArmMVP.m);
            MeshSystem::draw(m_cubeMesh);
        }
    }

    if (stealthed) glDisable(GL_BLEND);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void Engine::render(f32 alpha) {
    // Interpolate camera between previous and current tick for smooth gyro/look.
    // Save tick state, interpolate for rendering, then restore after frame.
    Vec3 tickPos   = m_camera.position;
    f32  tickYaw   = m_camera.yaw;
    f32  tickPitch = m_camera.pitch;
    if (m_gameState == GameState::IN_GAME) {
        m_camera.position = m_camera.prevPosition + (tickPos   - m_camera.prevPosition) * alpha;
        // Angle-aware yaw interpolation — handles ±π wrapping without snapping
        f32 yawDiff = tickYaw - m_camera.prevYaw;
        if (yawDiff >  3.14159f) yawDiff -= 6.28318f;
        if (yawDiff < -3.14159f) yawDiff += 6.28318f;
        m_camera.yaw      = m_camera.prevYaw + yawDiff * alpha;
        m_camera.pitch    = m_camera.prevPitch + (tickPitch - m_camera.prevPitch) * alpha;
        // Apply screen shake offset — decays over time, doesn't affect saved tick state
        Vec3 shakeOffset = m_camera.shake.update(static_cast<f32>(FIXED_DT));
        m_camera.position = m_camera.position + shakeOffset;
    }

    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();

    if (m_gameState == GameState::MENU) {
        renderMenu();
        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return;
    }

    if (m_gameState == GameState::CONNECTING) {
        // Pulsing dot to indicate connecting
        f32 pulse = (sinf(m_statsTimer * 6.0f) + 1.0f) * 0.5f;
        HUD::drawCrosshair(sw, sh, {pulse, pulse, 0.5f + pulse * 0.5f});
        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return;
    }

    if (m_gameState == GameState::FLOOR_TRANSITION) {
        glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        u32 sw = Window::getWidth();
        u32 sh = Window::getHeight();
        f32 uiScale = static_cast<f32>(sh) / 720.0f;
        FontSystem::setUIScale(uiScale);

        // Fade: full opacity for first 1.5s, fade out in last 0.5s
        f32 alpha = fminf(m_transition.timer * 2.0f, 1.0f);

        // Difficulty prefix + floor number — large gold
        char floorStr[48];
        const char* diffPrefix = "";
        if (m_difficulty == 1) diffPrefix = "Nightmare - ";
        else if (m_difficulty == 2) diffPrefix = "Hell - ";
        std::snprintf(floorStr, sizeof(floorStr), "%sFloor %u", diffPrefix, m_level.currentFloor);
        f32 floorW = FontSystem::textWidth(floorStr, 4);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - floorW) * 0.5f, sh * 0.6f,
                             floorStr, {0.9f * alpha, 0.7f * alpha, 0.2f * alpha}, 4);

        // Theme name — smaller muted gold
        const char* themeName = "The Dungeon";
        if (m_level.currentFloor >= 41)      themeName = "The Void";
        else if (m_level.currentFloor >= 31) themeName = "The Hellforge";
        else if (m_level.currentFloor >= 21) themeName = "Spider Caverns";
        else if (m_level.currentFloor >= 11) themeName = "The Catacombs";
        f32 themeW = FontSystem::textWidth(themeName, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - themeW) * 0.5f, sh * 0.50f,
                             themeName, {0.7f * alpha, 0.55f * alpha, 0.2f * alpha}, 2);

        // Kill count
        char killStr[32];
        std::snprintf(killStr, sizeof(killStr), "Enemies slain: %u", m_transition.snapshotKills);
        f32 killW = FontSystem::textWidth(killStr, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - killW) * 0.5f, sh * 0.35f,
                             killStr, {0.8f * alpha, 0.8f * alpha, 0.8f * alpha}, 2);

        // Time — format as M:SS
        u32 totalSec = static_cast<u32>(m_transition.snapshotTime);
        u32 minutes = totalSec / 60;
        u32 seconds = totalSec % 60;
        char timeStr[32];
        std::snprintf(timeStr, sizeof(timeStr), "Floor time: %u:%02u", minutes, seconds);
        f32 timeW = FontSystem::textWidth(timeStr, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - timeW) * 0.5f, sh * 0.28f,
                             timeStr, {0.8f * alpha, 0.8f * alpha, 0.8f * alpha}, 2);

        // Total play time
        u32 totalAll = static_cast<u32>(m_transition.totalPlayTime);
        u32 totalMin = totalAll / 60;
        u32 totalSc  = totalAll % 60;
        char totalStr[32];
        std::snprintf(totalStr, sizeof(totalStr), "Total time: %u:%02u", totalMin, totalSc);
        f32 totalW = FontSystem::textWidth(totalStr, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - totalW) * 0.5f, sh * 0.21f,
                             totalStr, {0.6f * alpha, 0.6f * alpha, 0.6f * alpha}, 2);

        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return;
    }

    if (m_gameState == GameState::VICTORY) {
        // --- Victory screen ---
        glClearColor(0.01f, 0.02f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        FontSystem::setUIScale(static_cast<f32>(sh) / 720.0f);

        // Final victory — only shown after Hell floor 50
        const char* title = "You conquered the Dungeon Engine.";
        const char* subtitle = nullptr;

        f32 titleW = FontSystem::textWidth(title, 4);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - titleW) * 0.5f, sh * 0.65f,
                             title, {0.9f, 0.8f, 0.2f}, 4);

        if (subtitle) {
            f32 subW = FontSystem::textWidth(subtitle, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - subW) * 0.5f, sh * 0.54f,
                                 subtitle, {0.7f, 0.9f, 0.5f}, 2);
        }

        // Stats — total play time
        {
            u32 totalSec = static_cast<u32>(m_transition.totalPlayTime);
            u32 totalMin = totalSec / 60;
            u32 totalS   = totalSec % 60;
            char timeStr[48];
            std::snprintf(timeStr, sizeof(timeStr), "Total time: %u:%02u", totalMin, totalS);
            f32 timeW = FontSystem::textWidth(timeStr, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - timeW) * 0.5f, sh * 0.40f,
                                 timeStr, {0.7f, 0.7f, 0.7f}, 2);
        }

        // Class name
        {
            const char* className = kClassDefs[static_cast<u32>(m_playerClass)].name;
            char classStr[48];
            std::snprintf(classStr, sizeof(classStr), "Class: %s", className);
            f32 classW = FontSystem::textWidth(classStr, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - classW) * 0.5f, sh * 0.32f,
                                 classStr, {0.7f, 0.7f, 0.7f}, 2);
        }

        // Prompt
        const char* prompt = "Press any key to continue";
        f32 promptW = FontSystem::textWidth(prompt, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - promptW) * 0.5f, sh * 0.18f,
                             prompt, {0.5f, 0.5f, 0.5f}, 1);

        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return;
    }

    if (m_gameState == GameState::GAME_OVER) {
        // --- Death screen ---
        const char* deathTitle = "YOU DIED";
        f32 titleW = FontSystem::textWidth(deathTitle, 4);
        FontSystem::drawText(sw, sh, (sw - titleW) * 0.5f, sh * 0.6f, deathTitle, {0.8f, 0.1f, 0.1f}, 4);

        char floorStr[48];
        std::snprintf(floorStr, sizeof(floorStr), "Floor %u", m_level.currentFloor);
        f32 floorW = FontSystem::textWidth(floorStr, 2);
        FontSystem::drawText(sw, sh, (sw - floorW) * 0.5f, sh * 0.48f, floorStr, {0.6f, 0.6f, 0.6f}, 2);

        if (m_menu.confirmQuit) {
            // "Are you sure?" overlay
            const char* confirmTxt = "Quit to menu?";
            f32 cW = FontSystem::textWidth(confirmTxt, 2);
            FontSystem::drawText(sw, sh, (sw - cW) * 0.5f, sh * 0.35f, confirmTxt, {1.0f, 0.8f, 0.3f}, 2);

            f32 cy = sh * 0.26f;
            f32 cx = static_cast<f32>(sw) * 0.5f;
            bool qp = Input::isGamepadConnected(0);
            HUD::drawKeySymbol(sw, sh, cx - 60.0f, cy, qp ? "A" : "Ent", true);
            FontSystem::drawText(sw, sh, cx - 30.0f, cy + 4.0f, "Yes", {0.8f, 0.8f, 0.8f}, 1);
            HUD::drawKeySymbol(sw, sh, cx + 15.0f, cy, qp ? "B" : "Esc", true);
            FontSystem::drawText(sw, sh, cx + 43.0f, cy + 4.0f, "No", {0.8f, 0.8f, 0.8f}, 1);
        } else {
            // Three options with key icons
            f32 cx = static_cast<f32>(sw) * 0.5f;
            f32 optY = sh * 0.35f;

            bool pad = Input::isGamepadConnected(0);
            HUD::drawKeySymbol(sw, sh, cx - 80.0f, optY, pad ? "A" : "Spc", true);
            FontSystem::drawText(sw, sh, cx - 50.0f, optY + 4.0f, "Respawn at entrance",
                                 {0.5f, 0.9f, 0.5f}, 1);

            optY -= 25.0f;
            // Only show "Reload last save" in singleplayer
            if (m_netRole == NetRole::NONE) {
                HUD::drawKeySymbol(sw, sh, cx - 80.0f, optY, pad ? "X" : "Ent", true);
                FontSystem::drawText(sw, sh, cx - 50.0f, optY + 4.0f, "Reload last save",
                                     {0.5f, 0.6f, 0.9f}, 1);
                optY -= 25.0f;
            }
            HUD::drawKeySymbol(sw, sh, cx - 80.0f, optY, pad ? "-" : "Esc", true);
            FontSystem::drawText(sw, sh, cx - 50.0f, optY + 4.0f, "Quit to menu",
                                 {0.7f, 0.4f, 0.4f}, 1);
        }

        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return;
    }

    if (m_gameState != GameState::IN_GAME) {
        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return;
    }

    PROFILE_SCOPE(3, "Render");

    // Split-screen: render each player's view
    for (u8 sp = 0; sp < m_splitPlayerCount; sp++) {
    // Swap in this player's camera and state
    if (m_splitPlayerCount > 1) {
        swapInPlayer(sp);
    }

    // Compute viewport for this player
    u32 vpX = 0, vpY = 0, vpW = sw, vpH = sh;
    if (m_splitPlayerCount > 1) {
        if (m_splitMode == 0) {
            // Horizontal split: P1=top, P2=bottom
            vpH = sh / 2;
            vpY = (sp == 0) ? vpH : 0;
        } else {
            // Vertical split: P1=left, P2=right
            vpW = sw / 2;
            vpX = (sp == 0) ? 0 : vpW;
        }
    }

    // Switch constraint mode
    if (m_switchMode) {
        vpW = SWITCH_RES_W;
        vpH = SWITCH_RES_H;
        if (m_splitPlayerCount > 1) {
            if (m_splitMode == 0) vpH /= 2;
            else vpW /= 2;
        }
    }

    glViewport(vpX, vpY, vpW, vpH);
    glScissor(vpX, vpY, vpW, vpH);
    glEnable(GL_SCISSOR_TEST);

    // Only clear depth per player (color was cleared at top of render)
    if (sp > 0) glClear(GL_DEPTH_BUFFER_BIT);

    f32 aspect = static_cast<f32>(vpW) / static_cast<f32>(vpH);
    CameraSystem::computeMatrices(m_camera, aspect);

    Renderer::beginFrame(m_camera);
    // Per-floor lighting themes — each tier gets distinct atmosphere
    struct FloorTheme { u32 minFloor; Vec3 lightColor; Vec3 ambient; };
    static const FloorTheme kThemes[] = {
        { 1,  {1.0f, 0.95f, 0.9f},  {0.18f, 0.18f, 0.22f}},  // Dungeon: warm torchlight
        {11,  {0.8f, 0.9f,  0.7f},  {0.12f, 0.15f, 0.12f}},  // Catacombs: sickly green
        {21,  {0.7f, 0.7f,  1.0f},  {0.20f, 0.18f, 0.28f}},  // Caverns: cold blue-purple
        {31,  {1.0f, 0.6f,  0.3f},  {0.25f, 0.10f, 0.05f}},  // Hellforge: hot orange
        {41,  {0.6f, 0.6f,  0.8f},  {0.15f, 0.15f, 0.25f}},  // Void: dark but visible
    };
    const FloorTheme* theme = &kThemes[0];
    for (u32 t = 0; t < 5; t++) {
        if (m_level.currentFloor >= kThemes[t].minFloor) theme = &kThemes[t];
    }
    Renderer::setDirectionalLight(
        normalize(Vec3{-0.3f, -1.0f, -0.5f}),
        theme->lightColor,
        theme->ambient
    );

    // Send nearest 4 point lights to the shader
    // Collect all light candidates: static level lights + dynamic weapon flashes + projectile lights.
    // Pick the nearest 4 to the camera and send to the shader.
    {
        Vec3 camPos = m_camera.position;
        // Candidate pool — positions and colors collected from all sources
        static constexpr u32 MAX_CANDIDATES = 80; // 64 static + 4 dynamic + ~12 lit projectiles
        Vec3 candPos[MAX_CANDIDATES];
        Vec3 candCol[MAX_CANDIDATES];
        u32 candCount = 0;

        // Static level lights
        for (u32 li = 0; li < m_pointLightCount && candCount < MAX_CANDIDATES; li++) {
            candPos[candCount] = m_pointLights[li].position;
            candCol[candCount] = m_pointLights[li].color;
            candCount++;
        }
        // Dynamic weapon flash lights (decaying muzzle flashes)
        for (u32 di = 0; di < MAX_DYNAMIC_LIGHTS && candCount < MAX_CANDIDATES; di++) {
            if (m_dynamicLights[di].timer > 0.0f) {
                candPos[candCount] = m_dynamicLights[di].position;
                // Fade light intensity as timer expires
                f32 fade = m_dynamicLights[di].timer * 10.0f; // 0.1s → fades over duration
                if (fade > 1.0f) fade = 1.0f;
                candCol[candCount] = m_dynamicLights[di].color * fade;
                candCount++;
            }
        }
        // Active projectiles with non-zero lightColor
        for (u32 pi = 0; pi < MAX_PROJECTILES && candCount < MAX_CANDIDATES; pi++) {
            const Projectile& p = m_projectiles.projectiles[pi];
            if (!p.active) continue;
            if (p.lightColor.x == 0.0f && p.lightColor.y == 0.0f && p.lightColor.z == 0.0f) continue;
            candPos[candCount] = p.position;
            candCol[candCount] = p.lightColor;
            candCount++;
        }

        // Find nearest 4 candidates to camera
        Vec3 positions[4], colors[4];
        u32 nearCount = 0;
        struct LightDist { u32 idx; f32 dist; };
        LightDist nearest[4];
        for (u32 ci = 0; ci < candCount; ci++) {
            f32 d = lengthSq(candPos[ci] - camPos);
            if (nearCount < 4) {
                nearest[nearCount++] = {ci, d};
            } else {
                u32 worstIdx = 0;
                for (u32 n = 1; n < 4; n++) {
                    if (nearest[n].dist > nearest[worstIdx].dist) worstIdx = n;
                }
                if (d < nearest[worstIdx].dist) {
                    nearest[worstIdx] = {ci, d};
                }
            }
        }
        for (u32 n = 0; n < nearCount; n++) {
            positions[n] = candPos[nearest[n].idx];
            colors[n]    = candCol[nearest[n].idx];
        }
        Renderer::setPointLights(positions, colors, nearCount);
    }

    // Level geometry
    LevelMeshSystem::submitAll(m_level.sections, m_level.sectionCount, m_basicShader);

    // Choose entity source based on role
    const EntityPool& entPool = (m_netRole == NetRole::CLIENT) ? m_renderInterp.entities : m_entities;

    renderEntities(vpW, vpH);

    DebugDraw::clear();

    renderProjectilesAndEffects(vpW, vpH);
    renderWorldItems(vpW, vpH);

    { PROFILE_SCOPE(4, "Flush");
    Renderer::flush();
    }

    // --- Debug overlay (F1 toggle — boxes only, lines already accumulated above) ---
    if (DebugDraw::isEnabled()) {
        Vec3 feet = m_localPlayer.position;
        AABB playerBox = {
            feet + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
            feet + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
        };
        Vec3 boxColor = m_localPlayer.onGround ? Vec3{0,1,0} : Vec3{1,1,0};
        DebugDraw::box(playerBox, boxColor);

        for (u32 _a = 0; _a < entPool.activeCount; _a++) { u32 i = entPool.activeList[_a];
            const Entity& e = entPool.entities[i];
            if (!(e.flags & ENT_ACTIVE)) continue;
            Vec3 c = (e.flags & ENT_DEAD) ? Vec3{0.5f,0.5f,0.5f}
                   : (e.flags & ENT_FLYING) ? Vec3{0.3f,0.3f,1.0f}
                   : Vec3{1.0f,0.3f,0.3f};
            DebugDraw::box(entityAABB(e), c);
        }

        if (m_lastCombatHit.hit) {
            Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
            DebugDraw::line(eyePos, m_lastCombatHit.position, {1,0,0});
            DebugDraw::cross(m_lastCombatHit.position, 0.15f, {1,0.5f,0});
            DebugDraw::ray(m_lastCombatHit.position, m_lastCombatHit.normal, 0.5f, {1,1,0});
        }

        // Draw enemy tactical AI debug info: A* paths (green) and squad role indicators
        for (u32 ai = 0; ai < entPool.activeCount; ai++) {
            u32 idx = entPool.activeList[ai];
            const Entity& ent = entPool.entities[idx];
            if (ent.flags & ENT_DEAD) continue;
            if (ent.flags & ENT_FRIENDLY) continue;

            // Path visualization: connect position to each remaining waypoint in green
            if (ent.pathLen > 0 && ent.pathIdx < ent.pathLen) {
                Vec3 prev = ent.position;
                for (u8 p = ent.pathIdx; p < ent.pathLen; p++) {
                    DebugDraw::line(prev, ent.pathWaypoints[p], {0.2f, 0.9f, 0.2f});
                    prev = ent.pathWaypoints[p];
                }
            }

            // Vertical role-color indicator: shows squad assignment at a glance
            Vec3 roleColor = {0, 0, 0};
            switch (ent.squadRole) {
                case SquadRole::ROLE_RUSH:   roleColor = {1.0f, 0.2f, 0.2f}; break;
                case SquadRole::ROLE_FLANK:  roleColor = {1.0f, 0.8f, 0.0f}; break;
                case SquadRole::ROLE_HOLD:   roleColor = {0.2f, 0.5f, 1.0f}; break;
                case SquadRole::ROLE_HARASS: roleColor = {0.8f, 0.2f, 1.0f}; break;
                default: continue; // ROLE_NONE: no indicator
            }
            DebugDraw::line(ent.position, ent.position + Vec3{0, 1.5f, 0}, roleColor);
        }
    }

    // Target lock indicator
    if (m_localPlayer.lockActive) {
        const EntityPool& lockPool = (m_netRole == NetRole::CLIENT) ? m_renderInterp.entities : m_entities;
        EntityHandle h = {m_localPlayer.lockIndex, m_localPlayer.lockGeneration};
        Entity* target = handleGet(const_cast<EntityPool&>(lockPool), h);
        if (target) {
            AABB lockBox = entityAABB(*target);
            lockBox.min = lockBox.min - Vec3{0.05f, 0.05f, 0.05f};
            lockBox.max = lockBox.max + Vec3{0.05f, 0.05f, 0.05f};
            bool wasEnabled = DebugDraw::isEnabled();
            DebugDraw::setEnabled(true);
            DebugDraw::box(lockBox, {0.0f, 1.0f, 1.0f});
            DebugDraw::setEnabled(wasEnabled);
        }
    }

    DebugDraw::flush(m_camera.viewProjection);

    // Herald aura effect — golden rotating ring at the feet of the herald
    // and all aura-buffed enemies. Uses the quad mesh with blob texture + alpha.
    {
        Renderer::flush();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);

        const Material* blobMat = MaterialSystem::get(m_particleBlobMatId);
        const Texture& blobTex = blobMat ? blobMat->texture : MaterialSystem::get(0)->texture;

        for (u32 _a = 0; _a < entPool.activeCount; _a++) { u32 i = entPool.activeList[_a];
            const Entity& e = entPool.entities[i];
            if (!(e.flags & ENT_ACTIVE)) continue;
            if (e.flags & ENT_DEAD) continue;
            // Show disc under heralds (larger, brighter) and buffed enemies (smaller, dimmer)
            bool isHerald = (e.enemyRole & EnemyRole::AURA) != 0;
            if (!isHerald && !e.hasAuraBuff) continue;

            Vec3 feetPos = e.position - Vec3{0, e.halfExtents.y - 0.03f, 0};
            f32 t = e.animTimer * 2.0f;
            f32 discR = isHerald ? (1.2f + 0.15f * sinf(t * 3.0f))   // herald: large pulsing disc
                                 : (0.5f + 0.05f * sinf(t * 4.0f));  // buffed: small pulsing disc
            Vec4 discCol = isHerald ? Vec4{0.2f, 0.85f, 0.75f, 0.45f}   // herald: bright teal
                                    : Vec4{0.25f, 0.7f, 0.6f, 0.25f};  // buffed: dim teal

            // Lay quad flat on ground: quad is in XY plane, we need XZ plane.
            // Column 0 = local X → world X (with Y-rotation for spin)
            // Column 1 = local Y → world Z (quad's Y becomes ground forward)
            // Column 2 = local Z → world Y (quad faces up)
            f32 rot = isHerald ? t : -t * 0.5f;
            f32 cr = cosf(rot), sr = sinf(rot);
            Mat4 discMat = {};
            discMat.m[0]  =  cr * discR;  // col0.x
            discMat.m[1]  =  0.0f;        // col0.y
            discMat.m[2]  =  sr * discR;  // col0.z
            discMat.m[4]  = -sr * discR;  // col1.x  (local Y → world XZ rotated)
            discMat.m[5]  =  0.0f;        // col1.y
            discMat.m[6]  =  cr * discR;  // col1.z
            discMat.m[8]  =  0.0f;        // col2.x  (local Z → world Y = up)
            discMat.m[9]  =  1.0f;        // col2.y
            discMat.m[10] =  0.0f;        // col2.z
            discMat.m[12] = feetPos.x;
            discMat.m[13] = feetPos.y;
            discMat.m[14] = feetPos.z;
            discMat.m[15] = 1.0f;

            Mat4 discMvp = m_camera.viewProjection * discMat;
            glUseProgram(m_unlitShader.program);
            glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, discMvp.ptr());
            glUniform4f(m_unlitShader.loc_color, discCol.x, discCol.y, discCol.z, discCol.w);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, blobTex.handle);
            if (m_unlitShader.loc_texture0 >= 0) glUniform1i(m_unlitShader.loc_texture0, 0);
            glBindVertexArray(m_quadMesh.vao);
            glDrawElements(GL_TRIANGLES, m_quadMesh.indexCount, GL_UNSIGNED_INT, 0);
        }

        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    renderSpeechBubbles(vpW, vpH);
    renderDamageNumbers(vpW, vpH);

    // First-person viewmodel (hand + weapon) — drawn after world, before HUD
    renderViewmodel();

    renderHUD(vpW, vpH);

    // Dead player overlay — shows "YOU DIED" on this player's viewport while game continues
    if (m_playerDead[sp]) {
        // Dark overlay
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        const char* deathText = "YOU DIED";
        f32 dw = FontSystem::textWidth(deathText, 3);
        FontSystem::drawText(vpW, vpH, (vpW - dw) * 0.5f, vpH * 0.55f,
                             deathText, {0.8f, 0.1f, 0.1f}, 3);

        bool pad = Input::isGamepadConnected(0);
        const char* respawnText = pad ? "Press A to respawn" : "Press Space to respawn";
        f32 rw = FontSystem::textWidth(respawnText, 2);
        FontSystem::drawText(vpW, vpH, (vpW - rw) * 0.5f, vpH * 0.4f,
                             respawnText, {0.7f, 0.7f, 0.7f}, 2);

        glDisable(GL_BLEND);
    }

    // Flush all accumulated HUD lines in a single draw call (was 36 per frame)
    HUD::flush(vpW, vpH);

    } // end split-screen player loop

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, sw, sh); // restore full viewport

    // Swap in player 0 state as default after rendering
    if (m_splitPlayerCount > 1) swapInPlayer(0);

    // Restore tick-accurate camera state after rendering (interpolation is visual only)
    m_camera.position = tickPos;
    m_camera.yaw      = tickYaw;
    m_camera.pitch    = tickPitch;

    GLContext::swapBuffers(Window::getHandle());
}

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

    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& e = entPool.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;

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
        if (e.enemyType != EnemyType::GENERIC && !(e.flags & ENT_DEAD)) {
            Vec3 toCamera = m_camera.position - e.position;
            if (lengthSq(toCamera) < LIMB_LOD_DIST_SQ) {
                // Use boss-specific limb config if available (extra limbs)
                const LimbConfig& limbCfg = (e.bossLimbConfig > 0)
                    ? LimbSystem::getBossConfig(e.bossLimbConfig)
                    : LimbSystem::getConfig(e.enemyType);

                for (u32 li = 0; li < limbCfg.limbCount; li++) {
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

    // Second pass: render translucent webs with alpha blending (batched, single state change)
    {
        bool hasCavernWebs = (m_level.currentFloor >= 21 && m_level.currentFloor <= 30);
        if (hasCavernWebs) {
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
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& e = entPool.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;
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
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& e = entPool.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;
        if (e.flags & ENT_DEAD) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;

        f32 distSq = lengthSq(e.position - m_camera.position);
        if (distSq > 225.0f) continue;

        f32 fade = 1.0f - distSq / 225.0f;
        f32 pulse = 0.7f + 0.3f * sinf(e.animTimer * 3.0f + static_cast<f32>(i));

        Vec3 col;
        f32 r;
        if (e.enemyType == EnemyType::BOSS) {
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

    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& e = entPool.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;
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
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& e = entPool.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;
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
        } else if (e.enemyType == EnemyType::BOSS) {
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
}

// ---------------------------------------------------------------------------
// renderProjectilesAndEffects — projectile cubes + spark orbs, floor door portal
// debug lines, and fire AoE effects (molotov splash). All DebugDraw::line calls
// here are accumulated before DebugDraw::flush in render().
// ---------------------------------------------------------------------------
void Engine::renderProjectilesAndEffects(u32 sw, u32 sh) {
    (void)sw; (void)sh; // sw/sh reserved for future use

    const ProjectilePool& projPool = (m_netRole == NetRole::CLIENT) ? m_renderInterp.projectiles : m_projectiles;
    const Texture& defaultTex = MaterialSystem::get(0)->texture;

    // Instanced render: batch mesh-based projectiles (arrows, bolts, thrown weapons)
    ProjectileRenderer::render(projPool, m_camera.viewProjection, m_meshDefs, m_meshDefCount,
                               m_meshIdArrow, m_meshIdBolt);

    // Per-projectile special effects (orbs, sparks, generic cubes)
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        const Projectile& p = projPool.projectiles[i];
        if (!p.active) continue;

        // Skip mesh-based projectiles handled by the instanced path above
        if (p.meshId > 0 && !(p.projFlags & (PROJ_ORB | PROJ_SPARK | PROJ_SPLASH))) continue;

        if (p.projFlags & PROJ_ORB) {
            // Frozen Orb — layered crystalline sphere with frost spiral trail
            f32 t = p.lifetime;
            f32 pulse = 0.7f + 0.3f * sinf(t * 12.0f);

            // Core: bright white-blue inner orb
            f32 coreSize = p.radius * 2.0f * pulse;
            Mat4 coreModel = Mat4::translate(p.position)
                           * Mat4::rotateY(t * 8.0f)
                           * Mat4::rotateX(t * 5.0f)
                           * Mat4::scale({coreSize, coreSize, coreSize});
            AABB coreBounds = {p.position - Vec3{coreSize,coreSize,coreSize},
                               p.position + Vec3{coreSize,coreSize,coreSize}};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, coreModel, coreBounds,
                             {0.8f, 0.9f, 1.0f, 1.0f});

            // Outer shell: larger translucent ice-blue
            f32 shellSize = p.radius * 4.5f * (0.9f + 0.1f * sinf(t * 20.0f));
            Mat4 shellModel = Mat4::translate(p.position)
                            * Mat4::rotateY(-t * 3.0f)
                            * Mat4::rotateZ(t * 4.0f)
                            * Mat4::scale({shellSize, shellSize, shellSize});
            AABB shellBounds = {p.position - Vec3{shellSize,shellSize,shellSize},
                                p.position + Vec3{shellSize,shellSize,shellSize}};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, shellModel, shellBounds,
                             {0.2f, 0.5f, 1.0f, 0.4f});

            // Frost spiral trail — 6 trailing ice motes in a helix
            Vec3 vel = p.velocity;
            f32 spd = length(vel);
            if (spd > 0.01f) {
                Vec3 dir = vel * (1.0f / spd);
                for (u32 m = 0; m < 6; m++) {
                    f32 offset = m * 0.15f;
                    f32 angle = t * 10.0f + m * 1.05f;
                    Vec3 spiral = {sinf(angle) * 0.15f, cosf(angle) * 0.15f, 0};
                    Vec3 motePos = p.position - dir * (offset + 0.1f) + spiral;
                    f32 moteSize = coreSize * (0.3f - m * 0.04f);
                    if (moteSize < 0.02f) moteSize = 0.02f;
                    Mat4 moteModel = Mat4::translate(motePos)
                                   * Mat4::scale({moteSize, moteSize, moteSize});
                    AABB moteBounds = {motePos - Vec3{moteSize,moteSize,moteSize},
                                       motePos + Vec3{moteSize,moteSize,moteSize}};
                    f32 fade = 1.0f - m * 0.15f;
                    Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, moteModel, moteBounds,
                                     {0.4f * fade, 0.7f * fade, 1.0f * fade, 0.7f * fade});
                }
            }

        } else if (p.projFlags & PROJ_ORB_SHARD) {
            // Frozen Orb shard — elongated tumbling ice crystal with sparkle
            f32 t = p.lifetime;
            f32 shardW = p.radius * 1.2f;
            f32 shardH = p.radius * 3.0f;  // elongated
            Mat4 model = Mat4::translate(p.position)
                       * Mat4::rotateY(t * 25.0f)
                       * Mat4::rotateX(t * 15.0f)
                       * Mat4::scale({shardW, shardH, shardW});
            AABB bounds = {p.position - Vec3{shardH,shardH,shardH},
                           p.position + Vec3{shardH,shardH,shardH}};
            f32 sparkle = 0.7f + 0.3f * sinf(t * 40.0f);
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, model, bounds,
                             {0.6f * sparkle, 0.9f * sparkle, 1.0f, 0.9f});

        } else if (p.projFlags & PROJ_SPARK) {
            // Electric bolt — intense pulsing core + crackling arcs + trail
            f32 t = p.lifetime;
            f32 pulse = 0.5f + 0.5f * sinf(t * 30.0f); // more dramatic flicker

            // Bright white-blue core (larger, more visible)
            f32 coreSize = p.radius * 3.0f * pulse;
            Mat4 coreModel = Mat4::translate(p.position)
                           * Mat4::rotateY(t * 20.0f)
                           * Mat4::rotateX(t * 14.0f)
                           * Mat4::scale({coreSize, coreSize, coreSize});
            AABB coreBounds = {p.position - Vec3{coreSize,coreSize,coreSize},
                               p.position + Vec3{coreSize,coreSize,coreSize}};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, coreModel, coreBounds,
                             {0.9f, 0.95f, 1.0f, 1.0f});

            // Outer electric glow (brighter)
            f32 glowSize = p.radius * 5.0f;
            Mat4 glowModel = Mat4::translate(p.position)
                           * Mat4::rotateZ(t * 12.0f)
                           * Mat4::scale({glowSize, glowSize * 0.6f, glowSize});
            AABB glowBounds = {p.position - Vec3{glowSize,glowSize,glowSize},
                               p.position + Vec3{glowSize,glowSize,glowSize}};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, glowModel, glowBounds,
                             {0.4f, 0.6f, 1.0f, 0.6f});

            // 6 jagged electric arcs radiating outward (longer, more dramatic)
            for (u32 arc = 0; arc < 6; arc++) {
                f32 a = t * 15.0f + arc * (6.28318f / 6.0f);
                f32 jitter = sinf(t * 50.0f + arc * 7.0f) * 0.15f;
                f32 reach = 0.3f + jitter;
                Vec3 arcEnd = p.position + Vec3{sinf(a) * reach,
                                                 cosf(a * 1.3f) * reach * 0.6f,
                                                 cosf(a) * reach};
                Vec3 col = {0.5f + pulse * 0.5f, 0.7f + pulse * 0.3f, 1.0f};
                DebugDraw::line(p.position, arcEnd, col);
                // Fine sub-arc (half length, offset phase)
                Vec3 subEnd = p.position + Vec3{sinf(a + 0.5f) * reach * 0.5f,
                                                 cosf(a * 0.8f + 1.0f) * reach * 0.3f,
                                                 cosf(a + 0.5f) * reach * 0.5f};
                DebugDraw::line(p.position, subEnd, col * 0.6f);
            }

            // Electric trail behind (more segments, brighter)
            Vec3 vel = p.velocity;
            f32 spd = length(vel);
            if (spd > 0.01f) {
                Vec3 dir = vel * (1.0f / spd);
                for (u32 tr = 0; tr < 4; tr++) {
                    Vec3 trailPos = p.position - dir * (tr * 0.18f + 0.08f);
                    f32 trailSize = coreSize * (0.6f - tr * 0.12f);
                    Mat4 trailModel = Mat4::translate(trailPos)
                                    * Mat4::rotateY(-t * 18.0f + tr)
                                    * Mat4::scale({trailSize, trailSize, trailSize});
                    AABB trailBounds = {trailPos - Vec3{trailSize,trailSize,trailSize},
                                        trailPos + Vec3{trailSize,trailSize,trailSize}};
                    f32 fade = 0.8f - tr * 0.18f;
                    Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, trailModel, trailBounds,
                                     {0.3f * fade, 0.5f * fade, 1.0f * fade, fade});
                }
            }

        } else {
            if (p.meshId > 0 && p.meshId < m_meshDefCount) {
                Vec3 vel = p.velocity;
                f32 spd = length(vel);
                f32 flyYaw = (spd > 0.01f) ? atan2f(-vel.x, -vel.z) : 0.0f;

                // Check if this is an arrow or bolt (fly straight, no spin)
                bool isArrowOrBolt = (p.meshId == m_meshIdArrow || p.meshId == m_meshIdBolt);

                const AABB& mb = m_meshDefs[p.meshId].bounds;
                f32 maxDim = mb.max.y - mb.min.y;
                f32 mw = mb.max.x - mb.min.x;
                f32 md = mb.max.z - mb.min.z;
                if (mw > maxDim) maxDim = mw;
                if (md > maxDim) maxDim = md;

                Mat4 model;
                if (isArrowOrBolt) {
                    // Arrow/bolt: larger, aligned with velocity, tip forward, no spin
                    f32 projScale = (maxDim > 0.001f) ? (1.2f / maxDim) : 1.2f;
                    f32 flyPitch = (spd > 0.01f) ? asinf(vel.y / spd) : 0.0f;
                    model = Mat4::translate(p.position)
                          * Mat4::rotateY(flyYaw)
                          * Mat4::rotateX(-flyPitch)
                          * Mat4::scale({projScale, projScale, projScale});
                } else {
                    // Thrown weapon: spinning
                    f32 projScale = (maxDim > 0.001f) ? (0.4f / maxDim) : 0.4f;
                    f32 spinAngle = p.lifetime * 15.0f;
                    model = Mat4::translate(p.position)
                          * Mat4::rotateY(flyYaw)
                          * Mat4::rotateX(spinAngle)
                          * Mat4::scale({projScale, projScale, projScale});
                }

                AABB bounds = {p.position - Vec3{0.3f,0.3f,0.3f},
                               p.position + Vec3{0.3f,0.3f,0.3f}};
                Renderer::submit(m_basicShader, defaultTex, m_meshDefs[p.meshId].mesh, model, bounds,
                                 {0.8f, 0.8f, 0.8f, 1.0f});
            } else {
                // Magic wand / default projectile — glowing energy bolt with trail
                f32 t = p.lifetime;
                f32 pulse = 0.7f + 0.3f * sinf(t * 25.0f);
                bool isPlayer = p.fromPlayer;

                // Core bolt (bright)
                f32 coreSize = p.radius * 1.8f * pulse;
                Mat4 coreModel = Mat4::translate(p.position)
                               * Mat4::rotateY(t * 15.0f)
                               * Mat4::rotateX(t * 10.0f)
                               * Mat4::scale({coreSize, coreSize, coreSize});
                AABB coreBounds = {p.position - Vec3{coreSize,coreSize,coreSize},
                                   p.position + Vec3{coreSize,coreSize,coreSize}};
                // Void-tagged projectiles get purple tint (only basic attacks, not skills)
                bool isVoidProj = (p.projFlags & PROJ_VOID) != 0;
                Vec4 coreColor = isVoidProj
                    ? Vec4{0.6f * pulse, 0.15f, 0.9f, 1.0f}    // dark purple
                    : isPlayer
                    ? Vec4{1.0f, 0.8f * pulse, 0.3f, 1.0f}     // warm golden
                    : Vec4{0.9f * pulse, 0.2f, 1.0f, 1.0f};    // enemy purple
                Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, coreModel, coreBounds, coreColor);

                // Outer glow (larger, dimmer, different rotation)
                f32 glowSize = p.radius * 3.5f;
                Mat4 glowModel = Mat4::translate(p.position)
                               * Mat4::rotateZ(t * 8.0f)
                               * Mat4::rotateY(-t * 6.0f)
                               * Mat4::scale({glowSize, glowSize * 0.7f, glowSize});
                AABB glowBounds = {p.position - Vec3{glowSize,glowSize,glowSize},
                                   p.position + Vec3{glowSize,glowSize,glowSize}};
                Vec4 glowColor = isVoidProj
                    ? Vec4{0.4f, 0.05f, 0.7f, 0.4f}
                    : isPlayer
                    ? Vec4{1.0f, 0.4f, 0.05f, 0.35f}
                    : Vec4{0.5f, 0.1f, 0.8f, 0.35f};
                Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, glowModel, glowBounds, glowColor);

                // Trailing energy motes (3 fading behind)
                Vec3 vel = p.velocity;
                f32 spd = length(vel);
                if (spd > 0.01f) {
                    Vec3 dir = vel * (1.0f / spd);
                    for (u32 tr = 0; tr < 3; tr++) {
                        f32 offset = (tr + 1) * 0.12f;
                        Vec3 trailPos = p.position - dir * offset;
                        f32 trailSize = coreSize * (0.6f - tr * 0.15f);
                        if (trailSize < 0.01f) trailSize = 0.01f;
                        Mat4 trailModel = Mat4::translate(trailPos)
                                        * Mat4::rotateY(t * 12.0f + tr * 2.0f)
                                        * Mat4::scale({trailSize, trailSize, trailSize});
                        AABB trailBounds = {trailPos - Vec3{trailSize,trailSize,trailSize},
                                            trailPos + Vec3{trailSize,trailSize,trailSize}};
                        f32 fade = 0.6f - tr * 0.18f;
                        Vec4 tc = isPlayer
                            ? Vec4{1.0f * fade, 0.5f * fade, 0.1f * fade, fade}
                            : Vec4{0.6f * fade, 0.1f * fade, 0.9f * fade, fade};
                        Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, trailModel, trailBounds, tc);
                    }
                }
            }
        }
    }

    // --- Floor door — prominent glowing portal to next level ---
    if (m_level.floorDoorActive) {
        Vec3 dp = m_level.floorDoorPos;
        f32 t = static_cast<f32>(m_statsTimer);
        f32 pulse = 0.5f + 0.5f * sinf(t * 3.0f);
        f32 fastPulse = 0.5f + 0.5f * sinf(t * 8.0f);

        // Tall vertical beam (bright green, visible from far away)
        Vec3 beamCol = {0.1f, 0.9f * pulse, 0.2f};
        for (f32 ox = -0.08f; ox <= 0.08f; ox += 0.04f) {
            DebugDraw::line(dp + Vec3{ox, 0, 0}, dp + Vec3{ox, 4.0f, 0}, beamCol);
            DebugDraw::line(dp + Vec3{0, 0, ox}, dp + Vec3{0, 4.0f, ox}, beamCol);
        }

        // Spinning portal ring at waist height
        f32 ringY = dp.y + 1.0f;
        f32 ringR = 0.6f + fastPulse * 0.1f;
        Vec3 ringCol = {0.3f * pulse, 1.0f * pulse, 0.4f * pulse};
        for (u32 s = 0; s < 12; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / 12.0f) + t * 2.0f;
            f32 a1 = a0 + (6.28318f / 12.0f);
            Vec3 p0 = dp + Vec3{cosf(a0) * ringR, ringY - dp.y, sinf(a0) * ringR};
            Vec3 p1 = dp + Vec3{cosf(a1) * ringR, ringY - dp.y, sinf(a1) * ringR};
            DebugDraw::line(p0, p1, ringCol);
        }

        // Second ring at head height
        f32 ringY2 = dp.y + 2.0f;
        for (u32 s = 0; s < 12; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / 12.0f) - t * 1.5f;
            f32 a1 = a0 + (6.28318f / 12.0f);
            Vec3 p0 = dp + Vec3{cosf(a0) * ringR * 0.7f, ringY2 - dp.y, sinf(a0) * ringR * 0.7f};
            Vec3 p1 = dp + Vec3{cosf(a1) * ringR * 0.7f, ringY2 - dp.y, sinf(a1) * ringR * 0.7f};
            DebugDraw::line(p0, p1, {0.2f, 0.8f * pulse, 0.3f});
        }

        // Ground circle (large, static)
        for (u32 s = 0; s < 16; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / 16.0f);
            f32 a1 = a0 + (6.28318f / 16.0f);
            Vec3 p0 = dp + Vec3{cosf(a0) * 0.8f, 0.02f, sinf(a0) * 0.8f};
            Vec3 p1 = dp + Vec3{cosf(a1) * 0.8f, 0.02f, sinf(a1) * 0.8f};
            DebugDraw::line(p0, p1, {0.15f, 0.5f, 0.2f});
        }

        // Stairway steps descending
        for (u32 step = 0; step < 4; step++) {
            f32 s = static_cast<f32>(step);
            f32 y = dp.y - s * 0.2f;
            f32 z = dp.z + s * 0.3f;
            f32 w = 0.45f;
            Vec3 stepCol = {0.35f, 0.3f, 0.2f};
            DebugDraw::line({dp.x - w, y, z}, {dp.x + w, y, z}, stepCol);
            DebugDraw::line({dp.x - w, y, z + 0.25f}, {dp.x + w, y, z + 0.25f}, stepCol);
            DebugDraw::line({dp.x - w, y, z}, {dp.x - w, y, z + 0.25f}, stepCol);
            DebugDraw::line({dp.x + w, y, z}, {dp.x + w, y, z + 0.25f}, stepCol);
        }
    }

    // --- Fire AoE effects (molotov splash) ---
    for (u32 i = 0; i < MAX_FIRE_FX; i++) {
        if (!m_fx.fireFX[i].active) continue;
        const FireFX& fx = m_fx.fireFX[i];
        f32 t = 1.0f - fx.timer; // 0→1 over lifetime
        f32 alpha = fx.timer;    // fades out
        f32 r = fx.radius * (0.3f + t * 0.7f); // expands outward

        // Draw radiating lines from center (fire burst pattern)
        static constexpr u32 FIRE_RAYS = 12;
        for (u32 ray = 0; ray < FIRE_RAYS; ray++) {
            f32 angle = static_cast<f32>(ray) * (6.28318f / FIRE_RAYS) + t * 2.0f;
            f32 dx = cosf(angle) * r;
            f32 dz = sinf(angle) * r;
            // Flame color: orange core, red tips
            Vec3 col = {1.0f * alpha, (0.4f + 0.3f * sinf(angle * 3.0f)) * alpha, 0.1f * alpha};
            // Ground-level radiating lines
            DebugDraw::line(fx.pos, fx.pos + Vec3{dx, 0.1f, dz}, col);
            // Upward flame wisps
            f32 h = 0.5f + sinf(angle * 2.0f + t * 8.0f) * 0.3f;
            DebugDraw::line(fx.pos + Vec3{dx * 0.5f, 0, dz * 0.5f},
                            fx.pos + Vec3{dx * 0.3f, h * alpha, dz * 0.3f},
                            {1.0f * alpha, 0.6f * alpha, 0.0f});
        }
    }

    // --- Hitscan impact sparks — dust/blood burst at hit position ---
    for (u32 i = 0; i < MAX_IMPACT_FX; i++) {
        if (!m_fx.impactFX[i].active) continue;
        const ImpactFX& fx = m_fx.impactFX[i];
        f32 alpha = fx.timer / 0.3f; // fade over 0.3s lifetime
        f32 t = 1.0f - alpha; // 0→1 over lifetime

        // Color: orange sparks on walls, red on entities
        Vec3 col = fx.isEntity ? Vec3{1.0f, 0.2f, 0.1f} : Vec3{0.8f, 0.7f, 0.5f};

        // Small expanding spark burst — 8 rays radiating from hit point
        for (u32 ray = 0; ray < 8; ray++) {
            f32 a = ray * (6.28318f / 8.0f) + t * 3.0f;
            f32 spread = 0.05f + t * 0.15f; // expands outward
            Vec3 dir = {cosf(a) * spread, sinf(a * 1.5f) * spread * 0.5f + t * 0.08f,
                        sinf(a) * spread};
            // Offset along hit normal so sparks fly outward from surface
            Vec3 sparkEnd = fx.pos + fx.normal * (0.02f + t * 0.05f) + dir;
            DebugDraw::line(fx.pos + fx.normal * 0.02f, sparkEnd,
                            col * alpha);
        }

        // Central bright flash (small cube that fades quickly)
        if (t < 0.5f) {
            f32 flashSize = 0.04f * (1.0f - t * 2.0f);
            Mat4 flashModel = Mat4::translate(fx.pos + fx.normal * 0.03f)
                            * Mat4::scale({flashSize, flashSize, flashSize});
            AABB flashBounds = {fx.pos - Vec3{0.1f,0.1f,0.1f},
                                fx.pos + Vec3{0.1f,0.1f,0.1f}};
            Vec3 flashCol = fx.isEntity ? Vec3{1.0f, 0.5f, 0.3f} : Vec3{1.0f, 0.9f, 0.6f};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, flashModel, flashBounds,
                             {flashCol.x, flashCol.y, flashCol.z, alpha});
        }
    }

    // --- Chain Lightning — thick jagged electric arcs between bounce targets ---
    for (u32 i = 0; i < MAX_CHAIN_FX; i++) {
        if (!m_fx.chainFX[i].active) continue;
        const ChainFX& cfx = m_fx.chainFX[i];
        f32 alpha = cfx.timer / 0.8f; // fade over 0.8s lifetime

        for (u8 seg = 0; seg + 1 < cfx.pointCount; seg++) {
            Vec3 a = cfx.points[seg];
            Vec3 b = cfx.points[seg + 1];
            Vec3 diff = b - a;
            f32 segLen = length(diff);

            // Core arc — bright white-blue, drawn multiple times for thickness
            Vec3 coreCol = {0.6f * alpha, 0.8f * alpha, 1.0f * alpha};
            DebugDraw::line(a, b, coreCol);
            DebugDraw::line(a + Vec3{0, 0.02f, 0}, b + Vec3{0, 0.02f, 0}, coreCol);
            DebugDraw::line(a + Vec3{0.02f, 0, 0}, b + Vec3{0.02f, 0, 0}, coreCol);

            // 3 jagged sub-arcs per segment — each with different jitter phase
            Vec3 mid = (a + b) * 0.5f;
            Vec3 q1 = a + diff * 0.33f;
            Vec3 q3 = a + diff * 0.66f;

            for (u32 arc = 0; arc < 3; arc++) {
                f32 phase = cfx.timer * (40.0f + arc * 15.0f) + seg * 4.0f + arc * 2.1f;
                f32 jScale = 0.15f + arc * 0.08f;
                Vec3 j1 = {sinf(phase) * jScale, cosf(phase * 1.3f) * jScale * 0.8f,
                           cosf(phase * 0.9f) * jScale};
                Vec3 j2 = {cosf(phase * 1.1f) * jScale, sinf(phase * 0.7f) * jScale,
                           sinf(phase * 1.4f) * jScale * 0.7f};
                Vec3 j3 = {sinf(phase * 0.8f) * jScale * 0.6f, cosf(phase) * jScale * 0.5f,
                           cosf(phase * 1.2f) * jScale};

                f32 brightness = (0.5f - arc * 0.12f) * alpha;
                Vec3 arcCol = {0.3f * brightness, 0.5f * brightness, 1.0f * brightness};

                // Multi-segment jagged arc: a → q1+j1 → mid+j2 → q3+j3 → b
                DebugDraw::line(a, q1 + j1, arcCol);
                DebugDraw::line(q1 + j1, mid + j2, arcCol);
                DebugDraw::line(mid + j2, q3 + j3, arcCol);
                DebugDraw::line(q3 + j3, b, arcCol);
            }

            // Bright impact flash at each bounce point (except origin)
            if (seg > 0) {
                f32 flashSize = 0.1f + 0.05f * sinf(cfx.timer * 30.0f + seg);
                for (u32 ray = 0; ray < 6; ray++) {
                    f32 ra = ray * (6.28318f / 6.0f) + cfx.timer * 10.0f;
                    Vec3 rayEnd = a + Vec3{cosf(ra) * flashSize, sinf(ra * 1.5f) * flashSize * 0.5f,
                                           sinf(ra) * flashSize};
                    DebugDraw::line(a, rayEnd, {0.7f * alpha, 0.85f * alpha, 1.0f * alpha});
                }
            }
        }
    }

    // --- Scorch zones — persistent ground fire rings ---
    for (u32 i = 0; i < MAX_SCORCH; i++) {
        if (!m_fx.scorchZones[i].active) continue;
        const ScorchZone& sz = m_fx.scorchZones[i];
        f32 alpha = (sz.timer < 0.5f) ? sz.timer * 2.0f : 1.0f; // fade in last 0.5s
        f32 r = sz.radius;

        // Pulsing fire ring on the ground
        static constexpr u32 SCORCH_SEGS = 16;
        f32 pulse = 0.7f + 0.3f * sinf(sz.timer * 6.0f);
        for (u32 s = 0; s < SCORCH_SEGS; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / SCORCH_SEGS);
            f32 a1 = a0 + (6.28318f / SCORCH_SEGS);
            Vec3 p0 = sz.pos + Vec3{cosf(a0) * r, 0.05f, sinf(a0) * r};
            Vec3 p1 = sz.pos + Vec3{cosf(a1) * r, 0.05f, sinf(a1) * r};
            DebugDraw::line(p0, p1, {1.0f * alpha * pulse, 0.4f * alpha * pulse, 0.0f});
        }
        // Inner ring
        f32 rInner = r * 0.5f;
        for (u32 s = 0; s < SCORCH_SEGS; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / SCORCH_SEGS) + sz.timer * 2.0f;
            f32 a1 = a0 + (6.28318f / SCORCH_SEGS);
            Vec3 p0 = sz.pos + Vec3{cosf(a0) * rInner, 0.08f, sinf(a0) * rInner};
            Vec3 p1 = sz.pos + Vec3{cosf(a1) * rInner, 0.08f, sinf(a1) * rInner};
            DebugDraw::line(p0, p1, {1.0f * alpha, 0.6f * alpha, 0.1f * alpha});
        }
        // Small flame wisps rising from the zone
        for (u32 w = 0; w < 4; w++) {
            f32 angle = sz.timer * 3.0f + w * 1.57f;
            f32 wr = r * 0.6f;
            Vec3 base = sz.pos + Vec3{cosf(angle) * wr, 0.05f, sinf(angle) * wr};
            f32 h = 0.3f + 0.2f * sinf(sz.timer * 8.0f + w);
            DebugDraw::line(base, base + Vec3{0, h, 0},
                            {1.0f * alpha, 0.5f * alpha, 0.0f});
        }
    }

    // --- Blood Nova — expanding blood tendrils + shockwave rings ---
    for (u32 i = 0; i < MAX_NOVA_FX; i++) {
        if (!m_fx.novaFX[i].active) continue;
        const NovaFX& nfx = m_fx.novaFX[i];
        f32 t = 1.0f - nfx.timer / 0.6f; // 0→1 over lifetime
        f32 alpha = nfx.timer / 0.6f;
        f32 r = nfx.maxRadius * t;

        // Outer shockwave ring (expanding)
        static constexpr u32 NOVA_SEGS = 24;
        for (u32 s = 0; s < NOVA_SEGS; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / NOVA_SEGS);
            f32 a1 = a0 + (6.28318f / NOVA_SEGS);
            Vec3 p0 = nfx.pos + Vec3{cosf(a0) * r, 0.1f, sinf(a0) * r};
            Vec3 p1 = nfx.pos + Vec3{cosf(a1) * r, 0.1f, sinf(a1) * r};
            DebugDraw::line(p0, p1, nfx.color * alpha);
        }

        // Inner ring (smaller, brighter)
        f32 r2 = r * 0.6f;
        for (u32 s = 0; s < NOVA_SEGS; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / NOVA_SEGS) + t * 3.0f;
            f32 a1 = a0 + (6.28318f / NOVA_SEGS);
            Vec3 p0 = nfx.pos + Vec3{cosf(a0) * r2, 0.3f, sinf(a0) * r2};
            Vec3 p1 = nfx.pos + Vec3{cosf(a1) * r2, 0.3f, sinf(a1) * r2};
            DebugDraw::line(p0, p1, nfx.color * alpha * 1.2f);
        }

        // Blood tendrils radiating outward with wavy motion
        for (u32 tendril = 0; tendril < 8; tendril++) {
            f32 baseAngle = tendril * (6.28318f / 8.0f);
            Vec3 prev = nfx.pos + Vec3{0, 0.2f, 0};
            for (f32 d = 0.2f; d < r; d += 0.2f) {
                f32 wave = sinf(d * 6.0f + t * 10.0f + tendril) * 0.15f;
                f32 a = baseAngle + wave;
                f32 h = 0.2f + sinf(d * 3.0f) * 0.3f * alpha;
                Vec3 cur = nfx.pos + Vec3{cosf(a) * d, h, sinf(a) * d};
                f32 fade = alpha * (1.0f - d / nfx.maxRadius);
                DebugDraw::line(prev, cur, nfx.color * fade);
                prev = cur;
            }
        }
    }

    // --- Phase Dash — ghostly afterimage trail with energy wisps ---
    for (u32 i = 0; i < MAX_DASH_FX; i++) {
        if (!m_fx.dashFX[i].active) continue;
        const DashFX& dfx = m_fx.dashFX[i];
        f32 alpha = dfx.timer / 0.5f;
        Vec3 dir = dfx.end - dfx.start;
        f32 len = length(dir);
        if (len < 0.1f) continue;
        Vec3 step = dir * (1.0f / len);
        Vec3 perp = {-step.z, 0, step.x};

        // Central energy beam (thick, bright)
        for (f32 h = 0.1f; h < 1.8f; h += 0.4f) {
            Vec3 s0 = dfx.start + Vec3{0, h, 0};
            Vec3 s1 = dfx.end + Vec3{0, h, 0};
            f32 hAlpha = alpha * (1.0f - h / 2.0f);
            DebugDraw::line(s0, s1, {0.2f * hAlpha, 0.5f * hAlpha, 1.0f * hAlpha});
        }

        // Swirling energy wisps along the corridor
        for (f32 d = 0; d < len; d += 0.15f) {
            f32 t2 = d + dfx.timer * 8.0f;
            f32 wave = sinf(t2 * 4.0f) * 0.25f;
            Vec3 p = dfx.start + step * d + Vec3{0, 0.8f, 0};
            Vec3 wispA = p + perp * (0.35f + wave);
            Vec3 wispB = p - perp * (0.35f + wave);
            f32 flicker = 0.5f + 0.5f * sinf(t2 * 12.0f);
            f32 fade = alpha * flicker * (1.0f - d / len);
            DebugDraw::line(p + Vec3{0, wave * 0.5f, 0}, wispA,
                            {0.3f * fade, 0.6f * fade, 1.0f * fade});
            DebugDraw::line(p + Vec3{0, -wave * 0.5f, 0}, wispB,
                            {0.2f * fade, 0.4f * fade, 0.9f * fade});
        }
    }

    // --- Beam Trails (Marksman) — bright fading hitscan lines ---
    for (u32 i = 0; i < MAX_BEAM_FX; i++) {
        if (!m_fx.beamFX[i].active) continue;
        const BeamFX& bfx = m_fx.beamFX[i];
        f32 alpha = bfx.timer / 0.3f;
        if (alpha <= 0.0f) continue;
        Vec3 c = bfx.color * alpha;

        // Thick central beam
        DebugDraw::line(bfx.start, bfx.end, c);

        // Two thinner parallel beams for width (offset perpendicular)
        Vec3 dir = bfx.end - bfx.start;
        f32 len = length(dir);
        if (len > 0.1f) {
            Vec3 fwd = dir * (1.0f / len);
            Vec3 perp = {-fwd.z, 0.0f, fwd.x};
            Vec3 up = {0.0f, 1.0f, 0.0f};
            DebugDraw::line(bfx.start + perp * 0.03f, bfx.end + perp * 0.03f, c * 0.6f);
            DebugDraw::line(bfx.start - perp * 0.03f, bfx.end - perp * 0.03f, c * 0.6f);
            DebugDraw::line(bfx.start + up * 0.03f, bfx.end + up * 0.03f, c * 0.4f);
            DebugDraw::line(bfx.start - up * 0.03f, bfx.end - up * 0.03f, c * 0.4f);
        }
    }

    // --- Meteor Strike / Holy Pillar — descending pillar + pulsing rune circle ---
    {
        extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
        for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
            if (!s_meteors[i].active) continue;
            Vec3 mp = s_meteors[i].position;
            f32 mr = s_meteors[i].radius;
            f32 timer = s_meteors[i].timer;
            Vec3 col = s_meteors[i].color; // fire=orange, holy=gold
            bool isHoly = s_meteors[i].healsPlayer;
            f32 urgency = 1.0f - timer; // 0→1 as impact approaches
            if (urgency < 0.0f) urgency = 0.0f;
            if (urgency > 1.0f) urgency = 1.0f;
            f32 pulse = 0.5f + 0.5f * sinf(urgency * 30.0f);

            // Outer targeting rune circle (pulsing, accelerating)
            static constexpr u32 RUNE_SEGS = 20;
            for (u32 s = 0; s < RUNE_SEGS; s++) {
                f32 a0 = static_cast<f32>(s) * (6.28318f / RUNE_SEGS) + urgency * 4.0f;
                f32 a1 = a0 + (6.28318f / RUNE_SEGS);
                Vec3 p0 = mp + Vec3{cosf(a0) * mr, 0.05f, sinf(a0) * mr};
                Vec3 p1 = mp + Vec3{cosf(a1) * mr, 0.05f, sinf(a1) * mr};
                DebugDraw::line(p0, p1, col * pulse);
            }

            // Inner rune circle (counter-rotating, smaller)
            f32 innerR = mr * 0.5f;
            for (u32 s = 0; s < RUNE_SEGS; s++) {
                f32 a0 = static_cast<f32>(s) * (6.28318f / RUNE_SEGS) - urgency * 6.0f;
                f32 a1 = a0 + (6.28318f / RUNE_SEGS);
                Vec3 p0 = mp + Vec3{cosf(a0) * innerR, 0.08f, sinf(a0) * innerR};
                Vec3 p1 = mp + Vec3{cosf(a1) * innerR, 0.08f, sinf(a1) * innerR};
                DebugDraw::line(p0, p1, col * (pulse * 1.2f));
            }

            // Rune cross-lines (pentagram-like)
            for (u32 s = 0; s < 5; s++) {
                f32 a = s * (6.28318f / 5.0f) + urgency * 2.0f;
                Vec3 p0 = mp + Vec3{cosf(a) * mr, 0.06f, sinf(a) * mr};
                Vec3 p1 = mp + Vec3{cosf(a + 2.513f) * mr, 0.06f, sinf(a + 2.513f) * mr};
                DebugDraw::line(p0, p1, col * (0.8f * pulse));
            }

            // Falling pillar — descends from sky to impact point
            f32 meteorY = 8.0f * (1.0f - urgency);
            f32 rockSize = isHoly ? (0.15f + urgency * 0.1f) : (0.3f + urgency * 0.2f);
            Vec3 meteorPos = mp + Vec3{0, meteorY, 0};
            Mat4 meteorModel = Mat4::translate(meteorPos)
                             * Mat4::rotateY(urgency * 12.0f)
                             * Mat4::rotateX(urgency * 8.0f)
                             * Mat4::rotateZ(urgency * 5.0f)
                             * Mat4::scale({rockSize, rockSize, rockSize});
            AABB meteorBounds = {meteorPos - Vec3{rockSize,rockSize,rockSize},
                                 meteorPos + Vec3{rockSize,rockSize,rockSize}};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, meteorModel, meteorBounds,
                             {col.x, col.y * (0.5f + 0.5f * pulse), col.z, 1.0f});

            // Glow shell
            f32 glowSize = rockSize * 2.0f;
            Mat4 glowModel = Mat4::translate(meteorPos)
                           * Mat4::rotateY(-urgency * 10.0f)
                           * Mat4::scale({glowSize, glowSize, glowSize});
            AABB glowBounds = {meteorPos - Vec3{glowSize,glowSize,glowSize},
                               meteorPos + Vec3{glowSize,glowSize,glowSize}};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, glowModel, glowBounds,
                             {col.x, col.y * pulse, col.z * 0.5f, 0.35f});

            // Trail sparks behind pillar
            for (u32 trail = 0; trail < 8; trail++) {
                f32 tOff = trail * 0.15f;
                f32 ty = meteorY + 0.3f + tOff;
                f32 ta = urgency * 10.0f + trail * 1.2f;
                f32 tr = 0.1f + trail * 0.03f;
                Vec3 tp = mp + Vec3{cosf(ta) * tr, ty, sinf(ta) * tr};
                Vec3 tp2 = mp + Vec3{cosf(ta + 0.4f) * tr * 0.6f, ty + 0.15f, sinf(ta + 0.4f) * tr * 0.6f};
                f32 fade = 1.0f - trail * 0.1f;
                DebugDraw::line(tp, tp2, col * fade * pulse);
            }
        }
    }

    // Particle pool — rendered after all DebugDraw calls so particles composite on top
    ParticleSystem::render(m_particles, m_camera, m_unlitShader, m_cubeMesh,
                           m_particleBlobMatId, m_particleSparkMatId);
}

// ---------------------------------------------------------------------------
// renderWorldItems — dropped items with mesh normalization + rarity glow lines,
// plus remote player models (multiplayer only)
// ---------------------------------------------------------------------------
void Engine::renderWorldItems(u32 sw, u32 sh) {
    (void)sw; (void)sh; // sw/sh reserved for future use (e.g. distance-based culling)

    const Texture& defaultTex = MaterialSystem::get(0)->texture;

    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        const WorldItem& wi = m_worldItems.items[i];
        if (!wi.active) continue;

        Vec3 color = rarityColor(wi.item.rarity);

        // Snap item to floor level of its grid cell
        f32 floorY = 0.0f;
        u32 gx, gz;
        if (LevelGridSystem::worldToGrid(m_level.grid, wi.position, gx, gz) &&
            !LevelGridSystem::isSolid(m_level.grid, gx, gz)) {
            floorY = LevelGridSystem::getFloorHeight(m_level.grid, gx, gz);
        }

        // Hover bob just above the floor (globes float lower and use smaller scale)
        static constexpr f32 ITEM_SCALE = 1.4f;
        bool isGlobeItem = isGlobe(wi.item);
        f32 renderScale = isGlobeItem ? 0.4f : ITEM_SCALE; // globes are small orbs
        f32 bobY = sinf(wi.bobTimer * 3.0f) * 0.08f;
        Vec3 pos = {wi.position.x, floorY + renderScale * 0.5f + bobY, wi.position.z};

        // Non-weapon items spin slower (weapons tumble quickly; armor/accessories drift).
        // Spin speed is set before any slot-specific Y adjustments.
        bool isWeaponSlot = (wi.item.defId < m_itemDefCount &&
                             m_itemDefs[wi.item.defId].slot == ItemSlot::WEAPON);
        f32 spin = isWeaponSlot ? wi.bobTimer * 2.0f : wi.bobTimer * 0.8f;

        // Helmets float a touch higher so they appear to rest at head-level rather
        // than the same floor-hover as flat items like rings and boots.
        if (!isGlobeItem && wi.item.defId < m_itemDefCount &&
            m_itemDefs[wi.item.defId].slot == ItemSlot::HELMET) {
            pos.y += 0.15f * renderScale;
        }

        // Globes render as small colored cubes; regular items use their mesh
        const Mesh* itemMesh = &m_cubeMesh;
        Texture itemTex = defaultTex;
        Vec4 tint = {color.x, color.y, color.z, 1.0f};

        if (isGlobeItem) {
            // Globe: green with a touch of blue and red
            tint = {0.3f, 0.9f, 0.5f, 1.0f};
        } else if (wi.item.defId < m_itemDefCount) {
            // Use weapon-specific mesh and material if available
            const ItemDef& def = m_itemDefs[wi.item.defId];
            if (def.meshId > 0 && def.meshId < m_meshDefCount) {
                itemMesh = &m_meshDefs[def.meshId].mesh;
            }
            if (def.materialId > 0) {
                const Material* mat = MaterialSystem::get(def.materialId);
                if (mat) {
                    itemTex = mat->texture;
                    // Use the material's natural tint as base — higher rarity items
                    // get a subtle colored hue blended in instead of full color override
                    Vec3 baseTint = {mat->tint.x, mat->tint.y, mat->tint.z};
                    f32 hueStrength = 0.0f;
                    if (wi.item.rarity == Rarity::MAGIC)     hueStrength = 0.15f;
                    else if (wi.item.rarity == Rarity::RARE) hueStrength = 0.20f;
                    // Legendary handled separately below
                    tint = {baseTint.x * (1.0f - hueStrength) + color.x * hueStrength,
                            baseTint.y * (1.0f - hueStrength) + color.y * hueStrength,
                            baseTint.z * (1.0f - hueStrength) + color.z * hueStrength, 1.0f};
                }
            }
            // Legendary items override with glowing legendary material
            if (wi.item.rarity == Rarity::LEGENDARY) {
                // Cached legendary material IDs (avoid per-frame string lookups)
                static const u8 legIds[] = {
                    MaterialSystem::getIdByName("legendary_weapon"),  // WEAPON=0
                    MaterialSystem::getIdByName("legendary_shield"),  // OFFHAND=1
                    MaterialSystem::getIdByName("legendary_helm"),    // HELMET=2
                    MaterialSystem::getIdByName("legendary_armor"),   // ARMOR=3
                    MaterialSystem::getIdByName("legendary_boots"),   // BOOTS=4
                    MaterialSystem::getIdByName("legendary_ring"),    // RING=5
                };
                u8 slotIdx = static_cast<u8>(def.slot);
                if (slotIdx < 6) {
                    const Material* legMat = MaterialSystem::get(legIds[slotIdx]);
                    if (legMat && legIds[slotIdx] > 0) {
                        itemTex = legMat->texture;
                        tint = legMat->tint;
                    }
                }
            }
        }

        // Item mesh — normalize size so all items render at consistent visual scale
        Mat4 model;
        if (itemMesh != &m_cubeMesh && !isGlobeItem && wi.item.defId < m_itemDefCount) {
            const ItemDef& idef = m_itemDefs[wi.item.defId];
            if (idef.meshId > 0 && idef.meshId < m_meshDefCount) {
                // Scale mesh so its largest axis fills 0.6 units, then multiply by ITEM_SCALE
                const AABB& mb = m_meshDefs[idef.meshId].bounds;
                f32 maxDim = mb.max.y - mb.min.y;
                f32 mw = mb.max.x - mb.min.x;
                f32 md = mb.max.z - mb.min.z;
                if (mw > maxDim) maxDim = mw;
                if (md > maxDim) maxDim = md;
                f32 normScale = (maxDim > 0.001f) ? (0.6f / maxDim) : 1.0f;
                f32 finalScale = normScale * renderScale;
                Vec3 mc = {(mb.min.x + mb.max.x) * 0.5f,
                           (mb.min.y + mb.max.y) * 0.5f,
                           (mb.min.z + mb.max.z) * 0.5f};
                model = Mat4::translate(pos) * Mat4::rotateY(spin)
                      * Mat4::scale({finalScale, finalScale, finalScale})
                      * Mat4::translate({-mc.x, -mc.y, -mc.z});
            } else {
                model = Mat4::translate(pos) * Mat4::rotateY(spin) * Mat4::scale({renderScale, renderScale, renderScale});
            }
        } else {
            f32 cubeS = isGlobeItem ? renderScale : 0.3f;
            model = Mat4::translate(pos) * Mat4::rotateY(spin) * Mat4::scale({cubeS, cubeS, cubeS});
        }
        AABB bounds = {pos - Vec3{renderScale,renderScale,renderScale},
                       pos + Vec3{renderScale,renderScale,renderScale}};

        if (!isGlobeItem) {
            // Rarity disc — camera-facing billboard behind the item mesh, colored by rarity.
            // Legendary gets a larger, brighter disc; other rarities are subtler.
            Vec4 discColor;
            f32 discSize;
            discColor = {0.9f, 0.9f, 0.9f, 0.3f};  // safe init (white/common)
            discSize  = renderScale * 1.2f;
            switch (wi.item.rarity) {
                case Rarity::MAGIC:
                    discColor = {0.2f, 0.9f, 0.2f, 0.4f};   // green
                    break;
                case Rarity::RARE:
                    discColor = {0.2f, 0.4f, 1.0f, 0.4f};   // blue
                    break;
                case Rarity::LEGENDARY:
                    discColor = {1.0f, 0.8f, 0.2f, 0.5f};   // gold
                    discSize  = renderScale * 1.5f;
                    break;
                default: // COMMON
                    discColor = {0.9f, 0.9f, 0.9f, 0.3f};   // white
                    break;
            }

            // Build billboard matrix: right/up/look axes from camera + world up,
            // same pattern as the particle system (particles.cpp).
            // Billboard faces camera: local +Z points toward camera (negated forward)
            Vec3 bRight = m_camera.right;
            Vec3 bUp    = {0.0f, 1.0f, 0.0f};
            Vec3 bFwd   = m_camera.forward * -1.0f;  // toward camera
            Mat4 discMat = Mat4::identity();
            discMat.m[0]  = bRight.x * discSize; discMat.m[1]  = bRight.y * discSize; discMat.m[2]  = bRight.z * discSize;
            discMat.m[4]  = bUp.x    * discSize; discMat.m[5]  = bUp.y    * discSize; discMat.m[6]  = bUp.z    * discSize;
            discMat.m[8]  = bFwd.x   * discSize; discMat.m[9]  = bFwd.y   * discSize; discMat.m[10] = bFwd.z   * discSize;
            discMat.m[12] = pos.x; discMat.m[13] = pos.y; discMat.m[14] = pos.z;

            // Draw disc directly with raw GL — bypasses Renderer::submit which
            // is designed for opaque batched draws and doesn't respect blend state.
            Renderer::flush(); // clear any pending opaque draws first

            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);

            const Material* blobMat = MaterialSystem::get(m_particleBlobMatId);
            const Texture& blobTex = blobMat ? blobMat->texture : MaterialSystem::get(0)->texture;
            Mat4 discMvp = m_camera.viewProjection * discMat;

            glUseProgram(m_unlitShader.program);
            glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, discMvp.ptr());
            glUniform4f(m_unlitShader.loc_color, discColor.x, discColor.y, discColor.z, discColor.w);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, blobTex.handle);
            if (m_unlitShader.loc_texture0 >= 0)
                glUniform1i(m_unlitShader.loc_texture0, 0);

            glBindVertexArray(m_quadMesh.vao);
            glDrawElements(GL_TRIANGLES, m_quadMesh.indexCount, GL_UNSIGNED_INT, 0);

            glEnable(GL_CULL_FACE);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        Renderer::submit(m_unlitShader, itemTex, *itemMesh, model, bounds, tint);
    }

    // Other players (network multiplayer + split-screen co-op)
    {
        // Render network remote players
        if (m_netRole != NetRole::NONE) {
            for (u32 i = 0; i < MAX_PLAYERS; i++) {
                if (i == m_localPlayerIndex) continue;

                bool active = false;
                Vec3 pos;
                f32 yaw = 0.0f;

                if (m_netRole == NetRole::CLIENT) {
                    active = m_renderInterp.playerActive[i];
                    pos = m_renderInterp.playerPositions[i];
                    yaw = m_renderInterp.playerYaws[i];
                } else {
                    active = m_players[i].active;
                    pos = m_players[i].position;
                    yaw = m_players[i].yaw;
                }
                if (!active) continue;

                // Human model — scale mesh to match NPC height (1.8m)
                u8 humanMesh = findMeshByName("human");
                u8 humanMat = MaterialSystem::getIdByName("human_skin");
                f32 targetH = 1.8f; // same as NPC halfExtents.y * 2
                f32 meshH = (humanMesh > 0) ? (m_meshDefs[humanMesh].bounds.max.y - m_meshDefs[humanMesh].bounds.min.y) : 1.0f;
                f32 scale = (meshH > 0.001f) ? (targetH / meshH) : 1.0f;
                Mat4 model = Mat4::translate(pos)
                           * Mat4::rotateY(yaw)
                           * Mat4::scale({scale, scale, scale});
                AABB bounds = {pos - Vec3{0.35f, 0, 0.35f}, pos + Vec3{0.35f, 1.8f, 0.35f}};

                const Material* humanMatPtr = MaterialSystem::get(humanMat);
                Texture humanTex = humanMatPtr ? humanMatPtr->texture : defaultTex;
                Vec4 humanTint = humanMatPtr ? humanMatPtr->tint : Vec4{1,1,1,1};

                if (humanMesh > 0 && m_meshDefs[humanMesh].mesh.vao) {
                    Renderer::submit(m_basicShader, humanTex, m_meshDefs[humanMesh].mesh,
                                     model, bounds, humanTint);
                } else {
                    Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, model, bounds,
                                     {0.5f, 0.5f, 0.5f, 1.0f});
                }

                // Render equipped weapon in hand
                const ItemInstance& wpn = m_inventories[i].equipped[static_cast<u32>(ItemSlot::WEAPON)];
                if (!isItemEmpty(wpn) && wpn.defId < m_itemDefCount) {
                    u8 wpnMeshId = m_itemDefs[wpn.defId].meshId;
                    u8 wpnMatId  = m_itemDefs[wpn.defId].materialId;
                    if (wpnMeshId > 0 && m_meshDefs[wpnMeshId].mesh.vao) {
                        Vec3 right = {-sinf(yaw + 1.57f), 0, -cosf(yaw + 1.57f)};
                        Vec3 fwd   = {-sinf(yaw), 0, -cosf(yaw)};
                        Vec3 wpnPos = pos + Vec3{0, 0.8f, 0} + right * 0.35f + fwd * 0.3f;
                        Mat4 wpnModel = Mat4::translate(wpnPos)
                                      * Mat4::rotateY(yaw)
                                      * Mat4::scale({0.4f, 0.4f, 0.4f});
                        AABB wpnBounds = {wpnPos - Vec3{0.2f,0.2f,0.2f}, wpnPos + Vec3{0.2f,0.2f,0.2f}};
                        const Material* wm = MaterialSystem::get(wpnMatId);
                        Renderer::submit(m_basicShader, wm ? wm->texture : defaultTex,
                                         m_meshDefs[wpnMeshId].mesh, wpnModel, wpnBounds,
                                         wm ? wm->tint : Vec4{1,1,1,1});
                    }
                }
            }
        }

        // Split-screen co-op: render the other local player (not the current viewport's player)
        if (m_splitPlayerCount > 1) {
            u8 otherP = (m_localPlayerIndex == 0) ? 1 : 0;
            if (!m_playerDead[otherP]) {
                Vec3 pos = m_localPlayers[otherP].position;
                f32 yaw  = m_localPlayers[otherP].yaw;

                u8 humanMesh = findMeshByName("human");
                f32 targetH = 1.8f;
                f32 meshH = (humanMesh > 0) ? (m_meshDefs[humanMesh].bounds.max.y - m_meshDefs[humanMesh].bounds.min.y) : 1.0f;
                f32 scale = (meshH > 0.001f) ? (targetH / meshH) : 1.0f;
                Mat4 model = Mat4::translate(pos)
                           * Mat4::rotateY(yaw)
                           * Mat4::scale({scale, scale, scale});
                AABB bounds = {pos - Vec3{0.35f, 0, 0.35f}, pos + Vec3{0.35f, 1.8f, 0.35f}};

                u8 skinMat = MaterialSystem::getIdByName("human_skin");
                const Material* skinMatPtr = MaterialSystem::get(skinMat);
                Texture skinTex = skinMatPtr ? skinMatPtr->texture : defaultTex;
                // Tint by player slot (P1=greenish, P2=bluish)
                Vec4 skinTint = (otherP == 0) ? Vec4{0.7f, 1.0f, 0.7f, 1} : Vec4{0.7f, 0.7f, 1.0f, 1};

                if (humanMesh > 0 && m_meshDefs[humanMesh].mesh.vao) {
                    Renderer::submit(m_basicShader, skinTex, m_meshDefs[humanMesh].mesh,
                                     model, bounds, skinTint);
                } else {
                    Vec4 col = (otherP == 0) ? Vec4{0.2f,0.8f,0.2f,1} : Vec4{0.2f,0.5f,1,1};
                    Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, model, bounds, col);
                }

                // Render equipped weapon in hand
                const ItemInstance& wpn = m_inventories[otherP].equipped[static_cast<u32>(ItemSlot::WEAPON)];
                if (!isItemEmpty(wpn) && wpn.defId < m_itemDefCount) {
                    u8 wpnMeshId = m_itemDefs[wpn.defId].meshId;
                    u8 wpnMatId  = m_itemDefs[wpn.defId].materialId;
                    if (wpnMeshId > 0 && m_meshDefs[wpnMeshId].mesh.vao) {
                        Vec3 right = {-sinf(yaw + 1.57f), 0, -cosf(yaw + 1.57f)};
                        Vec3 fwd   = {-sinf(yaw), 0, -cosf(yaw)};
                        Vec3 wpnPos = pos + Vec3{0, 0.8f, 0} + right * 0.35f + fwd * 0.3f;
                        Mat4 wpnModel = Mat4::translate(wpnPos)
                                      * Mat4::rotateY(yaw)
                                      * Mat4::scale({0.4f, 0.4f, 0.4f});
                        AABB wpnBounds = {wpnPos - Vec3{0.2f,0.2f,0.2f}, wpnPos + Vec3{0.2f,0.2f,0.2f}};
                        const Material* wm = MaterialSystem::get(wpnMatId);
                        Renderer::submit(m_basicShader, wm ? wm->texture : defaultTex,
                                         m_meshDefs[wpnMeshId].mesh, wpnModel, wpnBounds,
                                         wm ? wm->tint : Vec4{1,1,1,1});
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// renderSpeechBubbles — world-to-screen projection for entity speech text,
// plus the floor-door interaction prompt when the player is in range
// ---------------------------------------------------------------------------
void Engine::renderSpeechBubbles(u32 sw, u32 sh) {
    // Uses the render entity pool (client uses interpolated snapshot, SP uses live pool)
    const EntityPool& speechPool = (m_netRole == NetRole::CLIENT) ? m_renderInterp.entities : m_entities;
    for (u32 a = 0; a < speechPool.activeCount; a++) {
        u32 idx = speechPool.activeList[a];
        const Entity& e = speechPool.entities[idx];
        if (!e.speechText || e.speechTimer <= 0.0f) continue;

        // Project a point above the entity's head into clip space.
        // The mesh is rendered with feet at (position.y - halfExtents.y) and the
        // top at (position.y + halfExtents.y), so the bubble sits 0.5m above that.
        f32 topOfHead = e.position.y + e.halfExtents.y;
        Vec3 headPos = {e.position.x, topOfHead + 0.5f, e.position.z};

        // Manual column-major Mat4 * Vec4 (no operator overload assumed)
        const f32* vp = m_camera.viewProjection.m;
        f32 cx = vp[0]*headPos.x + vp[4]*headPos.y + vp[8]*headPos.z  + vp[12];
        f32 cy = vp[1]*headPos.x + vp[5]*headPos.y + vp[9]*headPos.z  + vp[13];
        f32 cw = vp[3]*headPos.x + vp[7]*headPos.y + vp[11]*headPos.z + vp[15];

        if (cw <= 0.01f) continue; // behind the camera

        // NDC to pixel screen coords (y is flipped: NDC +1 = screen top)
        f32 ndcX = cx / cw;
        f32 ndcY = cy / cw;
        f32 screenX = (ndcX + 1.0f) * 0.5f * static_cast<f32>(sw);
        f32 screenY = (1.0f - ndcY) * 0.5f * static_cast<f32>(sh);

        // Cull bubbles that are well off-screen
        if (screenX < -100.0f || screenX > static_cast<f32>(sw) + 100.0f) continue;
        if (screenY < -50.0f  || screenY > static_cast<f32>(sh) + 50.0f)  continue;

        // drawSpeechBubble places text starting at y going downward, so shift
        // the screen position up by the bubble height so it appears ABOVE the head
        f32 textH = FontSystem::textHeight(1);
        f32 bubbleH = textH + 8.0f + 6.0f; // text + padding + triangle
        screenY -= bubbleH;

        // Fade alpha in the last second of the timer
        f32 alpha = (e.speechTimer < 1.0f) ? e.speechTimer : 1.0f;

        // Green for allies, red for hostile entities
        Vec3 textColor = (e.flags & ENT_FRIENDLY)
            ? Vec3{0.4f, 1.0f, 0.5f}   // ally green
            : Vec3{1.0f, 0.4f, 0.4f};  // enemy red

        HUD::drawSpeechBubble(sw, sh, screenX, screenY, e.speechText, textColor, alpha);
    }

    // Floor door interaction prompt — shown when player is within trigger range
    if (m_level.floorDoorActive && m_gameState == GameState::IN_GAME) {
        Vec3 toDoor = m_level.floorDoorPos - m_localPlayer.position;
        if (lengthSq(toDoor) < 4.0f) {
            char doorStr[32];
            std::snprintf(doorStr, sizeof(doorStr), "Descend to Floor %u", m_level.currentFloor + 1);
            f32 textW = FontSystem::textWidth(doorStr, 1);
            f32 totalW = 22.0f + textW;
            f32 cx = (static_cast<f32>(sw) - totalW) * 0.5f;
            f32 cy = static_cast<f32>(sh) * 0.4f;
            HUD::drawKeySymbol(sw, sh, cx, cy - 2.0f, Input::isGamepadConnected(0) ? "X" : "E", true);
            FontSystem::drawText(sw, sh, cx + 22.0f, cy, doorStr, {0.3f, 1.0f, 0.4f}, 1);
        }
    }

    // Item pickup prompt — show item name in rarity color when aiming at a nearby item
    {
        f32 bestDot = 0.85f; // minimum alignment (must be roughly looking at it)
        f32 bestDist = 3.5f; // max pickup range
        const WorldItem* bestItem = nullptr;
        const ItemDef* bestDef = nullptr;

        Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
        Vec3 fwd = m_localPlayer.forward;

        for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
            const WorldItem& wi = m_worldItems.items[i];
            if (!wi.active) continue;
            if (isGlobe(wi.item)) continue;

            Vec3 toItem = wi.position - eyePos;
            f32 dist = length(toItem);
            if (dist > bestDist || dist < 0.1f) continue;

            Vec3 dir = toItem * (1.0f / dist);
            f32 dot = fwd.x * dir.x + fwd.y * dir.y + fwd.z * dir.z;
            if (dot > bestDot && wi.item.defId < m_itemDefCount) {
                bestDot = dot;
                bestDist = dist;
                bestItem = &wi;
                bestDef = &m_itemDefs[wi.item.defId];
            }
        }

        if (bestItem && bestDef) {
            Vec3 rColor = rarityColor(bestItem->item.rarity);
            // Build display text — legendaries append the skill name in brackets
            char hintBuf[96];
            if (bestItem->item.rarity == Rarity::LEGENDARY &&
                bestDef->legendarySkillId != SkillId::NONE) {
                const char* skillName = nullptr;
                for (u32 si = 0; si < m_skillDefCount; si++) {
                    if (m_skillDefs[si].id == bestDef->legendarySkillId) {
                        skillName = m_skillDefs[si].name; break;
                    }
                }
                if (skillName)
                    std::snprintf(hintBuf, sizeof(hintBuf), "%s [%s]", bestDef->name, skillName);
                else
                    std::snprintf(hintBuf, sizeof(hintBuf), "%s", bestDef->name);
            } else {
                std::snprintf(hintBuf, sizeof(hintBuf), "%s", bestDef->name);
            }
            f32 hintW = FontSystem::textWidth(hintBuf, 1);
            f32 totalW = 22.0f + hintW;
            f32 cx = (static_cast<f32>(sw) - totalW) * 0.5f;
            f32 cy = static_cast<f32>(sh) * 0.35f;
            HUD::drawKeySymbol(sw, sh, cx, cy - 2.0f, Input::isGamepadConnected(0) ? "X" : "E", true);
            FontSystem::drawText(sw, sh, cx + 22.0f, cy, hintBuf, rColor, 1);
        }
    }
}

// ---------------------------------------------------------------------------
// renderDamageNumbers — projects floating damage numbers from world space
// onto the screen, drifting upward and fading out over 1 second.
// Uses the same world-to-screen projection as renderSpeechBubbles.
// ---------------------------------------------------------------------------
void Engine::renderDamageNumbers(u32 sw, u32 sh) {
    const f32* vp = m_camera.viewProjection.m;
    for (u32 i = 0; i < MAX_DAMAGE_NUMBERS; i++) {
        const DamageNumber& dn = m_fx.damageNumbers[i];
        if (!dn.active) continue;

        // World-to-screen projection (same as speech bubbles)
        f32 cx = vp[0]*dn.position.x + vp[4]*dn.position.y + vp[8]*dn.position.z + vp[12];
        f32 cy = vp[1]*dn.position.x + vp[5]*dn.position.y + vp[9]*dn.position.z + vp[13];
        f32 cw = vp[3]*dn.position.x + vp[7]*dn.position.y + vp[11]*dn.position.z + vp[15];
        if (cw <= 0.01f) continue;

        f32 ndcX = cx / cw;
        f32 ndcY = cy / cw;
        f32 screenX = (ndcX + 1.0f) * 0.5f * static_cast<f32>(sw);
        f32 screenY = (1.0f - ndcY) * 0.5f * static_cast<f32>(sh);

        // Fade out in last 0.33s
        f32 alpha = fminf(dn.timer * 3.0f, 1.0f);

        // Color: white normal, yellow crit, green heal
        Vec3 color;
        if (dn.isHeal)      color = {0.2f * alpha, 1.0f * alpha, 0.3f * alpha};
        else if (dn.isCrit) color = {1.0f * alpha, 0.9f * alpha, 0.2f * alpha};
        else                color = {1.0f * alpha, 1.0f * alpha, 1.0f * alpha};

        f32 scale = dn.isCrit ? 3.0f : 2.0f;
        char buf[16];
        if (dn.isHeal)
            std::snprintf(buf, sizeof(buf), "+%.0f", dn.amount);
        else
            std::snprintf(buf, sizeof(buf), "%.0f", dn.amount);

        f32 textW = FontSystem::textWidth(buf, scale);
        FontSystem::drawText(sw, sh, screenX - textW * 0.5f, screenY, buf, color, scale);
    }
}

