# Netplay D4: Inventory Item-Add Prediction Plan

> superpowers:subagent-driven-development. Builds on M8 (predicted pickups) + D1.2 (SV_PICKUP_RESULT).

**Goal:** When a CLIENT picks up an item via CL_PICKUP_ITEM, predict the inventory add immediately. Server confirms via SV_PICKUP_RESULT; on reject, roll back the local add. Resolves the long-standing gap where the CLIENT's local m_inventories doesn't receive items from net-pickups today (server-side m_inventories[clientSlot] gets updated, but CLIENT's local copy doesn't — the server only sends pickups back via the unused SV_INVENTORY_SYNC).

---

## Task 1: Inventory API refactor + ItemInstance.predicted flag

**Files:**
- Modify: `src/game/item.h` — `ItemInstance` gains `bool predicted`; `addToBackpack` signature change
- Modify: `src/game/inventory.cpp` — implementation
- Modify: 5 callers of `addToBackpack` to use new return type

- [ ] **Step 1**: In `src/game/item.h`, add to `ItemInstance`:
```cpp
    bool predicted = false;             // M-D4: client-predicted pickup awaiting server confirm
```

Change `addToBackpack` signature in the `Inventory` namespace (around line 505):
```cpp
    // Returns the backpack slot the item was placed in, or -1 if backpack is full.
    s8 addToBackpack(PlayerInventory& inv, const ItemInstance& item);

    // Removes the item at the given backpack slot. No-op if slot is already empty.
    void removeFromBackpack(PlayerInventory& inv, u8 slot);
```

- [ ] **Step 2**: In `src/game/inventory.cpp`, update `addToBackpack` to return slot:
```cpp
s8 Inventory::addToBackpack(PlayerInventory& inv, const ItemInstance& item) {
    for (u8 i = 0; i < MAX_INVENTORY_ITEMS; i++) {
        if (inv.backpack[i].defId == 0xFFFF) {
            inv.backpack[i] = item;
            if (i >= inv.backpackCount) inv.backpackCount = static_cast<u8>(i + 1);
            return static_cast<s8>(i);
        }
    }
    return -1;
}

void Inventory::removeFromBackpack(PlayerInventory& inv, u8 slot) {
    if (slot >= MAX_INVENTORY_ITEMS) return;
    inv.backpack[slot].defId = 0xFFFF;
    inv.backpack[slot].predicted = false;
    // Don't try to shrink backpackCount — preserves intermediate empties for the UI.
}
```

- [ ] **Step 3**: Update the 5 callers. Each was `if (Inventory::addToBackpack(inv, item)) { ... } else { ... }`. Convert to `s8 slot = Inventory::addToBackpack(inv, item); if (slot >= 0) { ... } else { ... }`.

Caller sites (from `grep`):
- `engine.cpp:439` — starter weapon
- `engine_update.cpp:960` — local pickup leftover (possibly the old SP path; check context)
- `engine_update.cpp:1072` — local pickup add (SP/host)
- `engine_update.cpp:1267` — server's handlePickupRequest
- `tools/*` — nothing

- [ ] **Step 4**: Build. Expect compile errors at every caller; fix each. Tests still 64/64 pass.

- [ ] **Step 5**: Commit: `refactor(inventory): addToBackpack returns slot; add removeFromBackpack + ItemInstance::predicted — D4.1`.

---

## Task 2: Wire predicted inventory-add on CLIENT pickup

**Files:**
- Modify: `src/net/pending_pickup_ring.h` — extend `PendingPickup` with `s8 predictedSlot`
- Modify: `src/net/pending_pickup_ring.cpp` — `record` takes slot, `findSlotByUid` accessor
- Modify: `src/engine/engine_update.cpp::sendPickupRequest` — predict-add + record slot
- Modify: `src/engine/engine.cpp::onPickupResult` (or wherever the handler lives) — accept: clear predicted flag; reject: removeFromBackpack

- [ ] **Step 1**: Extend `PendingPickup` in src/net/pending_pickup_ring.h:
```cpp
struct PendingPickup {
    u32 clientTick = 0;
    u32 itemUid    = 0;
    s8  predictedSlot = -1;   // backpack slot the predicted item occupies; for rollback
};
```

Update `record` to take slot:
```cpp
void record(PendingPickupRing& r, u32 clientTick, u32 itemUid, s8 predictedSlot);
```

Add accessor:
```cpp
// Returns predictedSlot for the given uid (or -1 if not pending).
s8 findSlotByUid(const PendingPickupRing& r, u32 itemUid);
```

Update existing tests in tests/net/test_pending_pickup_ring.cpp to pass slot=-1 (or update to the new signature). All 4 cases should still work — slot is just an extra payload field.

- [ ] **Step 2**: Update `record` and add `findSlotByUid` in pending_pickup_ring.cpp.

- [ ] **Step 3**: In src/engine/engine_update.cpp::sendPickupRequest, replace the M8 prediction block. The new flow:

```cpp
    // M8/D4: predict pickup. Hide world item locally + add to inventory with predicted=true.
    // SV_PICKUP_RESULT confirms or rolls back. Inventory rollback uses removeFromBackpack.
    s8 predictedSlot = -1;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = m_worldItems.items[i];
        if (wi.active && wi.item.uid == uid) {
            ItemInstance copy = wi.item;
            copy.predicted = true;
            predictedSlot = Inventory::addToBackpack(m_inventories[m_localPlayerIndex], copy);
            if (predictedSlot < 0) {
                // Backpack full — don't even send the request; play "full backpack" notify.
                m_fullBackpackNotifyTimer = 2.0f;
                return;
            }
            wi.active = false;   // hide locally
            if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
            break;
        }
    }
    if (predictedSlot < 0) return;   // uid not in local world items (race)

    PendingPickupRingOps::record(m_pendingPickups, m_clientTick, uid, predictedSlot);

    // Send the request reliably.
    u8 buf[sizeof(PacketHeader) + 4];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_PICKUP_ITEM;
    hdr->flags = 0;
    hdr->seq   = 0;
    std::memcpy(buf + sizeof(PacketHeader), &uid, 4);
    Net::sendToServer(buf, sizeof(buf), true);
```

Move the inventory-add BEFORE the send (so we can early-out cleanly if backpack is full without sending a request the server would reject).

Remove the existing D4-deferred comment block.

- [ ] **Step 4**: In src/engine/engine.cpp::onPickupResult, route accept/reject:
```cpp
void Engine::onPickupResult(u8 accept, u32 uid) {
    if (!s_engine) return;
    s8 slot = PendingPickupRingOps::findSlotByUid(s_engine->m_pendingPickups, uid);
    if (accept) {
        // Server accepted — clear the predicted flag on the slot (now real).
        if (slot >= 0) {
            s_engine->m_inventories[s_engine->m_localPlayerIndex].backpack[slot].predicted = false;
        }
    } else {
        // Server rejected — roll back the predicted add.
        if (slot >= 0) {
            Inventory::removeFromBackpack(s_engine->m_inventories[s_engine->m_localPlayerIndex], slot);
        }
        // World item will reappear in next snapshot's mirrorWorldItems (M8 behavior).
    }
    PendingPickupRingOps::ack(s_engine->m_pendingPickups, uid);
}
```

If `Engine::onPickupResult` already exists from D1.2, just extend the existing handler.

- [ ] **Step 5**: Build, 64/64 tests pass.

- [ ] **Step 6**: Commit: `feat(net): predict inventory item-add on CLIENT pickup — D4.2`.

---

## Task 3: Visual indicator for predicted items (optional, nice-to-have)

Render predicted items in the inventory grid slightly translucent or with a faint outline, so the player sees that the item is "pending confirmation". Skip for v1 if HUD rendering is too tangled — the prediction works correctly without the visual flag.

- [ ] **Step 1**: In the inventory rendering path (src/engine/engine_hud.cpp / src/game/inventory_ui.cpp), where each backpack item is drawn, check `item.predicted` and reduce alpha or add a yellow border.

- [ ] **Step 2**: Commit: `feat(hud): visualize predicted inventory items — D4.3` (optional).

---

## Verify

- [ ] Clean tree, build, 64/64 tests pass. D4 = 2 (or 3) commits.

## Definition of Done
- [ ] 64/64 tests pass
- [ ] `addToBackpack` returns s8 (slot index)
- [ ] `removeFromBackpack` exists
- [ ] `ItemInstance::predicted` exists
- [ ] sendPickupRequest predict-adds; onPickupResult clears flag or rolls back
- [ ] Backpack-full case is handled (no CL_PICKUP_ITEM sent if can't fit)
