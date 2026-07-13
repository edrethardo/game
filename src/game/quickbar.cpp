// quickbar.cpp — Quickbar hotbar: slot assignment, weapon sync, drag-and-drop resolution.
#include "game/item.h"

// ============================================================
//  Quickbar
// ============================================================

void Quickbar::init(QuickbarState& qb, const PlayerInventory& inv) {
    qb = QuickbarState{};
    // Seed the bar with the equipped weapon (syncWeaponSlot picks the first FREE slot — on a fresh
    // bar that is slot 0, but no slot is reserved).
    syncWeaponSlot(qb, inv);
}

void Quickbar::assignItem(QuickbarState& qb, const PlayerInventory& inv, u8 backpackIdx) {
    if (backpackIdx >= MAX_INVENTORY_ITEMS) return;
    const ItemInstance& item = inv.backpack[backpackIdx];
    if (isItemEmpty(item)) return;

    // Check if already assigned — avoid duplicate entries
    for (u32 i = 0; i < QUICKBAR_SLOTS; i++) {
        if ((qb.slots[i].type == QuickbarSlot::BACKPACK_REF ||
             qb.slots[i].type == QuickbarSlot::EQUIPPED_REF) &&
            qb.slots[i].itemUid == item.uid) return;
    }

    // Find first free slot — any slot can hold any item type
    for (u32 i = 0; i < QUICKBAR_SLOTS; i++) {
        if (qb.slots[i].type == QuickbarSlot::EMPTY) {
            qb.slots[i].type = QuickbarSlot::BACKPACK_REF;
            qb.slots[i].sourceIndex = backpackIdx;
            qb.slots[i].itemUid = item.uid;
            return;
        }
    }
}

void Quickbar::removeItem(QuickbarState& qb, u8 slotIdx) {
    if (slotIdx >= QUICKBAR_SLOTS) return;
    qb.slots[slotIdx] = QuickbarSlot{};
}

void Quickbar::syncWeaponSlot(QuickbarState& qb, const PlayerInventory& inv) {
    // Fix stale EQUIPPED_REFs: if an item was unequipped (swapped to backpack),
    // convert its quickbar ref from EQUIPPED_REF to BACKPACK_REF so it stays valid
    for (u32 i = 0; i < QUICKBAR_SLOTS; i++) {
        if (qb.slots[i].type != QuickbarSlot::EQUIPPED_REF) continue;
        u8 eqSlot = qb.slots[i].sourceIndex;
        if (eqSlot >= static_cast<u8>(ItemSlot::COUNT)) continue;

        const ItemInstance& eq = inv.equipped[eqSlot];
        if (isItemEmpty(eq) || eq.uid != qb.slots[i].itemUid) {
            // Item is no longer in this equipment slot — find it in backpack
            bool found = false;
            for (u8 bp = 0; bp < MAX_INVENTORY_ITEMS; bp++) {
                if (!isItemEmpty(inv.backpack[bp]) && inv.backpack[bp].uid == qb.slots[i].itemUid) {
                    qb.slots[i].type = QuickbarSlot::BACKPACK_REF;
                    qb.slots[i].sourceIndex = bp;
                    found = true;
                    break;
                }
            }
            if (!found) {
                qb.slots[i] = QuickbarSlot{}; // item gone entirely, clear slot
            }
        }
    }

    // Reclaim dead BACKPACK_REFs: a slot whose item was dropped/consumed still resolves to nullptr
    // (the UID check fails) so it DRAWS blank — but the free-slot scans below test `type == EMPTY`,
    // so a blank-looking slot kept blocking assignment forever. With only 4 slots and auto-assign
    // on weapon pickup, that permanently jammed the bar. Note the backpack never compacts
    // (removeFromBackpack leaves holes), so a still-valid sourceIndex stays valid — only genuinely
    // vanished items are cleared here.
    for (u32 i = 0; i < QUICKBAR_SLOTS; i++) {
        if (qb.slots[i].type != QuickbarSlot::BACKPACK_REF) continue;
        if (!resolveSlot(qb, inv, static_cast<u8>(i)))
            qb.slots[i] = QuickbarSlot{};
    }

    // Now handle the newly equipped weapon
    const ItemInstance& wpn = inv.equipped[static_cast<u32>(ItemSlot::WEAPON)];
    if (isItemEmpty(wpn)) return;

    // Check if any slot already references this weapon (by UID)
    for (u32 i = 0; i < QUICKBAR_SLOTS; i++) {
        if (qb.slots[i].itemUid == wpn.uid && qb.slots[i].type != QuickbarSlot::EMPTY) {
            qb.slots[i].type = QuickbarSlot::EQUIPPED_REF;
            qb.slots[i].sourceIndex = static_cast<u8>(ItemSlot::WEAPON);
            qb.slots[i].itemUid = wpn.uid;
            return;
        }
    }

    // No slot references this weapon — assign to first free slot
    for (u32 i = 0; i < QUICKBAR_SLOTS; i++) {
        if (qb.slots[i].type == QuickbarSlot::EMPTY) {
            qb.slots[i].type = QuickbarSlot::EQUIPPED_REF;
            qb.slots[i].sourceIndex = static_cast<u8>(ItemSlot::WEAPON);
            qb.slots[i].itemUid = wpn.uid;
            return;
        }
    }
}

const ItemInstance* Quickbar::resolveSlot(const QuickbarState& qb, const PlayerInventory& inv, u8 slot) {
    if (slot >= QUICKBAR_SLOTS) return nullptr;
    const QuickbarSlot& qs = qb.slots[slot];

    switch (qs.type) {
        case QuickbarSlot::EQUIPPED_REF: {
            if (qs.sourceIndex >= static_cast<u8>(ItemSlot::COUNT)) return nullptr;
            const ItemInstance& item = inv.equipped[qs.sourceIndex];
            // Validate UID to catch stale references (e.g. item was unequipped)
            if (isItemEmpty(item) || item.uid != qs.itemUid) return nullptr;
            return &item;
        }
        case QuickbarSlot::BACKPACK_REF: {
            if (qs.sourceIndex >= MAX_INVENTORY_ITEMS) return nullptr;
            const ItemInstance& item = inv.backpack[qs.sourceIndex];
            // Validate UID to catch stale references (e.g. item was dropped/used)
            if (isItemEmpty(item) || item.uid != qs.itemUid) return nullptr;
            return &item;
        }
        default:
            return nullptr;
    }
}

void Quickbar::assignToSlot(QuickbarState& qb, const PlayerInventory& inv,
                             u8 targetSlot, DragSource source, u8 sourceIndex) {
    if (targetSlot >= QUICKBAR_SLOTS) return;

    // Determine the item's UID from the source
    u32 uid = 0;
    switch (source) {
        case DragSource::BACKPACK:
            if (sourceIndex < MAX_INVENTORY_ITEMS && !isItemEmpty(inv.backpack[sourceIndex]))
                uid = inv.backpack[sourceIndex].uid;
            break;
        case DragSource::EQUIPMENT:
            if (sourceIndex < static_cast<u8>(ItemSlot::COUNT) && !isItemEmpty(inv.equipped[sourceIndex]))
                uid = inv.equipped[sourceIndex].uid;
            break;
        default: return;
    }
    if (uid == 0) return;

    // Remove any existing quickbar slot with the same UID (prevent duplicates)
    for (u32 i = 0; i < QUICKBAR_SLOTS; i++) {
        if (qb.slots[i].itemUid == uid && qb.slots[i].type != QuickbarSlot::EMPTY) {
            qb.slots[i] = QuickbarSlot{};
        }
    }

    // Set the target slot
    qb.slots[targetSlot].sourceIndex = sourceIndex;
    qb.slots[targetSlot].itemUid = uid;
    if (source == DragSource::BACKPACK) {
        qb.slots[targetSlot].type = QuickbarSlot::BACKPACK_REF;
    } else {
        qb.slots[targetSlot].type = QuickbarSlot::EQUIPPED_REF;
    }
}

void Quickbar::swapSlots(QuickbarState& qb, u8 a, u8 b) {
    if (a >= QUICKBAR_SLOTS || b >= QUICKBAR_SLOTS || a == b) return;
    QuickbarSlot tmp = qb.slots[a];
    qb.slots[a] = qb.slots[b];
    qb.slots[b] = tmp;
}
