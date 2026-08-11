// hud_menu_frame.cpp — the shared chrome of the tabbed character menu.
//
// One backdrop, one border, one tab strip, drawn before whichever page is up. It exists because
// the three pages used to be three unrelated screens: the inventory drew its panels straight onto
// the live world with NO backdrop at all (the dungeon, the enemies and the floor showed through
// the item grid and the quest prose — see the screenshots that prompted this rework), while the
// character sheet dimmed the world with a quad of its own and the journal painted a lone dark box
// in the middle of the inventory.
//
// Geometry comes entirely from InventoryUI::menuFrameLayout — this file re-derives no rect, which
// is what keeps the clickable tabs under the drawn ones.
#include "renderer/hud.h"
#include "renderer/hud_internal.h"   // fillRect/pushLine/pushQuad/flushHUD
#include "renderer/font.h"
#include "game/inventory_ui.h"

namespace {

// The page names, in tab order. Indexed by Engine::MENU_TAB_* — kept here rather than passed in
// because a label is presentation, and the engine already owns the ordinal.
const char* kTabNames[InventoryUI::MENU_TABS] = { "INVENTORY", "CHARACTER", "QUESTS" };

} // namespace

void HUD::drawMenuChrome(u32 sw, u32 sh, u8 activeTab, bool gamepad, bool cursorOnTabs,
                         s32 mouseX, s32 mouseY) {
    const InventoryUI::MenuFrameRects r = InventoryUI::menuFrameLayout(sw, sh);
    const InventoryUI::SlotHit hover = InventoryUI::hitTestMenuTabs(sw, sh, mouseX, mouseY);
    const f32 s = r.uiScale;

    // --- Backdrop: dim the whole screen, not just the panel ---------------------------------
    // Deliberately dark rather than translucent-grey: every text colour on every page (white,
    // gold, green, grey-blue) is chosen to sit on a dark ground, and a half-lit dungeon behind
    // them is what made the old screens unreadable.
    fillRect(0.0f, 0.0f, static_cast<f32>(sw), static_cast<f32>(sh), Vec3{0.015f, 0.015f, 0.025f});

    // --- The panel itself --------------------------------------------------------------------
    // A hair lighter than the backdrop so the frame edge reads without needing a bright border.
    fillRect(r.x, r.y, r.x + r.w, r.y + r.h, Vec3{0.055f, 0.056f, 0.075f});

    // Double border, the outer one dim and the inner one warm — the cheapest way to make a flat
    // rect look like a framed panel with the primitives this HUD has.
    pushQuad(r.x, r.y, r.x + r.w, r.y + r.h, Vec3{0.30f, 0.26f, 0.18f});
    const f32 in = 3.0f * s;
    pushQuad(r.x + in, r.y + in, r.x + r.w - in, r.y + r.h - in, Vec3{0.16f, 0.15f, 0.13f});

    // --- Tab strip ---------------------------------------------------------------------------
    // TWO passes, and the split is load-bearing. HUD line primitives are BATCHED and rasterised by
    // flushHUD(); FontSystem::drawText draws IMMEDIATELY. So every fill must be flushed BEFORE any
    // label is drawn, or the batch lands on top and the text vanishes — which is exactly what the
    // first build of this screen did: the plates drew, every tab label was invisible.
    for (u32 t = 0; t < InventoryUI::MENU_TABS; t++) {
        const f32 x0 = r.tabX + (r.tabW + r.tabGap) * static_cast<f32>(t);
        const f32 x1 = x0 + r.tabW;
        const bool on  = (t == activeTab);
        const bool hov = hover.panel == InventoryUI::SlotHit::MENU_TAB && hover.index == t;

        // The ACTIVE tab is filled and joined to the page below it by leaving its bottom edge
        // open; the others are drawn as recessed plates. That join is the whole read — it is what
        // says "this tab is the thing you are looking at" without needing a second cue.
        const Vec3 fill = on  ? Vec3{0.135f, 0.120f, 0.075f}
                        : hov ? Vec3{0.085f, 0.085f, 0.105f}
                              : Vec3{0.040f, 0.040f, 0.055f};
        fillRect(x0, r.tabY, x1, r.tabY + r.tabH, fill);

        const Vec3 edge = on ? Vec3{0.85f, 0.70f, 0.35f} : Vec3{0.24f, 0.24f, 0.30f};
        pushLine(x0, r.tabY + r.tabH, x1, r.tabY + r.tabH, edge);   // top
        pushLine(x0, r.tabY, x0, r.tabY + r.tabH, edge);            // left
        pushLine(x1, r.tabY, x1, r.tabY + r.tabH, edge);            // right
        if (!on) pushLine(x0, r.tabY, x1, r.tabY, edge);            // bottom: closed unless active

    }

    // A rule under the strip, broken where the active tab meets it — the visual "join" above.
    {
        const f32 y  = r.tabY;
        const f32 ax0 = r.tabX + (r.tabW + r.tabGap) * static_cast<f32>(activeTab);
        const f32 ax1 = ax0 + r.tabW;
        const Vec3 rule = {0.30f, 0.27f, 0.20f};
        pushLine(r.x + in, y, ax0, y, rule);
        pushLine(ax1, y, r.x + r.w - in, y, rule);
    }

    // The cursor is parked ON the strip: ring the active tab and hang a caret either side of it.
    // Without this a pad player who pressed UP has no way to tell whether their next A/D moves
    // between items or between PAGES — the two are a keypress apart and look identical.
    if (cursorOnTabs) {
        const f32 x0 = r.tabX + (r.tabW + r.tabGap) * static_cast<f32>(activeTab);
        const f32 x1 = x0 + r.tabW;
        const Vec3 sel = {1.00f, 0.90f, 0.55f};
        const f32 o = 2.0f * s;
        pushQuad(x0 - o, r.tabY - o, x1 + o, r.tabY + r.tabH + o, sel);
        // Carets: a small filled triangle on each side, pointing the way A and D move.
        const f32 cy = r.tabY + r.tabH * 0.5f, ch = 5.0f * s, gap = 7.0f * s;
        for (f32 d = 0.0f; d <= ch; d += 1.0f) {
            const f32 t = (ch - d) * 0.8f;
            pushLine(x0 - gap - t, cy - d, x0 - gap, cy - d, sel);   // left caret
            pushLine(x0 - gap - t, cy + d, x0 - gap, cy + d, sel);
            pushLine(x1 + gap, cy - d, x1 + gap + t, cy - d, sel);   // right caret
            pushLine(x1 + gap, cy + d, x1 + gap + t, cy + d, sel);
        }
    }

    flushHUD();   // every fill above lands NOW, before a single glyph is drawn

    // Pass 2: the labels.
    for (u32 t = 0; t < InventoryUI::MENU_TABS; t++) {
        const f32 x0 = r.tabX + (r.tabW + r.tabGap) * static_cast<f32>(t);
        const bool on  = (t == activeTab);
        const bool hov = hover.panel == InventoryUI::SlotHit::MENU_TAB && hover.index == t;
        const Vec3 txt = on  ? Vec3{1.00f, 0.88f, 0.45f}
                       : hov ? Vec3{0.90f, 0.90f, 0.95f}
                             : Vec3{0.55f, 0.55f, 0.62f};
        const f32 tw = FontSystem::textWidth(kTabNames[t], 1.0f * s);
        FontSystem::drawText(sw, sh, x0 + (r.tabW - tw) * 0.5f, r.tabY + 9.0f * s,
                             kTabNames[t], txt, 1.0f * s);
    }

    // Page-cycle hint, right-aligned in the strip, naming the keys for the device ACTUALLY IN USE.
    //
    // It used to say "LB / RB" unconditionally, which is worse than saying nothing to a
    // mouse-and-keyboard player: the one thing on screen that claims to tell you how to change
    // page named two buttons they do not have. That is the reported "I can't switch the tab".
    //
    // Full scale, not 0.85: this font is a pixel face and sub-1.0 scaling smudged "LB / RB" into
    // "LD / RD" at 720p.
    {
        // Teaches the route the player actually uses: the strip is a CURSOR POSITION, so W walks
        // onto it and A/D walk along it (and the tabs are clickable). PgUp/PgDn and the brackets
        // stay bound as shortcuts but are no longer advertised — naming a shortcut instead of the
        // navigation is how the first version ended up unusable on keyboard.
        //
        // The bracket keys are deliberately NOT in any label: SDL scancodes are PHYSICAL key
        // positions, so SDL_SCANCODE_LEFTBRACKET is "[" on a US layout and "u-umlaut" on the
        // German QWERTZ this is developed on.
        const char* hint = gamepad ? "LB / RB  switch page"
                                   : "W to tabs   A/D  switch page";
        const f32 hw = FontSystem::textWidth(hint, 1.0f * s);
        FontSystem::drawText(sw, sh, r.x + r.w - hw - 16.0f * s, r.tabY + 9.0f * s,
                             hint, Vec3{0.46f, 0.46f, 0.55f}, 1.0f * s);
    }
}
