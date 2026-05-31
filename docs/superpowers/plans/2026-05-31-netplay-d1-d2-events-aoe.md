# Netplay Phase 2 D1+D2: Reliable Events + AOE Lag-Comp Plan

> superpowers:subagent-driven-development. Builds on M10's reliable channel + M5's lag-comp ring.

**Goals:**
- **D1**: Add the three reliable events deferred from M10:
  - `SV_KILL` — server confirms a kill (killer slot + victim entity/player + weapon)
  - `SV_PICKUP_RESULT` — server explicitly accepts/rejects a pickup request
  - `SV_LOOT_SPAWN` — server announces a loot drop with full inventory entry
- **D2**: Wire lag compensation around AOE skill resolution (METEOR_STRIKE etc.) so AOE damage hits the entity positions the firing client saw at activation time.

These are completeness additions; the game already works without them. They close gaps in the reliable-event surface and lag-comp coverage.

---

## D1.1: SV_KILL reliable event

**Files:**
- `src/net/net.h` — add `SV_KILL = 0x1A` (or next free) and `OnKillFn` typedef
- `src/net/net.cpp` — dispatch case
- `src/game/combat.cpp` — emit when applyDamage drops HP to 0
- `src/engine/engine.h` — onKill callback, register

Wire payload: `u8 killerSlot + u8 victimType(0=entity,1=player) + u16 victimIdx + u8 weaponMeshId + u8 isCrit = 6 B`.

- [ ] **Step 1**: Add the enum value + handler typedef.

- [ ] **Step 2**: In `Combat::applyDamage` (entity damage path) and `Combat::applyDamageToPlayer`, on HP-drop-to-zero, emit SV_KILL. The emitter doesn't need to be inside combat.cpp itself — it can be in a callback installed by the engine. Simplest: add a `void(*g_onKill)(...)` static pointer + setter in combat.cpp; engine sets it on init.

- [ ] **Step 3**: Client `Engine::onKill(killerSlot, victimType, victimIdx, weaponMeshId, isCrit)` — for v1 just LOG_INFO. Future enhancements can add kill feed UI.

- [ ] **Step 4**: Build, tests (54/54). Commit: `feat(net): SV_KILL reliable event — D1.1`.

---

## D1.2: SV_PICKUP_RESULT reliable event

**Files:**
- `src/net/net.h` — `SV_PICKUP_RESULT = 0x1B`
- `src/engine/engine_update.cpp::handlePickupRequest` — emit result back to caller
- `src/engine/engine_net.cpp` or client handler — consume ack
- `src/net/pending_pickup_ring.h` — already has the ring; consume entries on accept, restore on reject (best-effort: trigger the next mirrorWorldItems to bring the item back, which it already does)

Wire payload: `u8 accept(0/1) + u8 reserved + u32 itemUid = 6 B`.

- [ ] **Step 1**: Add enum + typedef + dispatch.

- [ ] **Step 2**: In `Engine::handlePickupRequest`, on accept (item exists, in range, transferred to inventory), emit SV_PICKUP_RESULT with accept=1; on reject (uid not found, out of range, exclusive timer), emit with accept=0.

- [ ] **Step 3**: Client `Engine::onPickupResult(accept, uid)` — calls `PendingPickupRingOps::ack(m_pendingPickups, uid)` regardless (the entry is no longer pending either way). On accept the item already disappeared (predicted + confirmed). On reject the next mirrorWorldItems brings it back (no extra action needed).

- [ ] **Step 4**: Build, tests. Commit: `feat(net): SV_PICKUP_RESULT reliable event — D1.2`.

---

## D1.3: SV_LOOT_SPAWN reliable event

**Files:**
- `src/net/net.h` — `SV_LOOT_SPAWN = 0x1C`
- Wherever loot drops on entity kill (search for `Combat::dropLoot` or similar) — emit
- Client handler — for v1, just LOG_INFO; the existing snapshot pipeline already mirrors the WorldItem so the visual already works. SV_LOOT_SPAWN is for "reliable delivery so a UDP-loss session doesn't miss a drop on a sprint-pickup race".

Wire payload: minimal — `u32 uid + Vec3 position(packed) + u16 itemDefId + u8 reserved = ~16 B`. The inventory entry contents are already in the snapshot's WorldItem mirror; the reliable event is just a "guaranteed delivery sentinel" pinning the uid.

- [ ] **Step 1**: Add enum + typedef + dispatch.

- [ ] **Step 2**: At loot-drop emit site (server-only path; search `dropLoot` / `WorldItemSystem::spawn` / similar), if `m_netRole == SERVER`, also send SV_LOOT_SPAWN to all clients.

- [ ] **Step 3**: Client `Engine::onLootSpawn(uid, ...)` — for v1, just LOG_INFO once.

- [ ] **Step 4**: Build, tests. Commit: `feat(net): SV_LOOT_SPAWN reliable event — D1.3`.

---

## D2: AOE lag-compensation

**Files:**
- `src/engine/engine_update_skills.cpp` (or wherever AOE skills resolve their damage) — wrap the entity-iteration in beginLagComp/endLagComp using the activating client's lag-comp ticks

The challenge: skills resolve immediately on activation. For the activating local player, this is on the same tick the input arrived → lag-comp ticks are computed from the client's RTT and the skill is rewound. For host/SP, lagCompTicks = 0 and rewind is a no-op.

- [ ] **Step 1**: Find AOE skill damage iteration. Search for `SkillId::METEOR_STRIKE` or `SkillId::MORTAR` damage paths in `engine_update_skills.cpp`. There's likely a loop iterating `m_entities` to apply damage at the AOE center.

- [ ] **Step 2**: Wrap with lag-comp on SERVER role only (host doesn't need rewind for itself; remote skill activation comes through the input pipeline + AOE resolution then runs):

```cpp
    u32 lagCompTicks = 0;
    if (m_netRole == NetRole::SERVER && remotePlayerSlot != 0xFF) {
        lagCompTicks = computeLagCompTicks(remotePlayerSlot);
        if (lagCompTicks > 0) beginLagComp(lagCompTicks);
    }
    // ... existing AOE iteration / damage application ...
    if (lagCompTicks > 0) endLagComp();
```

- [ ] **Step 3**: Build, tests (54/54). Commit: `feat(net): apply lag-comp to AOE skill damage resolution — D2`.

---

## Verify

- [ ] Clean tree, build, 54/54 tests pass. Total commits for D1+D2 = 4.

## Definition of Done
- [ ] 54/54 tests pass
- [ ] `grep -c 'SV_KILL\|SV_PICKUP_RESULT\|SV_LOOT_SPAWN' src/net/net.h` returns 3
- [ ] AOE skill code has beginLagComp / endLagComp wrap
