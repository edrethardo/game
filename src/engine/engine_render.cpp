// Engine module — see engine.h for class definition.
// Split from engine.cpp for manageability. Engine::render stays here;
// renderViewmodel/renderEntities/renderProjectilesAndEffects/renderWorldItems/
// renderSpeechBubbles/renderDamageNumbers are in their own sibling files.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
#include "engine/credits.h"   // credits roll rows (CREDITS state render)
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
#include "renderer/screenshot.h"
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
#include <ctime>
#include <cstdlib>

// Shared statics defined in engine.cpp
// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;
extern bool s_engineSlain;    // secret superboss — Engine defeated this session (victory variant)

// ---------------------------------------------------------------------------
// renderTransitionScreens — draws non-IN_GAME full-screen states
// (MENU, CONNECTING, FLOOR_TRANSITION, VICTORY, GAME_OVER, and any unknown
// state). Returns true if one was drawn; render() early-outs on true.
// ---------------------------------------------------------------------------
bool Engine::renderTransitionScreens(u32 sw, u32 sh) {
    if (m_gameState == GameState::MENU) {
        renderMenu();
        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return true;
    }

    if (m_gameState == GameState::CONNECTING) {
        // Animated "Connecting…" label + host + a cancel hint. A lone pulsing dot read as a hang and
        // gave no clue the join could be backed out of (MENU_BACK/Esc cancels; it also times out).
        u32 dots = static_cast<u32>(m_statsTimer * 2.0f) % 4u;   // 0..3 trailing dots
        char msg[96];
        if (m_menu.connectAddress[0])   // empty on a Steam invite/browse join (routed by SteamID)
            std::snprintf(msg, sizeof(msg), "Connecting to %s%.*s", m_menu.connectAddress, dots, "...");
        else
            std::snprintf(msg, sizeof(msg), "Connecting%.*s", dots, "...");
        f32 mw = FontSystem::textWidth(msg, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - mw) * 0.5f, sh * 0.52f, msg, {0.9f, 0.85f, 0.4f}, 3);
        const char* hint = Input::activeDeviceIsGamepad() ? "Press B to cancel" : "Press Esc to cancel";
        f32 hw = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hw) * 0.5f, sh * 0.44f, hint, {0.6f, 0.6f, 0.6f}, 1);

        // Pulsing dot to indicate connecting
        f32 pulse = (sinf(m_statsTimer * 6.0f) + 1.0f) * 0.5f;
        HUD::drawCrosshair(sw, sh, {pulse, pulse, 0.5f + pulse * 0.5f});
        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return true;
    }

    if (m_gameState == GameState::FLOOR_TRANSITION) {
        glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

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

        // Kill count — the CHARACTER's lifetime total (stats sidecar), not this floor's tally.
        // Lane 0 is the character whose save/floor drives the session. Live-read is safe: the
        // counter never resets on descent (only equipFreshLane zeroes it).
        char killStr[40];
        std::snprintf(killStr, sizeof(killStr), "Enemies deleted: %u", m_totalKills[0]);
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
        return true;
    }

    if (m_gameState == GameState::CREDITS) {
        // --- Credits roll --- rows scroll up from the bottom edge; the table + pacing live in
        // engine/credits.h (single-sourced with the update case's scroll-end check).
        glClearColor(0.01f, 0.02f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        FontSystem::setUIScale(static_cast<f32>(sh) / 720.0f);

        const f32 s = static_cast<f32>(sh) / 720.0f;   // reference px -> screen px
        for (u32 i = 0; i < Credits::ROW_COUNT; i++) {
            const Credits::Row& row = Credits::kRows[i];
            if (row.text[0] == '\0') continue;   // spacer
            // Row i rises from just below the bottom edge as the scroll advances.
            f32 y = (m_creditsScroll - Credits::rowOffset(i)) * s - 40.0f * s;
            if (y < -60.0f * s || y > static_cast<f32>(sh) + 60.0f * s) continue;
            const Vec3 col = (row.size >= 4) ? Vec3{0.9f, 0.8f, 0.2f}
                          : (row.size == 3) ? Vec3{0.95f, 0.95f, 0.95f}
                                            : Vec3{0.62f, 0.62f, 0.68f};
            f32 w = FontSystem::textWidth(row.text, row.size);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - w) * 0.5f, y,
                                 row.text, col, row.size);
        }

        // Skip hint, faded in the corner.
        const char* hint = "[Enter] skip";
        f32 hw = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, static_cast<f32>(sw) - hw - 14.0f * s, 12.0f * s,
                             hint, {0.4f, 0.4f, 0.45f}, 1);

        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return true;
    }

    if (m_gameState == GameState::VICTORY) {
        // --- Victory screen ---
        glClearColor(0.01f, 0.02f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        FontSystem::setUIScale(static_cast<f32>(sh) / 720.0f);

        // Final victory — three variants: the secret superboss ending (Engine slain in The Source),
        // the demo "thanks for playing" headline, or the ordinary Hell-floor-50 clear. The stats +
        // prompt below are unchanged across all three.
        const char* title;
        const char* subtitle;
        if (s_engineSlain) {
            title    = "You unmade the Dungeon Engine.";
            subtitle = "The curse is broken. The loop will run no more.";
        } else if (GameConst::kDemoBuild) {
            title    = "Thanks for playing the demo!";
            subtitle = "The full game: 50 floors, 9 classes, 3 difficulties, and the Grim Reaper.";
        } else {
            title    = "You conquered the Dungeon Engine.";
            subtitle = nullptr;
        }

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

        // Prompt — name the keys the handler actually accepts (Enter/Space/Jump/Confirm on kb, A on
        // pad); the old "press any key" was a lie (letter keys and mouse clicks did nothing).
        const char* prompt = "Press Enter or A to continue";
        f32 promptW = FontSystem::textWidth(prompt, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - promptW) * 0.5f, sh * 0.18f,
                             prompt, {0.5f, 0.5f, 0.5f}, 1);

        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return true;
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

        // Mouse hover highlight: m_deathHover is the option/button under the cursor (set in the
        // GAME_OVER input handler). Hovered rows render white; the layout here MUST match the
        // hit-tests in engine_update.cpp (deathOptionHit / deathConfirmHit).
        const Vec3 kHoverCol = {1.0f, 1.0f, 1.0f};
        if (m_menu.confirmQuit) {
            // "Are you sure?" overlay
            const char* confirmTxt = "Quit to menu?";
            f32 cW = FontSystem::textWidth(confirmTxt, 2);
            FontSystem::drawText(sw, sh, (sw - cW) * 0.5f, sh * 0.35f, confirmTxt, {1.0f, 0.8f, 0.3f}, 2);

            f32 cy = sh * 0.26f;
            f32 cx = static_cast<f32>(sw) * 0.5f;
            bool qp = Input::activeDeviceIsGamepad();
            HUD::drawKeySymbol(sw, sh, cx - 60.0f, cy, qp ? "A" : "Ent", true);
            FontSystem::drawText(sw, sh, cx - 30.0f, cy + 4.0f, "Yes",
                                 m_deathHover == 0 ? kHoverCol : Vec3{0.8f, 0.8f, 0.8f}, 1);
            HUD::drawKeySymbol(sw, sh, cx + 15.0f, cy, qp ? "B" : "Esc", true);
            FontSystem::drawText(sw, sh, cx + 43.0f, cy + 4.0f, "No",
                                 m_deathHover == 1 ? kHoverCol : Vec3{0.8f, 0.8f, 0.8f}, 1);
        } else {
            // Three options with key icons
            f32 cx = static_cast<f32>(sw) * 0.5f;
            f32 optY = sh * 0.35f;
            // Row spacing + offsets scale with resolution (like the pause menu already does): the
            // labels go through the scaling FontSystem, so a fixed 25 px row pitch let the rows
            // overlap and outgrow their click bands above 720p. deathOptionHit scales identically.
            const f32 s = static_cast<f32>(sh) / 720.0f;
            const f32 rowGap = 25.0f * s;

            bool pad = Input::activeDeviceIsGamepad();
            // Respawn is the preferred option — render it big (scale 2) and bright green so
            // players read it as "do this".
            HUD::drawKeySymbol(sw, sh, cx - 80.0f * s, optY, pad ? "A" : "Spc", true);
            FontSystem::drawText(sw, sh, cx - 50.0f * s, optY + 4.0f * s, "Respawn",
                                 m_deathHover == 0 ? kHoverCol : Vec3{0.4f, 1.0f, 0.55f}, 2);

            optY -= rowGap;
            // Only show "Reload last save" in singleplayer
            if (m_netRole == NetRole::NONE) {
                // "Tab", not "Ent": Enter respawns now. A prompt that names the wrong key is worse
                // than no prompt — it actively teaches the player to press the destructive one.
                HUD::drawKeySymbol(sw, sh, cx - 80.0f * s, optY, pad ? "X" : "Tab", true);
                FontSystem::drawText(sw, sh, cx - 50.0f * s, optY + 4.0f * s, "Reload last save",
                                     m_deathHover == 1 ? kHoverCol : Vec3{0.5f, 0.6f, 0.9f}, 1);
                optY -= rowGap;
            }
            HUD::drawKeySymbol(sw, sh, cx - 80.0f * s, optY, pad ? "-" : "Esc", true);
            FontSystem::drawText(sw, sh, cx - 50.0f * s, optY + 4.0f * s, "Quit to menu",
                                 m_deathHover == 2 ? kHoverCol : Vec3{0.7f, 0.4f, 0.4f}, 1);
        }

        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return true;
    }

    if (m_gameState != GameState::IN_GAME) {
        // Unknown / lobby states — flush and present
        HUD::flush(sw, sh);
        GLContext::swapBuffers(Window::getHandle());
        return true;
    }

    return false; // IN_GAME — let render() continue with the 3D scene
}

// ---------------------------------------------------------------------------
// selectPointLights — collects all light candidates (static + dynamic +
// projectile) and selects the nearest 4 to the camera, then uploads them
// to the shader via Renderer::setPointLights.
// ---------------------------------------------------------------------------
void Engine::selectPointLights() {
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
    // Active projectiles with non-zero lightColor. N4: on CLIENT the local ghost projectile
    // pool (m_projectiles) is frozen at spawn (ProjectileSystem::update is gated off), so
    // sourcing lights from it parks a glow at the muzzle of every local fire — looks like a
    // stuck "wrong direction" projectile while the snapshot-fed visible spark travels
    // correctly. Source from m_renderInterp.projectiles on CLIENT (same switch as the entity
    // light source at engine_render_entities.cpp:66 and the projectile sprite source at
    // engine_render_effects.cpp:66). Client::interpolateProjectiles reconstructs lightColor
    // from projFlags so glows still emit the right color after the swap.
    const ProjectilePool& lightPool = (m_netRole == NetRole::CLIENT)
                                      ? m_renderInterp.projectiles
                                      : m_projectiles;
    u32 plSeen = 0;
    for (u32 pi = 0; pi < MAX_PROJECTILES && candCount < MAX_CANDIDATES && plSeen < lightPool.activeCount; pi++) {
        const Projectile& p = lightPool.projectiles[pi];
        if (!p.active) continue;
        plSeen++;
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

// ---------------------------------------------------------------------------
// renderAuraDiscs — herald/buffed-enemy ground disc pass.
// Does its own Renderer::flush + blend state setup so it can draw additive
// alpha quads after the opaque world has been flushed.
// ---------------------------------------------------------------------------
void Engine::renderAuraDiscs(const EntityPool& entPool) {
    // Herald aura effect — golden rotating ring at the feet of the herald
    // and all aura-buffed enemies. Uses the quad mesh with blob texture + alpha.
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

// ---------------------------------------------------------------------------
// drawScreenQuad — shared helper for full-viewport overlays with a given shader.
// Sets up an ortho MVP that maps pixel coordinates to NDC, draws the unit
// quad mesh scaled to (sw × sh) pixels with the given RGBA color.
// Caller is responsible for enabling/disabling blend and depth-test as needed.
// ---------------------------------------------------------------------------
void Engine::drawScreenQuad(u32 sw, u32 sh, Vec4 rgba, const Shader& shader) {
    glUseProgram(shader.program);
    Mat4 ortho = Mat4::identity();
    ortho.m[0]  =  2.0f / static_cast<f32>(sw);
    ortho.m[5]  =  2.0f / static_cast<f32>(sh);
    ortho.m[10] = -1.0f;
    ortho.m[12] = -1.0f;
    ortho.m[13] = -1.0f;
    if (shader.loc_color >= 0)
        glUniform4f(shader.loc_color, rgba.x, rgba.y, rgba.z, rgba.w);

    // Scale a centered unit quad to fill the full viewport; UVs span 0..1 across it.
    Mat4 quadModel = Mat4::translate({static_cast<f32>(sw) * 0.5f,
                                      static_cast<f32>(sh) * 0.5f, 0.0f})
                   * Mat4::scale({static_cast<f32>(sw), static_cast<f32>(sh), 1.0f});
    if (shader.loc_mvp >= 0)
        glUniformMatrix4fv(shader.loc_mvp, 1, GL_FALSE, (ortho * quadModel).ptr());

    glBindVertexArray(m_quadMesh.vao);
    glDrawElements(GL_TRIANGLES, m_quadMesh.indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// renderPostOverlays — red damage feedback (edge-gradient vignette: per-hit
// fade + steady low-HP border glow, never a full-screen red sheet, never
// flashing). Called once per player INSIDE the split-screen loop with that
// player's viewport dims, so each player sees only their own feedback.
// ---------------------------------------------------------------------------
void Engine::renderPostOverlays(u32 sw, u32 sh) {
    // Red damage feedback — BioShock-style radial vignette (vignette.frag): red
    // blooms in from the corners/edges with a smooth falloff, center always clear.
    // Never a flat full-screen red sheet, never flashing. Combines the transient
    // per-hit fade (m_localPlayer.hurtVignette, set on each hit and decayed in
    // tickVisualFeedback) with a STEADY low-HP glow computed directly from current
    // HP. Because the low-HP term is a function of HP alone — no sine, no timer —
    // it never oscillates, so this is safe for photosensitivity (WCAG 2.3.1).
    // The combined intensity is passed to the shader as the quad's alpha.
    f32 hpFrac = (m_localPlayer.maxHealth > 0.0f)
               ? (m_localPlayer.health / m_localPlayer.maxHealth) : 1.0f;
    f32 lowHp = 0.0f;
    if (hpFrac > 0.0f && hpFrac < GameConst::LOW_HP_FRACTION)
        lowHp = (GameConst::LOW_HP_FRACTION - hpFrac) / GameConst::LOW_HP_FRACTION * 0.40f;   // 0 at threshold -> 0.40 near death, constant per frame
    f32 vig = fmaxf(m_localPlayer.hurtVignette, lowHp);
    if (vig > 0.60f) vig = 0.60f;
    if (vig > 0.0f) {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Deep red, intensity in alpha; the shader does the radial corner-weighted falloff.
        drawScreenQuad(sw, sh, {0.55f, 0.0f, 0.0f, vig}, m_vignetteShader);

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    // M13: big-divergence screen flash — black overlay that fades from 0.5 s back to 0.
    // Triggered in clientNetPost when prediction correction exceeds 10 m (distSq > 100).
    // Alpha is linear in remaining time: 1.0 at trigger, 0.0 at expiry.
    if (m_localPlayer.screenFlashTimer > 0.0f) {
        f32 flashAlpha = fminf(m_localPlayer.screenFlashTimer * 2.0f, 1.0f);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // Use the vignette shader for the full-screen quad; black with flashAlpha.
        drawScreenQuad(sw, sh, {0.0f, 0.0f, 0.0f, flashAlpha}, m_vignetteShader);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }
}

// Lazily (re)create an offscreen render target at w x h — a linear-filtered RGB colour texture
// + 24-bit depth renderbuffer. Backs the internal render-scale path (render the 3D scene low-res, then
// upscale-blit to the window) AND the character-inspect model panel. Recreated only when the size
// changes. A method (not a file-static) so engine_render_character.cpp can reuse it.
void Engine::ensureFbo(u32& fbo, u32& colorTex, u32& depthRbo, u32& curW, u32& curH, u32 w, u32 h) {
    if (fbo && curW == w && curH == h) return;
    if (!fbo)      glGenFramebuffers(1, &fbo);
    if (!colorTex) glGenTextures(1, &colorTex);
    if (!depthRbo) glGenRenderbuffers(1, &depthRbo);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (s32)w, (s32)h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (s32)w, (s32)h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        LOG_ERROR("FBO incomplete (%ux%u)", w, h); // shared by scene render-scale + inspect FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    curW = w; curH = h;
}

void Engine::render(f32 alpha) {
    // The town and the arena are OUTDOORS: their cells have no ceiling, so the clear color
    // IS the sky.
    if (m_level.inTown || m_level.inArena) glClearColor(0.47f, 0.65f, 0.88f, 1.0f);
    else                                   glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();

    // Non-IN_GAME states: render the appropriate full-screen screen and early-out
    if (renderTransitionScreens(sw, sh)) return;

    PROFILE_SCOPE(3, "Render");

    // Internal render-scale: when m_renderScale < 1, render the whole frame into an offscreen FBO at
    // baseW x baseH = scale * window, then upscale-blit it to the window after the player loop (below).
    // Cuts fragment fill/overdraw on the fill-bound Switch GPU while the upscale still fills the screen.
    // 1.0 = native (FBO bypassed). One FBO backs all split-screen viewports.
    // Gated to single-player: the Switch (the only place scale < 1) is single-player, and desktop
    // split-screen runs at scale 1.0 — so the native HUD pass below is simply sw x sh, no scaled
    // sub-viewport math.
    const bool useFbo = (m_renderScale < 0.999f) && (m_splitPlayerCount == 1);
    u32 baseW = sw, baseH = sh;
    if (useFbo) {
        baseW = (u32)((f32)sw * m_renderScale); if (baseW < 16) baseW = 16;
        baseH = (u32)((f32)sh * m_renderScale); if (baseH < 16) baseH = 16;
        ensureFbo(m_sceneFbo, m_sceneColorTex, m_sceneDepthRbo, m_sceneFboW, m_sceneFboH, baseW, baseH);
        glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFbo);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

#ifdef __SWITCH__
    // Distance fog ramps in over the last ~half of the clamped far plane so the far-plane cull fades
    // into the background color instead of hard-popping. Fog color matches the in-game clear color.
    // Tracks m_camera.farPlane, so the F6 / L+R3 A/B (60 / 70 / 200) moves the fog with it. Desktop
    // never calls setFog → fog stays off (start/end remain huge).
    Renderer::setFog({0.05f, 0.05f, 0.08f}, m_camera.farPlane * 0.5f, m_camera.farPlane * 0.95f);
#endif

    // Split-screen: render each player's view
    for (u8 sp = 0; sp < m_splitPlayerCount; sp++) {
    // Swap in this player's camera + state. Done unconditionally (even single-player) so
    // m_camera is always a fresh copy of the per-player store — the render interpolation
    // below mutates that transient copy, so repeated renders between ticks never compound.
    swapInPlayer(sp);

    // Route device-dependent HUD glyphs (button prompts, skill/quick bars) to THIS viewport's
    // player, so in couch co-op each player sees glyphs for the device they actually use instead of
    // the single global last-used device. No-op outside split-screen (lane -1 → global device).
    Input::setGlyphLane(m_splitPlayerCount > 1 ? static_cast<s8>(sp) : -1);

    // Compute viewport for this player from the base (post-Switch) dimensions.
    u32 vpX = 0, vpY = 0, vpW = baseW, vpH = baseH;
    if (m_splitPlayerCount > 1) {
        if (m_splitMode == 0) {
            // Horizontal split: P1=top, P2=bottom (GL viewport Y is bottom-origin)
            vpH = baseH / 2;
            vpY = (sp == 0) ? vpH : 0;
        } else {
            // Vertical split: P1=left, P2=right
            vpW = baseW / 2;
            vpX = (sp == 0) ? 0 : vpW;
        }
    }

    glViewport(vpX, vpY, vpW, vpH);
    glScissor(vpX, vpY, vpW, vpH);
    glEnable(GL_SCISSOR_TEST);

    // Only clear depth per player (color was cleared at top of render)
    if (sp > 0) glClear(GL_DEPTH_BUFFER_BIT);

    // Per-viewport camera interpolation between previous and current tick for smooth
    // gyro/look. Each player's prev-tick state rides in their swapped-in m_camera, so this
    // MUST run inside the loop: the old once-before-the-loop version was immediately
    // overwritten by swapInPlayer, so split-screen rendered raw, stuttery cameras (H2).
    if (m_gameState == GameState::IN_GAME) {
        Vec3 tickPos   = m_camera.position;
        f32  tickYaw   = m_camera.yaw;
        f32  tickPitch = m_camera.pitch;
        m_camera.position = m_camera.prevPosition + (tickPos - m_camera.prevPosition) * alpha;
        // Angle-aware yaw interpolation — handles ±π wrapping without snapping
        f32 yawDiff = tickYaw - m_camera.prevYaw;
        if (yawDiff >  3.14159f) yawDiff -= 6.28318f;
        if (yawDiff < -3.14159f) yawDiff += 6.28318f;
        m_camera.yaw   = m_camera.prevYaw + yawDiff * alpha;
        m_camera.pitch = m_camera.prevPitch + (tickPitch - m_camera.prevPitch) * alpha;
        // Screen shake offset — decays over time, applied to the transient render copy
        Vec3 shakeOffset = m_camera.shake.update(static_cast<f32>(FIXED_DT));
        m_camera.position = m_camera.position + shakeOffset;
    }

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
    if (m_level.inTown || m_level.inArena) {
        // DAYLIGHT: the town/arena are outdoors. Dungeon themes light ~8m around the player and
        // leave the rest black — under an open sky that reads as a void, so the sun does the
        // work: warm directional + high ambient, no darkness anywhere.
        Renderer::setDirectionalLight(
            normalize(Vec3{-0.35f, -1.0f, -0.25f}),
            Vec3{0.55f, 0.52f, 0.45f},
            Vec3{0.62f, 0.64f, 0.68f}
        );
    } else {
        Renderer::setDirectionalLight(
            normalize(Vec3{-0.3f, -1.0f, -0.5f}),
            theme->lightColor,
            theme->ambient
        );
    }

    // Send nearest 4 point lights to the shader
    selectPointLights();

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

    // Soft target lock-on is currently inert: lockActive is never set true (see
    // updateTargetLock in engine_combat.cpp), so the lock indicator UI was dead code
    // and has been removed (R7-6). The lockIndex/lockActive fields were also retired
    // from the wire in CV-4 (SnapPlayer no longer carries them) — only the in-memory
    // Player/NetPlayer fields remain so consumers compile until lock-on is brought back.

    DebugDraw::flush(m_camera.viewProjection);

    renderAuraDiscs(entPool);

    renderSpeechBubbles(vpW, vpH);
    renderDamageNumbers(vpW, vpH);

    // First-person viewmodel (hand + weapon) — drawn after world, before HUD
    renderViewmodel();

    // 3D scene is complete. If it was rendered into the low-res FBO, upscale-blit it to the window
    // now, switch to the native viewport, and draw the 2D HUD below at full resolution so UI/text stay
    // crisp (3D-only scaling). hudW/hudH carry the native dims (== vp dims when the FBO isn't used).
    u32 hudW = vpW, hudH = vpH;
    if (useFbo) {
        glDisable(GL_SCISSOR_TEST);   // glBlitFramebuffer is scissor-clipped: the per-player scissor is
                                      // still the SCALED viewport here, so without this the upscale only
                                      // fills that ~65% corner of the window instead of the whole screen.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, (s32)baseW, (s32)baseH, 0, 0, (s32)sw, (s32)sh,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, (s32)sw, (s32)sh);
        glScissor(0, 0, (s32)sw, (s32)sh);
        glEnable(GL_SCISSOR_TEST);    // re-enable (full-window) for the native HUD pass; loop disables it after
        hudW = sw; hudH = sh;
    }

    // Character-inspect screen: render the rotatable armored model into its offscreen FBO now (it
    // binds its own framebuffer + viewport and restores framebuffer 0 / the native viewport after),
    // so the 2D composite in renderCharacterInspect (driven from the HUD pass below) can sample it.
    // Done before the HUD pass and only for the active lane (the inspect screen is mouse-driven /
    // single-player; split-screen runs at render scale 1.0 so no FBO upscale is in flight here).
    if (m_characterScreenOpen && !m_hideHud) {
        renderInspectModelToFbo();
        // renderInspectModelToFbo restores framebuffer 0; re-assert the native HUD viewport/scissor
        // (it set the inspect-sized viewport internally) so the HUD pass below draws full-window.
        glViewport(0, 0, (s32)hudW, (s32)hudH);
        glScissor(0, 0, (s32)hudW, (s32)hudH);
        glEnable(GL_SCISSOR_TEST);
    }

    // Cinematic mode (F10) hides all HUD/overlay UI so F8 screenshots capture clean key art;
    // the 3D scene above still rendered normally. HUD::flush below stays UNCONDITIONAL so any
    // world-overlay verts queued earlier (speech bubbles / damage numbers) are emitted and the
    // shared HUD vertex buffer never leaks into the next frame.
    if (!m_hideHud) {
    renderHUD(hudW, hudH);

    // Interaction prompts (floor-descend / item-pickup) — native pass so the
    // button-glyph background projects through the correct HUD ortho (was drawn
    // in the scaled 3D pass, which dropped the background and left only the letter).
    renderInteractionPrompts(hudW, hudH);

    // Dead player overlay — shows "YOU DIED" on this player's viewport while game continues
    // In the arena, suppress the per-viewport death overlay once the match is decided: the winner
    // banner + final score table own the screen then, and "YOU DIED"/"Respawning in 3" (frozen,
    // since arenaTick early-returns while m_arenaOverTimer > 0) would draw overlapping it.
    if (m_playerDead[sp] && !(m_level.inArena && m_arenaOverTimer > 0.0f)) {
        // Dark overlay
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        const char* deathText = "YOU DIED";
        f32 dw = FontSystem::textWidth(deathText, 3);
        FontSystem::drawText(hudW, hudH, (hudW - dw) * 0.5f, hudH * 0.55f,
                             deathText, {0.8f, 0.1f, 0.1f}, 3);

        if (m_level.inArena) {
            // Arena: no manual respawn — show the auto-respawn countdown instead. The slot's
            // clock is authoritative on the host; a CLIENT ticks its own cosmetic copy
            // (arenaTick), so both read the same array.
            u8 rslot = (m_netRole == NetRole::CLIENT) ? activeNetSlot()
                                                      : m_localPlayerIndex;
            u32 secs = static_cast<u32>(ceilf(fmaxf(m_arenaRespawn[rslot], 0.0f)));
            char cd[40];
            if (secs > 0) std::snprintf(cd, sizeof(cd), "Respawning in %u...", secs);
            else          std::snprintf(cd, sizeof(cd), "Respawning...");
            f32 cw = FontSystem::textWidth(cd, 2);
            FontSystem::drawText(hudW, hudH, (hudW - cw) * 0.5f, hudH * 0.4f,
                                 cd, {0.9f, 0.75f, 0.3f}, 2);
        } else {
            bool pad = Input::activeDeviceIsGamepad();
            // Networked MP frees the cursor while dead (engine_update.cpp), so the prompt is
            // clickable; advertise that. Split-screen co-op keeps the cursor captured (key only).
            const char* respawnText = pad ? "Press A to respawn"
                                    : (m_netRole != NetRole::NONE) ? "Press Space or click to respawn"
                                    : "Press Space to respawn";
            f32 rw = FontSystem::textWidth(respawnText, 2);
            FontSystem::drawText(hudW, hudH, (hudW - rw) * 0.5f, hudH * 0.4f,
                                 respawnText, {0.7f, 0.7f, 0.7f}, 2);
        }

        glDisable(GL_BLEND);
    }

    // Arena match decided: winner banner + final table over the (frozen-scoring) live view,
    // on every peer, for the ~8 s before arenaTick tears down to the menu.
    if (m_level.inArena && m_arenaOverTimer > 0.0f) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        char winStr[32];
        std::snprintf(winStr, sizeof(winStr), "PLAYER %u WINS", m_arenaWinner + 1);
        f32 ww = FontSystem::textWidth(winStr, 4);
        FontSystem::drawText(hudW, hudH, (hudW - ww) * 0.5f, hudH * 0.62f,
                             winStr, {1.0f, 0.8f, 0.2f}, 4);
        f32 rowY = hudH * 0.5f;
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            // Same combatant test as the live score strip (engine_hud.cpp): a CLIENT never sets
            // m_players[].active, so source liveness from the interp mirror there and always include
            // the local slot — otherwise a guest's final table drops itself and every 0-kill loser.
            bool combatant = (i < m_splitPlayerCount) || (i == activeNetSlot()) ||
                             (m_netRole == NetRole::CLIENT ? m_renderInterp.playerActive[i]
                                                           : m_players[i].active) ||
                             m_arenaScore.kills[i] > 0;
            if (!combatant) continue;
            char row[32];
            std::snprintf(row, sizeof(row), "P%u  -  %u kills", i + 1,
                          static_cast<u32>(m_arenaScore.kills[i]));
            f32 rw2 = FontSystem::textWidth(row, 2);
            Vec3 col = (i == m_arenaWinner) ? Vec3{1.0f, 0.85f, 0.3f} : Vec3{0.7f, 0.7f, 0.7f};
            FontSystem::drawText(hudW, hudH, (hudW - rw2) * 0.5f, rowY, row, col, 2);
            rowY -= 26.0f;
        }
        const char* backStr = "Returning to the menu...";
        f32 bw = FontSystem::textWidth(backStr, 1);
        FontSystem::drawText(hudW, hudH, (hudW - bw) * 0.5f, hudH * 0.28f,
                             backStr, {0.6f, 0.6f, 0.6f}, 1);
        glDisable(GL_BLEND);
    }

    } // end if (!m_hideHud) — HUD/overlay UI block

    // Flush all accumulated HUD lines in a single draw call (was 36 per frame). Unconditional:
    // see the cinematic-mode note above.
    HUD::flush(hudW, hudH);

    // Per-player damage vignette / low-HP glow — drawn into THIS player's viewport
    // (scissor still enabled) from the swapped-in m_localPlayer, so each split-screen
    // player sees only their own feedback instead of P1's covering the whole window.
    if (!m_hideHud) {
        renderPostOverlays(hudW, hudH);
    }

    } // end split-screen player loop

    // Back to the global device for the shared overlays/menus drawn after the per-viewport loop.
    Input::setGlyphLane(-1);

    glDisable(GL_SCISSOR_TEST);

    glViewport(0, 0, sw, sh); // restore full viewport (the FBO upscale-blit now happens in-loop,
                              // before the native HUD pass, so the 3D scales but the HUD stays sharp)

    // Default the active aliases back to player 0 for any code that reads them outside
    // render. This also reloads m_camera from the persistent (tick-accurate) store, so the
    // visual-only interpolation applied above is discarded — no manual restore needed.
    swapInPlayer(0);

    if (m_gameState == GameState::IN_GAME && m_menu.optionsFromPause) {
        // Scrim first. The options screens are mostly thin text (the key-binding list especially) and
        // would be unreadable straight over a lit dungeon — but a fully opaque backdrop would hide the
        // run, which is the whole thing we are trying not to do. 0.80 alpha reads cleanly while the
        // scene stays plainly visible behind it.
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        drawScreenQuad(sw, sh, {0.02f, 0.02f, 0.04f, 0.80f}, m_dimShader);

        // renderMenu() draws no title on the options substates (3, 15-18), so what lands on top is
        // exactly the options UI and nothing else.
        renderMenu();
        HUD::flush(sw, sh);

        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    // Service a pending screenshot now (F8 or the --screenshot-interval auto-timer): the full frame
    // (3D scene, and HUD unless F10 hid it) is composited but not yet presented, so glReadPixels sees
    // exactly what's on screen. Written to the CWD as screenshot_NNNN.png using a monotonic counter
    // (m_frameCount resets every second, so it can't name files). Cleared immediately = one per shot.
    if (m_screenshotPending) {
        m_screenshotPending = false;
        // Timestamped name so sessions never overwrite each other's shots (the seq counter resets
        // per launch); the trailing seq disambiguates two captures in the same second.
        std::time_t t = std::time(nullptr);
        char ts[24];
        std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", std::localtime(&t));
        char path[64];
        std::snprintf(path, sizeof(path), "screenshot_%s_%02u.png", ts, ++m_screenshotSeq);
        Screenshot::capture(path, sw, sh);
    }

    // Options opened from the pause menu: draw the real options screens OVER the live (frozen) scene.
    //
    // Composited here, at the very end, so it lands on top of the finished frame — world, HUD and
    // all — and beneath nothing. The alternative was switching to GameState::MENU, but
    // renderTransitionScreens early-outs there and never draws the world, so the player would be
    // staring at the title backdrop with their run invisible behind it. A paused game should still
    // look like the game.
    //
    // A dim scrim goes down first: the options text is thin and would be unreadable over a lit
    // dungeon. renderMenu() draws no title on the options substates (3, 15-18), so what lands here
    // is exactly the options UI and nothing else.

    GLContext::swapBuffers(Window::getHandle());
}
