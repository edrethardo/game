// engine_render_world.cpp — Engine::renderWorldItems, renderSpeechBubbles, renderDamageNumbers.
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
// renderWorldItems — dropped items with mesh normalization + rarity glow lines,
// plus remote player models (multiplayer only)
// ---------------------------------------------------------------------------
void Engine::renderWorldItems(u32 sw, u32 sh) {
    (void)sw; (void)sh;

    const Texture& defaultTex = MaterialSystem::get(0)->texture;

    // Collect disc billboard data so we can draw them all in one batched pass
    // instead of flushing per item (was 32 flushes → 1 flush).
    struct DiscData { Mat4 mvp; Vec4 color; };
    DiscData discs[MAX_WORLD_ITEMS];
    u32 discCount = 0;

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

        static constexpr f32 ITEM_SCALE = 1.4f;
        bool isGlobeItem = isGlobe(wi.item);
        f32 renderScale = isGlobeItem ? 0.4f : ITEM_SCALE;
        f32 bobY = sinf(wi.bobTimer * 3.0f) * 0.08f;
        Vec3 pos = {wi.position.x, floorY + renderScale * 0.5f + bobY, wi.position.z};

        bool isWeaponSlot = (wi.item.defId < m_itemDefCount &&
                             m_itemDefs[wi.item.defId].slot == ItemSlot::WEAPON);
        f32 spin = isWeaponSlot ? wi.bobTimer * 2.0f : wi.bobTimer * 0.8f;

        if (!isGlobeItem && wi.item.defId < m_itemDefCount &&
            m_itemDefs[wi.item.defId].slot == ItemSlot::HELMET) {
            pos.y += 0.15f * renderScale;
        }

        const Mesh* itemMesh = &m_cubeMesh;
        Texture itemTex = defaultTex;
        Vec4 tint = {color.x, color.y, color.z, 1.0f};

        if (isGlobeItem) {
            tint = {0.3f, 0.9f, 0.5f, 1.0f};
        } else if (wi.item.defId < m_itemDefCount) {
            const ItemDef& def = m_itemDefs[wi.item.defId];
            if (def.meshId > 0 && def.meshId < m_meshDefCount) {
                itemMesh = &m_meshDefs[def.meshId].mesh;
            }
            if (def.materialId > 0) {
                const Material* mat = MaterialSystem::get(def.materialId);
                if (mat) {
                    itemTex = mat->texture;
                    Vec3 baseTint = {mat->tint.x, mat->tint.y, mat->tint.z};
                    f32 hueStrength = 0.0f;
                    if (wi.item.rarity == Rarity::MAGIC)     hueStrength = 0.15f;
                    else if (wi.item.rarity == Rarity::RARE) hueStrength = 0.20f;
                    tint = {baseTint.x * (1.0f - hueStrength) + color.x * hueStrength,
                            baseTint.y * (1.0f - hueStrength) + color.y * hueStrength,
                            baseTint.z * (1.0f - hueStrength) + color.z * hueStrength, 1.0f};
                }
            }
            if (wi.item.rarity == Rarity::LEGENDARY) {
                static const u8 legIds[] = {
                    MaterialSystem::getIdByName("legendary_weapon"),
                    MaterialSystem::getIdByName("legendary_shield"),
                    MaterialSystem::getIdByName("legendary_helm"),
                    MaterialSystem::getIdByName("legendary_armor"),
                    MaterialSystem::getIdByName("legendary_boots"),
                    MaterialSystem::getIdByName("legendary_ring"),
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

        // Item mesh — normalize size
        Mat4 model;
        if (itemMesh != &m_cubeMesh && !isGlobeItem && wi.item.defId < m_itemDefCount) {
            const ItemDef& idef = m_itemDefs[wi.item.defId];
            if (idef.meshId > 0 && idef.meshId < m_meshDefCount) {
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

        // Collect disc billboard for batched rendering below
        if (!isGlobeItem && discCount < MAX_WORLD_ITEMS) {
            Vec4 discColor = {0.9f, 0.9f, 0.9f, 0.3f};
            f32 discSize = renderScale * 1.2f;
            switch (wi.item.rarity) {
                case Rarity::MAGIC:     discColor = {0.2f, 0.9f, 0.2f, 0.4f}; break;
                case Rarity::RARE:      discColor = {0.2f, 0.4f, 1.0f, 0.4f}; break;
                case Rarity::LEGENDARY: discColor = {1.0f, 0.8f, 0.2f, 0.5f}; discSize = renderScale * 1.5f; break;
                default: break;
            }
            Vec3 bRight = m_camera.right;
            Vec3 bUp    = {0.0f, 1.0f, 0.0f};
            Vec3 bFwd   = m_camera.forward * -1.0f;
            Mat4 discMat = Mat4::identity();
            discMat.m[0]  = bRight.x * discSize; discMat.m[1]  = bRight.y * discSize; discMat.m[2]  = bRight.z * discSize;
            discMat.m[4]  = bUp.x    * discSize; discMat.m[5]  = bUp.y    * discSize; discMat.m[6]  = bUp.z    * discSize;
            discMat.m[8]  = bFwd.x   * discSize; discMat.m[9]  = bFwd.y   * discSize; discMat.m[10] = bFwd.z   * discSize;
            discMat.m[12] = pos.x; discMat.m[13] = pos.y; discMat.m[14] = pos.z;
            discs[discCount++] = {m_camera.viewProjection * discMat, discColor};
        }

        Renderer::submit(m_unlitShader, itemTex, *itemMesh, model, bounds, tint);
    }

    // Batched rarity disc pass — one flush + one GL state change for all discs
    if (discCount > 0) {
        Renderer::flush();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);

        const Material* blobMat = MaterialSystem::get(m_particleBlobMatId);
        const Texture& blobTex = blobMat ? blobMat->texture : MaterialSystem::get(0)->texture;
        glUseProgram(m_unlitShader.program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, blobTex.handle);
        if (m_unlitShader.loc_texture0 >= 0)
            glUniform1i(m_unlitShader.loc_texture0, 0);
        glBindVertexArray(m_quadMesh.vao);

        for (u32 d = 0; d < discCount; d++) {
            glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, discs[d].mvp.ptr());
            glUniform4f(m_unlitShader.loc_color, discs[d].color.x, discs[d].color.y,
                        discs[d].color.z, discs[d].color.w);
            glDrawElements(GL_TRIANGLES, m_quadMesh.indexCount, GL_UNSIGNED_INT, 0);
        }

        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
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
                    // Skip dead remotes: isDead rides synced animFlags bit2 (see snapshot.cpp
                    // buildFromState + interpolateRemotePlayers' outAnimFlags). Without this a
                    // dead remote keeps rendering as an upright live figure until it respawns.
                    if (m_renderInterp.playerAnimFlags[i] & (1 << 2)) continue;
                } else {
                    active = m_players[i].active;
                    pos = m_players[i].position;
                    yaw = m_players[i].yaw;
                    // Host has the authoritative NetPlayer.isDead directly — same gate as CLIENT.
                    if (m_players[i].isDead) continue;
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

    // Wanderer: Exploit Weakness — pulsing "!" above ALL marked entities (AoE mark)
    if (m_playerClass == PlayerClass::WANDERER && m_localPlayer.markTimer > 0.0f) {
        const EntityPool& markPool = (m_netRole == NetRole::CLIENT) ? m_renderInterp.entities : m_entities;
        f32 pulse = 0.7f + 0.3f * sinf(m_statsTimer * 8.0f);
        f32 fade = (m_localPlayer.markTimer < 1.0f) ? m_localPlayer.markTimer : 1.0f;
        Vec3 markCol = {1.0f * pulse * fade, 0.4f * pulse * fade, 0.0f};
        f32 textH = FontSystem::textHeight(2);

        for (u32 a = 0; a < markPool.activeCount; a++) {
            u32 idx = markPool.activeList[a];
            const Entity& me = markPool.entities[idx];
            if (me.flags & ENT_DEAD) continue;
            if (me.markPreyTimer <= 0.0f) continue; // only show on marked entities

            f32 topOfHead = me.position.y + me.halfExtents.y;
            Vec3 headPos = {me.position.x, topOfHead + 0.4f, me.position.z};

            const f32* vp = m_camera.viewProjection.m;
            f32 cx = vp[0]*headPos.x + vp[4]*headPos.y + vp[8]*headPos.z  + vp[12];
            f32 cy = vp[1]*headPos.x + vp[5]*headPos.y + vp[9]*headPos.z  + vp[13];
            f32 cw = vp[3]*headPos.x + vp[7]*headPos.y + vp[11]*headPos.z + vp[15];

            if (cw > 0.01f) {
                f32 screenX = ((cx / cw) + 1.0f) * 0.5f * static_cast<f32>(sw);
                f32 screenY = (1.0f - (cy / cw)) * 0.5f * static_cast<f32>(sh);
                if (screenX >= -50.0f && screenX <= static_cast<f32>(sw) + 50.0f
                    && screenY >= -50.0f && screenY <= static_cast<f32>(sh) + 50.0f)
                {
                    FontSystem::drawText(sw, sh, screenX - 3.0f, screenY - textH, "!", markCol, 2);
                }
            }
        }
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
