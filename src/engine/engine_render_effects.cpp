// engine_render_effects.cpp — Engine::renderProjectilesAndEffects: projectiles, sparks, portals, AoE.
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
    u32 pxSeen = 0;
    for (u32 i = 0; i < MAX_PROJECTILES && pxSeen < projPool.activeCount; i++) {
        const Projectile& p = projPool.projectiles[i];
        if (!p.active) continue;
        pxSeen++;

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

        // Red while a milestone boss seals the exit, green once open — mirrors the
        // minimap door marker so the portal reads as locked from a distance too.
        bool portalLocked = m_level.floorHasBoss && floorBossAlive();

        // Tall vertical beam (bright green, visible from far away)
        Vec3 beamCol = portalLocked ? Vec3{0.9f * pulse, 0.12f, 0.12f}
                                    : Vec3{0.1f, 0.9f * pulse, 0.2f};
        for (f32 ox = -0.08f; ox <= 0.08f; ox += 0.04f) {
            DebugDraw::line(dp + Vec3{ox, 0, 0}, dp + Vec3{ox, 4.0f, 0}, beamCol);
            DebugDraw::line(dp + Vec3{0, 0, ox}, dp + Vec3{0, 4.0f, ox}, beamCol);
        }

        // Spinning portal ring at waist height
        f32 ringY = dp.y + 1.0f;
        f32 ringR = 0.6f + fastPulse * 0.1f;
        Vec3 ringCol = portalLocked ? Vec3{1.0f * pulse, 0.2f * pulse, 0.2f * pulse}
                                    : Vec3{0.3f * pulse, 1.0f * pulse, 0.4f * pulse};
        for (u32 s = 0; s < 12; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / 12.0f) + t * 2.0f;
            f32 a1 = a0 + (6.28318f / 12.0f);
            Vec3 p0 = dp + Vec3{cosf(a0) * ringR, ringY - dp.y, sinf(a0) * ringR};
            Vec3 p1 = dp + Vec3{cosf(a1) * ringR, ringY - dp.y, sinf(a1) * ringR};
            DebugDraw::line(p0, p1, ringCol);
        }

        // Second ring at head height
        Vec3 ring2Col = portalLocked ? Vec3{0.8f * pulse, 0.15f, 0.15f}
                                     : Vec3{0.2f, 0.8f * pulse, 0.3f};
        f32 ringY2 = dp.y + 2.0f;
        for (u32 s = 0; s < 12; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / 12.0f) - t * 1.5f;
            f32 a1 = a0 + (6.28318f / 12.0f);
            Vec3 p0 = dp + Vec3{cosf(a0) * ringR * 0.7f, ringY2 - dp.y, sinf(a0) * ringR * 0.7f};
            Vec3 p1 = dp + Vec3{cosf(a1) * ringR * 0.7f, ringY2 - dp.y, sinf(a1) * ringR * 0.7f};
            DebugDraw::line(p0, p1, ring2Col);
        }

        // Ground circle (large, static)
        Vec3 groundCol = portalLocked ? Vec3{0.5f, 0.1f, 0.1f}
                                      : Vec3{0.15f, 0.5f, 0.2f};
        for (u32 s = 0; s < 16; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / 16.0f);
            f32 a1 = a0 + (6.28318f / 16.0f);
            Vec3 p0 = dp + Vec3{cosf(a0) * 0.8f, 0.02f, sinf(a0) * 0.8f};
            Vec3 p1 = dp + Vec3{cosf(a1) * 0.8f, 0.02f, sinf(a1) * 0.8f};
            DebugDraw::line(p0, p1, groundCol);
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

    // --- The Source portal — the hidden second portal beside the floor-50 exit (secret superboss) ---
    // Distinct void-violet so it reads as "other" next to the green exit. Host/SP only (a client
    // never sets sourcePortalActive — it's teleported by the host's sentinel broadcast instead).
    if (m_level.sourcePortalActive) {
        Vec3 dp = m_level.sourcePortalPos;
        f32 t = static_cast<f32>(m_statsTimer);
        f32 pulse = 0.5f + 0.5f * sinf(t * 3.0f);
        f32 fastPulse = 0.5f + 0.5f * sinf(t * 8.0f);
        Vec3 beamCol = {0.62f * pulse + 0.2f, 0.18f, 0.95f * pulse};
        for (f32 ox = -0.08f; ox <= 0.08f; ox += 0.04f) {
            DebugDraw::line(dp + Vec3{ox, 0, 0}, dp + Vec3{ox, 4.5f, 0}, beamCol);
            DebugDraw::line(dp + Vec3{0, 0, ox}, dp + Vec3{0, 4.5f, ox}, beamCol);
        }
        f32 ringR = 0.7f + fastPulse * 0.12f;
        Vec3 ringCol = {0.7f * pulse, 0.25f * pulse, 1.0f * pulse};
        for (u32 s = 0; s < 14; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / 14.0f) + t * 2.5f;
            f32 a1 = a0 + (6.28318f / 14.0f);
            DebugDraw::line(dp + Vec3{cosf(a0) * ringR, 1.0f, sinf(a0) * ringR},
                            dp + Vec3{cosf(a1) * ringR, 1.0f, sinf(a1) * ringR}, ringCol);
            DebugDraw::line(dp + Vec3{cosf(a0) * ringR * 0.7f, 2.2f, sinf(a0) * ringR * 0.7f},
                            dp + Vec3{cosf(a1) * ringR * 0.7f, 2.2f, sinf(a1) * ringR * 0.7f}, ringCol);
        }
    }

    // --- The EXIT portal — spawned when the Dungeon Engine dies; entering rolls the credits ---
    // Warm gold/white so it reads as "the way out" against the void-violet entry portal above.
    // Drawn on EVERY machine: unlike sourcePortalActive, exitPortalActive is replicated to
    // clients via SV_EVENT::EXIT_PORTAL (guests must be able to find and enter it).
    if (m_level.exitPortalActive) {
        Vec3 dp = m_level.exitPortalPos;
        f32 t = static_cast<f32>(m_statsTimer);
        f32 pulse = 0.5f + 0.5f * sinf(t * 3.0f);
        f32 fastPulse = 0.5f + 0.5f * sinf(t * 8.0f);
        Vec3 beamCol = {1.0f * pulse, 0.85f * pulse + 0.1f, 0.35f * pulse + 0.15f};
        for (f32 ox = -0.08f; ox <= 0.08f; ox += 0.04f) {
            DebugDraw::line(dp + Vec3{ox, 0, 0}, dp + Vec3{ox, 4.5f, 0}, beamCol);
            DebugDraw::line(dp + Vec3{0, 0, ox}, dp + Vec3{0, 4.5f, ox}, beamCol);
        }
        f32 ringR = 0.7f + fastPulse * 0.12f;
        Vec3 ringCol = {1.0f * pulse, 0.9f * pulse, 0.55f * pulse};
        for (u32 s = 0; s < 14; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / 14.0f) + t * 2.5f;
            f32 a1 = a0 + (6.28318f / 14.0f);
            DebugDraw::line(dp + Vec3{cosf(a0) * ringR, 1.0f, sinf(a0) * ringR},
                            dp + Vec3{cosf(a1) * ringR, 1.0f, sinf(a1) * ringR}, ringCol);
            DebugDraw::line(dp + Vec3{cosf(a0) * ringR * 0.7f, 2.2f, sinf(a0) * ringR * 0.7f},
                            dp + Vec3{cosf(a1) * ringR * 0.7f, 2.2f, sinf(a1) * ringR * 0.7f}, ringCol);
        }
    }

    // --- Loot goblin escape portal — drawn while it stands channeling over its hoard ---
    // Goblin-only flavor, NOT an interactable (contrast the two real portals above): the player
    // can't use it, so there is no interact/priority plumbing — it exists purely so the idle
    // goblin reads as "caught mid-escape" rather than parked. Keyed entirely on replicated state
    // (ENT_LOOT_GOBLIN flag + IDLE aiState + yaw/pos, all in SnapEntity), so it needs no entity
    // slot, no wire change, and vanishes BY CONSTRUCTION the instant the goblin flees, dies or
    // escapes — every path leaves IDLE. Amber/gold (loot!), distinct from the green exit and the
    // void-violet Source portal. CLIENTs read the interpolated pool, same switch as projPool above.
    {
        const EntityPool& portalPool = (m_netRole == NetRole::CLIENT) ? m_renderInterp.entities
                                                                      : m_entities;
        for (u32 a = 0; a < portalPool.activeCount; a++) {
            const Entity& g = portalPool.entities[portalPool.activeList[a]];
            if (!(g.flags & ENT_LOOT_GOBLIN) || (g.flags & ENT_DEAD)) continue;
            if (g.aiState != AIState::IDLE) continue;

            // In front of the goblin along its facing, on the floor it stands on.
            const Vec3 fwd = {sinf(g.yaw), 0.0f, cosf(g.yaw)};
            const Vec3 pp  = {g.position.x + fwd.x * 1.4f,
                              g.position.y - g.halfExtents.y,
                              g.position.z + fwd.z * 1.4f};
            const f32 t = static_cast<f32>(m_statsTimer);
            const f32 pulse     = 0.5f + 0.5f * sinf(t * 3.0f);
            const f32 fastPulse = 0.5f + 0.5f * sinf(t * 8.0f);

            // Two counter-rotating rings, goblin-sized (the Source portal's construction at ~60%).
            const Vec3 ringCol = {1.0f * pulse, 0.75f * pulse, 0.2f * pulse};
            const f32  ringR   = 0.45f + fastPulse * 0.08f;
            for (u32 s = 0; s < 12; s++) {
                f32 a0 = static_cast<f32>(s) * (6.28318f / 12.0f) + t * 2.2f;
                f32 a1 = a0 + (6.28318f / 12.0f);
                DebugDraw::line(pp + Vec3{cosf(a0) * ringR, 0.7f, sinf(a0) * ringR},
                                pp + Vec3{cosf(a1) * ringR, 0.7f, sinf(a1) * ringR}, ringCol);
                f32 b0 = -a0, b1 = b0 - (6.28318f / 12.0f);   // counter-rotation
                DebugDraw::line(pp + Vec3{cosf(b0) * ringR * 0.65f, 1.4f, sinf(b0) * ringR * 0.65f},
                                pp + Vec3{cosf(b1) * ringR * 0.65f, 1.4f, sinf(b1) * ringR * 0.65f},
                                ringCol);
            }
            // Channel wisps: three slow arcs from the goblin's hands toward the ring — the
            // "it is summoning this" read.
            const Vec3 hands = {g.position.x, g.position.y + 0.1f, g.position.z};
            const Vec3 wispCol = {0.95f * fastPulse, 0.65f * fastPulse, 0.15f * fastPulse};
            for (u32 w = 0; w < 3; w++) {
                f32 ph  = t * 1.7f + static_cast<f32>(w) * 2.09f;
                Vec3 mid = (hands + pp) * 0.5f;
                mid.y += 0.6f + 0.15f * sinf(ph);
                Vec3 tgt = pp + Vec3{cosf(ph) * ringR * 0.5f, 1.0f, sinf(ph) * ringR * 0.5f};
                DebugDraw::line(hands, mid, wispCol);
                DebugDraw::line(mid, tgt, wispCol);
            }
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

    // --- Melee swing slash arc — a diagonal slash drawn in CAMERA space so it always sits
    //     in the same impactful spot relative to the crosshair (like a viewmodel element),
    //     instead of drifting with aim. Built from the live camera each frame; ownerLane
    //     keeps it to the swinging player's own viewport in split-screen. ---
    {
        const Camera& cam = m_camera;
        Vec3 camUp = cross(cam.right, cam.forward);   // orthonormal view up
        constexpr f32 DEPTH = 1.8f;                   // metres in front of the camera
        Vec3 anchor = cam.position + cam.forward * DEPTH;
        for (u32 i = 0; i < MAX_SWING_FX; i++) {
            if (!m_fx.swingFX[i].active) continue;
            const SwingFX& sw = m_fx.swingFX[i];
            if (sw.ownerLane != m_localPlayerIndex) continue;  // only the swinger's viewport

            f32 life = sw.timer / 0.18f;     // 1 -> 0 over lifetime (overall fade)
            f32 t    = 1.0f - life;          // 0 -> 1 sweep progress (leading edge)

            // Stroke shape (start + delta + bow, view-plane units) chosen per weapon subtype so
            // the arc matches that weapon's viewmodel swing direction. x = right, y = up.
            f32 sx, sy, ddx, ddy, bowBase;
            switch (static_cast<WeaponSubtype>(sw.style)) {
                case WeaponSubtype::CLAYMORE: // wide horizontal sweep, right -> left
                    sx = 1.15f; sy = 0.05f; ddx = -2.30f; ddy =  0.00f; bowBase = 0.30f; break;
                case WeaponSubtype::AXE:      // overhead vertical chop, top -> bottom
                case WeaponSubtype::CLEAVER:  // cleaver chops overhead like the axe
                    sx = 0.10f; sy = 0.95f; ddx = -0.20f; ddy = -1.90f; bowBase = 0.18f; break;
                case WeaponSubtype::DAGGER:   // quick short stab, compact near the crosshair
                    sx = 0.30f; sy = 0.22f; ddx = -0.60f; ddy = -0.44f; bowBase = 0.10f; break;
                default:                      // SWORD / CLEAVER / fists: diagonal upper-right -> lower-left
                    sx = 0.95f; sy = 0.55f; ddx = -1.90f; ddy = -1.10f; bowBase = 0.28f; break;
            }
            sx *= sw.scale; sy *= sw.scale;
            const f32 dx = ddx * sw.scale, dy = ddy * sw.scale;  // delta to end of the stroke
            // Perpendicular (in view plane) for the bow + thickness offset.
            f32 plen = sqrtf(dx*dx + dy*dy);
            f32 pxn = (plen > 1e-4f) ? (-dy / plen) : 0.0f;
            f32 pyn = (plen > 1e-4f) ? ( dx / plen) : 0.0f;
            const f32 BOW = bowBase;          // outward curve depth (per-subtype)
            constexpr u32 SWING_SEGS = 16;

            // Draw two parallel bowed strokes (offset along the perpendicular) for thickness.
            for (s32 layer = -1; layer <= 1; layer++) {
                f32 off = static_cast<f32>(layer) * 0.06f * sw.scale;
                Vec3 prev = {0,0,0};
                bool havePrev = false;
                for (u32 s = 0; s <= SWING_SEGS; s++) {
                    f32 p = static_cast<f32>(s) / static_cast<f32>(SWING_SEGS);
                    if (p > t) break;                       // not yet swept past the leading edge
                    f32 bow = sinf(p * 3.14159f) * BOW * sw.scale;
                    f32 vx = sx + dx * p + pxn * (bow + off);
                    f32 vy = sy + dy * p + pyn * (bow + off);
                    Vec3 cur = anchor + cam.right * vx + camUp * vy;
                    // Brightest at the cutting (leading) edge, fading down the trail + overall.
                    f32 a = life * (1.0f - (t - p) * 0.8f);
                    if (a < 0.0f) a = 0.0f;
                    if (havePrev) DebugDraw::line(prev, cur, sw.color * a);
                    prev = cur;
                    havePrev = true;
                }
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
    ParticleSystem::render(m_particles, m_camera, m_particleShader, m_unlitShader,
                           m_cubeMesh, m_particleBlobMatId, m_particleSparkMatId);
}
