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
#include "audio/audio.h"

// ---------------------------------------------------------------------------
// Consumable-use intercept. The inventory has no consumable concept ("use means equip"),
// so every use/equip entry point (A button, double-click, quickbar) calls this FIRST and
// skips its equip path when it returns true. Infinite uses: the item never leaves the bag.
// SP/host lanes toggle directly (lane index == net slot); a CLIENT lane defers to the
// server with a reliable CL_USE_PET packet naming the defId (Engine::onUsePet validates
// it) — the summon is server-authoritative and the entity comes back through the snapshot.
// ---------------------------------------------------------------------------
// The tabbed character menu — open / close / page cycle.
//
// These three exist so the open-and-close RITUAL has one home. Before the tab bar there were
// seven sites writing `m_inventoryOpen = false` and three writing it true, each re-deriving the
// mouse-mode and drag-reset steps by hand; the character screen had a second such set of its own.
// That is the per-site-list shape CLAUDE.md keeps recording (the town portal flag, the level
// flags) — a page or hotkey added later would miss a step, and the step it missed would be the
// mouse capture, which strands the pointer.
// ---------------------------------------------------------------------------
void Engine::openMenu(u8 tab) {
    const bool wasOpen = m_inventoryOpen;

    m_inventoryOpen = true;
    m_inventoryOpenArr[m_localPlayerIndex] = true;
    // Out-of-range degrades to the inventory rather than indexing a page that does not exist.
    m_menuTab = (tab < MENU_TAB_COUNT) ? tab : MENU_TAB_INVENTORY;
    // BOTH the alias and the per-lane array, because openMenu is called from OUTSIDE the
    // per-player swap as well as inside it (the --menu dev door runs in the frame loop;
    // talkToGiver runs in the interact handler). swapInPlayer re-reads every alias from its array
    // at the top of the next per-player pass, so an alias-only write is silently undone one frame
    // later — measured exactly that way: --menu quests opened on the inventory page.
    m_menuTabArr[m_localPlayerIndex] = m_menuTab;
    m_inventoryOpenedOnce = true;              // dismisses the "Open Inventory" tutorial prompt

    // The pointer is freed on EVERY page now, not just the inventory: the tab bar is clickable
    // everywhere, so the character page can no longer keep the cursor captured for its drag-rotate
    // (it reads a left-button drag instead). Player 0 only — the mouse belongs to that lane, and a
    // couch P2 opening a menu must not release P1's mouse-look.
    if (m_localPlayerIndex == 0) Input::setRelativeMouseMode(false);

    if (!wasOpen) {
        if (m_localPlayerIndex == 0) {
            // Seed the input mode from THIS lane's device: a controller opens in cursor mode, a
            // keyboard/mouse in mouse mode, and either can take over later (last-input-wins). Read
            // per-lane, not globally — the global flips to Gamepad the moment a couch P2 touches a
            // pad, which would open P1's menu in cursor mode and make P1's double-click inert.
            m_invCursorActive = Input::laneDeviceIsGamepad(m_localPlayerIndex);
            m_invLastMouseX = -1;
            m_invLastMouseY = -1;
        }
        if (m_firstPickupTooltipShown && !m_equipTooltipShown) m_equipTooltipShown = true;
    }

    // Open the quest log on the act the player is actually IN. D2R does this, and the alternative
    // is worse than it sounds: m_invJournalAct persists, so a player who glanced at Act I in the
    // town and then walked into Act 2 opens the log to five completed rows and no sign of the
    // quest they are on. Only on a fresh open, so paging away and back keeps your place.
    if (!wasOpen && m_level.inZone) {
        const u8 act = Quest::actOf(m_level.zoneFloor);
        if (act >= 1 && act - 1 < InventoryUI::JOURNAL_TABS) {
            const u8 tab = static_cast<u8>(act - 1);
            if (tab != m_invJournalAct) { m_invJournalAct = tab; m_invCursorQuest = 0; }
        }
    }

    // A drag begun on a previous open must not survive into this one.
    m_dragState = {};
    m_dblClickState = {};
}

void Engine::closeMenu() {
    m_inventoryOpen = false;
    m_inventoryOpenArr[m_localPlayerIndex] = false;
    // Next open lands on the page its own hotkey asks for, so nothing here has to remember one.
    m_menuTab = MENU_TAB_INVENTORY;
    m_menuTabArr[m_localPlayerIndex] = MENU_TAB_INVENTORY;
    // Recapture only for the mouse-owning lane, and only if nothing else wants the pointer free.
    if (m_localPlayerIndex == 0) Input::setRelativeMouseMode(true);
    m_dragState = {};
    m_dblClickState = {};
}

void Engine::cycleMenuTab(s8 delta) {
    if (!m_inventoryOpen) return;
    const s16 n = static_cast<s16>(MENU_TAB_COUNT);
    s16 t = static_cast<s16>(m_menuTab) + delta;
    while (t < 0)  t += n;
    while (t >= n) t -= n;
    if (static_cast<u8>(t) == m_menuTab) return;
    m_menuTab = static_cast<u8>(t);
    m_menuTabArr[m_localPlayerIndex] = m_menuTab;   // see openMenu — alias-only writes get undone

    // Leaving the inventory page parks its cursor somewhere legal. The stash lives OUTSIDE the
    // page cycle (it is entered from the town chest), so a tab flip must not leave the synthetic
    // cursor pointing at a grid nobody is looking at.
    if (m_menuTab != MENU_TAB_INVENTORY && m_invCursorPanel == INV_PANEL_STASH) {
        m_invCursorPanel = INV_PANEL_BACKPACK;
        m_invCursorIndex = 0;
    }
    AudioSystem::play(SfxId::UI_CLICK);
}

// ---------------------------------------------------------------------------
bool Engine::tryUsePetItem(u8 backpackIndex) {
    if (backpackIndex >= MAX_INVENTORY_ITEMS) return false;
    const ItemInstance& it = m_inventories[m_localPlayerIndex].backpack[backpackIndex];
    if (isItemEmpty(it) || it.defId >= m_itemDefCount) return false;
    if (!m_itemDefs[it.defId].petSummon) return false;

    if (m_netRole == NetRole::CLIENT) {
        sendUsePetPacket(it.defId);   // reliable CL_USE_PET; server validates + toggles (onUsePet)
    } else {
        togglePetCompanion(static_cast<u8>(m_localPlayerIndex), it.defId);
    }
    recordPetSummon(it.defId);   // menagerie collection — every role's use path crosses here
    AudioSystem::play(SfxId::ITEM_EQUIP);   // local feedback on the click itself
    return true;
}

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

// Inventory drag-and-drop state machine — handles click, double-click, and drag
// across backpack, equipment, and quickbar panels.
// Place the synthetic cursor at the centre of the D-pad-selected slot.
//
// This is what lets the gamepad reuse the MOUSE hover path instead of growing a second one: every
// hover test on this screen (items and now skills) reads a cursor position, and on a gamepad we
// simply write that position ourselves. Previously the math lived in TWO copies (here and in
// renderInventoryHUD) which had to be kept in step by hand; this is now the only one.
void Engine::inventoryCursorToMouse(u32 sw, u32 sh, s32& mx, s32& my) const {
    const f32 uiScale = static_cast<f32>(sh) / 720.0f;

    // Stash grid (controller/keyboard nav while the stash is open). Handled FIRST because its panel
    // value is > CLASS_SKILL and would otherwise fall into the skill-bar branch below. Maps to the
    // selected stash slot's centre so drawStashPanel's hover highlight + hint line follow the cursor.
    if (m_invCursorPanel == INV_PANEL_BUILD) {
        // Park on the selected grid cell (or the toggle row), so hover highlight follows the pad.
        const InventoryUI::BuildGridRects r = InventoryUI::buildGridLayout(sw, sh);
        if (m_invCursorBuild >= 9) {
            mx = static_cast<s32>(r.toggleX + r.toggleW * 0.5f);
            my = static_cast<s32>(r.toggleY + r.toggleH * 0.5f);
        } else {
            const u8 row = m_invCursorBuild / 3, col = m_invCursorBuild % 3;
            mx = static_cast<s32>(r.gridX + col * (r.cell + r.gap) + r.cell * 0.5f);
            my = static_cast<s32>(r.gridY + (2 - row) * (r.cell + r.gap) + r.cell * 0.5f);
        }
        return;
    }

    // The page TAB STRIP. First, because it sits above every page and its cursor position has
    // nothing to do with whichever page is behind it.
    if (m_invCursorPanel == INV_PANEL_MENUTAB) {
        const InventoryUI::MenuFrameRects r = InventoryUI::menuFrameLayout(sw, sh);
        mx = static_cast<s32>(r.tabX + (r.tabW + r.tabGap) * static_cast<f32>(m_menuTab)
                              + r.tabW * 0.5f);
        my = static_cast<s32>(r.tabY + r.tabH * 0.5f);
        return;
    }

    // Quest page. Handled BEFORE the skill-bar branch below because the page has no item panel at
    // all: without this the synthetic cursor would sit wherever the inventory page left it and the
    // pad's hover highlight would land on a row nobody selected.
    if (questTabUp()) {
        const InventoryUI::JournalRects r = InventoryUI::journalLayout(sw, sh);
        // Rows DESCEND from listTopY, so row i's centre is listTopY - rowH*(i + 0.5) — the same
        // inversion hitTestJournal does, which is what makes the pad's hover highlight land on the
        // row the pad selected.
        mx = static_cast<s32>(r.listX + r.listW * 0.5f);
        my = static_cast<s32>(r.listTopY - r.rowH * (static_cast<f32>(m_invCursorQuest) + 0.5f));
        return;
    }

    if (m_invCursorPanel == INV_PANEL_STASH) {
        const InventoryUI::StashRects r = InventoryUI::stashLayout(sw, sh);
        const u32 col = m_invCursorIndex % InventoryUI::STASH_COLS;
        const u32 row = m_invCursorIndex / InventoryUI::STASH_COLS;
        mx = static_cast<s32>(r.x + static_cast<f32>(col) * (r.cell + r.gap) + r.cell * 0.5f);
        my = static_cast<s32>(r.startY - static_cast<f32>(row) * (r.cell + r.gap) + r.cell * 0.5f);
        return;
    }

    if (m_invCursorPanel >= INV_PANEL_CLASS_SKILL) {
        HUD::EquipSkillSlot slots[MAX_EQUIP_SKILL_SLOTS];
        const u32 n = buildEquipSkillSlots(slots);
        const auto sb = InventoryUI::skillBarLayout(sw, sh, n);
        const bool classBar = (m_invCursorPanel == INV_PANEL_CLASS_SKILL);
        const f32 x = (classBar ? sb.classX : sb.equipX) + m_invCursorIndex * (sb.slot + sb.gap);
        const f32 y = (classBar ? sb.classY : sb.equipY);
        mx = static_cast<s32>(x + sb.slot * 0.5f);
        my = static_cast<s32>(y + sb.slot * 0.5f);
        return;
    }

    if (m_invCursorPanel == INV_PANEL_BACKPACK) {
        const f32 bpCell = InventoryUI::BP_CELL * uiScale;
        const f32 bpGap  = InventoryUI::BP_GAP  * uiScale;
        const u32 col = m_invCursorIndex % InventoryUI::BP_COLS;
        const u32 row = m_invCursorIndex / InventoryUI::BP_COLS;
        const f32 bpX = InventoryUI::backpackOriginX(sw, uiScale);
        const f32 bpStartY = static_cast<f32>(sh) * 0.5f + 180.0f * uiScale;
        mx = static_cast<s32>(bpX + col * (bpCell + bpGap) + bpCell * 0.5f);
        my = static_cast<s32>(bpStartY - row * (bpCell + bpGap) + bpCell * 0.5f);
    } else {
        const f32 eqH = InventoryUI::EQ_H   * uiScale;
        const f32 eqW = InventoryUI::EQ_W   * uiScale;
        const f32 eqGap = InventoryUI::EQ_GAP * uiScale;
        const f32 eqX = InventoryUI::equipmentOriginX(sw);
        const f32 eqStartY = static_cast<f32>(sh) * 0.5f + 220.0f * uiScale;
        mx = static_cast<s32>(eqX + eqW * 0.5f);
        my = static_cast<s32>(eqStartY - m_invCursorIndex * (eqH + eqGap) + eqH * 0.5f);
    }
}

void Engine::updateInventoryInteraction(f32 dt) {
    if (!m_inventoryOpen) return;

    s32 mx, my;
    Input::getMousePosition(mx, my);
    my = static_cast<s32>(Window::getHeight()) - my; // flip to HUD coords

    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();

    // Split-screen: the inventory is DRAWN inside this player's viewport (half the window — see the
    // render loop's vpW/vpH → renderInventoryHUD), but the mouse is one full-window device. Remap the
    // pointer into P1's viewport-local space and shrink sw/sh to the viewport, or the hit-test runs in
    // a DIFFERENT coordinate space than the draw and every click misses the drawn slots — which is why
    // couch P1's mouse equip / drag / double-click silently did nothing (most visible in Local Versus /
    // the Arena). P1 is always the FIRST viewport: the top half on a horizontal split, the left half on
    // a vertical one — mirroring the vpX/vpY/vpW/vpH the render loop assigns for sp==0. (P2 is
    // controller-only, so its lane never reaches this mouse path.)
    if (m_splitPlayerCount > 1) {
        if (m_splitMode == 0) { sh /= 2; my -= static_cast<s32>(sh); }  // horizontal: P1 = top half
        else                  { sw /= 2; }                              // vertical:   P1 = left half
    }

    // Inventory input mode (player 0): the physical mouse and the WASD/E + D-pad cursor are BOTH live
    // at once — last input wins for the highlight + tooltip. A mouse move or click switches to mouse
    // mode HERE; the nav reads below switch to cursor mode. This is what lets keyboard+mouse keep
    // working even with a controller connected (the old code let a connected pad hijack the pointer).
    if (m_localPlayerIndex == 0) {
        const s32 ddx = mx - m_invLastMouseX, ddy = my - m_invLastMouseY;
        const bool mouseMoved = (m_invLastMouseX >= 0) && (ddx * ddx + ddy * ddy > 4);  // >2 px
        m_invLastMouseX = mx;
        m_invLastMouseY = my;
        // Any physical-mouse activity — move, press, OR release — switches this lane to mouse mode.
        // Release matters: without it a click that landed while cursor mode was momentarily true (a
        // D-pad nav between mouse-down and mouse-up) would skip the drag-release handler and strand
        // m_dragState, so the NEXT click can't register as a double-click.
        if (mouseMoved || Input::isMouseButtonPressed(SDL_BUTTON_LEFT) ||
                          Input::isMouseButtonPressed(SDL_BUTTON_RIGHT) ||
                          Input::isMouseButtonReleased(SDL_BUTTON_LEFT) ||
                          Input::isMouseButtonReleased(SDL_BUTTON_RIGHT)) {
            m_invCursorActive = false;
        }
    }

    // --- Stash mode: the stash panel replaces the equipment side and CLICK means TRANSFER ---
    // (not equip/drag). Handled exclusively so none of the drag/equip/quickbar machinery below
    // can fire under the stash; ESC/B/Tab close via the usual paths + the gameUpdate reconciler.
    if (m_stashOpen) {
        PlayerInventory& inv = m_inventories[m_localPlayerIndex];
        s32 padIdx = static_cast<s32>(m_localPlayerIndex);

        // Shared transfer helpers so the mouse click path and the controller/keyboard cursor path
        // can't diverge (the quickbarLayout discipline, applied to behaviour). Both are no-ops on a
        // full destination and never lose the item.
        auto withdrawStashSlot = [&](u32 slot) {
            ItemInstance out{};
            if (Stash::withdraw(m_stash, m_stash.page, slot, out)) {
                if (Inventory::addToBackpack(inv, out) >= 0) {
                    AudioSystem::play(SfxId::ITEM_PICKUP);
                    sendInventorySync(m_localPlayerIndex, activeNetSlot());   // no-op unless CLIENT
                } else {
                    Stash::deposit(m_stash, m_stash.page, out);   // restore — nothing lost
                    addChatMessage("Stash", "Your backpack is full.", {1.0f, 0.85f, 0.4f});
                }
            }
        };
        auto depositBackpackSlot = [&](u32 slot) {
            if (slot < MAX_INVENTORY_ITEMS && !isItemEmpty(inv.backpack[slot])) {
                if (Stash::deposit(m_stash, m_stash.page, inv.backpack[slot])) {  // onto the viewed page
                    inv.backpack[slot] = ItemInstance{};
                    AudioSystem::play(SfxId::ITEM_PICKUP);
                    sendInventorySync(m_localPlayerIndex, activeNetSlot());
                } else {
                    addChatMessage("Stash", "That page is full.", {1.0f, 0.85f, 0.4f});
                }
            }
        };

        // Cursor init: entering the stash parks the selection on the stash grid unless it's already on
        // one of the two stash-mode panels (stash grid or backpack).
        if (m_invCursorPanel != INV_PANEL_STASH && m_invCursorPanel != INV_PANEL_BACKPACK) {
            m_invCursorPanel = INV_PANEL_STASH;
            m_invCursorIndex = 0;
        }

        // Page flip: arrows, or shoulders on the active pad.
        bool prev = Input::isKeyPressed(SDL_SCANCODE_LEFT)  ||
                    Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        bool next = Input::isKeyPressed(SDL_SCANCODE_RIGHT) ||
                    Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        if (prev && m_stash.page > 0)                              m_stash.page--;
        if (next && m_stash.page + 1 < Stash::PAGE_COUNT)          m_stash.page++;

        // --- Controller / keyboard cursor nav + transfer (Switch/couch-P2 have no mouse) ---
        // Mirrors the main inventory's cursor: D-pad OR player-0 WASD moves the highlight; the stash
        // grid sits LEFT and the backpack RIGHT, so crossing happens at their inner edges; A / E
        // transfers the selection (withdraw from the stash side, deposit from the backpack side).
        {
            const bool kb = (m_localPlayerIndex == 0);
            const bool cursorMode = inventoryUsesCursor();   // previous-frame pointer mode (see main inv)
            const bool navR = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || (kb && Input::isActionPressed(GameAction::MOVE_RIGHT));
            const bool navL = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || (kb && Input::isActionPressed(GameAction::MOVE_LEFT));
            const bool navD = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_DOWN)  || (kb && Input::isActionPressed(GameAction::MOVE_BACKWARD));
            const bool navU = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_UP)    || (kb && Input::isActionPressed(GameAction::MOVE_FORWARD));
            if (navR || navL || navD || navU) m_invCursorActive = true;   // any nav flips lane 0 to cursor mode
            const bool takePressed = cursorMode &&
                (Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_A) || (kb && Input::isActionPressed(GameAction::PICKUP)));

            const u32 SC = InventoryUI::STASH_COLS, SR = InventoryUI::STASH_ROWS;   // 8 x 6
            const u32 BC = InventoryUI::BP_COLS,    BR = InventoryUI::BP_ROWS;      // 6 x 4
            if (m_invCursorPanel == INV_PANEL_STASH) {
                if (m_invCursorIndex >= SC * SR) m_invCursorIndex = 0;              // safety clamp
                u32 col = m_invCursorIndex % SC, row = m_invCursorIndex / SC;
                if (navU && row > 0)        m_invCursorIndex -= SC;                 // row 0 is the top
                if (navD && row + 1 < SR)   m_invCursorIndex += SC;
                if (navL && col > 0)        m_invCursorIndex -= 1;                  // left edge: stay
                if (navR) {
                    if (col + 1 < SC) m_invCursorIndex += 1;
                    else { m_invCursorPanel = INV_PANEL_BACKPACK;                   // cross right → backpack
                           m_invCursorIndex = static_cast<u8>((row < BR ? row : BR - 1) * BC); }
                }
                if (takePressed) withdrawStashSlot(m_invCursorIndex);
            } else { // INV_PANEL_BACKPACK
                if (m_invCursorIndex >= BC * BR) m_invCursorIndex = 0;
                u32 col = m_invCursorIndex % BC, row = m_invCursorIndex / BC;
                if (navU && row > 0)        m_invCursorIndex -= BC;
                if (navD && row + 1 < BR)   m_invCursorIndex += BC;
                if (navR && col + 1 < BC)   m_invCursorIndex += 1;                  // right edge: stay
                if (navL) {
                    if (col > 0) m_invCursorIndex -= 1;
                    else { m_invCursorPanel = INV_PANEL_STASH;                      // cross left → stash
                           m_invCursorIndex = static_cast<u8>((row < SR ? row : SR - 1) * SC + (SC - 1)); }
                }
                if (takePressed) depositBackpackSlot(m_invCursorIndex);
            }
        }

        // --- Mouse click path (unchanged behaviour, now via the shared helpers) ---
        if (Input::isMouseButtonPressed(SDL_BUTTON_LEFT)) {
            InventoryUI::SlotHit sHit = InventoryUI::hitTestStash(sw, sh, mx, my);
            if (sHit.panel == InventoryUI::SlotHit::STASH_TAB && sHit.index < Stash::PAGE_COUNT) {
                m_stash.page = sHit.index;
            } else if (sHit.panel == InventoryUI::SlotHit::STASH) {
                withdrawStashSlot(sHit.index);
            } else {
                InventoryUI::SlotHit hit = InventoryUI::hitTest(sw, sh, mx, my);
                if (hit.panel == InventoryUI::SlotHit::BACKPACK)
                    depositBackpackSlot(hit.index);
            }
        }
        return;
    }

    // --- Cursor navigation: D-pad OR keyboard WASD/E (both live at once) ---
    // This used to be gated on isGamepadConnected, so a keyboard player got no cursor nav and a
    // connected pad hijacked the mouse. Now it runs for EVERY lane: gamepad buttons work for any pad,
    // and WASD/E (player-0 keyboard) work alongside them. Any nav press flips player 0 into cursor
    // mode (highlight + tooltip follow the selection; the mouse drag/click path below is skipped).
    s32 padIdx = static_cast<s32>(m_localPlayerIndex);
    {
        const bool kb = (m_localPlayerIndex == 0);   // the physical keyboard belongs to the player-0 lane
        // Pointer mode as of the START of this frame: is the cursor (WASD/D-pad) the live pointer, or
        // the physical mouse? inventoryUsesCursor() reads m_invCursorActive, which this frame's nav
        // hasn't touched yet, so it reflects the previous frame's decision.
        const bool cursorMode = inventoryUsesCursor();

        // Navigation — always live. D-pad OR keyboard WASD (MOVE_* is bound to plain W/S/A/D with no
        // stick/pad binding, a clean keyboard-only edge). Pressing any of these ENTERS cursor mode.
        // ARROW KEYS work everywhere WASD does. They were not accepted at all before, which on a
        // menu is a genuine dead end rather than a preference: a player reading a list reaches for
        // the arrows, and the quest log's act tabs had NO other keyboard route (the footer even
        // said "Left/Right", which was simply untrue). Raw scancodes rather than new GameActions —
        // those ordinals ARE controls.json's on-disk format, so appending four would force a
        // BINDINGS_REV migration for navigation keys nobody would ever rebind.
        const bool navR = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || (kb && (Input::isActionPressed(GameAction::MOVE_RIGHT)    || Input::isKeyPressed(SDL_SCANCODE_RIGHT)));
        const bool navL = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || (kb && (Input::isActionPressed(GameAction::MOVE_LEFT)     || Input::isKeyPressed(SDL_SCANCODE_LEFT)));
        const bool navD = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_DOWN)  || (kb && (Input::isActionPressed(GameAction::MOVE_BACKWARD) || Input::isKeyPressed(SDL_SCANCODE_DOWN)));
        const bool navU = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_UP)    || (kb && (Input::isActionPressed(GameAction::MOVE_FORWARD)  || Input::isKeyPressed(SDL_SCANCODE_UP)));

        // PAGE CYCLE. The shoulders are the controller's route; `[` / `]` and PageUp/PageDown are
        // the keyboard's. Before this there was NO keyboard cycle at all — a mouse-and-keyboard
        // player could only reach a page by clicking its tab or knowing the direct hotkey (T/K, J),
        // and the on-screen hint named the controller's buttons, so the screen actively misinformed
        // them. All four keys are unbound in the default scheme (Q/E/Z/X/C/V and 1-4 are taken).
        //
        // PgUp/PgDn are what the on-screen hint NAMES, because SDL scancodes are physical key
        // POSITIONS: SDL_SCANCODE_LEFTBRACKET is the slot that carries "[" on a US layout and
        // "u-umlaut" on the German one this is developed on. The brackets stay bound for US
        // players who reach for them; PgUp/PgDn are the pair that is engraved the same everywhere.
        const bool pageL = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
                        || (kb && (Input::isKeyPressed(SDL_SCANCODE_LEFTBRACKET)  || Input::isKeyPressed(SDL_SCANCODE_PAGEUP)));
        const bool pageR = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
                        || (kb && (Input::isKeyPressed(SDL_SCANCODE_RIGHTBRACKET) || Input::isKeyPressed(SDL_SCANCODE_PAGEDOWN)));
        if (navR || navL || navD || navU) m_invCursorActive = true;

        // Actions on the CURRENT selection — gated on cursorMode so they fire ONLY when the cursor is
        // the live pointer. In mouse mode a player equips with double-click and drops with right-click,
        // so E/F/A/Y stay inert; this is what stops F from dropping a slot the mouse isn't pointing at.
        //   A / E     = equip / unequip the selection
        //   Y / F     = drop the selected item. F is the raw scancode, NOT GameAction::BOOT_SKILL —
        //               that action's CONTROLLER binding is an A+LB chord, and here A=equip / LB=cycle-
        //               panel, so routing drop through it would drop whenever a pad player equipped
        //               while holding LB. The F key itself carries no such overload.
        //   BACK (-)  = drop the whole backpack + close (controller only)
        const bool equipPressed   = cursorMode && (Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_A) || (kb && Input::isActionPressed(GameAction::PICKUP)));
        const bool dropPressed    = cursorMode && (Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_Y) || (kb && Input::isKeyPressed(SDL_SCANCODE_F)));
        const bool dropAllPressed = cursorMode &&  Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_BACK);

        // How many equipment skills are on the bar right now — the equip-skill panel only exists
        // when something is equipped that grants one, so the cursor must skip it otherwise.
        HUD::EquipSkillSlot navEquip[MAX_EQUIP_SKILL_SLOTS];
        const u32 navEquipCount = buildEquipSkillSlots(navEquip);

        // J jumps to the QUESTS page and back. NOT gated on cursorMode, and that is the entire
        // point: the page cycle lives on the shoulder buttons (controller only), so without a key
        // a keyboard-and-mouse player would have to click the tab — discoverable, but not fast.
        //
        // A FIXED scancode, not a new GameAction: those ordinals ARE controls.json's on-disk format
        // (serialized by enum position), so appending one for a convenience key would mean a
        // BINDINGS_REV migration. Same shape as the character page's T/K.
        //
        // It TOGGLES back to the INVENTORY page rather than closing the menu, so the key is a page
        // flip and never a surprise exit.
        if (kb && Input::isKeyPressed(SDL_SCANCODE_J)) {
            m_menuTab = questTabUp() ? MENU_TAB_INVENTORY : MENU_TAB_QUESTS;
            m_menuTabArr[m_localPlayerIndex] = m_menuTab;
            if (m_menuTab == MENU_TAB_QUESTS) m_invCursorQuest = 0;   // enter on row 0
            AudioSystem::play(SfxId::UI_CLICK);
        }
        // Slots in the panel the cursor is currently on.
        const u8 panelSlots =
            (m_invCursorPanel == INV_PANEL_BACKPACK)  ? static_cast<u8>(InventoryUI::BP_COLS * InventoryUI::BP_ROWS) :
            (m_invCursorPanel == INV_PANEL_EQUIPMENT) ? static_cast<u8>(InventoryUI::EQ_SLOTS) :
            (m_invCursorPanel == INV_PANEL_CLASS_SKILL) ? static_cast<u8>(InventoryUI::CLASS_SKILL_SLOTS)
                                                        : static_cast<u8>(navEquipCount);

        // Does the CURSOR own this frame's input, or the physical mouse?
        //
        // This gate is load-bearing and its absence was a real, shipped bug. The three panel blocks
        // below (tab strip, build grid, quest page) each end in an unconditional `return`, and they
        // sit ABOVE the physical-mouse path — so whenever one of them was on screen the mouse was
        // dead for the WHOLE menu: on the quest page you could not click a quest row, an act tab, a
        // page tab, or anything else. Reported as, simply, "mouse doesn't work".
        //
        // `cursorMode` alone is not enough: it is sampled BEFORE this frame's nav presses, so the
        // first W in mouse mode would fall past these blocks into the generic item handling with a
        // non-item page on screen. OR-ing this frame's nav in routes that press to the page that
        // owns it, while a frame carrying only mouse input falls through as it must.
        const bool cursorDrives = cursorMode || navR || navL || navD || navU;

        // L/R shoulder switches the PAGE — Inventory / Character / Quests — which is what Diablo 2
        // puts there on a controller, and the reason the quest log is no longer buried five panels
        // deep in an item cursor cycle.
        //
        // The inventory page's own panels are reached by the D-pad instead (the cross-moves below
        // now form a complete graph: equipment <-> backpack <-> build, and down into the skill
        // bars), so nothing became unreachable when the shoulders were reassigned.
        if (pageL || pageR) {
            cycleMenuTab(pageR ? +1 : -1);
            return;   // the page changed; this frame's remaining input belongs to the new page
        }

        // --- The cursor is ON the tab strip -------------------------------------------------
        // LEFT/RIGHT walk the pages (the page changes as you move, so the page you can see is
        // always the page you have selected — a confirm step here would only add a keypress to a
        // choice that is already reversible). DOWN enters the page. This is the WASD/D-pad route
        // to the tabs; it must be handled before any page's own nav, or the page below would eat
        // the same keys.
        if (cursorDrives && m_invCursorPanel == INV_PANEL_MENUTAB) {
            if (navL) cycleMenuTab(-1);
            if (navR) cycleMenuTab(+1);
            // DOWN enters the page — but only where there is something to enter. The CHARACTER
            // page has no cursor panels at all (it is a model and a stats sheet), so dropping into
            // it would park the cursor on a backpack slot that page never draws: a highlight
            // nobody can see, on a panel nobody is looking at. The cursor stays on the strip there.
            if (navD && m_menuTab != MENU_TAB_CHARACTER) {
                m_invCursorPanel = INV_PANEL_BACKPACK;
                m_invCursorIndex = 0;
                m_invCursorQuest = 0;
                AudioSystem::play(SfxId::UI_CLICK);
            }
            return;   // nothing else on any page may act while the cursor is on the strip
        }

        // Navigate the cursor. Left/right also CROSS between the two item panels at the edges
        // (equipment sits left of the backpack on screen) — the keyboard's substitute for the
        // controller's shoulder cycle, and a nicety for the pad too.
        if (navR) {
            if (m_invCursorPanel == INV_PANEL_BACKPACK) {
                u8 col = m_invCursorIndex % InventoryUI::BP_COLS;
                if (col < InventoryUI::BP_COLS - 1) m_invCursorIndex++;
                // Right edge crosses into the build grid, which sits in the right column on screen.
                // Added when the shoulders were reassigned to the page tabs: the build grid was
                // reachable ONLY by that cycle, so without this cross it would have become
                // unreachable on a controller — and it is the panel that owns the Auto Loot toggle.
                else { m_invCursorPanel = INV_PANEL_BUILD; m_invCursorBuild = 0; }
            } else if (m_invCursorPanel == INV_PANEL_EQUIPMENT) {
                m_invCursorPanel = INV_PANEL_BACKPACK; m_invCursorIndex = 0;   // cross right → backpack
            } else if (m_invCursorPanel >= INV_PANEL_CLASS_SKILL) {
                // Skill bars are horizontal rows — left/right walks along them.
                if (panelSlots > 0 && m_invCursorIndex + 1 < panelSlots) m_invCursorIndex++;
            }
        }
        if (navL) {
            if (m_invCursorPanel == INV_PANEL_BACKPACK) {
                u8 col = m_invCursorIndex % InventoryUI::BP_COLS;
                if (col > 0) m_invCursorIndex--;
                else { m_invCursorPanel = INV_PANEL_EQUIPMENT; m_invCursorIndex = 0; }   // cross left → equipment
            } else if (m_invCursorPanel >= INV_PANEL_CLASS_SKILL) {
                if (m_invCursorIndex > 0) m_invCursorIndex--;
            }
        }
        if (navD) {
            // Dropping out of an item panel's last row lands on the skill bars, which sit below
            // them on screen. Prefer the EQUIP bar when it holds anything (it is the upper of the
            // two); the class bar is the floor. Both crossings exist for the same reason as the
            // build one above — the shoulders that used to reach these now switch pages.
            const u8 skillPanelBelow = (navEquipCount > 0) ? INV_PANEL_EQUIP_SKILL
                                                           : INV_PANEL_CLASS_SKILL;
            if (m_invCursorPanel == INV_PANEL_BACKPACK) {
                if (m_invCursorIndex + InventoryUI::BP_COLS < InventoryUI::BP_COLS * InventoryUI::BP_ROWS)
                    m_invCursorIndex += InventoryUI::BP_COLS;
                else { m_invCursorPanel = skillPanelBelow; m_invCursorIndex = 0; }
            } else if (m_invCursorPanel == INV_PANEL_EQUIPMENT) {
                if (m_invCursorIndex < InventoryUI::EQ_SLOTS - 1) m_invCursorIndex++;
                else { m_invCursorPanel = skillPanelBelow; m_invCursorIndex = 0; }
            } else if (m_invCursorPanel == INV_PANEL_EQUIP_SKILL) {
                // The equip bar sits directly ABOVE the class bar on screen, so "down" drops to it.
                m_invCursorPanel = INV_PANEL_CLASS_SKILL;
                if (m_invCursorIndex >= InventoryUI::CLASS_SKILL_SLOTS)
                    m_invCursorIndex = static_cast<u8>(InventoryUI::CLASS_SKILL_SLOTS - 1);
            }
        }
        if (navU) {
            // Off the TOP of a page and you are on the tab strip. Every page's topmost row exits
            // upward the same way, which is what makes "press up to change page" a rule rather
            // than a list of special cases.
            if (m_invCursorPanel == INV_PANEL_BACKPACK) {
                if (m_invCursorIndex >= InventoryUI::BP_COLS)
                    m_invCursorIndex -= InventoryUI::BP_COLS;
                else { m_invCursorPanel = INV_PANEL_MENUTAB; AudioSystem::play(SfxId::UI_CLICK); }
            } else if (m_invCursorPanel == INV_PANEL_EQUIPMENT) {
                if (m_invCursorIndex > 0) m_invCursorIndex--;
                else { m_invCursorPanel = INV_PANEL_MENUTAB; AudioSystem::play(SfxId::UI_CLICK); }
            } else if (m_invCursorPanel == INV_PANEL_CLASS_SKILL && navEquipCount > 0) {
                m_invCursorPanel = INV_PANEL_EQUIP_SKILL;
                if (m_invCursorIndex >= navEquipCount)
                    m_invCursorIndex = static_cast<u8>(navEquipCount - 1);
            } else if (m_invCursorPanel == INV_PANEL_EQUIP_SKILL ||
                       m_invCursorPanel == INV_PANEL_CLASS_SKILL) {
                // Top of the skill stack climbs back into the bag — the return leg of the navD
                // crossings above. Without it a pad player who dropped onto a bar could walk along
                // it and back down, but never out.
                m_invCursorPanel = INV_PANEL_BACKPACK;
                m_invCursorIndex = static_cast<u8>(InventoryUI::BP_COLS *
                                                   (InventoryUI::BP_ROWS - 1));   // bottom row
            }
        }

        // Build panel: D-pad walks the 3x3 (up from the top row reaches the mode toggle); A/E on
        // the toggle flips Auto Loot & Equip, on a cell selects the build. Selecting either way
        // re-gears the whole bag on the spot (autoEquipBackpack), so the change is visible NOW.
        if (cursorDrives && m_invCursorPanel == INV_PANEL_BUILD) {
            if (navU) {
                if (m_invCursorBuild < 3)      m_invCursorBuild = 9;                 // top row -> toggle
                else if (m_invCursorBuild < 9) m_invCursorBuild -= 3;
                else { m_invCursorPanel = INV_PANEL_MENUTAB;                          // toggle -> tabs
                       AudioSystem::play(SfxId::UI_CLICK); return; }
            }
            if (navD) {
                if (m_invCursorBuild >= 9)     m_invCursorBuild = 1;                 // toggle -> top mid
                else if (m_invCursorBuild < 6) m_invCursorBuild += 3;
            }
            if (navL && m_invCursorBuild < 9) {
                if ((m_invCursorBuild % 3) > 0) m_invCursorBuild--;
                // Left edge returns to the bag — the return leg of the backpack's right-edge cross.
                else { m_invCursorPanel = INV_PANEL_BACKPACK; m_invCursorIndex = 0; return; }
            }
            if (navR && m_invCursorBuild < 9 && (m_invCursorBuild % 3) < 2) m_invCursorBuild++;
            if (equipPressed) {
                PlayerInventory& binv = m_inventories[m_localPlayerIndex];
                if (m_invCursorBuild >= 9) {
                    binv.autoMode = binv.autoMode ? 0 : 1;
                    AudioSystem::play(SfxId::UI_CONFIRM);
                    if (binv.autoMode) autoEquipBackpack(m_localPlayerIndex);
                    sendInventorySync(m_localPlayerIndex, activeNetSlot());
                } else if (binv.autoMode) {
                    binv.buildCell = m_invCursorBuild;
                    AudioSystem::play(SfxId::UI_CONFIRM);
                    autoEquipBackpack(m_localPlayerIndex);
                    sendInventorySync(m_localPlayerIndex, activeNetSlot());
                } else {
                    AudioSystem::play(SfxId::UI_BACK);   // grid is inert in classic — audible refusal
                }
            }
            return;   // the generic slot handling below is for item panels
        }

        // Quest page: D-pad up/down walks the visible act's quest list, left/right flips the act
        // tab. Read-only by design — there is nothing to activate, so nothing can be mis-pressed.
        // Placed after the shoulder handler so L/R can still leave the page.
        if (cursorDrives && questTabUp()) {
            // How many quests the visible act actually has. Derived from the table every frame
            // rather than cached: the row cursor is only ever meaningful against THIS count, and a
            // cached one would go stale the moment a quest is appended.
            u8 rows = 0;
            for (u32 i = 0; i < Quest::COUNT; i++)
                if (Quest::actOf(Quest::QUESTS[i].zoneFloor) == m_invJournalAct + 1) rows++;

            // Up off row 0 leaves for the tab strip, exactly as the item pages do — the quest log
            // has no panel below the tabs to fall back on, so without this it was a page you could
            // enter and not leave without a hotkey.
            if (navU) {
                if (m_invCursorQuest > 0) m_invCursorQuest--;
                else { m_invCursorPanel = INV_PANEL_MENUTAB;
                       AudioSystem::play(SfxId::UI_CLICK); return; }
            }
            if (navD && m_invCursorQuest + 1 < rows) m_invCursorQuest++;
            // Flipping the act resets the row: a cursor left at row 4 on an act with two quests
            // would draw no detail pane at all and read as the journal being broken.
            if (navL && m_invJournalAct > 0) { m_invJournalAct--; m_invCursorQuest = 0; }
            if (navR && m_invJournalAct < InventoryUI::JOURNAL_TABS - 1) {
                m_invJournalAct++; m_invCursorQuest = 0;
            }
            if (m_invCursorQuest >= rows) m_invCursorQuest = 0;   // safety clamp, same reason
            return;   // read-only panel — the item actions below have nothing to act on
        }

        // A / E = equip (backpack → equipment) or unequip (equipment → backpack)
        if (equipPressed) {
            if (m_invCursorPanel == 0 && m_invCursorIndex < MAX_INVENTORY_ITEMS) {
                if (!isItemEmpty(m_inventories[m_localPlayerIndex].backpack[m_invCursorIndex])) {
                    if (!tryUsePetItem(m_invCursorIndex)) { // consumable? A = use, not equip
                    // Capture BEFORE the equip — the item moves out of the backpack, so read its slot
                    // now while it still holds the weapon we're about to equip.
                    const bool qbEquipWeapon =
                        !isItemEmpty(m_inventories[m_localPlayerIndex].backpack[m_invCursorIndex]) &&
                        m_itemDefs[m_inventories[m_localPlayerIndex].backpack[m_invCursorIndex].defId].slot == ItemSlot::WEAPON;
                    Inventory::equip(m_inventories[m_localPlayerIndex], m_invCursorIndex, m_itemDefs);
                    Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                    AudioSystem::play(SfxId::ITEM_EQUIP);
                    m_itemEquippedOnce = true;
                    sendInventorySync(m_localPlayerIndex, activeNetSlot()); // R7: push the new equipped state so the host's fire/reload dispatch sees the right weapon (no-op off-client)
                    // Bind the just-equipped WEAPON to the ACTIVE quickbar slot (loadout UX). Non-weapon equips skip.
                    if (qbEquipWeapon) bindWeaponToActiveQuickbar();
                    }
                }
            } else if (m_invCursorPanel == 1 && m_invCursorIndex < static_cast<u8>(ItemSlot::COUNT)) {
                if (!isItemEmpty(m_inventories[m_localPlayerIndex].equipped[m_invCursorIndex])) {
                    Inventory::unequip(m_inventories[m_localPlayerIndex], static_cast<ItemSlot>(m_invCursorIndex));
                    Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                    AudioSystem::play(SfxId::ITEM_EQUIP);
                    sendInventorySync(m_localPlayerIndex, activeNetSlot()); // R7
                }
            }
        }
        // Y / F = drop the selected item. Arena: drops are refused everywhere (the progression
        // firewall — gear can't be lost or duped in a mode that never saves).
        if (dropPressed && m_level.inArena) {
            AudioSystem::play(SfxId::UI_BACK);
        } else if (dropPressed) {
            Vec3 dropPos = m_localPlayer.position + m_localPlayer.forward * 1.5f + Vec3{0, 0.5f, 0};
            if (m_invCursorPanel == 0 && m_invCursorIndex < MAX_INVENTORY_ITEMS) {
                u8 idx = m_invCursorIndex;
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], idx);
                if (!isItemEmpty(dropped)) {
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos, &m_level.grid);
                    AudioSystem::play(SfxId::ITEM_DROP);
                    if (m_netRole == NetRole::CLIENT) sendDropRequest(0, idx, dropped, dropPos); // R11
                }
            } else if (m_invCursorPanel == 1 && m_invCursorIndex < static_cast<u8>(ItemSlot::COUNT)) {
                u8 eqIdx = m_invCursorIndex;
                ItemInstance dropped = Inventory::dropFromEquipment(m_inventories[m_localPlayerIndex],
                    static_cast<ItemSlot>(eqIdx));
                if (!isItemEmpty(dropped)) {
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos, &m_level.grid);
                    AudioSystem::play(SfxId::ITEM_DROP);
                    if (m_netRole == NetRole::CLIENT) sendDropRequest(1, eqIdx, dropped, dropPos); // R11
                }
            }
        }

        // - button = drop entire backpack + close inventory (controller only)
        if (dropAllPressed) {
            Vec3 dropPos = m_localPlayer.position + m_localPlayer.forward * 1.5f + Vec3{0, 0.5f, 0};
            for (u8 bi = 0; bi < MAX_INVENTORY_ITEMS; bi++) {
                // Spare quickbar-assigned gear from the bulk drop — "drop all" is "shed my loot",
                // not "wipe my configured weapon-swap bar". (Peek before dropFromBackpack removes it.)
                if (Quickbar::holdsBackpackItem(m_quickbars[m_localPlayerIndex],
                                                m_inventories[m_localPlayerIndex].backpack[bi].uid)) continue;
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], bi);
                if (!isItemEmpty(dropped)) {
                    f32 scatter = (bi % 5) * 0.3f - 0.6f;
                    Vec3 spawnPos = dropPos + Vec3{scatter, 0, (bi / 5) * 0.3f};
                    WorldItemSystem::spawn(m_worldItems, dropped, spawnPos, &m_level.grid);
                    if (m_netRole == NetRole::CLIENT) sendDropRequest(0, bi, dropped, spawnPos); // R11
                }
            }
            m_inventoryOpen = false;
            m_inventoryOpenArr[m_localPlayerIndex] = false;
            Input::setRelativeMouseMode(true);
        }

        // Quickbar hotkeys (Z/X/C/V, or L+D-pad) work IN the inventory too — the gameplay handler
        // (engine_update.cpp) that does this is frozen while the inventory is open. Select the slot
        // AND equip its item, so you can build/switch your quickbar loadout from the inventory.
        // Not gated on cursor mode — a keyboard+mouse player pressing Z should still fire; it lives
        // per-lane here (before the cursor-mode return below), never in the mouse-only section.
        static constexpr GameAction kQbSlots[QUICKBAR_SLOTS] = {
            GameAction::QUICKBAR_SLOT_1, GameAction::QUICKBAR_SLOT_2,
            GameAction::QUICKBAR_SLOT_3, GameAction::QUICKBAR_SLOT_4,
        };
        for (u8 qi = 0; qi < QUICKBAR_SLOTS; qi++) {
            if (!Input::isActionPressed(kQbSlots[qi])) continue;
            m_quickbars[m_localPlayerIndex].activeSlot = qi;
            useQuickbarSlot(qi);   // equips slot qi's item (no-op if empty); sets EQUIPPED_REF + syncs
            break;                 // one slot per frame (a chord can't claim two)
        }

    }   // end cursor-navigation block

    // Cursor mode — WASD/E or the D-pad just drove the selection, or this is a gamepad-only split
    // lane. Park the synthetic cursor on the selected slot so the highlight + tooltip follow it (one
    // helper, shared with renderInventoryHUD), then RETURN so the mouse drag/click machinery is
    // skipped and the two input styles never fight over the shared m_dragState.
    if (inventoryUsesCursor()) {
        inventoryCursorToMouse(sw, sh, mx, my);
        return;
    }

    // Physical-mouse path below — player 0 only (a couch P2 has no mouse), and only in mouse mode.
    // m_dragState is shared, not per-player, so a P1 drag must never reach a P2 inventory.
    if (m_localPlayerIndex != 0) return;

    // Tick double-click timer
    m_dblClickState.timer += dt;

    // ---- WHICH PAGE OWNS THE MOUSE ------------------------------------------------------------
    // Decided ONCE, here, before any item machinery. Every hit-test below (hitTest, hitTestStash,
    // hitTestBuildGrid) is pure GEOMETRY — it answers from the layout alone and neither knows nor
    // cares which page is on screen. So while the Character or Quests page was up, the inventory's
    // panels were still fully clickable where they would have been drawn:
    //   * right-click on a quest row landed on the invisible EQUIPMENT column and dropped the
    //     equipped item on the floor, behind an opaque menu, with no icon and no tooltip to warn;
    //   * a left-click that missed a quest row fell through to the invisible BACKPACK grid, so a
    //     double-click on narration equipped a bag item and a 3 px drag DROPPED it;
    //   * a click on the stats sheet hit the invisible BUILD GRID and silently re-geared the whole
    //     character;
    //   * Q (drop the entire backpack) and middle-click fired from any page at all.
    // The gate is the page, applied once, rather than a condition repeated per mouse button — the
    // per-button version is what let right-click, Q and middle-click slip through while left-click
    // looked handled.

    // The TAB STRIP is the one thing clickable on every page: it is drawn over all of them, so it
    // is tested over all of them, and it is the only mouse route between pages.
    if (Input::isMouseButtonPressed(SDL_BUTTON_LEFT) && m_dragState.source == DragSource::NONE) {
        const InventoryUI::SlotHit mt = InventoryUI::hitTestMenuTabs(sw, sh, mx, my);
        if (mt.panel == InventoryUI::SlotHit::MENU_TAB && mt.index < MENU_TAB_COUNT) {
            if (mt.index != m_menuTab) {
                // Go through the shared cycler rather than assigning: it carries the stash-cursor
                // parking rule, which a direct write here would miss.
                cycleMenuTab(static_cast<s8>(static_cast<s16>(mt.index) -
                                             static_cast<s16>(m_menuTab)));
            }
            return;
        }
    }

    // The QUESTS page owns its own clicks — act tabs and quest rows — and NOTHING else. It returns
    // unconditionally: a miss inside this page is a miss, not a fall-through into the bag.
    if (questTabUp()) {
        if (Input::isMouseButtonPressed(SDL_BUTTON_LEFT)) {
            const InventoryUI::SlotHit jr = InventoryUI::hitTestJournal(sw, sh, mx, my);
            if (jr.panel == InventoryUI::SlotHit::JOURNAL_TAB &&
                jr.index < InventoryUI::JOURNAL_TABS) {
                m_invJournalAct  = jr.index;
                m_invCursorQuest = 0;      // per-act row cursor — see the nav block
                AudioSystem::play(SfxId::UI_CLICK);
            } else if (jr.panel == InventoryUI::SlotHit::JOURNAL_ROW) {
                // The hit-test reports any row in the column; only the ones this act actually
                // fills are selectable, or the detail pane would draw nothing and read broken.
                u8 rows = 0;
                for (u32 i = 0; i < Quest::COUNT; i++)
                    if (Quest::actOf(Quest::QUESTS[i].zoneFloor) == m_invJournalAct + 1) rows++;
                if (jr.index < rows) {
                    m_invCursorQuest = jr.index;
                    AudioSystem::play(SfxId::UI_CLICK);
                }
            }
        }
        return;
    }

    // The CHARACTER page has no clickable content at all — a model and a stats sheet. Its only
    // mouse gesture is the drag-rotate, which gameUpdate reads directly from the button state.
    if (m_menuTab == MENU_TAB_CHARACTER) return;

    // ---- INVENTORY page: the item machinery ---------------------------------------------------
    if (m_dragState.source == DragSource::NONE) {
        // --- No drag active ---

        // Left mouse pressed: detect double-click or begin potential drag
        if (Input::isMouseButtonPressed(SDL_BUTTON_LEFT)) {
            {
                const InventoryUI::SlotHit bg = InventoryUI::hitTestBuildGrid(sw, sh, mx, my);
                if (bg.panel == InventoryUI::SlotHit::BUILD_TOGGLE) {
                    PlayerInventory& binv = m_inventories[m_localPlayerIndex];
                    binv.autoMode = binv.autoMode ? 0 : 1;
                    AudioSystem::play(SfxId::UI_CONFIRM);
                    if (binv.autoMode) autoEquipBackpack(m_localPlayerIndex);
                    sendInventorySync(m_localPlayerIndex, activeNetSlot());
                    return;
                }
                if (bg.panel == InventoryUI::SlotHit::BUILD_CELL) {
                    PlayerInventory& binv = m_inventories[m_localPlayerIndex];
                    if (binv.autoMode) {
                        binv.buildCell = bg.index;
                        AudioSystem::play(SfxId::UI_CONFIRM);
                        autoEquipBackpack(m_localPlayerIndex);   // a build switch re-gears NOW
                        sendInventorySync(m_localPlayerIndex, activeNetSlot());
                    } else {
                        AudioSystem::play(SfxId::UI_BACK);       // inert in classic — audible refusal
                    }
                    return;
                }
            }
            InventoryUI::SlotHit hit = InventoryUI::hitTest(sw, sh, mx, my);

            if (hit.panel == InventoryUI::SlotHit::BACKPACK &&
                hit.index < MAX_INVENTORY_ITEMS &&
                !isItemEmpty(m_inventories[m_localPlayerIndex].backpack[hit.index])) {

                // Double-click detection: same backpack slot within 0.3s
                if (m_dblClickState.wasBackpack &&
                    m_dblClickState.lastSlot == hit.index &&
                    m_dblClickState.timer < 0.3f) {
                    // Double-click: use (consumable) or equip directly
                    if (!tryUsePetItem(hit.index)) {
                        // Capture BEFORE the equip — the item moves out of the backpack.
                        const bool qbEquipWeapon =
                            !isItemEmpty(m_inventories[m_localPlayerIndex].backpack[hit.index]) &&
                            m_itemDefs[m_inventories[m_localPlayerIndex].backpack[hit.index].defId].slot == ItemSlot::WEAPON;
                        Inventory::equip(m_inventories[m_localPlayerIndex], hit.index, m_itemDefs);
                        Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                        AudioSystem::play(SfxId::ITEM_EQUIP);
                        m_itemEquippedOnce = true;
                        sendInventorySync(m_localPlayerIndex, activeNetSlot()); // R7
                        // Bind the just-equipped WEAPON to the ACTIVE quickbar slot (loadout UX). Non-weapon equips skip.
                        if (qbEquipWeapon) bindWeaponToActiveQuickbar();
                    }
                    m_dblClickState = {};
                } else {
                    // Record for potential double-click and begin potential drag
                    m_dblClickState.timer = 0.0f;
                    m_dblClickState.lastSlot = hit.index;
                    m_dblClickState.wasBackpack = true;

                    const ItemInstance& item = m_inventories[m_localPlayerIndex].backpack[hit.index];
                    m_dragState.source = DragSource::BACKPACK;
                    m_dragState.sourceIndex = hit.index;
                    m_dragState.itemUid = item.uid;
                    m_dragState.itemDefId = item.defId;
                    m_dragState.startX = mx;
                    m_dragState.startY = my;
                    m_dragState.dragging = false;
                }
            } else if (hit.panel == InventoryUI::SlotHit::EQUIPMENT &&
                       hit.index < static_cast<u8>(ItemSlot::COUNT) &&
                       !isItemEmpty(m_inventories[m_localPlayerIndex].equipped[hit.index])) {
                // Begin drag from equipment slot
                const ItemInstance& item = m_inventories[m_localPlayerIndex].equipped[hit.index];
                m_dragState.source = DragSource::EQUIPMENT;
                m_dragState.sourceIndex = hit.index;
                m_dragState.itemUid = item.uid;
                m_dragState.itemDefId = item.defId;
                m_dragState.startX = mx;
                m_dragState.startY = my;
                m_dragState.dragging = false;
                m_dblClickState = {};
            } else if (hit.panel == InventoryUI::SlotHit::QUICKBAR &&
                       hit.index < QUICKBAR_SLOTS) {
                const ItemInstance* qbItem = Quickbar::resolveSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex], hit.index);
                if (qbItem && !isItemEmpty(*qbItem)) {
                    m_dragState.source = DragSource::QUICKBAR;
                    m_dragState.sourceIndex = hit.index;
                    m_dragState.itemUid = qbItem->uid;
                    m_dragState.itemDefId = qbItem->defId;
                    m_dragState.startX = mx;
                    m_dragState.startY = my;
                    m_dragState.dragging = false;
                }
                m_dblClickState = {};
            } else {
                m_dblClickState = {};
            }
        }

        // Right-click: drop item to world (backpack or equipment). Arena: refused (firewall).
        if (Input::isMouseButtonPressed(SDL_BUTTON_RIGHT) && m_level.inArena) {
            AudioSystem::play(SfxId::UI_BACK);
        } else if (Input::isMouseButtonPressed(SDL_BUTTON_RIGHT)) {
            InventoryUI::SlotHit hit = InventoryUI::hitTest(sw, sh, mx, my);
            Vec3 dropPos = m_localPlayer.position + m_localPlayer.forward * 1.5f + Vec3{0, 0.5f, 0};
            if (hit.panel == InventoryUI::SlotHit::BACKPACK &&
                hit.index < MAX_INVENTORY_ITEMS &&
                !isItemEmpty(m_inventories[m_localPlayerIndex].backpack[hit.index])) {
                u8 idx = hit.index;
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], idx);
                if (!isItemEmpty(dropped)) {
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos, &m_level.grid);
                    AudioSystem::play(SfxId::ITEM_DROP);
                    if (m_netRole == NetRole::CLIENT) sendDropRequest(0, idx, dropped, dropPos); // R11
                }
            } else if (hit.panel == InventoryUI::SlotHit::EQUIPMENT &&
                       hit.index < static_cast<u8>(ItemSlot::COUNT) &&
                       !isItemEmpty(m_inventories[m_localPlayerIndex].equipped[hit.index])) {
                u8 eqIdx = hit.index;
                ItemInstance dropped = Inventory::dropFromEquipment(m_inventories[m_localPlayerIndex],
                    static_cast<ItemSlot>(eqIdx));
                if (!isItemEmpty(dropped)) {
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos, &m_level.grid);
                    Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                    AudioSystem::play(SfxId::ITEM_DROP);
                    if (m_netRole == NetRole::CLIENT) sendDropRequest(1, eqIdx, dropped, dropPos); // R11
                }
            }
        }

        // Q key: drop all backpack items to world + close inventory. Arena: refused (firewall).
        if (Input::isKeyPressed(SDL_SCANCODE_Q) && m_level.inArena) {
            AudioSystem::play(SfxId::UI_BACK);
        } else if (Input::isKeyPressed(SDL_SCANCODE_Q)) {
            Vec3 dropBase = m_localPlayer.position + m_localPlayer.forward * 1.5f + Vec3{0, 0.5f, 0};
            for (u8 si = 0; si < MAX_INVENTORY_ITEMS; si++) {
                if (isItemEmpty(m_inventories[m_localPlayerIndex].backpack[si])) continue;
                // Pet consumables sit out the bulk drop: Q is "shed my loot", and a 1-in-10000
                // companion is not loot — losing one to a reflexive bag-clear (then a floor
                // descent wiping the world items) is exactly how a player deletes their rarest
                // possession by accident. A deliberate single-item drop still works.
                {
                    const u16 dId = m_inventories[m_localPlayerIndex].backpack[si].defId;
                    if (dId < m_itemDefCount && m_itemDefs[dId].petSummon) continue;
                }
                // Quickbar-assigned items sit out the bulk drop too — clearing your bag shouldn't
                // strip the weapon-swap bar you deliberately configured.
                if (Quickbar::holdsBackpackItem(m_quickbars[m_localPlayerIndex],
                                                m_inventories[m_localPlayerIndex].backpack[si].uid)) continue;
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], si);
                if (!isItemEmpty(dropped)) {
                    f32 angle = si * 0.4f;
                    Vec3 offset = {sinf(angle) * 0.5f, 0, cosf(angle) * 0.5f};
                    Vec3 spawnPos = dropBase + offset;
                    WorldItemSystem::spawn(m_worldItems, dropped, spawnPos, &m_level.grid);
                    if (m_netRole == NetRole::CLIENT) sendDropRequest(0, si, dropped, spawnPos); // R11
                }
            }
            m_inventoryOpen = false;
            m_inventoryOpenArr[m_localPlayerIndex] = false;
            Input::setRelativeMouseMode(true);
        }

        // Middle mouse: equip from quickbar (item stays in quickbar as EQUIPPED_REF)
        if (Input::isMouseButtonPressed(SDL_BUTTON_MIDDLE)) {
            InventoryUI::SlotHit hit = InventoryUI::hitTest(sw, sh, mx, my);
            // useQuickbarSlot bounds-checks the index and owns the equip + ref fixup +
            // sendInventorySync (R7). This site used to inline all of that AND index straight into
            // slots[] with no bounds check — an over-range index from the old broken hit-test wrote
            // past the array, into activeSlot and the NEXT player's QuickbarState.
            if (hit.panel == InventoryUI::SlotHit::QUICKBAR)
                useQuickbarSlot(hit.index);
        }

    } else if (!m_dragState.dragging) {
        // --- Potential drag (mouse pressed but not moved far enough) ---

        if (Input::isMouseButtonDown(SDL_BUTTON_LEFT)) {
            s32 dx = mx - m_dragState.startX;
            s32 dy = my - m_dragState.startY;
            if (dx * dx + dy * dy > 9) { // > 3px dead zone
                m_dragState.dragging = true;
            }
        }
        if (Input::isMouseButtonReleased(SDL_BUTTON_LEFT)) {
            // Single click within dead zone — cancel drag, click was recorded for double-click
            m_dragState = {};
        }

    } else {
        // --- Active drag ---

        if (Input::isMouseButtonReleased(SDL_BUTTON_LEFT)) {
            InventoryUI::SlotHit drop = InventoryUI::hitTest(sw, sh, mx, my);

            if (drop.panel == InventoryUI::SlotHit::QUICKBAR) {
                // Drop on quickbar slot
                if (m_dragState.source == DragSource::QUICKBAR) {
                    Quickbar::swapSlots(m_quickbars[m_localPlayerIndex], m_dragState.sourceIndex, drop.index);
                } else {
                    Quickbar::assignToSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex],
                                            drop.index, m_dragState.source, m_dragState.sourceIndex);
                }
            } else if (drop.panel == InventoryUI::SlotHit::EQUIPMENT &&
                       m_dragState.source == DragSource::BACKPACK) {
                // Drop backpack item on equipment slot — equip it
                // Capture BEFORE the equip — the item moves out of the backpack.
                const bool qbEquipWeapon =
                    !isItemEmpty(m_inventories[m_localPlayerIndex].backpack[m_dragState.sourceIndex]) &&
                    m_itemDefs[m_inventories[m_localPlayerIndex].backpack[m_dragState.sourceIndex].defId].slot == ItemSlot::WEAPON;
                Inventory::equip(m_inventories[m_localPlayerIndex], m_dragState.sourceIndex, m_itemDefs);
                Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                AudioSystem::play(SfxId::ITEM_EQUIP);
                m_itemEquippedOnce = true;
                sendInventorySync(m_localPlayerIndex, activeNetSlot()); // R7
                // Bind the just-equipped WEAPON to the ACTIVE quickbar slot (loadout UX). Non-weapon equips skip.
                if (qbEquipWeapon) bindWeaponToActiveQuickbar();
            } else if (drop.panel == InventoryUI::SlotHit::NONE && m_level.inArena) {
                // Arena: drag-out drops are refused (firewall) — the item snaps back to its
                // source slot because a NONE drop with no action never removed it.
                AudioSystem::play(SfxId::UI_BACK);
            } else if (drop.panel == InventoryUI::SlotHit::NONE) {
                // Drop outside all panels — drop item to world
                Vec3 dropPos = m_localPlayer.position + Vec3{0, 0.5f, 0};
                if (m_dragState.source == DragSource::BACKPACK) {
                    ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], m_dragState.sourceIndex);
                    if (!isItemEmpty(dropped)) {
                        WorldItemSystem::spawn(m_worldItems, dropped, dropPos, &m_level.grid);
                        AudioSystem::play(SfxId::ITEM_DROP);
                        // R11 — this drag-out path was the ONE drop route that never told the
                        // server: on a CLIENT the predicted ground item was wiped by the next
                        // mirrorWorldItems pass (the server never spawned it) and the server's
                        // copy of the bag kept the item. Right-click and Q already send this.
                        if (m_netRole == NetRole::CLIENT) sendDropRequest(0, m_dragState.sourceIndex, dropped, dropPos);
                    }
                } else if (m_dragState.source == DragSource::EQUIPMENT) {
                    ItemInstance dropped = Inventory::dropFromEquipment(m_inventories[m_localPlayerIndex],
                        static_cast<ItemSlot>(m_dragState.sourceIndex));
                    if (!isItemEmpty(dropped)) {
                        WorldItemSystem::spawn(m_worldItems, dropped, dropPos, &m_level.grid);
                        Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                        AudioSystem::play(SfxId::ITEM_DROP);
                        // R11 — same fix as the backpack branch above (equipment flavor).
                        if (m_netRole == NetRole::CLIENT) sendDropRequest(1, m_dragState.sourceIndex, dropped, dropPos);
                    }
                } else if (m_dragState.source == DragSource::QUICKBAR) {
                    // Remove from quickbar only (item stays in backpack)
                    Quickbar::removeItem(m_quickbars[m_localPlayerIndex], m_dragState.sourceIndex);
                }
            }
            // Reset drag state
            m_dragState = {};
        }
    }
}

// Makes all active hostile entities walk back to their spawn positions.
// Called on player death/respawn to prevent spawn-camping.
