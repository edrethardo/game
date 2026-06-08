// engine_render_viewmodel.cpp — Engine::renderViewmodel: first-person weapon/arm/shield rendering.
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
    // During a dodge roll the player moves at 8 m/s, which would slam the velocity-driven bob to
    // max and make the weapon flail. Suppress bob + sway and instead lean/tuck the weapon with the
    // roll (computed below) so it reads as part of the tumble.
    const DodgeState& ds = m_localPlayer.dodgeState;
    if (ds.rolling) bobR = 0.0f;

    f32 weaponAngle = m_viewmodelState.bobTimer * 5.5f; // match view bob frequency
    f32 bobX = bobR * 0.035f * sinf(weaponAngle);             // wide lateral swing
    f32 bobY = bobR * 0.025f * sinf(weaponAngle * 2.0f);      // 2× freq = figure-8 vertical
    // Add look sway — weapon trails behind camera rotation (skipped mid-roll for a clean lean)
    if (!ds.rolling) {
        bobX += m_viewmodelState.swayYaw;
        bobY += m_viewmodelState.swayPitch;
    }

    // Dodge lean/tuck: the weapon leans into the roll (matching the camera's roll/pitch blend)
    // and tucks toward the body, instead of staying pinned upright while the view tumbles.
    // Scaled well below the camera's 360° so the weapon leans rather than spinning in hand.
    f32 rp = ds.rollProg;
    f32 dodgeRoll  = rp * 0.5f * static_cast<f32>(ds.rollSign)  * ds.rollWeight;
    f32 dodgePitch = rp * 0.5f * static_cast<f32>(ds.pitchSign) * ds.pitchWeight;
    f32 dodgeTuck  = rp * 0.12f;

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
                case WeaponSubtype::CLEAVER:
                    // Heavy overhead chop — big pitch rotation + downward drop.
                    // The cleaver shares this so its blade and slash VFX both read as a chop.
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
                    // Committed diagonal slash: wide lateral sweep + downward bite, blade
                    // rolls into the cut and reaches forward so it reads as a real swing
                    // rather than a flick.
                    attackYaw   = -1.9f * swing;
                    attackPitch = -0.5f * swing;
                    attackRoll  = -0.5f * swing;
                    attackZ     = -0.28f * swing;
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
    WeaponState& vmWs = m_players[activeNetSlot()].weaponState; // local player's net slot
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
            } else if (def.weaponSubtype == WeaponSubtype::CLEAVER) {
                // Cleaver uses the sword mesh (blade runs along +Y; broad flats on ±Z, edges on
                // ±X). At the sword's holdYaw the FLAT faces forward, so an overhead chop hit
                // with the flat. Yaw the hold an extra 90° about Y so the EDGE (±X) faces forward
                // and leads the chop. Pure yaw — the blade stays vertical, so the chop animation
                // is unchanged.
                holdYaw = 0.4f + 1.5708f;
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

    // Dodge tuck — pull the weapon down and toward the body during the roll
    offset.y -= dodgeTuck;
    offset.z += dodgeTuck;

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
                     * Mat4::rotateZ(attackRoll + dodgeRoll)
                     * Mat4::rotateX(recoilPitch + attackPitch + holdPitch + dodgePitch)
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
