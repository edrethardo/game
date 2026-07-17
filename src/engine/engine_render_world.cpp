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
#include "game/shrine.h"
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
    // instead of flushing per item (was 32 flushes → 1 flush). Pet-item beacon beams ride the
    // same batch (same shader/texture/blend state): 3 corners × 2 crossed quads per pet item,
    // capped so the array stays stack-friendly — more than 8 concurrent 1-in-10000 drops is
    // not a case worth budgeting for, and the cap only trims beams, never the item itself.
    struct DiscData { Mat4 mvp; Vec4 color; };
    static constexpr u32 PET_BEAM_QUADS = 8 * 6;
    DiscData discs[MAX_WORLD_ITEMS + PET_BEAM_QUADS];
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

        static constexpr f32 ITEM_SCALE = 1.1f;   // trimmed from 1.4 — loot was cluttering the floor
        bool isGlobeItem = isGlobe(wi.item);
        bool isShard     = isSourceShard(wi.item);   // secret superboss key — render the crystal mesh
        bool isShrineObj = isShrine(wi.item);        // walk-up buff shrine — architecture, not loot
        bool isChestObj  = isChest(wi.item);         // closed treasure chest — the mimic's twin
        bool isStashObj  = isStash(wi.item);         // the town's account stash — oversized + gold
        bool isFixture   = isShrineObj || isChestObj || isStashObj;
        f32 renderScale = isGlobeItem ? 0.4f : (isShard ? 0.9f : (isShrineObj ? 1.6f : ITEM_SCALE));
        // Shrines and chests are FIXTURES: no bob, no spin, feet on the floor. Loot hovers and
        // turns to catch the eye; a fixture that did the same would read as a pickup — and a
        // bobbing "chest" next to a stone-still mimic would be a free mimic detector.
        f32 bobY = isFixture ? 0.0f : sinf(wi.bobTimer * 3.0f) * 0.08f;
        Vec3 pos = isFixture
                 ? Vec3{wi.position.x, floorY, wi.position.z}
                 : Vec3{wi.position.x, floorY + renderScale * 0.5f + bobY, wi.position.z};

        bool isWeaponSlot = (wi.item.defId < m_itemDefCount &&
                             m_itemDefs[wi.item.defId].slot == ItemSlot::WEAPON);
        f32 spin = isFixture ? 0.0f : (isWeaponSlot ? wi.bobTimer * 2.0f : wi.bobTimer * 0.8f);

        if (!isGlobeItem && wi.item.defId < m_itemDefCount &&
            m_itemDefs[wi.item.defId].slot == ItemSlot::HELMET) {
            pos.y += 0.15f * renderScale;
        }

        const Mesh* itemMesh = &m_cubeMesh;
        Texture itemTex = defaultTex;
        Vec4 tint = {color.x, color.y, color.z, 1.0f};

        if (isShrineObj) {
            // The three shrines are told apart by COLOUR — the player has no tooltip until they are
            // close enough to read the prompt, so the colour has to carry the meaning from range.
            if (m_shrineMeshId > 0 && m_shrineMeshId < m_meshDefCount)
                itemMesh = &m_meshDefs[m_shrineMeshId].mesh;
            // Same colours the minimap icon uses — single-sourced in Shrine::colorOf so the crystal
            // in the room and the diamond on the map can never disagree about which shrine this is.
            const Vec3 sc = Shrine::colorOf(Shrine::buffOf(wi.item));
            tint = {sc.x, sc.y, sc.z, 1.0f};
        } else if (isChestObj) {
            // The dormant mimic's EXACT presentation — chest mesh, default texture, the same
            // chest-brown the mimic tint branch uses in engine_render_entities.cpp. Any visual
            // difference between a real chest and a disguised mimic is a tell that defeats both.
            if (m_meshIdChest > 0 && m_meshIdChest < m_meshDefCount)
                itemMesh = &m_meshDefs[m_meshIdChest].mesh;
            tint = {0.6f, 0.4f, 0.2f, 1.0f};
        } else if (isStashObj) {
            // The stash wants the OPPOSITE of the mimic-twin rule: unmistakably special. Same
            // chest mesh, gold tint, drawn half again as large in the size branch below.
            if (m_meshIdChest > 0 && m_meshIdChest < m_meshDefCount)
                itemMesh = &m_meshDefs[m_meshIdChest].mesh;
            tint = {1.0f, 0.82f, 0.35f, 1.0f};
        } else if (isGlobeItem) {
            tint = {0.3f, 0.9f, 0.5f, 1.0f};
        } else if (isShard) {
            // Distinct faceted crystal in emissive void-cyan so it reads as "not loot".
            if (m_meshIdShard > 0 && m_meshIdShard < m_meshDefCount)
                itemMesh = &m_meshDefs[m_meshIdShard].mesh;
            const Material* sm = MaterialSystem::get(MaterialSystem::getIdByName("shard_glow"));
            if (sm) { itemTex = sm->texture; tint = {sm->tint.x, sm->tint.y, sm->tint.z, 1.0f}; }
            else    { tint = {0.45f, 0.95f, 1.0f, 1.0f}; }
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
        if (isShrineObj) {
            // Own branch, because the item-mesh path below is gated on `defId < m_itemDefCount` and a
            // shrine's SENTINEL defId is far outside the real item range — it fell straight through to
            // the 0.3-scale cube fallback, which is why shrines looked like a pebble on the floor.
            // gen_mesh's shrine has its origin at the FEET, so it just sits at floorY, unscaled.
            model = Mat4::translate(pos);
        } else if (isStashObj && itemMesh != &m_cubeMesh) {
            // Stash: chest silhouette scaled to 1.2 m tall (1.5x the mimic) — reads as furniture.
            const AABB& mb = m_meshDefs[m_meshIdChest].bounds;
            f32 meshH = mb.max.y - mb.min.y;
            f32 cs = (meshH > 0.001f) ? (1.2f / meshH) : 1.0f;
            Vec3 mc = {(mb.min.x + mb.max.x) * 0.5f, mb.min.y, (mb.min.z + mb.max.z) * 0.5f};
            model = Mat4::translate(pos) * Mat4::scale({cs, cs, cs})
                  * Mat4::translate({-mc.x, -mc.y, -mc.z});
        } else if (isChestObj && itemMesh != &m_cubeMesh) {
            // Own branch for the same sentinel-defId reason as the shrine. Sized to the dormant
            // mimic's exact silhouette — mesh scaled to 0.8 m tall (the mimic's halfExtents.y
            // 0.4 × 2), XZ-centered, feet on the floor, unrotated — so the two are twins.
            const AABB& mb = m_meshDefs[m_meshIdChest].bounds;
            f32 meshH = mb.max.y - mb.min.y;
            f32 cs = (meshH > 0.001f) ? (0.8f / meshH) : 1.0f;
            Vec3 mc = {(mb.min.x + mb.max.x) * 0.5f, mb.min.y, (mb.min.z + mb.max.z) * 0.5f};
            model = Mat4::translate(pos) * Mat4::scale({cs, cs, cs})
                  * Mat4::translate({-mc.x, -mc.y, -mc.z});
        } else if (itemMesh != &m_cubeMesh && !isGlobeItem && wi.item.defId < m_itemDefCount) {
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
            // Shards render their crystal mesh at full renderScale (like globes); plain item cubes use 0.3.
            f32 cubeS = (isGlobeItem || isShard) ? renderScale : 0.3f;
            model = Mat4::translate(pos) * Mat4::rotateY(spin) * Mat4::scale({cubeS, cubeS, cubeS});
        }
        AABB bounds = {pos - Vec3{renderScale,renderScale,renderScale},
                       pos + Vec3{renderScale,renderScale,renderScale}};

        // Collect disc billboard for batched rendering below. Skip globes + shards (not loot) AND
        // COMMON loot — a subtle rarity glow only for magic+ keeps the floor from being a light show.
        if (!isGlobeItem && !isShard && wi.item.rarity != Rarity::COMMON &&
            discCount < MAX_WORLD_ITEMS + PET_BEAM_QUADS) {
            Vec4 discColor = {0.2f, 0.9f, 0.2f, 0.28f};   // magic default (dimmed from 0.4)
            f32 discSize = renderScale * 0.9f;            // was 1.2
            switch (wi.item.rarity) {
                case Rarity::MAGIC:     discColor = {0.2f, 0.9f, 0.2f, 0.28f}; break;
                case Rarity::RARE:      discColor = {0.2f, 0.4f, 1.0f, 0.28f}; break;
                case Rarity::LEGENDARY: discColor = {1.0f, 0.8f, 0.2f, 0.35f}; discSize = renderScale * 1.2f; break;
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

        // Pet-consumable beacon: three light rays rising from the corners of a slowly turning
        // triangle around the drop (a 1-in-10000 item must be findable from across the room).
        // Rays wear the drop's RARITY color — near-white for the COMMON enemy minis, gold for
        // the goblin's legendary jackpot — so the beacon says "pet" AND "how special" at once.
        // Each ray is two crossed vertical quads (readable from every angle without
        // billboarding; the pass below disables culling), stretched from the blob's radial
        // gradient so the shaft is soft-edged and fades at both ends.
        if (wi.item.defId < m_itemDefCount && m_itemDefs[wi.item.defId].petSummon &&
            discCount + 6 <= MAX_WORLD_ITEMS + PET_BEAM_QUADS) {
            static constexpr f32 BEAM_RADIUS = 0.55f;  // triangle circumradius around the item
            static constexpr f32 BEAM_HEIGHT = 4.5f;   // tall enough to clear prop clutter
            static constexpr f32 BEAM_WIDTH  = 0.14f;
            const Vec4 beamColor = {color.x, color.y, color.z, 0.45f};   // `color` = rarityColor above
            for (u32 k = 0; k < 3; k++) {
                const f32 a = wi.bobTimer * 0.6f + static_cast<f32>(k) * 2.0943951f; // 120° apart
                const Vec3 base = {wi.position.x + cosf(a) * BEAM_RADIUS,
                                   floorY + BEAM_HEIGHT * 0.5f,
                                   wi.position.z + sinf(a) * BEAM_RADIUS};
                // Two crossed quads per corner, aligned to the world X/Z axes.
                for (u32 q = 0; q < 2; q++) {
                    const Vec3 right = (q == 0) ? Vec3{BEAM_WIDTH, 0, 0} : Vec3{0, 0, BEAM_WIDTH};
                    Mat4 beamMat = Mat4::identity();
                    beamMat.m[0]  = right.x; beamMat.m[1]  = 0.0f;        beamMat.m[2]  = right.z;
                    beamMat.m[4]  = 0.0f;    beamMat.m[5]  = BEAM_HEIGHT; beamMat.m[6]  = 0.0f;
                    beamMat.m[8]  = (q == 0) ? 0.0f : 1.0f;               // sane normal column
                    beamMat.m[9]  = 0.0f;
                    beamMat.m[10] = (q == 0) ? 1.0f : 0.0f;
                    beamMat.m[12] = base.x; beamMat.m[13] = base.y; beamMat.m[14] = base.z;
                    discs[discCount++] = {m_camera.viewProjection * beamMat, beamColor};
                }
            }
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
                // Skip the LOCAL player(s) — they're drawn by the viewport's own camera + the
                // split-screen "other local" block below. On a CLIENT the local lane lives at its
                // server-assigned net slot; on a HOST the local lanes are slots 0..count-1 (so a
                // host-couch partner at slot 1 renders once, via the split block — not twice here).
                if (m_netRole == NetRole::CLIENT) {
                    // Skip BOTH local lanes' net slots (online couch co-op) — each renders from its
                    // own predicted state / the split-screen "other local" block, not interp.
                    if (i == m_clientNetSlot[0] ||
                        (m_splitPlayerCount > 1 && i == m_clientNetSlot[1])) continue;
                } else if (i < m_splitPlayerCount) {
                    continue;
                }

                bool active = false;
                Vec3 pos;
                f32 yaw = 0.0f;
                // Per-class visual identity. On CLIENT we use the wire-replicated playerClass
                // (m_renderInterp.playerClass[i]); on HOST/SP we read NetPlayer.playerClass directly.
                // Without this every remote rendered as the default "human" mesh regardless of class.
                u8 classByte = 0;

                if (m_netRole == NetRole::CLIENT) {
                    active = m_renderInterp.playerActive[i];
                    pos = m_renderInterp.playerPositions[i];
                    yaw = m_renderInterp.playerYaws[i];
                    classByte = m_renderInterp.playerClass[i];
                    // Skip dead remotes: isDead rides synced animFlags bit2 (see snapshot.cpp
                    // buildFromState + interpolateRemotePlayers' outAnimFlags). Without this a
                    // dead remote keeps rendering as an upright live figure until it respawns.
                    if (m_renderInterp.playerAnimFlags[i] & (1 << 2)) continue;
                } else {
                    active = m_players[i].active;
                    pos = m_players[i].position;
                    yaw = m_players[i].yaw;
                    classByte = static_cast<u8>(m_players[i].playerClass);
                    // Host has the authoritative NetPlayer.isDead directly — same gate as CLIENT.
                    if (m_players[i].isDead) continue;
                }
                if (!active) continue;

                // Resolve per-class mesh + material. classByte is clamped on the wire side
                // (snapshot.cpp deserialize) and on join (engine.cpp onPlayerJoin) so the index
                // is in range here — but fall back to "human" if either lookup misses (an
                // unbuilt asset would otherwise render as the magenta default cube).
                if (classByte >= static_cast<u8>(PlayerClass::CLASS_COUNT)) classByte = 0;
                const ClassDef& cd = kClassDefs[classByte];
                u8 classMesh = findMeshByName(cd.meshName);
                u8 classMat  = MaterialSystem::getIdByName(cd.materialName);
                if (classMesh == 0) classMesh = findMeshByName("human");
                if (classMat  == 0) classMat  = MaterialSystem::getIdByName("human_skin");
                f32 targetH = 1.8f; // same as NPC halfExtents.y * 2
                f32 meshH = (classMesh > 0) ? (m_meshDefs[classMesh].bounds.max.y - m_meshDefs[classMesh].bounds.min.y) : 1.0f;
                f32 scale = (meshH > 0.001f) ? (targetH / meshH) : 1.0f;
                Mat4 model = Mat4::translate(pos)
                           * Mat4::rotateY(yaw)
                           * Mat4::scale({scale, scale, scale});
                AABB bounds = {pos - Vec3{0.35f, 0, 0.35f}, pos + Vec3{0.35f, 1.8f, 0.35f}};

                const Material* classMatPtr = MaterialSystem::get(classMat);
                Texture classTex = classMatPtr ? classMatPtr->texture : defaultTex;
                Vec4 classTint = classMatPtr ? classMatPtr->tint : Vec4{1,1,1,1};

                if (classMesh > 0 && m_meshDefs[classMesh].mesh.vao) {
                    Renderer::submit(m_basicShader, classTex, m_meshDefs[classMesh].mesh,
                                     model, bounds, classTint);
                } else {
                    Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, model, bounds,
                                     {0.5f, 0.5f, 0.5f, 1.0f});
                }

                // Render equipped weapon + armor overlays. CLIENT uses wire mesh ids from the
                // snapshot (no remote inventory); HOST uses the real inventory for correct
                // mesh + material. submitPlayerEquipment / submitPlayerEquipmentIds share
                // the same body-attach math so the visual result is consistent.
                if (m_netRole == NetRole::CLIENT) {
                    u8 anim = m_renderInterp.playerAnimFlags[i];
                    submitPlayerEquipmentIds(pos, yaw, scale, anim, classMesh,
                                            m_renderInterp.playerWeaponMeshId[i],
                                            m_renderInterp.playerArmorMeshId[i]);
                } else {
                    u8 anim = 0;
                    if (m_players[i].weaponState.cooldownTimer > 0.0f) anim |= 1;
                    if (m_players[i].weaponState.reloading)            anim |= 2;
                    submitPlayerEquipment(pos, yaw, scale, anim, classMesh, m_inventories[i]);
                }
            }
        }

        // Split-screen co-op: render the other local player (not the current viewport's player)
        if (m_splitPlayerCount > 1) {
            u8 otherP = (m_localPlayerIndex == 0) ? 1 : 0;
            if (!m_playerDead[otherP]) {
                Vec3 pos = m_localPlayers[otherP].position;
                f32 yaw  = m_localPlayers[otherP].yaw;

                // Per-class mesh for the other split-screen player. Class is stored per-lane in
                // m_playerClasses[otherP] (the lobby writes it before startGame). Same fallback
                // semantics as the network branch above.
                u8 classByte = static_cast<u8>(m_playerClasses[otherP]);
                if (classByte >= static_cast<u8>(PlayerClass::CLASS_COUNT)) classByte = 0;
                const ClassDef& cd = kClassDefs[classByte];
                u8 classMesh = findMeshByName(cd.meshName);
                u8 classMat  = MaterialSystem::getIdByName(cd.materialName);
                if (classMesh == 0) classMesh = findMeshByName("human");
                if (classMat  == 0) classMat  = MaterialSystem::getIdByName("human_skin");
                f32 targetH = 1.8f;
                f32 meshH = (classMesh > 0) ? (m_meshDefs[classMesh].bounds.max.y - m_meshDefs[classMesh].bounds.min.y) : 1.0f;
                f32 scale = (meshH > 0.001f) ? (targetH / meshH) : 1.0f;
                Mat4 model = Mat4::translate(pos)
                           * Mat4::rotateY(yaw)
                           * Mat4::scale({scale, scale, scale});
                AABB bounds = {pos - Vec3{0.35f, 0, 0.35f}, pos + Vec3{0.35f, 1.8f, 0.35f}};

                const Material* skinMatPtr = MaterialSystem::get(classMat);
                Texture skinTex = skinMatPtr ? skinMatPtr->texture : defaultTex;
                // Tint by player slot (P1=greenish, P2=bluish) — kept on top of the class mesh so
                // local players can still distinguish P1 from P2 at a glance even if they picked
                // visually similar classes.
                Vec4 skinTint = (otherP == 0) ? Vec4{0.7f, 1.0f, 0.7f, 1} : Vec4{0.7f, 0.7f, 1.0f, 1};

                if (classMesh > 0 && m_meshDefs[classMesh].mesh.vao) {
                    Renderer::submit(m_basicShader, skinTex, m_meshDefs[classMesh].mesh,
                                     model, bounds, skinTint);
                } else {
                    Vec4 col = (otherP == 0) ? Vec4{0.2f,0.8f,0.2f,1} : Vec4{0.2f,0.5f,1,1};
                    Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, model, bounds, col);
                }

                // Compute anim flags for the partner (same bits used by the network path:
                // bit0=attacking, bit1=reloading) so weapon thrust/drop reflects live state.
                // WeaponState lives in the NetPlayer at the same index as the local lane.
                u8 partnerAnim = 0;
                if (m_players[otherP].weaponState.cooldownTimer > 0.0f) partnerAnim |= 1;
                if (m_players[otherP].weaponState.reloading)             partnerAnim |= 2;

                // Draw weapon + all equipped armor overlays via the shared helper.
                submitPlayerEquipment(pos, yaw, scale, partnerAnim, classMesh, m_inventories[otherP]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// submitPlayerEquipment — renders the equipped weapon + 4 armor-slot overlays
// on a 3rd-person body at (pos, yaw, scale). Called for the split-screen partner
// and (in later tasks) remote players and the character-inspect screen.
// ---------------------------------------------------------------------------
// Return the measured body-part regions for a body mesh; if unmeasured (non-player body or
// missing-asset fallback), synthesize coarse regions from the mesh's overall AABB so armor still
// fits something sensible. Regions are in body-LOCAL space (pre body-scale).
BodyRegions Engine::bodyRegionsFor(u8 bodyMeshId) const {
    if (bodyMeshId < MAX_MESH_DEFS && m_bodyRegions[bodyMeshId].valid)
        return m_bodyRegions[bodyMeshId];
    BodyRegions r;
    AABB b = (bodyMeshId > 0 && bodyMeshId < m_meshDefCount)
                 ? m_meshDefs[bodyMeshId].bounds
                 : AABB{{-0.3f, 0.0f, -0.3f}, {0.3f, 1.8f, 0.3f}};
    f32 base = b.min.y, H = b.max.y - b.min.y;
    auto band = [&](f32 lo, f32 hi) {
        return AABB{{b.min.x, base + lo * H, b.min.z}, {b.max.x, base + hi * H, b.max.z}};
    };
    r.head  = band(0.80f, 1.0f);  r.headValid  = true;
    r.torso = band(0.42f, 0.68f); r.torsoValid = true;
    r.feet  = band(0.0f,  0.16f); r.feetValid  = true;
    r.shoulderHalfW = fmaxf(fabsf(b.min.x), fabsf(b.max.x));
    f32 reach = r.shoulderHalfW;
    r.handL = {{b.min.x, base + 0.10f * H, b.min.z}, {-0.45f * reach, base + 0.32f * H, b.max.z}};
    r.handR = {{0.45f * reach, base + 0.10f * H, b.min.z}, {b.max.x, base + 0.32f * H, b.max.z}};
    r.handsValid = true;
    r.valid = true;
    return r;
}

// Single armor-fit transform: scale an armor mesh's local AABB so it exactly fills a target box
// (center + per-axis half-extents, body-local), then apply the body's pos/yaw/scale. One function
// serves every slot — the per-slot box is computed from measured landmarks + the spec table, which
// is where all the meaningful tuning lives.
Mat4 Engine::fitMeshToBox(u8 armorMesh, const Vec3& center, const Vec3& half,
                          const Vec3& pos, f32 yaw, f32 bodyScale) const {
    const AABB& ab = m_meshDefs[armorMesh].bounds;
    Vec3 aCenter = (ab.min + ab.max) * 0.5f;
    Vec3 aHalf   = (ab.max - ab.min) * 0.5f;
    Vec3 sc = {half.x / fmaxf(aHalf.x, 1e-4f),
               half.y / fmaxf(aHalf.y, 1e-4f),
               half.z / fmaxf(aHalf.z, 1e-4f)};
    Mat4 fit = Mat4::translate(center) * Mat4::scale(sc)
             * Mat4::translate(Vec3{-aCenter.x, -aCenter.y, -aCenter.z});
    return Mat4::translate(pos) * Mat4::rotateY(yaw)
         * Mat4::scale({bodyScale, bodyScale, bodyScale}) * fit;
}

// THE SLOT-SPEC TABLE. Given measured body landmarks, produce the target box each armor piece fills.
// This is the single place that decides "how big / where" — meaningful factors, not scattered magic:
//   HELMET: skullcap — rim at the brow, apex above the crown, 1.4x the head width (sits ON the head).
//   CHEST : width spans the SHOULDERS (with a floor so slim bodies still get a substantial cuirass),
//           extra depth so it wraps front/back.
//   BOOTS : the feet box, slightly enlarged.
//   GLOVES: the span enclosing BOTH measured hands (x already spans them), enlarged in Y/Z.
// Returns false when the slot's landmark is invalid (e.g. a robed body with no hands/feet) → skip.
bool Engine::armorSlotBox(int slot, const BodyRegions& reg, f32 bodyH,
                          Vec3& outCenter, Vec3& outHalf) const {
    auto boxOf = [](const AABB& a, Vec3& c, Vec3& h) {
        c = (a.min + a.max) * 0.5f;
        h = (a.max - a.min) * 0.5f;
    };
    if (slot == 0) {                       // HELMET
        if (!reg.headValid) return false;
        f32 hh = reg.head.max.y - reg.head.min.y;
        f32 yMin = reg.head.min.y + 0.50f * hh;   // brow
        f32 yMax = reg.head.max.y + 0.30f * hh;   // apex above the crown
        f32 hw = (reg.head.max.x - reg.head.min.x) * 0.5f;
        f32 hd = (reg.head.max.z - reg.head.min.z) * 0.5f;
        outCenter = {(reg.head.min.x + reg.head.max.x) * 0.5f, (yMin + yMax) * 0.5f,
                     (reg.head.min.z + reg.head.max.z) * 0.5f};
        outHalf   = {hw * 1.4f, (yMax - yMin) * 0.5f, hd * 1.4f};
        return true;
    }
    if (slot == 1) {                       // CHEST
        if (!reg.torsoValid) return false;
        Vec3 c, h; boxOf(reg.torso, c, h);
        f32 wHalf = fmaxf(reg.shoulderHalfW * 1.25f, bodyH * 0.20f); // spans shoulders, min floor
        outCenter = c;
        outHalf   = {wHalf, h.y * 1.3f, h.z * 1.75f};
        return true;
    }
    if (slot == 2) {                       // BOOTS
        if (!reg.feetValid) return false;
        Vec3 c, h; boxOf(reg.feet, c, h);
        outCenter = c;
        outHalf   = {h.x * 1.5f, h.y * 1.45f, h.z * 1.4f};
        return true;
    }
    // slot == 3: GLOVES — box enclosing both measured hands.
    if (!reg.handsValid) return false;
    AABB hands = {{fminf(reg.handL.min.x, reg.handR.min.x), fminf(reg.handL.min.y, reg.handR.min.y),
                   fminf(reg.handL.min.z, reg.handR.min.z)},
                  {fmaxf(reg.handL.max.x, reg.handR.max.x), fmaxf(reg.handL.max.y, reg.handR.max.y),
                   fmaxf(reg.handL.max.z, reg.handR.max.z)}};
    Vec3 c, h; boxOf(hands, c, h);
    outCenter = c;
    outHalf   = {h.x * 1.05f, h.y * 1.3f, h.z * 1.3f}; // x already spans both hands
    return true;
}

void Engine::submitPlayerEquipment(const Vec3& pos, f32 yaw, f32 scale, u8 anim,
                                   u8 bodyMeshId, const PlayerInventory& inv) {
    // Fallback texture: material slot 0 is always the white placeholder.
    const Texture& defaultTex = MaterialSystem::get(0)->texture;

    // --- Weapon: hand-attach math mirrored from the net remote-player block ---
    const ItemInstance& wpn = inv.equipped[static_cast<u32>(ItemSlot::WEAPON)];
    if (wpn.defId < m_itemDefCount) {
        u8 wMesh = m_itemDefs[wpn.defId].meshId;
        u8 wMat  = m_itemDefs[wpn.defId].materialId;
        if (wMesh > 0 && wMesh < m_meshDefCount && m_meshDefs[wMesh].mesh.vao) {
            // Thrust forward while attacking, drop while reloading (same telegraph as net path).
            f32 thrust = (anim & 1) ? 0.25f : 0.0f;
            f32 drop   = (anim & 2) ? -0.25f : 0.0f;
            Vec3 right = {-sinf(yaw + 1.57f), 0, -cosf(yaw + 1.57f)};
            Vec3 fwd   = {-sinf(yaw), 0, -cosf(yaw)};
            Vec3 wp = pos + Vec3{0, 0.8f + drop, 0}
                          + right * (0.35f * scale)
                          + fwd   * ((0.3f + thrust) * scale);
            // Normalize the held weapon to ~0.85 m longest (its native mesh size varies a lot),
            // so daggers and claymores both read at a sensible held size instead of the old
            // fixed 0.4 that shrank larger meshes to nothing.
            const AABB& wmb = m_meshDefs[wMesh].bounds;
            f32 wMax = fmaxf(wmb.max.x - wmb.min.x, fmaxf(wmb.max.y - wmb.min.y, wmb.max.z - wmb.min.z));
            f32 wScale = ((wMax > 0.001f) ? (0.85f / wMax) : 0.4f) * scale;
            // Per-type orientation (mirrors the FP viewmodel): gun barrels are authored toward +Z,
            // which rotateY(yaw) alone points BEHIND the body — flip them π so they face forward.
            // Melee/bows stay upright (blade along +Y reads fine held vertically).
            const ItemDef& wdef = m_itemDefs[wpn.defId];
            f32 holdYaw = 0.0f;
            if (wdef.weaponType == WeaponType::HITSCAN ||
                (wdef.weaponType == WeaponType::PROJECTILE &&
                 wdef.weaponSubtype == WeaponSubtype::CROSSBOW)) {
                holdYaw = 3.14159f; // barrel/stock faces forward
            }
            Mat4 mm = Mat4::translate(wp)
                    * Mat4::rotateY(yaw + holdYaw)
                    * Mat4::scale({wScale, wScale, wScale});
            AABB wb = {wp - Vec3{0.5f, 0.5f, 0.5f}, wp + Vec3{0.5f, 0.5f, 0.5f}};
            const Material* m = MaterialSystem::get(wMat);
            Renderer::submit(m_basicShader, m ? m->texture : defaultTex,
                             m_meshDefs[wMesh].mesh, mm, wb,
                             m ? m->tint : Vec4{1, 1, 1, 1});
        }
    }

    // --- Armor overlays — landmark-anchored, driven by the slot-spec table (armorSlotBox) ---
    // One loop: each piece's target box comes from the spec; fitMeshToBox seats the mesh there.
    // Invalid landmarks (e.g. a robed body's missing hands/feet) skip that piece.
    const BodyRegions reg = bodyRegionsFor(bodyMeshId);
    f32 bodyH = (bodyMeshId > 0 && bodyMeshId < m_meshDefCount)
                  ? (m_meshDefs[bodyMeshId].bounds.max.y - m_meshDefs[bodyMeshId].bounds.min.y) : 1.8f;
    const AABB worldBox = {pos - Vec3{0.8f, 0.0f, 0.8f}, pos + Vec3{0.8f, 2.2f, 0.8f}}; // cull box
    const ItemSlot kSlot[4] = {ItemSlot::HELMET, ItemSlot::ARMOR, ItemSlot::BOOTS, ItemSlot::GLOVES};
    for (int s = 0; s < 4; ++s) {
        const ItemInstance& it = inv.equipped[static_cast<u32>(kSlot[s])];
        if (it.defId >= m_itemDefCount) continue;          // empty slot
        const ItemDef& def = m_itemDefs[it.defId];
        u8 mesh = def.tierMeshId;
        if (mesh == 0 || mesh >= m_meshDefCount || !m_meshDefs[mesh].mesh.vao) continue;
        Vec3 center, half;
        if (!armorSlotBox(s, reg, bodyH, center, half)) continue; // invalid landmark → skip
        const Material* mat = MaterialSystem::get(def.materialId);
        Vec4 tint = mat ? mat->tint : Vec4{1, 1, 1, 1};
        if (it.rarity == Rarity::LEGENDARY) tint = {tint.x * 1.3f, tint.y * 1.15f, tint.z * 0.7f, tint.w};
        Mat4 mm = fitMeshToBox(mesh, center, half, pos, yaw, scale);
        Renderer::submit(m_basicShader, mat ? mat->texture : defaultTex,
                         m_meshDefs[mesh].mesh, mm, worldBox, tint);
    }
}

// ---------------------------------------------------------------------------
// submitPlayerEquipmentIds — mesh-id variant of submitPlayerEquipment for the
// CLIENT path, where remote inventories are unavailable. weaponMeshId and
// armorMeshId[4] (helmet, chest, boots, gloves) are the wire-carried tier-mesh
// indices from SnapPlayer. Material resolves to the default texture/tint (mat 0)
// which is acceptable for remote-player view; the host path uses real materials.
// ---------------------------------------------------------------------------
void Engine::submitPlayerEquipmentIds(const Vec3& pos, f32 yaw, f32 scale, u8 anim,
                                      u8 bodyMeshId, u8 weaponMeshId, const u8 armorMeshId[4]) {
    const Texture& defaultTex = MaterialSystem::get(0)->texture;

    // --- Weapon: same hand-attach math as submitPlayerEquipment and the old inline block ---
    if (weaponMeshId > 0 && weaponMeshId < m_meshDefCount && m_meshDefs[weaponMeshId].mesh.vao) {
        f32 thrust = (anim & 1) ? 0.25f : 0.0f;  // attacking: thrust forward
        f32 drop   = (anim & 2) ? -0.25f : 0.0f; // reloading: drop hand
        Vec3 right = {-sinf(yaw + 1.57f), 0, -cosf(yaw + 1.57f)};
        Vec3 fwd   = {-sinf(yaw), 0, -cosf(yaw)};
        Vec3 wp = pos + Vec3{0, 0.8f + drop, 0}
                      + right * (0.35f * scale)
                      + fwd   * ((0.3f + thrust) * scale);
        // Normalize held weapon to ~0.85 m longest (matches submitPlayerEquipment).
        const AABB& wmb = m_meshDefs[weaponMeshId].bounds;
        f32 wMax = fmaxf(wmb.max.x - wmb.min.x, fmaxf(wmb.max.y - wmb.min.y, wmb.max.z - wmb.min.z));
        f32 wScale = ((wMax > 0.001f) ? (0.85f / wMax) : 0.4f) * scale;
        Mat4 mm = Mat4::translate(wp)
                * Mat4::rotateY(yaw)
                * Mat4::scale({wScale, wScale, wScale});
        AABB wb = {wp - Vec3{0.5f, 0.5f, 0.5f}, wp + Vec3{0.5f, 0.5f, 0.5f}};
        Renderer::submit(m_basicShader, defaultTex, m_meshDefs[weaponMeshId].mesh, mm, wb,
                         Vec4{1, 1, 1, 1});
    }

    // --- Armor overlays — same landmark spec-table fit as submitPlayerEquipment so remote players
    // match the local view. armorMeshId order: [helmet, chest, boots, gloves] = slots 0..3. ---
    const BodyRegions reg = bodyRegionsFor(bodyMeshId);
    f32 bodyH = (bodyMeshId > 0 && bodyMeshId < m_meshDefCount)
                  ? (m_meshDefs[bodyMeshId].bounds.max.y - m_meshDefs[bodyMeshId].bounds.min.y) : 1.8f;
    const AABB worldBox = {pos - Vec3{0.8f, 0.0f, 0.8f}, pos + Vec3{0.8f, 2.2f, 0.8f}};
    for (int s = 0; s < 4; ++s) {
        u8 mesh = armorMeshId[s];
        if (mesh == 0 || mesh >= m_meshDefCount || !m_meshDefs[mesh].mesh.vao) continue;
        Vec3 center, half;
        if (!armorSlotBox(s, reg, bodyH, center, half)) continue;
        Mat4 mm = fitMeshToBox(mesh, center, half, pos, yaw, scale);
        Renderer::submit(m_basicShader, defaultTex, m_meshDefs[mesh].mesh, mm, worldBox,
                         Vec4{1, 1, 1, 1});
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

}

// ---------------------------------------------------------------------------
// renderInteractionPrompts — screen-space floor-descend + item-pickup button
// hints. Split out of renderSpeechBubbles and drawn in the NATIVE HUD pass: the
// button glyph's background is a HUD line batch projected through flushHUD's
// cached screen size, which only matches when drawn in the native pass — in the
// scaled 3D pass it mis-projected, so the background vanished and only the
// letter showed. The native pass also keeps the prompt crisp.
// ---------------------------------------------------------------------------
void Engine::renderInteractionPrompts(u32 sw, u32 sh) {
    // The prompts read the SAME resolved targets the button acts on (Engine::resolveInteractTargets),
    // so what is offered here and what E does can no longer disagree. They used to be independent
    // scans on different rules — the shrine prompt was pure proximity, while activation required aim,
    // so it happily advertised a shrine that pressing E would not activate.
    const InteractState& st = m_interact[m_localPlayerIndex];
    const bool  gamepad   = Input::activeDeviceIsGamepad();
    const char* keyGlyph  = gamepad ? "X" : "E";
    // An item outranks a shrine/exit on a tap, so those two are reachable only by HOLDING — say so,
    // and only when the conflict actually exists. With no loot in reach a tap still works and the
    // prompt stays the plain one; a hint that lies about the cheaper input is worse than none.
    const bool  needHold  = (st.itemIdx >= 0);
    const f32   heldSec   = st.hold.held;
    const f32   holdFrac  = (heldSec <= 0.0f) ? 0.0f
                          : (heldSec >= GameConst::INTERACT_HOLD_SEC ? 1.0f
                             : heldSec / GameConst::INTERACT_HOLD_SEC);

    // Draws "[E] Label" (or "[E] Hold — Label"), plus a fill bar tracking the hold. The bar is the
    // only thing telling the player the press is being counted rather than ignored.
    auto drawPrompt = [&](const char* label, Vec3 col, f32 cy) {
        char line[64];
        if (needHold) std::snprintf(line, sizeof(line), "Hold - %s", label);
        else          std::snprintf(line, sizeof(line), "%s", label);

        f32 textW  = FontSystem::textWidth(line, 1);
        f32 totalW = 22.0f + textW;
        f32 cx = (static_cast<f32>(sw) - totalW) * 0.5f;
        HUD::drawKeySymbol(sw, sh, cx, cy - 2.0f, keyGlyph, true);
        FontSystem::drawText(sw, sh, cx + 22.0f, cy, line, col, 1);

        if (needHold && holdFrac > 0.0f) {
            const f32 barH = 3.0f;
            const f32 barY = cy - 8.0f;
            HUD::drawRectAt(sw, sh, cx + 22.0f, barY, textW, barH, {0.18f, 0.18f, 0.20f});
            HUD::drawRectAt(sw, sh, cx + 22.0f, barY, textW * holdFrac, barH, col);
        }
    };

    // Floor exit — the lowest-priority target, so it is listed first but yields to everything.
    if (st.nearExit && m_gameState == GameState::IN_GAME) {
        char doorStr[32];
        std::snprintf(doorStr, sizeof(doorStr), "Descend to Floor %u", m_level.currentFloor + 1);
        drawPrompt(doorStr, {0.3f, 1.0f, 0.4f}, static_cast<f32>(sh) * 0.4f);
    }

    // The post-Engine exit portal — the run's ending, so the label says so. (The Source ENTRY
    // portal deliberately has no prompt: it's a secret. This one must be found by everyone.)
    if (st.nearExitPortal && m_gameState == GameState::IN_GAME) {
        drawPrompt("Leave the Dungeon", {1.0f, 0.85f, 0.4f}, static_cast<f32>(sh) * 0.4f);
    }

    // Town: the account stash and the to-dungeon portal.
    if (st.stashIdx >= 0 && st.itemIdx < 0 && m_gameState == GameState::IN_GAME) {
        drawPrompt("Open Stash", {1.0f, 0.85f, 0.4f}, static_cast<f32>(sh) * 0.45f);
    }
    if (st.nearTownPortal && m_gameState == GameState::IN_GAME) {
        drawPrompt("Enter the Dungeon", {0.5f, 1.0f, 0.5f}, static_cast<f32>(sh) * 0.4f);
    }

    // Shrine. A shrine you cannot tell is interactable is just scenery, and the prompt is the only
    // place the player learns which of the three it is before spending it.
    if (st.shrineIdx >= 0 && m_gameState == GameState::IN_GAME) {
        const u8 buff = Shrine::buffOf(m_worldItems.items[st.shrineIdx].item);
        const Vec3 c = Shrine::colorOf(buff);
        drawPrompt(Shrine::nameOf(buff), {c.x * 0.9f + 0.1f, c.y * 0.9f + 0.1f, c.z * 0.9f + 0.1f},
                   static_cast<f32>(sh) * 0.45f);
    }

    // Chest — real (CHEST_ID world-item sentinel) or mimic (dormant entity): ONE prompt, one
    // label, one colour, because the design is that the player cannot tell which they are
    // aiming at (the label must never say what it is; hiding the target bar for DORMANT
    // entities does half the job, this prompt does the other half). Suppressed while an item
    // is in reach: the item wins the tap anyway, and a prompt offering what the button won't
    // do is worse than none. Nudged below the shrine line for the rare case both resolve.
    if ((st.chestIdx >= 0 || st.mimicIdx >= 0) && st.itemIdx < 0 &&
        m_gameState == GameState::IN_GAME) {
        drawPrompt("Open Chest", {0.85f, 0.62f, 0.28f},
                   static_cast<f32>(sh) * (st.shrineIdx >= 0 ? 0.5f : 0.45f));
    }

    // Item pickup prompt — names the item the button will actually grab. It reads the ONE resolved
    // target (`st.itemIdx` from resolveInteractTargets), the exact index host pickup and the client's
    // CL_PICKUP_ITEM both act on, so the shown name can never differ from what E picks up. This used
    // to be an INDEPENDENT scan on entirely different rules — eye origin vs feet, 3D dot vs
    // horizontal, max-alignment vs `dot − 0.1·dist` (+0.5 for legendaries), a 0.85/3.5 m cone vs
    // Interact::inReach's grab-radius, and no loot-ownership check — so with two items close together
    // it happily named one while the button grabbed the other. One resolver, one answer.
    if (st.itemIdx >= 0) {
        const WorldItem& wi = m_worldItems.items[st.itemIdx];
        // st.itemIdx is only ever a real, in-range, owned-by-you item (resolveInteractTargets
        // filters globes/shards/shrines/chests and the defId bound), but keep the guard: the draw
        // below indexes m_itemDefs by defId.
        if (wi.item.defId < m_itemDefCount) {
            const ItemDef* bestDef = &m_itemDefs[wi.item.defId];
            Vec3 rColor = rarityColor(wi.item.rarity);
            // Build display text — legendaries append the skill name in brackets
            char hintBuf[96];
            if (wi.item.rarity == Rarity::LEGENDARY &&
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
            HUD::drawKeySymbol(sw, sh, cx, cy - 2.0f, Input::activeDeviceIsGamepad() ? "X" : "E", true);
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
