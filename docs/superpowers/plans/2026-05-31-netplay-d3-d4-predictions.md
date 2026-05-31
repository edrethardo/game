# Netplay Phase 2 D3+D4: HP Prediction + Inventory Prediction Plan

> superpowers:subagent-driven-development. Builds on M7 (predicted damage taken) + M8 (predicted pickups) + M10 (SV_DAMAGE_TO_ME, SV_PICKUP_RESULT).

**Goals:**
- **D3**: Extend M7's predicted-damage-taken to also decrement local HP on predicted impact (currently visual-only). Requires shipping `expectedDamage` on `SnapProjectile`. Server-authoritative HP via M4's `RenderOffset` reconciles any mispredict smoothly.
- **D4**: Extend M8's predicted-pickup to also add the item to local inventory immediately (currently world-item-disappearance only). Use SV_PICKUP_RESULT to confirm or roll back.

Both are "feel" wins for the local player — incoming-projectile damage is acknowledged immediately, picked-up items appear in your bag without ~RTT lag.

---

## D3.1: Add expectedDamage to SnapProjectile wire (TDD)

**Files:**
- Modify: `src/net/snapshot.h` — add `f32 expectedDamage` to `SnapProjectile` (or quantized u8/u16)
- Modify: `src/net/snapshot.cpp` — serialize/deserialize
- Modify: server snapshot-build — populate from authoritative Projectile.damage
- Modify: client — read expectedDamage from interpolated projectile pool

For wire compactness, use a u8 scaled to 0.5 increments (0-127.5 dmg range). Most enemy hits are 5-30 dmg; 0.5 increments are fine. Saves 3 B vs f32.

- [ ] **Step 1**: Add to SnapProjectile in snapshot.h:
```cpp
    u8  expectedDamageQ;  // damage / 0.5f, clamped to [0, 255] (M7/D3 — used by client for
                          // predicted HP decrement on local-player hits).
```

- [ ] **Step 2**: snapshot.cpp serialize: `w8(static_cast<u8>(std::min(255.0f, p.damage * 2.0f)))`. Deserialize: `sp.expectedDamageQ = r.readU8();`. Update wire size comment.

- [ ] **Step 3**: Server snapshot-build (where SnapProjectile is filled from Projectile): write `sp.expectedDamageQ = static_cast<u8>(std::min(255.0f, proj.damage * 2.0f));`.

- [ ] **Step 4**: Client `Client::interpolateProjectiles` reads expectedDamageQ into the corresponding render-pool Projectile's `damage` field (decode: `proj.damage = sp.expectedDamageQ * 0.5f;`).

- [ ] **Step 5**: Build, tests still 54/54. Commit: `feat(net): ship expectedDamage on SnapProjectile (D3.1)`.

---

## D3.2: HP prediction on incoming-projectile impact

**Files:**
- Modify: `src/engine/engine_update.cpp` — in M7's predicted-impact block, additionally decrement HP

- [ ] **Step 1**: Find M7's predicted-impact block (added in commit `bceda62` per M7.2). In the existing predicted-hit code:
```cpp
    if (distSq < (PLAYER_HIT_RADIUS * PLAYER_HIT_RADIUS) && approachSpeed > 0.0f) {
        m_localPlayer.damageFlashTimer = 0.15f;
        PendingDamageRingOps::record(m_pendingDamage, m_clientTick, key);
    }
```

Add HP decrement using the predicted expectedDamage. The interpolated projectile's `damage` field now carries it:
```cpp
        f32 predictedDmg = proj.damage;
        // Apply damage reduction the player would normally apply (if any). Simplest:
        // decrement raw — server-authoritative HP arrives in next snapshot.
        m_localPlayer.health -= predictedDmg;
        if (m_localPlayer.health < 0.0f) m_localPlayer.health = 0.0f;
```

- [ ] **Step 2**: Build, tests. Commit: `feat(net): predict local HP decrement on incoming-projectile impact (D3.2)`.

The HP will be slightly off (no defenses applied) — the next snapshot reconciles via M4's RenderOffset and the M13 renderedHealth lerp absorbs any discrepancy visibly. Worst-case mispredict on a missed/destroyed projectile: HP shows 5-10 hp too low for ~80 ms before snapshot corrects upward.

---

## D4: Predicted inventory item-add on pickup

**Files:**
- Modify: `src/engine/engine_update.cpp` — at CL_PICKUP_ITEM send site, also add to local inventory
- Modify: existing M8 PendingPickupRing — extend entry to record the predicted inventory slot for rollback
- Modify: `Engine::onPickupResult` — on reject, remove the predicted item from inventory

- [ ] **Step 1**: At the CL_PICKUP_ITEM send site (post-M8 wiring), look up the world item by uid to get its inventory entry. Add to local inventory:

```cpp
    // D4: predicted inventory add. Use the WorldItem's stored Item (defId + tier +
    // affixes) to construct a local inventory entry.
    s32 invSlot = -1;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        const WorldItem& wi = m_worldItems.items[i];
        if (wi.active && wi.uid == uid) {
            invSlot = Inventory::tryAdd(m_inventories[m_localPlayerIndex], wi.item);
            break;
        }
    }
```

Adapt to actual Inventory API. `Inventory::tryAdd` should return -1 if full or the slot index on success.

- [ ] **Step 2**: Record the predicted slot in PendingPickupRing — extend entry struct with `s32 predictedInvSlot;`. On reject (SV_PICKUP_RESULT accept=0), remove from inventory:

```cpp
    // Extend pending_pickup_ring.h:
    struct PendingPickup {
        u32 clientTick = 0;
        u32 itemUid    = 0;
        s32 predictedInvSlot = -1;
    };
```

Update `record` to take the slot. Update ops to add `getPredictedSlot(...)` accessor.

- [ ] **Step 3**: In `Engine::onPickupResult(accept, uid)`: if accept=0, look up the predicted slot for this uid and remove from inventory:
```cpp
    if (!accept) {
        for (u32 i = 0; i < s_engine->m_pendingPickups.count; i++) {
            if (s_engine->m_pendingPickups.entries[i].itemUid == uid) {
                s32 slot = s_engine->m_pendingPickups.entries[i].predictedInvSlot;
                if (slot >= 0) Inventory::remove(s_engine->m_inventories[s_engine->m_localPlayerIndex], slot);
                break;
            }
        }
    }
    PendingPickupRingOps::ack(s_engine->m_pendingPickups, uid);
```

Adapt to actual Inventory::remove signature.

- [ ] **Step 4**: Edge case — if the next snapshot's inventory state arrives BEFORE SV_PICKUP_RESULT, the snapshot might also add (or not add) the item depending on server state. To prevent double-adds, check if uid is already pending before adding. If pending, skip the snapshot-driven add.

This is risky. For v1 simplicity, the snapshot path SHOULD already win (last-writer wins for inventory). So if predicted add doesn't conflict, fine.

Actually — let me revise this. The Inventory currently isn't mirror-overwritten by snapshots (inventory is local state managed by the engine, with adds driven by pickup-confirm events). So the local predicted add can stand until the server's authoritative add (via the existing pickup confirmation flow) does its work. If they collide, the inventory has two of the item — bad.

**Safer approach for v1:** Mark the predicted slot's item with a "pending" flag. The HUD can render it slightly grey/translucent. On SV_PICKUP_RESULT accept, clear the flag (it's now real). On reject, remove. On any other server inventory update path, also clear pending slots first to avoid double-add.

For v1, this might be complex enough that we should pull back to "predicted disappearance only" (M8 behavior). Let me defer D4 if the inventory integration is gnarly.

- [ ] **Step 5 (v1 fallback)**: If D4 turns out to require deep changes to Inventory management, deliver only the SV_PICKUP_RESULT consumer (already done in D1.2 via the M8 PendingPickupRing ack) and document D4 as needing more thought. Skip the inventory add and just commit the documentation update.

- [ ] **Step 6**: Build, tests, commit: `feat(net): predict inventory item-add on pickup (D4)` OR `docs: defer inventory item-add prediction (D4 — needs Inventory rework)`.

---

## Verify

- [ ] Clean tree, build, 54/54 tests pass.

## Definition of Done
- [ ] 54/54 tests pass
- [ ] SnapProjectile has expectedDamageQ field
- [ ] D3.2 decrements m_localPlayer.health in M7 predicted-impact block
- [ ] D4 delivered as full prediction OR explicitly deferred with rationale
