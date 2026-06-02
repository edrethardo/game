// Engine main-menu / lobby screen rendering, split from engine_hud.cpp

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
// Menu rendering (simple text-based using HUD lines)
// ---------------------------------------------------------------------------
void Engine::renderMenu() {
    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    FontSystem::setUIScale(uiScale);

    // Title text — hidden on the Single Player submenu (1), Host-mode chooser
    // (10), and the save-slot select screen (6) to keep them uncluttered.
    if (m_menu.subState != 1 && m_menu.subState != 6 && m_menu.subState != 10) {
        const char* title = "CURSE OF THE DUNGEON ENGINE";
        f32 titleW = FontSystem::textWidth(title, 3);
        f32 titleX = (static_cast<f32>(sw) - titleW) * 0.5f;
        f32 titleY = sh * 0.65f;
        FontSystem::drawText(sw, sh, titleX, titleY, title, {0.9f, 0.85f, 0.7f}, 3);
    }

    if (m_menu.subState == 10) {
        // Host-mode chooser — LAN-only (no UPnP) vs Online (UPnP IGD port mapping).
        // Mirrors subState 1's two-option layout. The description sub-line under each
        // option explains the difference so the player doesn't have to guess.
        const char* subTitle = "Host Game";
        f32 stW = FontSystem::textWidth(subTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.55f, subTitle, {0.2f, 0.9f, 0.2f}, 2);

        static const char* subLabels[] = {"LAN only", "Online"};
        static const char* subDescs[]  = {
            "Same-network friends only (no router changes)",
            "Open the port via UPnP so friends across the internet can join"
        };
        for (u32 i = 0; i < 2; i++) {
            f32 y = sh * 0.38f + (1 - i) * 50.0f * uiScale;
            bool sel = (i == m_menu.subSelection);
            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.4f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 250.0f * uiScale, 35.0f * uiScale, col, sel);
            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.6f, 0.6f, 0.6f};
            f32 tw = FontSystem::textWidth(subLabels[i], 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 10.0f * uiScale,
                                 subLabels[i], tc, 2);
        }

        // Sub-line description for the highlighted option.
        u8 hi = (m_menu.subSelection < 2) ? m_menu.subSelection : static_cast<u8>(0);
        const char* desc = subDescs[hi];
        f32 dw = FontSystem::textWidth(desc, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - dw) * 0.5f, sh * 0.30f,
                             desc, {0.7f, 0.7f, 0.8f}, 1);

        const char* hint = Input::isGamepadConnected(0)
            ? "D-pad, A to confirm, B to go back"
            : "Up/Down, Enter to confirm, ESC to go back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.15f, hint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 1) {
        // Single player sub-menu — replaces main menu options
        const char* subTitle = "Single Player";
        f32 stW = FontSystem::textWidth(subTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.55f, subTitle, {0.2f, 0.9f, 0.2f}, 2);

        // Any populated slot means "Continue" is available
        bool hasSave = false;
        for (u32 si = 0; si < MAX_SAVE_SLOTS; si++) {
            if (m_saveSlots[si].exists) { hasSave = true; break; }
        }

        static const char* subLabels[] = {"New Game", "Continue"};
        for (u32 i = 0; i < 2; i++) {
            f32 y = sh * 0.38f + (1 - i) * 50.0f * uiScale;
            bool sel = (i == m_menu.subSelection);
            bool available = (i == 0) || hasSave;
            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.4f, 0.2f};
            if (!available) col = {0.2f, 0.2f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 250.0f * uiScale, 35.0f * uiScale, col, sel && available);
            Vec3 tc = available ? (sel ? Vec3{1,1,1} : Vec3{0.6f,0.6f,0.6f}) : Vec3{0.35f,0.35f,0.35f};
            f32 tw = FontSystem::textWidth(subLabels[i], 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 10.0f * uiScale, subLabels[i], tc, 2);
        }

        const char* hint = Input::isGamepadConnected(0)
            ? "D-pad, A to confirm, B to go back"
            : "Up/Down, Enter to confirm, ESC to go back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.15f, hint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 2) {
        // Class selection screen
        const char* subTitle = "Choose Your Class";
        f32 stW = FontSystem::textWidth(subTitle, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.62f,
                             subTitle, {0.9f, 0.8f, 0.3f}, 3);

        u8 classCount = static_cast<u8>(PlayerClass::CLASS_COUNT);
        f32 listTop = sh * 0.54f;
        f32 spacing = 38.0f * uiScale;

        for (u8 i = 0; i < classCount; i++) {
            const ClassDef& cls = kClassDefs[i];
            f32 y = listTop - i * spacing;
            bool sel = (i == m_menu.subSelection);

            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.35f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 400.0f * uiScale, 32.0f * uiScale, col, sel);

            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.55f, 0.55f, 0.55f};
            char label[64];
            std::snprintf(label, sizeof(label), "%s  (%.0f HP, %.0f EN)", cls.name, cls.baseHealth, cls.baseEnergy);
            f32 tw = FontSystem::textWidth(label, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 9.0f * uiScale, label, tc, 2);
        }

        // Show selected class description and stats above the game title
        if (m_menu.subSelection < classCount) {
            const ClassDef& sel = kClassDefs[m_menu.subSelection];
            f32 descY = sh * 0.78f; // above title (at 0.65)

            // Description centered
            f32 descW = FontSystem::textWidth(sel.description, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - descW) * 0.5f, descY,
                                 sel.description, {0.7f, 0.7f, 0.8f}, 2);

            char statLine[80];
            std::snprintf(statLine, sizeof(statLine), "HP: %.0f  Speed: %.1f  Energy: %.0f  Weapon: %s",
                          sel.baseHealth, sel.baseMoveSpeed, sel.baseEnergy, sel.startingWeaponName);
            f32 statW = FontSystem::textWidth(statLine, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - statW) * 0.5f, descY - 24.0f * uiScale,
                                 statLine, {0.6f, 0.8f, 0.6f}, 2);
        }

        const char* hint2 = Input::isGamepadConnected(0)
            ? "D-pad to select, A to confirm, B to go back"
            : "Up/Down to select, Enter to confirm, ESC to go back";
        f32 hintW2 = FontSystem::textWidth(hint2, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW2) * 0.5f, sh * 0.06f, hint2, {0.4f, 0.4f, 0.5f}, 2);

        // Transient message (e.g. "save file incompatible")
        if (m_menu.msgTimer > 0.0f && m_menu.msg) {
            f32 alpha = fminf(m_menu.msgTimer, 1.0f); // fade out in last second
            f32 msgW = FontSystem::textWidth(m_menu.msg, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - msgW) * 0.5f, sh * 0.12f,
                                 m_menu.msg, {1.0f * alpha, 0.3f * alpha, 0.3f * alpha}, 2);
        }
    } else if (m_menu.subState == 4) {
        // Waiting for Player 2 join screen
        const char* p1Class = kClassDefs[static_cast<u32>(m_playerClasses[0])].name;
        char p1Str[64];
        std::snprintf(p1Str, sizeof(p1Str), "Player 1: %s", p1Class);
        f32 p1W = FontSystem::textWidth(p1Str, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - p1W) * 0.5f, sh * 0.55f,
                             p1Str, {0.3f, 1.0f, 0.4f}, 2);

        const char* waitText = "Player 2: Press A to join co-op";
        f32 waitW = FontSystem::textWidth(waitText, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - waitW) * 0.5f, sh * 0.42f,
                             waitText, {0.7f, 0.7f, 0.9f}, 2);

        bool pad = Input::isGamepadConnected(0);
        const char* soloText = pad ? "Press + to start solo" : "Press Enter to start solo";
        f32 soloW = FontSystem::textWidth(soloText, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - soloW) * 0.5f, sh * 0.32f,
                             soloText, {0.5f, 0.5f, 0.6f}, 2);

        const char* backText = pad ? "B to go back" : "ESC to go back";
        f32 backW = FontSystem::textWidth(backText, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - backW) * 0.5f, sh * 0.1f,
                             backText, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 5) {
        // Player 2 class selection
        const char* subTitle = "Player 2: Choose Your Class";
        f32 stW = FontSystem::textWidth(subTitle, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.62f,
                             subTitle, {0.3f, 0.7f, 1.0f}, 3);

        u8 classCount = static_cast<u8>(PlayerClass::CLASS_COUNT);
        f32 listTop = sh * 0.54f;
        f32 spacing = 38.0f * uiScale;
        for (u8 i = 0; i < classCount; i++) {
            const ClassDef& cls = kClassDefs[i];
            f32 y = listTop - i * spacing;
            bool sel = (i == m_menu.subSelection);
            Vec3 col = sel ? Vec3{0.3f, 0.6f, 1.0f} : Vec3{0.15f, 0.25f, 0.45f};
            HUD::drawMenuOption(sw, sh, y, 400.0f * uiScale, 32.0f * uiScale, col, sel);
            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.55f, 0.55f, 0.55f};
            char label[64];
            std::snprintf(label, sizeof(label), "%s  (%.0f HP, %.0f EN)", cls.name, cls.baseHealth, cls.baseEnergy);
            f32 tw = FontSystem::textWidth(label, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 9.0f * uiScale, label, tc, 2);
        }

        const char* hint = "D-pad to select, A to confirm";
        f32 hintW3 = FontSystem::textWidth(hint, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW3) * 0.5f, sh * 0.06f,
                             hint, {0.4f, 0.4f, 0.5f}, 2);
    // (subState 7 difficulty selection removed — difficulty is automatic per save)
    } else if (m_menu.subState == 3) {
        // Options / controls rebinding screen
        const char* subTitle = "Controls";
        f32 stW = FontSystem::textWidth(subTitle, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.9f,
                             subTitle, {0.9f, 0.8f, 0.3f}, 3);

        // Column headers
        f32 colAction = sw * 0.1f;
        f32 colKey    = sw * 0.5f;
        f32 colBtn    = sw * 0.72f;
        f32 headerY   = sh * 0.82f;
        FontSystem::drawText(sw, sh, colAction, headerY, "Action",     {0.7f, 0.7f, 0.7f}, 1);
        FontSystem::drawText(sw, sh, colKey,    headerY, "Keyboard",   {0.7f, 0.7f, 0.7f}, 1);
        FontSystem::drawText(sw, sh, colBtn,    headerY, "Controller", {0.7f, 0.7f, 0.7f}, 1);

        static constexpr u32 REBIND_COUNT = static_cast<u32>(GameAction::INVENTORY) + 1;
        static constexpr u32 OPT_STICK_SENS   = REBIND_COUNT;
        static constexpr u32 OPT_GYRO_SENS    = REBIND_COUNT + 1;
        static constexpr u32 OPT_STICK_INVERT = REBIND_COUNT + 2;
        static constexpr u32 OPT_GYRO_INVERT  = REBIND_COUNT + 3;
        static constexpr u32 OPT_SPLIT_MODE   = REBIND_COUNT + 4;
        static constexpr u32 OPT_RESET        = REBIND_COUNT + 5;
        static constexpr u32 TOTAL_OPTIONS     = REBIND_COUNT + 6;

        f32 listTop = sh * 0.78f;
        f32 lineH = 22.0f * uiScale;

        u32 visibleRows = static_cast<u32>((listTop - sh * 0.1f) / lineH);
        u32 scrollOffset = 0;
        if (m_menu.subSelection >= visibleRows) scrollOffset = m_menu.subSelection - visibleRows + 1;

        for (u32 i = scrollOffset; i < TOTAL_OPTIONS && i - scrollOffset < visibleRows; i++) {
            f32 y = listTop - (i - scrollOffset) * lineH;
            bool sel = (i == m_menu.subSelection);

            if (i < REBIND_COUNT) {
                GameAction act = static_cast<GameAction>(i);
                const char* name = Input::actionName(act);
                const InputBinding& bind = Input::getBinding(act);

                // Action name
                FontSystem::drawText(sw, sh, colAction, y, name,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);

                // Keyboard binding
                const char* keyName = (bind.key >= 0) ? SDL_GetScancodeName(static_cast<SDL_Scancode>(bind.key)) : "";
                // Mouse button name
                char keyBuf[32] = "";
                if (bind.key >= 0) std::snprintf(keyBuf, sizeof(keyBuf), "%s", keyName);
                if (bind.mouseButton == MOUSE_LEFT)   std::snprintf(keyBuf, sizeof(keyBuf), "LMB");
                if (bind.mouseButton == MOUSE_RIGHT)  std::snprintf(keyBuf, sizeof(keyBuf), "RMB");
                if (bind.mouseButton == MOUSE_MIDDLE) std::snprintf(keyBuf, sizeof(keyBuf), "MMB");

                Vec3 keyCol = sel && m_menu.bindKeyboard ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.5f, 0.5f, 0.5f};
                if (sel && m_menu.bindCapture && m_menu.bindKeyboard) keyCol = {1.0f, 0.5f, 0.2f};
                FontSystem::drawText(sw, sh, colKey, y, keyBuf[0] ? keyBuf : "-", keyCol, 1);

                // Controller binding
                char btnBuf[32] = "-";
                if (bind.button >= 0) {
                    if (bind.modifier >= 0) {
                        std::snprintf(btnBuf, sizeof(btnBuf), "%s+%s",
                            Input::buttonName(bind.modifier), Input::buttonName(bind.button));
                    } else {
                        std::snprintf(btnBuf, sizeof(btnBuf), "%s", Input::buttonName(bind.button));
                    }
                } else if (bind.axis >= 0) {
                    std::snprintf(btnBuf, sizeof(btnBuf), "%s",
                        bind.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? "ZR" :
                        bind.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT  ? "ZL" : "Axis");
                }
                Vec3 btnCol = sel && !m_menu.bindKeyboard ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.5f, 0.5f, 0.5f};
                if (sel && m_menu.bindCapture && !m_menu.bindKeyboard) btnCol = {1.0f, 0.5f, 0.2f};
                FontSystem::drawText(sw, sh, colBtn, y, btnBuf, btnCol, 1);
            } else if (i == OPT_STICK_SENS) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Stick Sensitivity: %.2f", Input::getStickSensitivity());
                FontSystem::drawText(sw, sh, colAction, y, buf,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBtn, y, "<  Left/Right  >", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == OPT_GYRO_SENS) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Gyro Sensitivity: %.1f", Input::getGyroSensitivity());
                FontSystem::drawText(sw, sh, colAction, y, buf,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBtn, y, "<  Left/Right  >", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == OPT_STICK_INVERT) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Stick Invert Y: %s", Input::getStickInvertY() ? "ON" : "OFF");
                FontSystem::drawText(sw, sh, colAction, y, buf,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBtn, y, "Enter to toggle", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == OPT_SPLIT_MODE) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Split Screen: %s", m_splitMode == 0 ? "Horizontal" : "Vertical");
                FontSystem::drawText(sw, sh, colAction, y, buf,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBtn, y, "Enter to toggle", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == OPT_GYRO_INVERT) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Gyro Invert Y: %s", Input::getGyroInvertY() ? "ON" : "OFF");
                FontSystem::drawText(sw, sh, colAction, y, buf,
                    sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBtn, y, "Enter to toggle", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == OPT_RESET) {
                FontSystem::drawText(sw, sh, colAction, y, "Reset to Defaults",
                    sel ? Vec3{1.0f, 0.4f, 0.4f} : Vec3{0.5f, 0.3f, 0.3f}, 1);
            }
        }

        // Hint text
        const char* hint = m_menu.bindCapture
            ? "Press a key/button to rebind, ESC to cancel"
            : "Up/Down select, Left/Right adjust, Enter rebind/toggle, ESC back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.04f,
                             hint, {0.5f, 0.5f, 0.6f}, 1);
    } else if (m_menu.subState == 6) {
        // Save-slot selection screen — shown for both New Game and Continue.
        bool isContinue = (m_menu.msg && m_menu.msg[0] == 'c');
        const char* screenTitle = isContinue ? "Select Save Slot — Continue" : "Select Save Slot — New Game";
        f32 stW = FontSystem::textWidth(screenTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.88f,
                             screenTitle, {0.9f, 0.8f, 0.3f}, 2);

        // Scroll window: show up to ~14 slots at once
        static constexpr u32 VISIBLE = 14;
        u32 scrollOffset = 0;
        if (m_menu.subSelection >= VISIBLE)
            scrollOffset = m_menu.subSelection - VISIBLE + 1;

        f32 listTop = sh * 0.82f;
        f32 lineH   = 28.0f * uiScale;

        for (u32 i = scrollOffset; i < MAX_SAVE_SLOTS && (i - scrollOffset) < VISIBLE; i++) {
            const SaveSlotInfo& info = m_saveSlots[i];
            f32 y   = listTop - static_cast<f32>(i - scrollOffset) * lineH;
            bool sel = (i == m_menu.subSelection);

            // Background highlight for the selected row
            Vec3 bgCol = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.12f, 0.3f, 0.15f};
            HUD::drawMenuOption(sw, sh, y - 2.0f * uiScale, sw * 0.7f, lineH - 4.0f * uiScale, bgCol, sel);

            char label[80];
            if (!info.exists) {
                std::snprintf(label, sizeof(label), "Slot %2u:  Empty", i + 1);
            } else {
                // Format play time as MM:SS
                u32 totalSec = static_cast<u32>(info.totalPlayTime);
                u32 mins = totalSec / 60;
                u32 secs = totalSec % 60;

                // Build class string (one or two players)
                const char* cls1 = (info.playerClasses[0] < static_cast<u8>(PlayerClass::CLASS_COUNT))
                    ? kClassDefs[info.playerClasses[0]].name : "?";

                if (info.playerCount >= 2 && info.playerClasses[1] != 0xFF &&
                    info.playerClasses[1] < static_cast<u8>(PlayerClass::CLASS_COUNT)) {
                    const char* cls2 = kClassDefs[info.playerClasses[1]].name;
                    std::snprintf(label, sizeof(label), "Slot %2u:  Floor %2u — %s + %s — %02u:%02u",
                                  i + 1, info.floor, cls1, cls2, mins, secs);
                } else {
                    std::snprintf(label, sizeof(label), "Slot %2u:  Floor %2u — %s — %02u:%02u",
                                  i + 1, info.floor, cls1, mins, secs);
                }
            }

            Vec3 textCol;
            if (!info.exists) {
                // Empty slot: dimmer, but still selectable for new game
                textCol = isContinue ? Vec3{0.3f, 0.3f, 0.3f} : (sel ? Vec3{0.8f, 0.8f, 0.8f} : Vec3{0.45f, 0.45f, 0.45f});
            } else {
                textCol = sel ? Vec3{1, 1, 1} : Vec3{0.6f, 0.7f, 0.6f};
            }

            f32 lw = FontSystem::textWidth(label, 1);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - lw) * 0.5f, y + 7.0f * uiScale, label, textCol, 1);
        }

        // Scroll indicator when list overflows
        if (MAX_SAVE_SLOTS > VISIBLE) {
            char scrollBuf[16];
            std::snprintf(scrollBuf, sizeof(scrollBuf), "%u / %u", m_menu.subSelection + 1, MAX_SAVE_SLOTS);
            FontSystem::drawText(sw, sh, sw * 0.88f, sh * 0.88f, scrollBuf, {0.5f, 0.5f, 0.6f}, 1);
        }

        const char* slotHint = Input::isGamepadConnected(0)
            ? "D-pad to select, A to confirm, B to go back"
            : "Up/Down to select, Enter/Click to confirm, ESC to go back";
        f32 hintW2 = FontSystem::textWidth(slotHint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW2) * 0.5f, sh * 0.04f,
                             slotHint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 7) {
        // Credits screen — scrolling text
        f32 cx = static_cast<f32>(sw) * 0.5f;
        f32 baseY = static_cast<f32>(sh) * 1.2f - m_menu.creditsScroll;
        f32 lineH = 20.0f * uiScale;
        f32 sectionGap = 40.0f * uiScale;

        static const struct { const char* text; f32 scale; Vec3 color; bool gap; } credits[] = {
            {"CURSE OF THE DUNGEON ENGINE", 3, {1.0f, 0.9f, 0.3f}, false},
            {"", 1, {0,0,0}, true},
            {"Developed by", 2, {0.7f, 0.7f, 0.8f}, false},
            {"Aaron (edrethardo)", 2, {1.0f, 1.0f, 1.0f}, false},
            {"", 1, {0,0,0}, true},
            {"--- Libraries ---", 2, {1.0f, 0.6f, 0.1f}, false},
            {"ENet - Lee Salzman (MIT)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"SDL2 - Sam Lantinga (Zlib)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"SDL_mixer - Sam Lantinga (Zlib)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"nlohmann/json - Niels Lohmann (MIT)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"stb_image - Sean Barrett (MIT/PD)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"glad - OpenGL Loader Generator", 1, {0.7f, 0.7f, 0.7f}, false},
            {"miniupnpc - Thomas Bernard (BSD-3)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"", 1, {0,0,0}, true},
            {"--- Audio ---", 2, {1.0f, 0.6f, 0.1f}, false},
            {"Kenney.nl - RPG Audio, Impacts, UI (CC0)", 1, {0.7f, 0.7f, 0.7f}, false},
            {"OpenGameArt.org - CC0 RPG SFX,", 1, {0.7f, 0.7f, 0.7f}, false},
            {"  Retro Synth, Swishes, Thwack,", 1, {0.7f, 0.7f, 0.7f}, false},
            {"  RPG Sound Pack, Magic Spell SFX", 1, {0.7f, 0.7f, 0.7f}, false},
            {"", 1, {0,0,0}, true},
            {"Built with love, C++17, and AI", 2, {0.5f, 0.8f, 1.0f}, false},
            {"", 1, {0,0,0}, true},
            {"Press ESC to return", 1, {0.4f, 0.4f, 0.5f}, false},
        };

        f32 y = baseY;
        for (const auto& line : credits) {
            if (line.gap) { y -= sectionGap; continue; }
            if (y > -30.0f && y < static_cast<f32>(sh) + 30.0f) {
                f32 tw = FontSystem::textWidth(line.text, line.scale);
                FontSystem::drawText(sw, sh, cx - tw * 0.5f, y, line.text, line.color, line.scale);
            }
            y -= lineH * line.scale;
        }
    } else if (m_menu.subState == 9) {
        // Host-IP entry — shown to joiners between Main Menu → Join and the New/Continue
        // chooser. Pure text-entry screen (no list, no mouse): the user types digits + dots
        // and presses Enter to advance. Cursor is rendered as a trailing block so the
        // edit point is obvious.
        const char* subTitle = "Enter Host IP";
        f32 stW = FontSystem::textWidth(subTitle, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.62f,
                             subTitle, {1.0f, 0.7f, 0.2f}, 3);

        // Editable text box — show the current connectAddress + a blinking cursor block.
        // We draw a chrome rect via drawMenuOption and overlay the text. If the buffer
        // is empty (just-cleared default), the cursor still shows so the user has a
        // visual anchor for "type here".
        f32 boxY = sh * 0.5f;
        f32 boxW = 480.0f * uiScale;
        f32 boxH = 36.0f  * uiScale;
        HUD::drawMenuOption(sw, sh, boxY, boxW, boxH, Vec3{0.15f, 0.25f, 0.35f}, true);

        // Build display string: address + blinking '_' cursor (~2 Hz).
        char display[80];
        bool cursorOn = (static_cast<u32>(Clock::getElapsedSeconds() * 2.0) & 1u) == 0u;
        std::snprintf(display, sizeof(display), "%s%s",
                      m_menu.connectAddress,
                      cursorOn ? "_" : " ");
        f32 tw = FontSystem::textWidth(display, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, boxY + 10.0f * uiScale,
                             display, {1.0f, 1.0f, 1.0f}, 2);

        const char* hint = "Type digits and dots, Backspace to delete, Enter to confirm, ESC to cancel";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.12f, hint,
                             {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 8) {
        // Overwrite save confirmation — same style as singleplayer sub-menu
        char owTitle[64];
        std::snprintf(owTitle, sizeof(owTitle), "Overwrite Slot %u?", m_menu.overwriteSlot + 1);
        f32 stW = FontSystem::textWidth(owTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.55f, owTitle, {0.9f, 0.3f, 0.3f}, 2);

        static const char* owLabels[] = {"Yes", "No"};
        for (u32 i = 0; i < 2; i++) {
            f32 y = sh * 0.38f + (1 - i) * 50.0f * uiScale;
            bool sel = (i == m_menu.subSelection);
            Vec3 col = sel ? Vec3{0.9f, 0.3f, 0.3f} : Vec3{0.35f, 0.15f, 0.15f};
            HUD::drawMenuOption(sw, sh, y, 250.0f * uiScale, 35.0f * uiScale, col, sel);
            Vec3 tc = sel ? Vec3{1,1,1} : Vec3{0.6f,0.6f,0.6f};
            f32 tw = FontSystem::textWidth(owLabels[i], 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 10.0f * uiScale, owLabels[i], tc, 2);
        }

        const char* hint = "Up/Down to select, Enter to confirm, ESC to go back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.15f, hint, {0.4f, 0.4f, 0.5f}, 1);
    } else {
        // Main menu options
        static const char* labels[] = {"Single Player", "Host Game", "Join Game", "Options", "Credits", "Exit Game"};
        Vec3 colors[] = {
            {0.2f, 0.9f, 0.2f},
            {0.2f, 0.5f, 1.0f},
            {1.0f, 0.7f, 0.2f},
            {0.6f, 0.6f, 0.8f},
            {1.0f, 0.6f, 0.1f},   // orange for Credits
            {0.7f, 0.2f, 0.2f},
        };

        for (u32 i = 0; i < 6; i++) {
            f32 y = sh * 0.2f + (5 - i) * 50.0f * uiScale;
            Vec3 color = colors[i];
            bool selected = (i == m_menu.selection);
            if (!selected) color = color * 0.4f;
            HUD::drawMenuOption(sw, sh, y, 250.0f * uiScale, 35.0f * uiScale, color, selected);

            f32 textW = FontSystem::textWidth(labels[i], 2);
            f32 textX = (static_cast<f32>(sw) - textW) * 0.5f;
            FontSystem::drawText(sw, sh, textX, y + 10.0f * uiScale, labels[i],
                selected ? Vec3{1,1,1} : Vec3{0.6f,0.6f,0.6f}, 2);
        }

        const char* hint = Input::isGamepadConnected(0)
            ? "D-pad to select, A to confirm"
            : "Up/Down to select, Enter to confirm";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.1f, hint, {0.4f, 0.4f, 0.5f}, 1);
    }
}

void Engine::renderLobby() {
    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();
    HUD::drawCrosshair(sw, sh, {0.3f, 0.3f, 1.0f});
}
