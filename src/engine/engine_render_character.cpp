// engine_render_character.cpp — the character INSPECT screen (C key / LB+R3).
//
// Two halves, both driven from render() while the menu's CHARACTER page is up (characterTabUp()):
//   1. renderInspectModelToFbo() — draws the player's class body mesh + equipped weapon/armor
//      (via submitPlayerEquipment) into an offscreen FBO (m_inspectColorTex) using an orbit camera
//      that spins with m_inspectYaw. Called once per frame BEFORE the 2D HUD pass; it binds its own
//      framebuffer + viewport and restores framebuffer 0 afterwards.
//   2. renderCharacterInspect() — a 2D overlay: a dark backdrop, the FBO texture composited as a
//      square panel on the left, and a grouped OFFENSE/DEFENSE/UTILITY stat sheet on the right.
//      Called from renderHUD()'s branch (engine_hud.cpp) so it draws to the window (framebuffer 0).
//
// Fits the "data-driven hybrid" render path: it reuses Renderer::submit/flush and the existing
// submitPlayerEquipment body-attach math (engine_render_world.cpp) rather than duplicating draw code.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
#include "platform/window.h"
#include "renderer/renderer.h"
#include "renderer/camera.h"
#include "renderer/material.h"
#include "renderer/font.h"
#include "renderer/shader.h"
#include "game/player.h"
#include "game/item.h"
#include "game/inventory_ui.h"   // menuFrameLayout — this page lays out inside the shared menu frame
#include "game/weapon.h"
#include "core/math.h"

#include <glad/glad.h>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// renderInspectModelToFbo — render the class body + equipment into m_inspectColorTex.
// ---------------------------------------------------------------------------
void Engine::renderInspectModelToFbo() {
    // Inspect model render target. Switch's GPU is far weaker, so render the
    // model at a lower resolution (it's upscaled into the panel either way).
#ifdef __SWITCH__
    constexpr u32 kInspectFboSize = 320;
#else
    constexpr u32 kInspectFboSize = 512;
#endif
    const u32 size = kInspectFboSize;
    ensureFbo(m_inspectFbo, m_inspectColorTex, m_inspectDepthRbo, m_inspectFboW, m_inspectFboH, size, size);
    if (!m_inspectFbo) return;

    glBindFramebuffer(GL_FRAMEBUFFER, m_inspectFbo);
    glViewport(0, 0, (s32)size, (s32)size);
    glDisable(GL_SCISSOR_TEST);                 // the inspect FBO is full-target, no per-player scissor
    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);    // dark slate so the model reads against it
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    // FIXED front camera looking at the model's mid-torso. The MODEL spins (bodyModel +
    // submitPlayerEquipment both rotateY(m_inspectYaw)); the camera must NOT also orbit by
    // m_inspectYaw or the two cancel — the camera would track the same face and the model would
    // appear static while only the world-fixed light slides across it. Built as a standalone Camera
    // so Renderer::beginFrame consumes its viewProjection; matrices set directly via lookAt/perspective.
    const Vec3 target = {0.0f, 0.9f, 0.0f};
    Camera cam;
    cam.position = target + Vec3{ 0.0f, 0.35f, 2.6f };
    cam.view = Mat4::lookAt(cam.position, target, {0.0f, 1.0f, 0.0f});
    cam.projection = Mat4::perspective(radians(50.0f), 1.0f, 0.1f, 20.0f);
    cam.viewProjection = cam.projection * cam.view;
    // Light direction is set globally for the lit shader; reuse a soft front-key so the model
    // doesn't sit in shadow when it spins to face away from the world's directional light.
    Renderer::setDirectionalLight(normalize(Vec3{-0.4f, -0.7f, -0.6f}),
                                  {1.0f, 0.97f, 0.9f}, {0.30f, 0.30f, 0.34f});
    Renderer::beginFrame(cam);

    // Resolve the class body mesh + material the same way the remote/split-screen render does,
    // with the "human" / "human_skin" fallback for an unbuilt class asset.
    const ClassDef& cd = kClassDefs[static_cast<u32>(m_playerClass)];
    u8 classMesh = findMeshByName(cd.meshName);
    u8 classMat  = MaterialSystem::getIdByName(cd.materialName);
    if (classMesh == 0) classMesh = findMeshByName("human");
    if (classMat  == 0) classMat  = MaterialSystem::getIdByName("human_skin");

    // Normalize the body to a 1.8 m height (same target as the world render) so every class frames
    // identically regardless of its mesh's authored scale.
    f32 targetH = 1.8f;
    f32 meshH = (classMesh > 0) ? (m_meshDefs[classMesh].bounds.max.y - m_meshDefs[classMesh].bounds.min.y) : 1.0f;
    f32 scale = (meshH > 0.001f) ? (targetH / meshH) : 1.0f;

    const Texture& defaultTex = MaterialSystem::get(0)->texture;
    const Material* classMatPtr = MaterialSystem::get(classMat);
    Texture classTex = classMatPtr ? classMatPtr->texture : defaultTex;
    Vec4 classTint = classMatPtr ? classMatPtr->tint : Vec4{1, 1, 1, 1};

    Mat4 bodyModel = Mat4::translate({0, 0, 0}) * Mat4::rotateY(m_inspectYaw)
                   * Mat4::scale({scale, scale, scale});
    AABB bodyBounds = {Vec3{-0.5f, 0.0f, -0.5f}, Vec3{0.5f, 1.9f, 0.5f}};
    if (classMesh > 0 && m_meshDefs[classMesh].mesh.vao) {
        Renderer::submit(m_basicShader, classTex, m_meshDefs[classMesh].mesh,
                         bodyModel, bodyBounds, classTint);
    }

    // Equipment overlays at the same scaled body origin. submitPlayerEquipment applies its own
    // body-relative offsets * scale, so passing the same scale keeps the armor attached correctly.
    submitPlayerEquipment({0, 0, 0}, m_inspectYaw, scale, /*anim*/ 0,
                          classMesh, m_inventories[m_localPlayerIndex]);

    // Emit everything into the FBO.
    Renderer::flush();

    // Restore: bind the window framebuffer. The caller (render()) re-asserts the native HUD
    // viewport/scissor; we leave the GL clear color as render() sets its own at the top of each frame.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------
// Textured-quad helper — composite an arbitrary GL texture id into a screen rect.
// Uses the unlit shader path (loc_mvp/loc_color/loc_texture0) and an interleaved pos3+uv2 VBO.
// flipV=true samples the GL texture bottom-up (FBO color targets are bottom-origin) so the model
// isn't drawn upside down. vao/vbo are engine-owned (m_inspectQuadVao/m_inspectQuadVbo); they
// are lazily created on first call and freed in Engine::shutdown() in engine_init.cpp.
// ---------------------------------------------------------------------------
static void drawTexturedQuad(const Shader& shader, u32 sw, u32 sh,
                             f32 x, f32 y, f32 w, f32 h, u32 texHandle, Vec4 tint, bool flipV,
                             u32& vao, u32& vbo) {
    // Lazily create the unit quad VAO/VBO on first use; Engine::shutdown frees them.
    if (!vao) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, 6 * 5 * sizeof(f32), nullptr, GL_DYNAMIC_DRAW);
        // Interleaved pos3 + uv2. The unlit shader's layout is (0=pos, 1=normal, 2=uv): location 1
        // (normal) is unused here, so UV MUST bind to location 2 — same as ItemIconSystem's quad.
        glEnableVertexAttribArray(0); // pos3 at location 0
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(f32), (void*)0);
        glEnableVertexAttribArray(2); // uv2 at location 2 (skips the unused normal slot)
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(f32), (void*)(3 * sizeof(f32)));
        glBindVertexArray(0);
    }

    // Column-major ortho mapping pixels -> NDC (same convention as FontSystem/ItemIconSystem).
    Mat4 ortho = Mat4::identity();
    ortho.m[0]  =  2.0f / (f32)sw;
    ortho.m[5]  =  2.0f / (f32)sh;
    ortho.m[10] = -1.0f;
    ortho.m[12] = -1.0f;
    ortho.m[13] = -1.0f;

    // V coords: top/bottom selected by flipV so a bottom-origin FBO renders right-way-up.
    f32 vTop = flipV ? 1.0f : 0.0f;
    f32 vBot = flipV ? 0.0f : 1.0f;
    f32 verts[] = {
        x,     y,     0.0f, 0.0f, vBot,
        x + w, y,     0.0f, 1.0f, vBot,
        x + w, y + h, 0.0f, 1.0f, vTop,
        x,     y,     0.0f, 0.0f, vBot,
        x + w, y + h, 0.0f, 1.0f, vTop,
        x,     y + h, 0.0f, 0.0f, vTop,
    };

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(shader.program);
    if (shader.loc_mvp >= 0)   glUniformMatrix4fv(shader.loc_mvp, 1, GL_FALSE, ortho.ptr());
    if (shader.loc_color >= 0) glUniform4f(shader.loc_color, tint.x, tint.y, tint.z, tint.w);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texHandle);
    if (shader.loc_texture0 >= 0) glUniform1i(shader.loc_texture0, 0);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

// ---------------------------------------------------------------------------
// renderCharacterInspect — 2D composite: backdrop + model panel + stats sheet.
// ---------------------------------------------------------------------------
void Engine::renderCharacterInspect(u32 sw, u32 sh) {
    const f32 fsw = (f32)sw, fsh = (f32)sh;
    const f32 uiScale = fsh / 720.0f; // matches the rest of the HUD's resolution scaling

    // The default material's white texture, used as a solid-fill source for the panel rects.
    const Texture& whiteTex = MaterialSystem::get(0)->texture;

    // Everything on this page is laid out inside the menu frame's CONTENT area, which the shared
    // chrome (HUD::drawMenuChrome) has already painted and bordered. This page used to draw its
    // own full-screen backdrop; doing that now would paint over the tab strip and make the page
    // unleavable by mouse.
    const InventoryUI::MenuFrameRects mf = InventoryUI::menuFrameLayout(sw, sh);

    // --- Model panel: a square on the left of the content area, vertically centred ---
    f32 panel = mf.contentH * 0.80f;
    if (panel > mf.contentW * 0.40f) panel = mf.contentW * 0.40f;   // never crowd the stats sheet
    f32 panelX = mf.contentX + mf.contentW * 0.045f;
    f32 panelY = mf.contentY + (mf.contentH - panel) * 0.5f;
    // Frame: a slightly larger dark rect behind the model for separation.
    f32 pad = 6.0f * uiScale;
    drawTexturedQuad(m_unlitShader, sw, sh, panelX - pad, panelY - pad,
                     panel + 2 * pad, panel + 2 * pad, whiteTex.handle,
                     {0.10f, 0.11f, 0.14f, 0.95f}, false,
                     m_inspectQuadVao, m_inspectQuadVbo);
    // The FBO model itself (flipV: FBO color targets are bottom-origin).
    drawTexturedQuad(m_unlitShader, sw, sh, panelX, panelY, panel, panel,
                     m_inspectColorTex, {1, 1, 1, 1}, true,
                     m_inspectQuadVao, m_inspectQuadVbo);

    // --- Stats sheet on the right ---
    // Gather the live numbers. getEffectiveWeapon merges the equipped weapon + affixes; m_weaponDefs[0]
    // is the unarmed fallback (same call site as the dodge-riposte callback).
    const PlayerInventory& inv = m_inventories[m_localPlayerIndex];
    WeaponDef wpn = Inventory::getEffectiveWeapon(inv, m_itemDefs, m_weaponDefs[0]);

    f32 dmg        = wpn.damage;
    f32 cd         = (wpn.cooldown > 0.001f) ? wpn.cooldown : 0.001f;
    f32 attackSpd  = 1.0f / cd;
    f32 critChance = wpn.critChance;
    f32 critMult   = wpn.critMult;
    // Expected DPS: per-hit damage scaled by the average crit multiplier, divided by cooldown.
    f32 dps        = dmg * (1.0f + (critMult - 1.0f) * critChance) / cd;

    f32 ar         = Inventory::armorRating(inv);
    f32 armorMitig = 100.0f * ar / (ar + 100.0f);      // diminishing-returns mitigation %
    f32 regen      = Inventory::healthRegenRate(inv);
    f32 lifesteal  = Inventory::lifestealPct(inv);
    f32 thorns     = Inventory::thornsPct(inv);
    f32 manasteal  = Inventory::manastealPct(inv);
    f32 manaPerKill = Inventory::manaOnKill(inv);
    f32 maxEnergy  = m_skillStates[m_localPlayerIndex].maxEnergy;

    // Layout: a column on the right half. NOTE: FontSystem's ortho is BOTTOM-ORIGIN (y grows
    // UPWARD on screen — see font.cpp), so the sheet starts HIGH and each row DECREMENTS y to read
    // top-to-bottom. Text scales via setUIScale (set by renderHUD before this call); integer font
    // sizes are 1=body, 2=header, 3=title.
    // Anchored to the frame's content box, not to the raw screen: the sheet used to start at
    // 0.86*sh, which is above the frame's tab strip.
    const f32 sheetX = mf.contentX + mf.contentW * 0.50f;
    f32 colX  = sheetX;
    f32 colX2 = sheetX + mf.contentW * 0.33f;   // value column anchor
    f32 y     = mf.contentY + mf.contentH;      // top of the sheet (high y == near top)
    // Row pitch is DERIVED from the space available rather than a fixed 26 px: the sheet is
    // 3 headers + 15 rows + 3 spacers deep, which at the old constant ran past the frame's bottom
    // edge and clipped the last row off. Clamped so it never stretches absurdly on a tall window.
    const f32 rowsTall = 15.0f + 3.0f * 1.4f + 3.0f * 0.5f + 1.4f;   // rows + headers + gaps
    f32 lineH = (mf.contentH - 10.0f * uiScale) / rowsTall;
    if (lineH > 26.0f * uiScale) lineH = 26.0f * uiScale;
    f32 hdrH  = lineH * 1.4f;

    const Vec3 headerCol = {0.95f, 0.85f, 0.45f};
    const Vec3 labelCol  = {0.75f, 0.78f, 0.85f};
    const Vec3 valueCol  = {1.00f, 1.00f, 1.00f};
    char buf[64];

    auto header = [&](const char* text) {
        y -= hdrH;
        FontSystem::drawText(sw, sh, colX, y, text, headerCol, 2);
    };
    auto row = [&](const char* label, const char* value) {
        y -= lineH;
        FontSystem::drawText(sw, sh, colX,  y, label, labelCol, 1);
        FontSystem::drawText(sw, sh, colX2, y, value, valueCol, 1);
    };

    // Title
    y -= hdrH * 1.4f;
    FontSystem::drawText(sw, sh, colX, y, kClassDefs[static_cast<u32>(m_playerClass)].name,
                         {0.9f, 0.95f, 1.0f}, 3);
    y -= lineH * 0.6f;

    // OFFENSE
    header("OFFENSE");
    std::snprintf(buf, sizeof(buf), "%.0f", dmg);                 row("Damage", buf);
    std::snprintf(buf, sizeof(buf), "%.2f/s", attackSpd);         row("Attack Speed", buf);
    std::snprintf(buf, sizeof(buf), "%.0f%%  x%.1f", critChance * 100.0f, critMult);
                                                                  row("Crit", buf);
    std::snprintf(buf, sizeof(buf), "%.1f", dps);                 row("DPS", buf);
    y -= lineH * 0.5f;

    // DEFENSE
    header("DEFENSE");
    std::snprintf(buf, sizeof(buf), "%.0f / %.0f", m_localPlayer.health, m_localPlayer.maxHealth);
                                                                  row("Health", buf);
    std::snprintf(buf, sizeof(buf), "%.0f  (%.0f%%)", ar, armorMitig);
                                                                  row("Armor", buf);
    std::snprintf(buf, sizeof(buf), "%.0f%%", m_localPlayer.damageReduction * 100.0f);
                                                                  row("Dmg Reduction", buf);
    std::snprintf(buf, sizeof(buf), "%.1f/s", regen);             row("Regen", buf);
    std::snprintf(buf, sizeof(buf), "%.1f%%", lifesteal);         row("Lifesteal", buf);
    std::snprintf(buf, sizeof(buf), "%.0f%%", thorns);            row("Thorns", buf);
    y -= lineH * 0.5f;

    // UTILITY
    header("UTILITY");
    std::snprintf(buf, sizeof(buf), "%.1f m/s", m_localPlayer.moveSpeed);
                                                                  row("Move Speed", buf);
    std::snprintf(buf, sizeof(buf), "%.0f%%", inv.bonusCooldownReduction * 100.0f);
                                                                  row("Cooldown Reduction", buf);
    std::snprintf(buf, sizeof(buf), "%.0f", maxEnergy);           row("Energy", buf);
    std::snprintf(buf, sizeof(buf), "%.1f%%", manasteal);         row("Mana Steal", buf);
    std::snprintf(buf, sizeof(buf), "%.0f", manaPerKill);         row("Mana / Kill", buf);

    // Footer hint along the bottom of the content box. Names the LEFT-DRAG explicitly: the page
    // no longer spins the model on bare pointer motion, so "drag" is now literal.
    // "C" was wrong and inherited from the pre-quickbar days: C is QUICKBAR_SLOT_3 now, and the
    // character screen is bound to T (with K as an alias). Naming a key that does something
    // ELSE is the worst kind of hint.
    const char* footer = "Hold left mouse to rotate  -  T or Esc to close";
    f32 fw = FontSystem::textWidth(footer, 1);
    FontSystem::drawText(sw, sh, mf.contentX + (mf.contentW - fw) * 0.5f,
                         mf.contentY + 6.0f * uiScale, footer, {0.5f, 0.52f, 0.60f}, 1);
}
