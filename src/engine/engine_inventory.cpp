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
        const f32 bpX = static_cast<f32>(sw) * 0.42f;
        const f32 bpStartY = static_cast<f32>(sh) * 0.5f + 180.0f * uiScale;
        mx = static_cast<s32>(bpX + col * (bpCell + bpGap) + bpCell * 0.5f);
        my = static_cast<s32>(bpStartY - row * (bpCell + bpGap) + bpCell * 0.5f);
    } else {
        const f32 eqH = InventoryUI::EQ_H   * uiScale;
        const f32 eqW = InventoryUI::EQ_W   * uiScale;
        const f32 eqGap = InventoryUI::EQ_GAP * uiScale;
        const f32 eqX = static_cast<f32>(sw) * 0.12f;
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

    // Inventory input mode (player 0): the physical mouse and the WASD/E + D-pad cursor are BOTH live
    // at once — last input wins for the highlight + tooltip. A mouse move or click switches to mouse
    // mode HERE; the nav reads below switch to cursor mode. This is what lets keyboard+mouse keep
    // working even with a controller connected (the old code let a connected pad hijack the pointer).
    if (m_localPlayerIndex == 0) {
        const s32 ddx = mx - m_invLastMouseX, ddy = my - m_invLastMouseY;
        const bool mouseMoved = (m_invLastMouseX >= 0) && (ddx * ddx + ddy * ddy > 4);  // >2 px
        m_invLastMouseX = mx;
        m_invLastMouseY = my;
        if (mouseMoved || Input::isMouseButtonPressed(SDL_BUTTON_LEFT) ||
                          Input::isMouseButtonPressed(SDL_BUTTON_RIGHT)) {
            m_invCursorActive = false;
        }
    }

    // --- Stash mode: the stash panel replaces the equipment side and CLICK means TRANSFER ---
    // (not equip/drag). Handled exclusively so none of the drag/equip/quickbar machinery below
    // can fire under the stash; ESC/B/Tab close via the usual paths + the gameUpdate reconciler.
    if (m_stashOpen) {
        PlayerInventory& inv = m_inventories[m_localPlayerIndex];
        // Page flip: arrows, or shoulders on the active pad.
        s32 padIdx = static_cast<s32>(m_localPlayerIndex);
        bool prev = Input::isKeyPressed(SDL_SCANCODE_LEFT)  ||
                    Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        bool next = Input::isKeyPressed(SDL_SCANCODE_RIGHT) ||
                    Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        if (prev && m_stash.page > 0)                              m_stash.page--;
        if (next && m_stash.page + 1 < Stash::PAGE_COUNT)          m_stash.page++;

        if (Input::isMouseButtonPressed(SDL_BUTTON_LEFT)) {
            InventoryUI::SlotHit sHit = InventoryUI::hitTestStash(sw, sh, mx, my);
            if (sHit.panel == InventoryUI::SlotHit::STASH_TAB && sHit.index < Stash::PAGE_COUNT) {
                m_stash.page = sHit.index;
            } else if (sHit.panel == InventoryUI::SlotHit::STASH) {
                // Withdraw into the backpack; a full backpack puts the item straight back.
                ItemInstance out{};
                if (Stash::withdraw(m_stash, m_stash.page, sHit.index, out)) {
                    if (Inventory::addToBackpack(inv, out) >= 0) {
                        AudioSystem::play(SfxId::ITEM_PICKUP);
                        sendInventorySync(m_localPlayerIndex, activeNetSlot());   // no-op unless CLIENT
                    } else {
                        Stash::deposit(m_stash, m_stash.page, out);   // restore — nothing lost
                        addChatMessage("Stash", "Your backpack is full.", {1.0f, 0.85f, 0.4f});
                    }
                }
            } else {
                InventoryUI::SlotHit hit = InventoryUI::hitTest(sw, sh, mx, my);
                if (hit.panel == InventoryUI::SlotHit::BACKPACK &&
                    hit.index < MAX_INVENTORY_ITEMS &&
                    !isItemEmpty(inv.backpack[hit.index])) {
                    // Deposit onto the page the player is looking at (never spills elsewhere).
                    if (Stash::deposit(m_stash, m_stash.page, inv.backpack[hit.index])) {
                        inv.backpack[hit.index] = ItemInstance{};
                        AudioSystem::play(SfxId::ITEM_PICKUP);
                        sendInventorySync(m_localPlayerIndex, activeNetSlot());
                    } else {
                        addChatMessage("Stash", "That page is full.", {1.0f, 0.85f, 0.4f});
                    }
                }
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
        const bool navR = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || (kb && Input::isActionPressed(GameAction::MOVE_RIGHT));
        const bool navL = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || (kb && Input::isActionPressed(GameAction::MOVE_LEFT));
        const bool navD = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_DOWN)  || (kb && Input::isActionPressed(GameAction::MOVE_BACKWARD));
        const bool navU = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_UP)    || (kb && Input::isActionPressed(GameAction::MOVE_FORWARD));
        const bool panelL = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        const bool panelR = Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        if (navR || navL || navD || navU || panelL || panelR) m_invCursorActive = true;

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
        // Slots in the panel the cursor is currently on.
        const u8 panelSlots =
            (m_invCursorPanel == INV_PANEL_BACKPACK)  ? static_cast<u8>(InventoryUI::BP_COLS * InventoryUI::BP_ROWS) :
            (m_invCursorPanel == INV_PANEL_EQUIPMENT) ? static_cast<u8>(InventoryUI::EQ_SLOTS) :
            (m_invCursorPanel == INV_PANEL_CLASS_SKILL) ? static_cast<u8>(InventoryUI::CLASS_SKILL_SLOTS)
                                                        : static_cast<u8>(navEquipCount);

        // Navigate the cursor. Left/right also CROSS between the two item panels at the edges
        // (equipment sits left of the backpack on screen) — the keyboard's substitute for the
        // controller's shoulder cycle, and a nicety for the pad too.
        if (navR) {
            if (m_invCursorPanel == INV_PANEL_BACKPACK) {
                u8 col = m_invCursorIndex % InventoryUI::BP_COLS;
                if (col < InventoryUI::BP_COLS - 1) m_invCursorIndex++;
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
            if (m_invCursorPanel == INV_PANEL_BACKPACK) {
                if (m_invCursorIndex + InventoryUI::BP_COLS < InventoryUI::BP_COLS * InventoryUI::BP_ROWS)
                    m_invCursorIndex += InventoryUI::BP_COLS;
            } else if (m_invCursorPanel == INV_PANEL_EQUIPMENT) {
                if (m_invCursorIndex < InventoryUI::EQ_SLOTS - 1) m_invCursorIndex++;
            } else if (m_invCursorPanel == INV_PANEL_EQUIP_SKILL) {
                // The equip bar sits directly ABOVE the class bar on screen, so "down" drops to it.
                m_invCursorPanel = INV_PANEL_CLASS_SKILL;
                if (m_invCursorIndex >= InventoryUI::CLASS_SKILL_SLOTS)
                    m_invCursorIndex = static_cast<u8>(InventoryUI::CLASS_SKILL_SLOTS - 1);
            }
        }
        if (navU) {
            if (m_invCursorPanel == INV_PANEL_BACKPACK) {
                if (m_invCursorIndex >= InventoryUI::BP_COLS)
                    m_invCursorIndex -= InventoryUI::BP_COLS;
            } else if (m_invCursorPanel == INV_PANEL_EQUIPMENT) {
                if (m_invCursorIndex > 0) m_invCursorIndex--;
            } else if (m_invCursorPanel == INV_PANEL_CLASS_SKILL && navEquipCount > 0) {
                m_invCursorPanel = INV_PANEL_EQUIP_SKILL;
                if (m_invCursorIndex >= navEquipCount)
                    m_invCursorIndex = static_cast<u8>(navEquipCount - 1);
            }
        }
        // L/R shoulder cycles the panels: backpack -> equipment -> class skills -> equip skills
        // (controller only — the keyboard reaches the two item panels via the left/right cross above).
        if (panelL || panelR) {
            const bool fwd = panelR;
            for (u8 step = 0; step < INV_PANEL_COUNT; step++) {
                m_invCursorPanel = fwd
                    ? static_cast<u8>((m_invCursorPanel + 1) % INV_PANEL_COUNT)
                    : static_cast<u8>((m_invCursorPanel + INV_PANEL_COUNT - 1) % INV_PANEL_COUNT);
                if (m_invCursorPanel != INV_PANEL_EQUIP_SKILL || navEquipCount > 0) break;
            }
            m_invCursorIndex = 0;
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
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
                    AudioSystem::play(SfxId::ITEM_DROP);
                    if (m_netRole == NetRole::CLIENT) sendDropRequest(0, idx, dropped, dropPos); // R11
                }
            } else if (m_invCursorPanel == 1 && m_invCursorIndex < static_cast<u8>(ItemSlot::COUNT)) {
                u8 eqIdx = m_invCursorIndex;
                ItemInstance dropped = Inventory::dropFromEquipment(m_inventories[m_localPlayerIndex],
                    static_cast<ItemSlot>(eqIdx));
                if (!isItemEmpty(dropped)) {
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
                    AudioSystem::play(SfxId::ITEM_DROP);
                    if (m_netRole == NetRole::CLIENT) sendDropRequest(1, eqIdx, dropped, dropPos); // R11
                }
            }
        }

        // - button = drop entire backpack + close inventory (controller only)
        if (dropAllPressed) {
            Vec3 dropPos = m_localPlayer.position + m_localPlayer.forward * 1.5f + Vec3{0, 0.5f, 0};
            for (u8 bi = 0; bi < MAX_INVENTORY_ITEMS; bi++) {
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], bi);
                if (!isItemEmpty(dropped)) {
                    f32 scatter = (bi % 5) * 0.3f - 0.6f;
                    Vec3 spawnPos = dropPos + Vec3{scatter, 0, (bi / 5) * 0.3f};
                    WorldItemSystem::spawn(m_worldItems, dropped, spawnPos);
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

    if (m_dragState.source == DragSource::NONE) {
        // --- No drag active ---

        // Left mouse pressed: detect double-click or begin potential drag
        if (Input::isMouseButtonPressed(SDL_BUTTON_LEFT)) {
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
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
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
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
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
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], si);
                if (!isItemEmpty(dropped)) {
                    f32 angle = si * 0.4f;
                    Vec3 offset = {sinf(angle) * 0.5f, 0, cosf(angle) * 0.5f};
                    Vec3 spawnPos = dropBase + offset;
                    WorldItemSystem::spawn(m_worldItems, dropped, spawnPos);
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
                        WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
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
                        WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
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
