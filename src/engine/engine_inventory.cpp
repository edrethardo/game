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
void Engine::updateInventoryInteraction(f32 dt) {
    if (!m_inventoryOpen) return;

    s32 mx, my;
    Input::getMousePosition(mx, my);
    my = static_cast<s32>(Window::getHeight()) - my; // flip to HUD coords

    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();

    // --- D-pad inventory navigation (controller — routes to active player's controller) ---
    s32 padIdx = static_cast<s32>(m_localPlayerIndex);
    if (Input::isGamepadConnected(padIdx)) {
        // Navigate cursor with D-pad
        if (Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
            if (m_invCursorPanel == 0) {
                u8 col = m_invCursorIndex % InventoryUI::BP_COLS;
                if (col < InventoryUI::BP_COLS - 1) m_invCursorIndex++;
            }
        }
        if (Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
            if (m_invCursorPanel == 0) {
                u8 col = m_invCursorIndex % InventoryUI::BP_COLS;
                if (col > 0) m_invCursorIndex--;
            }
        }
        if (Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
            if (m_invCursorPanel == 0) {
                if (m_invCursorIndex + InventoryUI::BP_COLS < InventoryUI::BP_COLS * InventoryUI::BP_ROWS)
                    m_invCursorIndex += InventoryUI::BP_COLS;
            } else {
                if (m_invCursorIndex < InventoryUI::EQ_SLOTS - 1) m_invCursorIndex++;
            }
        }
        if (Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_DPAD_UP)) {
            if (m_invCursorPanel == 0) {
                if (m_invCursorIndex >= InventoryUI::BP_COLS)
                    m_invCursorIndex -= InventoryUI::BP_COLS;
            } else {
                if (m_invCursorIndex > 0) m_invCursorIndex--;
            }
        }
        // L/R shoulder to switch between backpack and equipment panels
        if (Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ||
            Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
            m_invCursorPanel = m_invCursorPanel == 0 ? 1 : 0;
            m_invCursorIndex = 0;
        }
        // A = equip (backpack → equipment) or unequip (equipment → backpack)
        if (Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_A)) {
            if (m_invCursorPanel == 0 && m_invCursorIndex < MAX_INVENTORY_ITEMS) {
                if (!isItemEmpty(m_inventories[m_localPlayerIndex].backpack[m_invCursorIndex])) {
                    Inventory::equip(m_inventories[m_localPlayerIndex], m_invCursorIndex, m_itemDefs);
                    Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                    AudioSystem::play(SfxId::ITEM_EQUIP);
                    m_itemEquippedOnce = true;
                }
            } else if (m_invCursorPanel == 1 && m_invCursorIndex < static_cast<u8>(ItemSlot::COUNT)) {
                if (!isItemEmpty(m_inventories[m_localPlayerIndex].equipped[m_invCursorIndex])) {
                    Inventory::unequip(m_inventories[m_localPlayerIndex], static_cast<ItemSlot>(m_invCursorIndex));
                    Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                    AudioSystem::play(SfxId::ITEM_EQUIP);
                }
            }
        }
        // Y = drop selected item
        if (Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_Y)) {
            Vec3 dropPos = m_localPlayer.position + m_localPlayer.forward * 1.5f + Vec3{0, 0.5f, 0};
            if (m_invCursorPanel == 0 && m_invCursorIndex < MAX_INVENTORY_ITEMS) {
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], m_invCursorIndex);
                if (!isItemEmpty(dropped)) { WorldItemSystem::spawn(m_worldItems, dropped, dropPos); AudioSystem::play(SfxId::ITEM_DROP); }
            } else if (m_invCursorPanel == 1 && m_invCursorIndex < static_cast<u8>(ItemSlot::COUNT)) {
                ItemInstance dropped = Inventory::dropFromEquipment(m_inventories[m_localPlayerIndex],
                    static_cast<ItemSlot>(m_invCursorIndex));
                if (!isItemEmpty(dropped)) { WorldItemSystem::spawn(m_worldItems, dropped, dropPos); AudioSystem::play(SfxId::ITEM_DROP); }
            }
        }

        // - button = drop entire backpack
        if (Input::isButtonPressed(padIdx, SDL_CONTROLLER_BUTTON_BACK)) {
            Vec3 dropPos = m_localPlayer.position + m_localPlayer.forward * 1.5f + Vec3{0, 0.5f, 0};
            for (u8 bi = 0; bi < MAX_INVENTORY_ITEMS; bi++) {
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], bi);
                if (!isItemEmpty(dropped)) {
                    // Scatter items slightly so they don't stack
                    f32 scatter = (bi % 5) * 0.3f - 0.6f;
                    WorldItemSystem::spawn(m_worldItems, dropped,
                        dropPos + Vec3{scatter, 0, (bi / 5) * 0.3f});
                }
            }
        }

        // Move mouse cursor to match D-pad selection (so tooltip renders at right position)
        // Scale relative to 720p reference (matches hud.cpp layout)
        f32 uiScale = static_cast<f32>(sh) / 720.0f;
        f32 bpCell = InventoryUI::BP_CELL * uiScale;
        f32 bpGap  = InventoryUI::BP_GAP * uiScale;
        f32 eqH    = InventoryUI::EQ_H * uiScale;
        f32 eqW    = InventoryUI::EQ_W * uiScale;
        f32 eqGap  = InventoryUI::EQ_GAP * uiScale;

        if (m_invCursorPanel == 0) {
            u32 col = m_invCursorIndex % InventoryUI::BP_COLS;
            u32 row = m_invCursorIndex / InventoryUI::BP_COLS;
            f32 bpX = static_cast<f32>(sw) * 0.42f;
            f32 bpStartY = static_cast<f32>(sh) * 0.5f + 180.0f * uiScale;
            mx = static_cast<s32>(bpX + col * (bpCell + bpGap) + bpCell * 0.5f);
            my = static_cast<s32>(bpStartY - row * (bpCell + bpGap) + bpCell * 0.5f);
        } else {
            f32 eqX = static_cast<f32>(sw) * 0.12f;
            f32 centerY = static_cast<f32>(sh) * 0.5f;
            f32 eqStartY = centerY + 220.0f * uiScale;
            mx = static_cast<s32>(eqX + eqW * 0.5f);
            my = static_cast<s32>(eqStartY - m_invCursorIndex * (eqH + eqGap) + eqH * 0.5f);
        }
    }

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
                    // Double-click: equip directly
                    Inventory::equip(m_inventories[m_localPlayerIndex], hit.index, m_itemDefs);
                    Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                    AudioSystem::play(SfxId::ITEM_EQUIP);
                    m_itemEquippedOnce = true;
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

        // Right-click: drop item to world (backpack or equipment)
        if (Input::isMouseButtonPressed(SDL_BUTTON_RIGHT)) {
            InventoryUI::SlotHit hit = InventoryUI::hitTest(sw, sh, mx, my);
            Vec3 dropPos = m_localPlayer.position + m_localPlayer.forward * 1.5f + Vec3{0, 0.5f, 0};
            if (hit.panel == InventoryUI::SlotHit::BACKPACK &&
                hit.index < MAX_INVENTORY_ITEMS &&
                !isItemEmpty(m_inventories[m_localPlayerIndex].backpack[hit.index])) {
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], hit.index);
                if (!isItemEmpty(dropped)) {
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
                    AudioSystem::play(SfxId::ITEM_DROP);
                }
            } else if (hit.panel == InventoryUI::SlotHit::EQUIPMENT &&
                       hit.index < static_cast<u8>(ItemSlot::COUNT) &&
                       !isItemEmpty(m_inventories[m_localPlayerIndex].equipped[hit.index])) {
                ItemInstance dropped = Inventory::dropFromEquipment(m_inventories[m_localPlayerIndex],
                    static_cast<ItemSlot>(hit.index));
                if (!isItemEmpty(dropped)) {
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
                    Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                    AudioSystem::play(SfxId::ITEM_DROP);
                }
            }
        }

        // Q key: drop all backpack items to world
        if (Input::isKeyPressed(SDL_SCANCODE_Q)) {
            Vec3 dropBase = m_localPlayer.position + m_localPlayer.forward * 1.5f + Vec3{0, 0.5f, 0};
            for (u8 si = 0; si < MAX_INVENTORY_ITEMS; si++) {
                if (isItemEmpty(m_inventories[m_localPlayerIndex].backpack[si])) continue;
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], si);
                if (!isItemEmpty(dropped)) {
                    // Spread items in a small arc so they don't stack
                    f32 angle = si * 0.4f;
                    Vec3 offset = {sinf(angle) * 0.5f, 0, cosf(angle) * 0.5f};
                    WorldItemSystem::spawn(m_worldItems, dropped, dropBase + offset);
                }
            }
        }

        // Middle mouse: equip from quickbar (item stays in quickbar as EQUIPPED_REF)
        if (Input::isMouseButtonPressed(SDL_BUTTON_MIDDLE)) {
            InventoryUI::SlotHit hit = InventoryUI::hitTest(sw, sh, mx, my);
            if (hit.panel == InventoryUI::SlotHit::QUICKBAR) {
                QuickbarSlot& qs = m_quickbars[m_localPlayerIndex].slots[hit.index];
                if (qs.type == QuickbarSlot::BACKPACK_REF &&
                    qs.sourceIndex < MAX_INVENTORY_ITEMS &&
                    !isItemEmpty(m_inventories[m_localPlayerIndex].backpack[qs.sourceIndex])) {
                    u32 uid = qs.itemUid;
                    ItemSlot itemSlot = m_itemDefs[m_inventories[m_localPlayerIndex].backpack[qs.sourceIndex].defId].slot;
                    Inventory::equip(m_inventories[m_localPlayerIndex], qs.sourceIndex, m_itemDefs);
                    AudioSystem::play(SfxId::ITEM_EQUIP);
                    // Update this quickbar slot to point to the equipment slot
                    qs.type = QuickbarSlot::EQUIPPED_REF;
                    qs.sourceIndex = static_cast<u8>(itemSlot);
                    qs.itemUid = uid;
                    Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                }
            }
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
                Inventory::equip(m_inventories[m_localPlayerIndex], m_dragState.sourceIndex, m_itemDefs);
                Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                AudioSystem::play(SfxId::ITEM_EQUIP);
                m_itemEquippedOnce = true;
            } else if (drop.panel == InventoryUI::SlotHit::NONE) {
                // Drop outside all panels — drop item to world
                Vec3 dropPos = m_localPlayer.position + Vec3{0, 0.5f, 0};
                if (m_dragState.source == DragSource::BACKPACK) {
                    ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[m_localPlayerIndex], m_dragState.sourceIndex);
                    if (!isItemEmpty(dropped)) {
                        WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
                        AudioSystem::play(SfxId::ITEM_DROP);
                    }
                } else if (m_dragState.source == DragSource::EQUIPMENT) {
                    ItemInstance dropped = Inventory::dropFromEquipment(m_inventories[m_localPlayerIndex],
                        static_cast<ItemSlot>(m_dragState.sourceIndex));
                    if (!isItemEmpty(dropped)) {
                        WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
                        Quickbar::syncWeaponSlot(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
                        AudioSystem::play(SfxId::ITEM_DROP);
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
