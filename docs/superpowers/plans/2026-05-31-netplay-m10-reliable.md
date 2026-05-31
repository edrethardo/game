# Netplay M10: Reliable Channel Cleanup Plan

> superpowers:subagent-driven-development. Checkbox `- [ ]`.

**Goal:** (1) Move CL_FIRE_WEAPON from unreliable+manual-retransmit to ENet reliable; delete the M0-era `s_clientFireTx` ring + `resendPendingFire()`. (2) Add `SV_DAMAGE_DONE` reliable event emitted when the server applies entity damage from a player's fire — client acks `PendingHitRing`. (3) Add `SV_DAMAGE_TO_ME` reliable event emitted when a player takes damage — client acks `PendingDamageRing`.

**Skipped from the design's M10 list (deferred to future enhancement):**
- `SV_KILL` (kill confirmation event)
- `SV_PICKUP_RESULT` (currently implicit via mirror — explicit ack is nice-to-have)
- `SV_LOOT_SPAWN` (loot drops are already snapshot-mirrored)

These can land in later milestones without blocking M10's main "kill the manual retransmit + ack predicted FX" goal.

---

## Task 1: CL_FIRE_WEAPON → reliable, delete manual retransmit

**Files:**
- Modify: `src/engine/engine_combat.cpp` — change `sendToServer(..., /*reliable=*/false)` to `true`; delete `s_clientFireTx` struct, `FIRE_TX_REPEATS`, the retransmit queue logic, `resendPendingFire()`.
- Modify: `src/engine/engine.h` — remove `void resendPendingFire();` declaration.
- Modify: `src/engine/engine_net.cpp` — remove the `resendPendingFire()` call from `clientNetPre`.
- Modify: server-side fire-dedup ring (`s_fireDedupRing`) — keep it for now (reliable still allows duplicate sends if the client sends multiple distinct CL_FIRE_WEAPONs in flight; dedup is cheap).

- [ ] **Step 1**: In `src/engine/engine_combat.cpp`:
  - Find `Net::sendToServer(buf, off, /*reliable=*/false);` for CL_FIRE_WEAPON. Change to `/*reliable=*/true`.
  - Delete the lines below that queue `s_clientFireTx` (lines 829-833 per current grep).
  - Delete the `ClientFireTx` struct, the `s_clientFireTx` static, `FIRE_TX_REPEATS`, and the `Engine::resendPendingFire()` function.

- [ ] **Step 2**: In `src/engine/engine.h`, remove `void resendPendingFire();` declaration.

- [ ] **Step 3**: In `src/engine/engine_net.cpp::clientNetPre`, remove the call to `resendPendingFire()` (from M0 baseline, around line 540-545).

- [ ] **Step 4**: Build, run tests. Expected: 49/49 still pass. Manual smoke deferred. Commit: `feat(net): CL_FIRE_WEAPON → reliable; remove manual retransmit ring — M10.1`.

---

## Task 2: SV_DAMAGE_DONE event + PendingHitRing ack

**Files:**
- Modify: `src/net/net.h` — add `SV_DAMAGE_DONE` enum entry
- Modify: `src/net/net.h`/`src/net/net.cpp` — add a callback typedef `OnDamageDoneFn` similar to OnTimePongFn
- Modify: server-side damage path — emit SV_DAMAGE_DONE when applyDamage runs from a remote player's fire
- Modify: client-side — register callback, ack PendingHitRing

- [ ] **Step 1**: Add `SV_DAMAGE_DONE = 0x18` (next free) to NetPacketType in net.h.

Wire payload: `u32 clientTick (the firing client's clientTick) + u16 targetEntityIdx + u16 reserved = 8 B`.

- [ ] **Step 2**: Add to `src/net/net.h`:
```cpp
    typedef void (*OnDamageDoneFn)(u32 clientTick, u16 targetEntityIdx);
    void setOnDamageDone(OnDamageDoneFn fn);
```

In `src/net/net.cpp`:
```cpp
    static OnDamageDoneFn s_onDamageDone = nullptr;
    void Net::setOnDamageDone(OnDamageDoneFn fn) { s_onDamageDone = fn; }
```
Add a dispatch case in the CLIENT receive switch:
```cpp
    case NetPacketType::SV_DAMAGE_DONE: {
        if (size < 4 + 8) break;
        PacketReader r(data + 4, size - 4);
        u32 clientTick = r.readU32();
        u16 targetIdx  = r.readU16();
        if (s_onDamageDone) s_onDamageDone(clientTick, targetIdx);
        break;
    }
```

- [ ] **Step 3**: Server emits SV_DAMAGE_DONE. The challenge: `Combat::applyDamage` is called in many places. The simplest hook is at the call sites in `engine_combat.cpp::handleWeaponFire` for the remote-net-player branch (engine_combat.cpp around 1090-1180 — the hitscan/melee/projectile result paths).

After a remote player's fire produces `result.hitEntity == true` (or any successful entity hit), broadcast SV_DAMAGE_DONE to ONLY that player's peer (it's their predicted hit to ack):

```cpp
    // Emit SV_DAMAGE_DONE to the firing client so their PendingHitRing can ack
    // the predicted hit (M6/M10).
    if (result.hitEntity) {
        u32 clientTick = /* fire request's clientTick — already in scope as in.clientTick */;
        u16 targetIdx  = static_cast<u16>(result.hitEntityHandle.index);
        PacketWriter w;
        w.writeU8(static_cast<u8>(NetPacketType::SV_DAMAGE_DONE));
        w.writeU8(0); w.writeU16(0);
        w.writeU32(clientTick);
        w.writeU16(targetIdx);
        w.writeU16(0);
        Net::sendToPeer(peerForSlot(np.slotIndex), w.data, w.cursor, /*reliable=*/true);
    }
```

Adapt `peerForSlot(np.slotIndex)` to the project's helper. If no per-peer helper exists, broadcast to the slot via existing send-to-slot pattern.

For melee with multiple hits, emit one event per hit (loop over the hits array).

- [ ] **Step 4**: Client-side handler — register a static `Engine::onDamageDone` callback that calls `PendingHitRingOps::ack(m_pendingHits, clientTick, targetIdx)`. Register in `engine_startgame.cpp` CLIENT branch alongside the other callbacks.

```cpp
    // Engine.h
    static void onDamageDone(u32 clientTick, u16 targetIdx);
    // engine.cpp or engine_net.cpp
    void Engine::onDamageDone(u32 clientTick, u16 targetIdx) {
        if (!s_engine) return;
        PendingHitRingOps::ack(s_engine->m_pendingHits, clientTick, targetIdx);
    }
```

- [ ] **Step 5**: Build, tests. 49/49 still pass. Commit: `feat(net): SV_DAMAGE_DONE event → PendingHitRing::ack — M10.2`.

---

## Task 3: SV_DAMAGE_TO_ME event + PendingDamageRing ack

Same shape as Task 2 but for damage-applied-to-player.

- [ ] **Step 1**: Add `SV_DAMAGE_TO_ME = 0x19` to NetPacketType.

Wire payload: `u32 projectileSrcKey + f32 damage + u16 reserved = 10 B`.

- [ ] **Step 2**: Add `OnDamageToMeFn` typedef + setter to net.h / net.cpp + client dispatch case.

- [ ] **Step 3**: Server emits when applying damage to a player. Search for `Combat::applyDamageToPlayer` calls (or wherever the server applies player damage). At each site, if the receiver is a remote (not host slot 0 on a SERVER role), emit SV_DAMAGE_TO_ME to that player:

```cpp
    // Compute the projectileSrcKey the way the client would (M7):
    // (ownerSlot << 24) | (proj.clientTick & 0xFFFFFFu)
    u32 key = (static_cast<u32>(proj.ownerSlot) << 24) | (proj.clientTick & 0xFFFFFFu);
    PacketWriter w;
    w.writeU8(static_cast<u8>(NetPacketType::SV_DAMAGE_TO_ME));
    w.writeU8(0); w.writeU16(0);
    w.writeU32(key);
    w.writeF32(damage);   // raw f32; assumes IEEE 754 little-endian on both ends
    w.writeU16(0);
    Net::sendToPeer(peerForSlot(victimSlot), w.data, w.cursor, /*reliable=*/true);
```

The hard part: matching the projectile that caused the damage to its `(ownerSlot, clientTick)` key. The server projectile has these fields available — pass them through to wherever damage is applied (likely in projectile.cpp's onCollideWithPlayer callback).

If wiring the key through is too invasive for v1, emit an "anonymous" SV_DAMAGE_TO_ME with `projectileSrcKey = 0` — the client side `ack` returns false and the entry stays for expireOlderThan to drop. The visual feedback already triggered; the ring is best-effort.

- [ ] **Step 4**: Client handler: ack `m_pendingDamage` on the matching key.

- [ ] **Step 5**: Build, tests. 49/49 pass. Commit: `feat(net): SV_DAMAGE_TO_ME event → PendingDamageRing::ack — M10.3`.

---

## Task 4: Verify

- [ ] Clean tree, build, 49/49 tests. M10 = 3 commits.

## Definition of Done
- [ ] 49/49 tests pass
- [ ] `grep -c 's_clientFireTx\|resendPendingFire\|FIRE_TX_REPEATS' src/` returns 0 (manual retransmit gone)
- [ ] CL_FIRE_WEAPON sent with `reliable=true`
- [ ] SV_DAMAGE_DONE and SV_DAMAGE_TO_ME enum entries exist
- [ ] Server emits both at appropriate sites
- [ ] Client acks via PendingHitRing / PendingDamageRing
