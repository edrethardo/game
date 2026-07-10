// Engine main-menu / lobby screen rendering, split from engine_hud.cpp

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
#include "platform/window.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "platform/steam.h"
#include "audio/audio.h"
#include <cstdio>
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
#include "engine/menu_osk.h"   // shared on-screen-keyboard layout (controller IP entry)
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


// Draws one rebindable action's label + its SINGLE binding column, for the Keyboard & Mouse
// (keyboardMode=true → key/mouse name) and Controller (false → button/modifier/axis name) options
// submenus. `capturing` tints the value orange while this row is being rebound. Lifts the old
// two-column formatters into one place so both submenus share the 19-action rendering.
static void drawRebindRow(u32 sw, u32 sh, f32 colLabel, f32 colBind, f32 y,
                          GameAction act, bool sel, bool capturing, bool keyboardMode) {
    FontSystem::drawText(sw, sh, colLabel, y, Input::actionName(act),
        sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);

    const InputBinding& bind = Input::getBinding(act);
    char buf[32] = "-";
    if (keyboardMode) {
        if (bind.key >= 0)
            std::snprintf(buf, sizeof(buf), "%s", SDL_GetScancodeName(static_cast<SDL_Scancode>(bind.key)));
        if (bind.mouseButton == MOUSE_LEFT)   std::snprintf(buf, sizeof(buf), "LMB");
        if (bind.mouseButton == MOUSE_RIGHT)  std::snprintf(buf, sizeof(buf), "RMB");
        if (bind.mouseButton == MOUSE_MIDDLE) std::snprintf(buf, sizeof(buf), "MMB");
    } else {
        if (bind.button >= 0) {
            if (bind.modifier >= 0)
                std::snprintf(buf, sizeof(buf), "%s+%s", Input::buttonName(bind.modifier), Input::buttonName(bind.button));
            else
                std::snprintf(buf, sizeof(buf), "%s", Input::buttonName(bind.button));
        } else if (bind.axis >= 0) {
            std::snprintf(buf, sizeof(buf), "%s",
                bind.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? "ZR" :
                bind.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT  ? "ZL" : "Axis");
        }
    }
    Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.5f, 0.5f, 0.5f};
    if (capturing) col = {1.0f, 0.5f, 0.2f};  // orange while awaiting the new binding
    FontSystem::drawText(sw, sh, colBind, y, buf, col, 1);
}

// ---------------------------------------------------------------------------
// Menu rendering (simple text-based using HUD lines)
// ---------------------------------------------------------------------------
void Engine::renderMenu() {
    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();
    f32 uiScale = static_cast<f32>(sh) / 720.0f;
    FontSystem::setUIScale(uiScale);

    // Title text — hidden on screens that fill the vertical space with their own layout: the
    // Single Player submenu (1), Host-mode chooser (10), save-slot select (6), and all of the
    // options screens (3 category list + 15/16/17/18 submenus) so it doesn't bleed over their lists.
    if (m_menu.subState != 1 && m_menu.subState != 6 && m_menu.subState != 10 &&
        m_menu.subState != 3 && m_menu.subState != 15 && m_menu.subState != 16 &&
        m_menu.subState != 17 && m_menu.subState != 18) {
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

        const char* hint = Input::activeDeviceIsGamepad()
            ? "D-pad, A to confirm, B to go back"
            : "Up/Down, Enter to confirm, ESC to go back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.15f, hint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 19) {
        // Steam join chooser (P3): Quick Join / Browse Games / Enter IP.
        const char* subTitle = "Join Game";
        f32 stW = FontSystem::textWidth(subTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.60f, subTitle, {0.2f, 0.9f, 0.2f}, 2);
        static const char* jLabels[] = {"Quick Join", "Browse Games", "Enter IP (LAN)"};
        for (u32 i = 0; i < 3; i++) {
            f32 y = sh * 0.30f + (2 - i) * 50.0f * uiScale;
            bool sel = (i == m_menu.subSelection);
            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.4f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 250.0f * uiScale, 35.0f * uiScale, col, sel);
            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.6f, 0.6f, 0.6f};
            f32 tw = FontSystem::textWidth(jLabels[i], 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 10.0f * uiScale, jLabels[i], tc, 2);
        }
        const char* hint = Input::activeDeviceIsGamepad() ? "D-pad, A to confirm, B to go back"
                                                          : "Up/Down, Enter to confirm, ESC to go back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.15f, hint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 20) {
        // Steam public lobby browser (P3): list of open games.
        const char* subTitle = "Public Games";
        f32 stW = FontSystem::textWidth(subTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.78f, subTitle, {0.2f, 0.9f, 0.2f}, 2);
        int n = Steam::lobbyListCount();
        if (n <= 0) {
            const char* none = "No public games found — press B / ESC to go back";
            f32 nw = FontSystem::textWidth(none, 1);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - nw) * 0.5f, sh * 0.50f, none, {0.7f, 0.7f, 0.8f}, 1);
        } else {
            int shown = n > 8 ? 8 : n;
            for (int i = 0; i < shown; i++) {
                char nm[64]; int mc = 0, mm = 0;
                Steam::lobbyListEntry(i, nm, sizeof(nm), &mc, &mm);
                // mc = authoritative in-game roster (host-published "players"); mm = lobby member limit.
                // Guard the denominator so a lobby that reports a 0 limit still shows the real cap, not "/0".
                if (mm <= 0) mm = static_cast<int>(MAX_PLAYERS);
                char row[96]; std::snprintf(row, sizeof(row), "%s  (%d/%d)", nm, mc, mm);
                bool sel = (i == m_steamBrowserSel);
                f32 y = sh * 0.62f - i * 34.0f * uiScale;
                Vec3 tc = sel ? Vec3{1.0f, 1.0f, 0.5f} : Vec3{0.6f, 0.6f, 0.6f};
                f32 tw = FontSystem::textWidth(row, 1);
                FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y, row, tc, 1);
            }
        }
        const char* hint = Input::activeDeviceIsGamepad() ? "D-pad, A to join, B to go back"
                                                          : "Up/Down, Enter to join, ESC to go back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.10f, hint, {0.4f, 0.4f, 0.5f}, 1);
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

        const char* hint = Input::activeDeviceIsGamepad()
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

        const char* hint2 = Input::activeDeviceIsGamepad()
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

        bool pad = Input::activeDeviceIsGamepad();
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
        // Options — top-level category list (modeled on the couch-start list, subState 13).
        // The game title is suppressed for this screen, so "Options" is the heading.
        const char* title = "Options";
        f32 tW = FontSystem::textWidth(title, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tW) * 0.5f, sh * 0.72f, title, {0.9f, 0.8f, 0.3f}, 3);

        static const char* cats[4] = {"Audio", "Keyboard & Mouse", "Controller", "Display"};
        for (u32 i = 0; i < 4; i++) {
            // y MUST match menuMouseForState case 3 (baseY + (count-1-i)*spacing, item 0 at top).
            f32 y = sh * 0.44f + static_cast<f32>(4 - 1 - i) * 46.0f * uiScale;
            bool sel = (i == m_menu.subSelection);
            Vec3 boxCol = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.4f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 360.0f * uiScale, 35.0f * uiScale, boxCol, sel);
            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.6f, 0.6f, 0.6f};
            f32 lw = FontSystem::textWidth(cats[i], 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - lw) * 0.5f, y + 10.0f * uiScale, cats[i], tc, 2);
        }
        const char* hint = "Up/Down select, Enter/A to open, B/ESC to go back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.12f, hint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 15) {
        // Options — Audio submenu: 3 volume sliders + reset.
        const char* title = "Audio";
        f32 tW = FontSystem::textWidth(title, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tW) * 0.5f, sh * 0.9f, title, {0.9f, 0.8f, 0.3f}, 3);

        f32 colLabel = sw * 0.3f, colHint = sw * 0.62f;
        f32 listTop = sh * 0.62f, lineH = 40.0f * uiScale;
        for (u32 i = 0; i < 4; i++) {
            f32 y = listTop - static_cast<f32>(i) * lineH;
            bool sel = (i == m_menu.subSelection);
            if (i == 3) {
                FontSystem::drawText(sw, sh, colLabel, y, "Reset Audio to Defaults",
                    sel ? Vec3{1.0f, 0.4f, 0.4f} : Vec3{0.5f, 0.3f, 0.3f}, 1);
            } else {
                f32 v = (i == 0) ? AudioSystem::getMasterVolume()
                      : (i == 1) ? AudioSystem::getSfxVolume() : AudioSystem::getMusicVolume();
                const char* nm = (i == 0) ? "Master Volume" : (i == 1) ? "SFX Volume" : "Music Volume";
                char buf[48];
                std::snprintf(buf, sizeof(buf), "%s: %.0f%%", nm, v * 100.0f);
                FontSystem::drawText(sw, sh, colLabel, y, buf, sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colHint, y, "<  Left/Right  >", {0.4f, 0.8f, 0.4f}, 1);
            }
        }
        const char* hint = "Up/Down select, Left/Right adjust, B/ESC back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.12f, hint, {0.5f, 0.5f, 0.6f}, 1);
    } else if (m_menu.subState == 16 || m_menu.subState == 17) {
        // Options — rebind submenu: Keyboard & Mouse (16, keyboard column) OR Controller
        // (17, controller column + sensitivity/invert). Shared render; `kb` selects the variant.
        bool kb = (m_menu.subState == 16);
        static constexpr u32 REBIND_COUNT = static_cast<u32>(GameAction::INVENTORY) + 1;
        // Keyboard & Mouse adds 1 extra row (Mouse Sensitivity); Controller adds 4 (stick/gyro
        // sensitivity + invert-Y). Both variants share this rebind-list render.
        const u32 extra = kb ? 1u : 4u;
        const u32 RESET_ROW = REBIND_COUNT + extra;     // trailing Reset row
        const u32 TOTAL = REBIND_COUNT + extra + 1;

        const char* title = kb ? "Keyboard & Mouse" : "Controller";
        f32 tW = FontSystem::textWidth(title, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tW) * 0.5f, sh * 0.9f, title, {0.9f, 0.8f, 0.3f}, 3);

        f32 colLabel = sw * 0.2f, colBind = sw * 0.62f;
        FontSystem::drawText(sw, sh, colLabel, sh * 0.82f, "Action", {0.7f, 0.7f, 0.7f}, 1);
        FontSystem::drawText(sw, sh, colBind, sh * 0.82f, kb ? "Keyboard" : "Controller", {0.7f, 0.7f, 0.7f}, 1);

        f32 listTop = sh * 0.78f, lineH = 22.0f * uiScale;
        u32 visibleRows = static_cast<u32>((listTop - sh * 0.1f) / lineH);
        u32 scrollOffset = 0;
        if (m_menu.subSelection >= visibleRows) scrollOffset = m_menu.subSelection - visibleRows + 1;

        for (u32 i = scrollOffset; i < TOTAL && i - scrollOffset < visibleRows; i++) {
            f32 y = listTop - static_cast<f32>(i - scrollOffset) * lineH;
            bool sel = (i == m_menu.subSelection);
            if (i < REBIND_COUNT) {
                drawRebindRow(sw, sh, colLabel, colBind, y, static_cast<GameAction>(i),
                              sel, sel && m_menu.bindCapture, kb);
            } else if (kb && i == REBIND_COUNT + 0) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Mouse Sensitivity: %.2f", Input::getMouseSensitivity());
                FontSystem::drawText(sw, sh, colLabel, y, buf, sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBind, y, "<  Left/Right  >", {0.4f, 0.8f, 0.4f}, 1);
            } else if (!kb && i == REBIND_COUNT + 0) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Stick Sensitivity: %.2f", Input::getStickSensitivity());
                FontSystem::drawText(sw, sh, colLabel, y, buf, sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBind, y, "<  Left/Right  >", {0.4f, 0.8f, 0.4f}, 1);
            } else if (!kb && i == REBIND_COUNT + 1) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Gyro Sensitivity: %.1f", Input::getGyroSensitivity());
                FontSystem::drawText(sw, sh, colLabel, y, buf, sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBind, y, "<  Left/Right  >", {0.4f, 0.8f, 0.4f}, 1);
            } else if (!kb && i == REBIND_COUNT + 2) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Stick Invert Y: %s", Input::getStickInvertY() ? "ON" : "OFF");
                FontSystem::drawText(sw, sh, colLabel, y, buf, sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBind, y, "Enter to toggle", {0.4f, 0.8f, 0.4f}, 1);
            } else if (!kb && i == REBIND_COUNT + 3) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "Gyro Invert Y: %s", Input::getGyroInvertY() ? "ON" : "OFF");
                FontSystem::drawText(sw, sh, colLabel, y, buf, sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colBind, y, "Enter to toggle", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == RESET_ROW) {
                FontSystem::drawText(sw, sh, colLabel, y,
                    kb ? "Reset Keyboard Bindings" : "Reset Controller to Defaults",
                    sel ? Vec3{1.0f, 0.4f, 0.4f} : Vec3{0.5f, 0.3f, 0.3f}, 1);
            }
        }

        const char* hint = m_menu.bindCapture
            ? (kb ? "Press a key to bind  -  B / ESC to cancel"
                  : "Press a controller button to bind  -  B / ESC to cancel")
            // Both variants now carry a slider row, so both mention Left/Right adjust.
            : "Up/Down select, Left/Right adjust, A/Enter rebind, B/ESC back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.04f, hint, {0.5f, 0.5f, 0.6f}, 1);
    } else if (m_menu.subState == 18) {
        // Options — Display submenu: borderless fullscreen + split-screen orientation + reset.
        const char* title = "Display";
        f32 tW = FontSystem::textWidth(title, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tW) * 0.5f, sh * 0.9f, title, {0.9f, 0.8f, 0.3f}, 3);

        f32 colLabel = sw * 0.3f, colHint = sw * 0.62f;
        f32 listTop = sh * 0.62f, lineH = 40.0f * uiScale;
        // Dynamic row layout — MUST match updateMenu's subState-18 handler. The monitor selector
        // row only exists on multi-display rigs.
        const bool multiDisplay = Window::getDisplayCount() > 1;
        const u32 D_FULLSCREEN = 0;
        const u32 D_DISPLAY    = multiDisplay ? 1u : 0xFFu;
        const u32 D_SPLIT      = multiDisplay ? 2u : 1u;
        const u32 D_RESET      = multiDisplay ? 3u : 2u;
        const u32 D_TOTAL      = multiDisplay ? 4u : 3u;
        for (u32 i = 0; i < D_TOTAL; i++) {
            f32 y = listTop - static_cast<f32>(i) * lineH;
            bool sel = (i == m_menu.subSelection);
            char buf[64];
            if (i == D_FULLSCREEN) {
                std::snprintf(buf, sizeof(buf), "Fullscreen: %s",
                              Window::isBorderlessFullscreen() ? "On (Borderless)" : "Off (Windowed)");
                FontSystem::drawText(sw, sh, colLabel, y, buf, sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colHint, y, "Enter to toggle", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == D_DISPLAY) {
                std::snprintf(buf, sizeof(buf), "Monitor: %s", Window::getDisplayName(Window::getDisplayIndex()));
                FontSystem::drawText(sw, sh, colLabel, y, buf, sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colHint, y, "<  Left/Right  >", {0.4f, 0.8f, 0.4f}, 1);
            } else if (i == D_SPLIT) {
                std::snprintf(buf, sizeof(buf), "Split Screen: %s", m_splitMode == 0 ? "Horizontal" : "Vertical");
                FontSystem::drawText(sw, sh, colLabel, y, buf, sel ? Vec3{1, 1, 0.6f} : Vec3{0.6f, 0.6f, 0.6f}, 1);
                if (sel) FontSystem::drawText(sw, sh, colHint, y, "Enter to toggle", {0.4f, 0.8f, 0.4f}, 1);
            } else {  // D_RESET
                FontSystem::drawText(sw, sh, colLabel, y, "Reset Display to Defaults",
                    sel ? Vec3{1.0f, 0.4f, 0.4f} : Vec3{0.5f, 0.3f, 0.3f}, 1);
            }
        }
        const char* hint = "Up/Down select, Enter / Left-Right adjust, B/ESC back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.12f, hint, {0.5f, 0.5f, 0.6f}, 1);
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

        const char* slotHint = Input::activeDeviceIsGamepad()
            ? "D-pad to select, A to confirm, B to go back"
            : "Up/Down to select, Enter/Click to confirm, ESC to go back";
        f32 hintW2 = FontSystem::textWidth(slotHint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW2) * 0.5f, sh * 0.04f,
                             slotHint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 11) {
        // Player 2 New/Continue chooser — blue P2 theme, mirrors the subState-1 chooser.
        const char* subTitle = "Player 2: New or Continue";
        f32 stW = FontSystem::textWidth(subTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.55f, subTitle, {0.3f, 0.7f, 1.0f}, 2);

        bool hasSave = false;
        for (u32 si = 0; si < MAX_SAVE_SLOTS; si++) if (m_saveSlots[si].exists) { hasSave = true; break; }

        static const char* p2Labels[] = {"New Game", "Continue"};
        for (u32 i = 0; i < 2; i++) {
            f32 y = sh * 0.38f + (1 - i) * 50.0f * uiScale;
            bool sel = (i == m_menu.subSelection);
            bool available = (i == 0) || hasSave;
            Vec3 col = sel ? Vec3{0.3f, 0.6f, 1.0f} : Vec3{0.15f, 0.25f, 0.45f};
            if (!available) col = {0.2f, 0.2f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 250.0f * uiScale, 35.0f * uiScale, col, sel && available);
            Vec3 tc = available ? (sel ? Vec3{1,1,1} : Vec3{0.6f,0.6f,0.6f}) : Vec3{0.35f,0.35f,0.35f};
            f32 tw = FontSystem::textWidth(p2Labels[i], 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 10.0f * uiScale, p2Labels[i], tc, 2);
        }

        const char* hint = "Player 2 pad: D-pad, A to confirm, B to go back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.15f, hint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 12) {
        // Player 2 save-slot select — blue theme. Player 1's slot is locked (can't share one file).
        const char* screenTitle = m_menu.p2Continue ? "Player 2: Select Save — Continue"
                                                     : "Player 2: Select Save — New Game";
        f32 stW = FontSystem::textWidth(screenTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.88f, screenTitle, {0.3f, 0.7f, 1.0f}, 2);

        static constexpr u32 VISIBLE = 14;
        u32 scrollOffset = 0;
        if (m_menu.subSelection >= VISIBLE) scrollOffset = m_menu.subSelection - VISIBLE + 1;
        f32 listTop = sh * 0.82f;
        f32 lineH   = 28.0f * uiScale;

        for (u32 i = scrollOffset; i < MAX_SAVE_SLOTS && (i - scrollOffset) < VISIBLE; i++) {
            const SaveSlotInfo& info = m_saveSlots[i];
            bool isP1Slot = (static_cast<u8>(i + 1) == m_playerSaveSlot[0]);
            f32 y   = listTop - static_cast<f32>(i - scrollOffset) * lineH;
            bool sel = (i == m_menu.subSelection);

            Vec3 bgCol = sel ? Vec3{0.3f, 0.6f, 1.0f} : Vec3{0.12f, 0.2f, 0.35f};
            HUD::drawMenuOption(sw, sh, y - 2.0f * uiScale, sw * 0.7f, lineH - 4.0f * uiScale, bgCol, sel);

            char label[96];
            if (isP1Slot) {
                std::snprintf(label, sizeof(label), "Slot %2u:  (Player 1) — locked", i + 1);
            } else if (!info.exists) {
                std::snprintf(label, sizeof(label), "Slot %2u:  Empty", i + 1);
            } else {
                u32 totalSec = static_cast<u32>(info.totalPlayTime);
                const char* cls1 = (info.playerClasses[0] < static_cast<u8>(PlayerClass::CLASS_COUNT))
                                    ? kClassDefs[info.playerClasses[0]].name : "?";
                std::snprintf(label, sizeof(label), "Slot %2u:  Floor %2u — %s — %02u:%02u",
                              i + 1, info.floor, cls1, totalSec / 60, totalSec % 60);
            }

            Vec3 textCol;
            if (isP1Slot)          textCol = {0.5f, 0.35f, 0.35f};
            else if (!info.exists) textCol = m_menu.p2Continue ? Vec3{0.3f,0.3f,0.3f}
                                                               : (sel ? Vec3{0.8f,0.8f,0.8f} : Vec3{0.45f,0.45f,0.45f});
            else                   textCol = sel ? Vec3{1,1,1} : Vec3{0.6f,0.65f,0.75f};

            f32 lw = FontSystem::textWidth(label, 1);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - lw) * 0.5f, y + 7.0f * uiScale, label, textCol, 1);
        }

        const char* slotHint = "Player 2 pad: D-pad, A to confirm, B to go back";
        f32 hintW2 = FontSystem::textWidth(slotHint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW2) * 0.5f, sh * 0.04f, slotHint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 13) {
        // Couch start-mode — both local players are set up; Start Local vs Host Online.
        const char* title = "Couch Co-op Ready";
        f32 tW = FontSystem::textWidth(title, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tW) * 0.5f, sh * 0.6f, title, {0.3f, 1.0f, 0.5f}, 2);

        static const char* modeLabels[] = {"Start Local Split-Screen",
                                           "Host Online (Friends Can Join)",
                                           "Join Online (Enter Host IP)"};
        // Demo: only local split-screen co-op; the two online options are hidden.
        const u32 modeCount = GameConst::kDemoBuild ? 1u : 3u;
        for (u32 i = 0; i < modeCount; i++) {
            f32 y = sh * 0.46f + (modeCount - 1 - i) * 46.0f * uiScale;
            bool sel = (i == m_menu.subSelection);
            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.4f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 360.0f * uiScale, 35.0f * uiScale, col, sel);
            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.6f, 0.6f, 0.6f};
            f32 lw = FontSystem::textWidth(modeLabels[i], 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - lw) * 0.5f, y + 10.0f * uiScale, modeLabels[i], tc, 2);
        }

        const char* hint = "Up/Down, Enter/A to confirm, B/ESC to go back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.12f, hint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menu.subState == 14) {
        // Free-Play level select — a cleared hero (Hell, floor > 50) picks difficulty + floor 1-50
        // to farm. Non-destructive: the no-downgrade save guard keeps the cleared slot pinned.
        const char* title = "Free Play";
        f32 tW = FontSystem::textWidth(title, 3);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tW) * 0.5f, sh * 0.68f, title,
                             {0.3f, 1.0f, 0.5f}, 3);

        static const char* diffNames[3] = {"Normal", "Nightmare", "Hell"};

        // Row 0 — difficulty (y = sh*0.50), Row 1 — floor (y = sh*0.50 - 46px). Matches the mouse
        // hit-test in engine_menu.cpp's sub-state-14 handler — keep both in sync.
        for (u32 row = 0; row < 2; row++) {
            bool sel = (m_menu.subSelection == row);
            f32 y = sh * 0.50f - static_cast<f32>(row) * 46.0f * uiScale;
            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.4f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 360.0f * uiScale, 35.0f * uiScale, col, sel);
            char buf[48];
            if (row == 0)
                std::snprintf(buf, sizeof(buf), "Difficulty:  < %s >", diffNames[m_menu.freePlayDifficulty]);
            else
                std::snprintf(buf, sizeof(buf), "Floor:  < %u >", static_cast<u32>(m_menu.freePlayFloor));
            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.6f, 0.6f, 0.6f};
            f32 lw = FontSystem::textWidth(buf, 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - lw) * 0.5f, y + 10.0f * uiScale, buf, tc, 2);
        }

        const char* hint = "Up/Down select   Left/Right change   Enter/A Descend   B/ESC Back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.12f, hint,
                             {0.4f, 0.4f, 0.5f}, 1);
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

        // When a controller is connected, draw an on-screen keyboard grid (MenuOsk) so a
        // gamepad-only user can enter the address — keyboard users keep the plain type-in hint.
        // Same gate as the input handler in engine_menu.cpp::updateMenu (subState 9).
        if (Input::isGamepadConnected(0) || Input::isGamepadConnected(1)) {
            const f32 cell = 44.0f * uiScale;          // square cell pitch
            const f32 inset = 4.0f * uiScale;          // gap between cells
            const f32 gridW = MenuOsk::COLS * cell;
            const f32 startX = (static_cast<f32>(sw) - gridW) * 0.5f;
            const f32 row0Y  = boxY - 56.0f * uiScale; // first row sits below the IP box
            for (u32 i = 0; i < MenuOsk::COUNT; i++) {
                const u32 r = i / MenuOsk::COLS;
                const u32 c = i % MenuOsk::COLS;
                const f32 cx = startX + c * cell;
                const f32 cy = row0Y - r * cell;       // lower rows = smaller y (bottom-origin)
                const bool sel = (i == m_menu.oskCursor);
                // Cell background — gold highlight on the cursor, dim slate otherwise.
                const Vec3 bg = sel ? Vec3{0.90f, 0.72f, 0.18f} : Vec3{0.16f, 0.21f, 0.30f};
                HUD::drawFilledBar(sw, sh, cx, cy, cell - inset, cell - inset, 1.0f, bg, bg);
                // Label: digit/symbol verbatim, or DEL / GO for the control keys.
                char lbl[4];
                if (MenuOsk::isBackspace(i))   { lbl[0]='D'; lbl[1]='E'; lbl[2]='L'; lbl[3]='\0'; }
                else if (MenuOsk::isDone(i))   { lbl[0]='G'; lbl[1]='O'; lbl[2]='\0'; }
                else                           { lbl[0]=MenuOsk::KEYS[i]; lbl[1]='\0'; }
                const f32 lw = FontSystem::textWidth(lbl, 2);
                const Vec3 tc = sel ? Vec3{0.10f,0.08f,0.04f} : Vec3{0.85f,0.85f,0.92f};
                FontSystem::drawText(sw, sh, cx + (cell - inset - lw) * 0.5f,
                                     cy + 12.0f * uiScale, lbl, tc, 2);
            }
            const char* ghint = "D-pad: move    A: type    X: backspace    GO: connect    B: cancel";
            f32 ghw = FontSystem::textWidth(ghint, 1);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - ghw) * 0.5f, sh * 0.06f, ghint,
                                 {0.45f, 0.5f, 0.62f}, 1);
        } else {
            const char* hint = "Type digits and dots, Backspace to delete, Enter to confirm, ESC to cancel";
            f32 hintW = FontSystem::textWidth(hint, 1);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.12f, hint,
                                 {0.4f, 0.4f, 0.5f}, 1);
        }
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
        // Main menu options. The demo build (kDemoBuild, constexpr) hides the online
        // Host/Join entries — it ships singleplayer + local couch only — leaving a 4-item
        // menu; the full build keeps all 6 with the demo branch dead-stripped.
        static const char* fullLabels[] = {"Single Player", "Host Game", "Join Game", "Options", "Credits", "Exit Game"};
        static const Vec3 fullColors[] = {
            {0.2f, 0.9f, 0.2f},
            {0.2f, 0.5f, 1.0f},
            {1.0f, 0.7f, 0.2f},
            {0.6f, 0.6f, 0.8f},
            {1.0f, 0.6f, 0.1f},   // orange for Credits
            {0.7f, 0.2f, 0.2f},
        };
        static const char* demoLabels[] = {"Single Player", "Options", "Credits", "Exit Game"};
        static const Vec3 demoColors[] = {
            {0.2f, 0.9f, 0.2f}, {0.6f, 0.6f, 0.8f}, {1.0f, 0.6f, 0.1f}, {0.7f, 0.2f, 0.2f},
        };
        const char* const* labels = GameConst::kDemoBuild ? demoLabels : fullLabels;
        const Vec3*        colors = GameConst::kDemoBuild ? demoColors : fullColors;
        const u32          count  = GameConst::kDemoBuild ? 4u : 6u;

        for (u32 i = 0; i < count; i++) {
            f32 y = sh * 0.2f + (count - 1 - i) * 50.0f * uiScale;
            Vec3 color = colors[i];
            bool selected = (i == m_menu.selection);
            if (!selected) color = color * 0.4f;
            HUD::drawMenuOption(sw, sh, y, 250.0f * uiScale, 35.0f * uiScale, color, selected);

            f32 textW = FontSystem::textWidth(labels[i], 2);
            f32 textX = (static_cast<f32>(sw) - textW) * 0.5f;
            FontSystem::drawText(sw, sh, textX, y + 10.0f * uiScale, labels[i],
                selected ? Vec3{1,1,1} : Vec3{0.6f,0.6f,0.6f}, 2);
        }

        const char* hint = Input::activeDeviceIsGamepad()
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
