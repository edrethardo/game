# Netplay Phase 2 D7: Per-Slot Delta Encoding Plan

> superpowers:subagent-driven-development. Builds on M11 (BaselineTracker).

**Goal:** Implement slot-level delta encoding. Server compares current snapshot vs the per-client baseline snapshot it last sent. For each slot (player/entity/projectile/worlditem), if the slot's payload is byte-identical to the baseline, set a "unchanged" bit and skip the payload. Client decoder, after applying changed slots, copies unchanged slots from its locally-stored baseline.

**Scope realism:** Per-FIELD delta within a slot (only ship the position bytes that changed, not the whole slot) is excluded — that's a deeper, more error-prone change. Slot-level only ships the simpler "all-or-nothing per slot" delta, which still saves substantial bandwidth at steady state.

---

## D7.1: Per-client baseline snapshot storage + slot-bitmask wire format (TDD)

**Files:**
- Modify: `src/net/snapshot.h` — add a `u32 changedSlotBitmask` to wire format header. With MAX_PLAYERS=4 + MAX_ENTITIES=64 + MAX_PROJECTILES=64 + MAX_WORLD_ITEMS=64 = 196 slots, we need ~7 u32 words of bitmask. Round up to 8.
- Modify: `src/net/snapshot.cpp` — serialize-delta function compares against baseline; deserialize-delta applies on top of baseline
- Modify: `src/engine/engine.h` — per-client `WorldSnapshot m_baselineSnap[MAX_PLAYERS]` (server-side memory cost: ~4 × ~50 KB = 200 KB total)
- Create: `tests/net/test_snapshot_delta.cpp` — TDD for the encode/decode roundtrip

This is a lot of memory + wire-format change. For correctness, TDD the compare/encode/decode primitives in isolation.

- [ ] **Step 1**: Create `tests/net/test_snapshot_delta.cpp` with TEST_CASEs for:
  - Roundtrip: serialize+deserialize with no baseline produces identical full snapshot
  - Compare: identical snapshots produce zero bitmask
  - Compare: one changed slot produces bitmask with exactly one bit set
  - Encode: changed-slot payload is written, unchanged is skipped
  - Decode: unchanged slots are copied from the locally-stored baseline

(Given the scope and the fact that exact wire-format details depend on snapshot.cpp internals, the tests should focus on a small "diff helper" — `snapshotSlotsEqual_Players(a, b, slot)` etc. — rather than the full encode pipeline.)

```cpp
#include <doctest/doctest.h>
#include "net/snapshot.h"

TEST_CASE("Delta: empty WorldSnapshots compare equal per slot") {
    WorldSnapshot a;
    WorldSnapshot b;
    // Per-player slot comparison
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        CHECK(Snapshot::playerSlotsEqual(a, b, i) == true);
    }
}

TEST_CASE("Delta: differing player position produces unequal slot") {
    WorldSnapshot a; WorldSnapshot b;
    a.players[0].active = true;
    b.players[0].active = true;
    a.players[0].posX = 100;
    b.players[0].posX = 200;
    CHECK(Snapshot::playerSlotsEqual(a, b, 0) == false);
    CHECK(Snapshot::playerSlotsEqual(a, b, 1) == true);  // slot 1 still equal
}

TEST_CASE("Delta: empty active flag flip produces unequal slot") {
    WorldSnapshot a; WorldSnapshot b;
    a.players[0].active = false;
    b.players[0].active = true;
    CHECK(Snapshot::playerSlotsEqual(a, b, 0) == false);
}
```

(Similar for entities, projectiles, world items.)

- [ ] **Step 2**: Implement the per-slot equality helpers in `src/net/snapshot.cpp`:

```cpp
bool Snapshot::playerSlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot) {
    if (slot >= MAX_PLAYERS) return true;
    return std::memcmp(&a.players[slot], &b.players[slot], sizeof(SnapPlayer)) == 0;
}
bool Snapshot::entitySlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot) {
    if (slot >= MAX_ENTITIES) return true;
    return std::memcmp(&a.entities[slot], &b.entities[slot], sizeof(SnapEntity)) == 0;
}
bool Snapshot::projectileSlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot) {
    if (slot >= MAX_PROJECTILES) return true;
    return std::memcmp(&a.projectiles[slot], &b.projectiles[slot], sizeof(SnapProjectile)) == 0;
}
bool Snapshot::worldItemSlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot) {
    if (slot >= MAX_WORLD_ITEMS) return true;
    return std::memcmp(&a.worldItems[slot], &b.worldItems[slot], sizeof(SnapWorldItem)) == 0;
}
```

Caveat: `memcmp` on structs may produce false-positives from padding bytes. If SnapPlayer / SnapEntity etc. have padding that uninitialized memory makes random, the compare will spuriously declare slots changed. To be safe, ensure all snapshot building paths zero-init the struct before populating (use `SnapPlayer sp{};` not `SnapPlayer sp;`). If padding is unavoidable, change to field-by-field comparison (~6-10 fields per struct — tractable).

For v1, document the padding assumption: snapshot.cpp's serialize already does `w8(sp.field)` per field, so the wire is field-by-field — the memcmp is just an optimization for the "all bytes equal" common case. If memcmp says equal, the WIRE bytes will also be equal because the wire is derived field-by-field.

To be fully correct: instead of memcmp on the WHOLE struct, compare only the fields that the serializer ships. That's a per-field whitelist. Pragmatic v1: just memcmp and accept the risk of occasional false-positives from padding (which would result in shipping an unchanged slot — bandwidth waste, not correctness bug).

- [ ] **Step 3**: Add declarations to `src/net/snapshot.h`:

```cpp
namespace Snapshot {
    // ... existing ...
    bool playerSlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot);
    bool entitySlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot);
    bool projectileSlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot);
    bool worldItemSlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot);
}
```

- [ ] **Step 4**: Wire test file into tests/CMakeLists.txt. Build, expect 57-60/57-60 pass (54 + new ones).

- [ ] **Step 5**: Commit: `feat(net): snapshot slot-equality helpers + TDD (D7.1)`.

---

## D7.2: Per-client baseline storage + slot-delta serialize/deserialize

**Files:**
- Modify: `src/engine/engine.h` — add `WorldSnapshot m_baselineSnap[MAX_PLAYERS]`
- Modify: `src/net/snapshot.h/.cpp` — add `serializeDelta(buf, size, current, baseline, deltaFlag)` + `deserializeDelta(buf, size, out, baseline)` that respect a bitmask header
- Modify: server snapshot send path — choose full or delta per client; copy current → baseline after send
- Modify: client snapshot receive path — apply delta against the locally-stored last snapshot

**Wire-format change:**

Snapshot header gains:
- `u8 isFullSnapshot` (1 = full, 0 = delta)
- `u32 changedBits[8]` (256 slot bits — covers MAX_PLAYERS + MAX_ENTITIES + MAX_PROJECTILES + MAX_WORLD_ITEMS up to 256)

Slot layout indices in the bitmask (bit positions):
- 0..3:        players
- 4..67:       entities (0..63)
- 68..131:     projectiles (0..63)
- 132..195:    world items (0..63)
- 196..255:    reserved

Each slot's bit set ⇒ that slot's payload follows in the snapshot body. Unchanged slots have no body bytes; client copies them from baseline.

This is a moderately complex wire-format change. For v1, restrict to:

**Simpler v1 wire format:**
- Header gets just `u8 isFullSnapshot`. (Always 1 for v1 — actual delta encoding lands in D7.3.)
- D7.2 just adds the FLAG infrastructure + per-client baseline storage. Server still sends full snapshots but stamps `isFullSnapshot=1`. Client stores the received snapshot as its baseline.

D7.3 then turns on actual delta encoding (using the helpers from D7.1).

- [ ] **Step 1**: Add `u8 isFullSnapshot` to wire format. Bump it in serialize; read in deserialize. For v1 always write 1.

- [ ] **Step 2**: Server-side: `WorldSnapshot m_baselineSnap[MAX_PLAYERS]` member. After sending a snapshot to client `i`, `m_baselineSnap[i] = currentSnap` (copy). For v1 this stores the full snapshot; D7.3 uses it for comparison.

- [ ] **Step 3**: Client-side: `WorldSnapshot m_lastAppliedSnap` member. After successful deserialize, copy the result into `m_lastAppliedSnap`. For v1 this is unused; D7.3 reads from it for unchanged slots.

- [ ] **Step 4**: Build, tests still pass (54+). Commit: `feat(net): snapshot delta-format flag + per-client baselines storage (D7.2)`.

---

## D7.3: Turn on actual slot-delta encoding

**Files:**
- Modify: `src/net/snapshot.cpp` — serialize: if `!isFullSnapshot`, write only slots whose bit is set in changedBits; deserialize: same in reverse
- Modify: server snapshot path — compute changedBits by comparing current vs baseline; emit delta if baseline matches client's acked tick; emit full otherwise
- Modify: client decode — for slots whose bit is NOT set in changedBits, copy from `m_lastAppliedSnap`

- [ ] **Step 1**: Add bitmask helpers:
```cpp
namespace Snapshot {
    void setSlotBit(u32* bits, u32 slot);
    bool getSlotBit(const u32* bits, u32 slot);
    // Compute changedBits[8] given current and baseline snapshots.
    void computeChangedBits(const WorldSnapshot& current,
                            const WorldSnapshot& baseline,
                            u32 outBits[8]);
}
```

`setSlotBit / getSlotBit` are 1-liners (`bits[s/32] |= 1u << (s % 32)` etc.).

`computeChangedBits` loops over all 4 slot families using the D7.1 helpers.

- [ ] **Step 2**: Modify serialize/deserialize to read/write changedBits when `!isFullSnapshot`.

When writing a delta snapshot:
- Write `isFullSnapshot = 0` + 32 bytes of bitmask.
- Loop over each slot. If bit is set, write the slot's full payload. If not, write nothing.

When reading:
- Read `isFullSnapshot`. If 1, read full snapshot as today.
- If 0, read 32 bytes of bitmask. For each slot, if bit set, read its payload; otherwise leave it (caller will copy from baseline).

- [ ] **Step 3**: After deserialize on client, for each unchanged slot, copy from `m_lastAppliedSnap`:
```cpp
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (!getSlotBit(snap.changedBits, i)) {
            snap.players[i] = m_lastAppliedSnap.players[i];
        }
    }
    // ... same for entities, projectiles, worldItems
```

- [ ] **Step 4**: Server snapshot-send decision: if `m_clientAckedSnap[slot]` matches `m_baselines[slot].baselineTick`, send delta with computeChangedBits against m_baselineSnap[slot]. Otherwise send full.

```cpp
    bool sendFull = BaselineTrackerOps::shouldSendFullSnapshot(m_baselines[slot], m_clientAckedSnap[slot]);
    if (!sendFull) {
        u32 changedBits[8];
        Snapshot::computeChangedBits(currentSnap, m_baselineSnap[slot], changedBits);
        Snapshot::serializeDelta(buf, currentSnap, changedBits);
    } else {
        Snapshot::serialize(buf, currentSnap);
    }
    // Update baseline for next tick
    m_baselineSnap[slot] = currentSnap;
    BaselineTrackerOps::store(m_baselines[slot], m_serverTick);
```

- [ ] **Step 5**: Build, tests. 54+ tests pass. Commit: `feat(net): actual slot-delta encoding turned on (D7.3)`.

---

## Verify

- [ ] Clean tree, build, 54+ tests pass. Total commits for D7 = 3.

## Definition of Done
- [ ] 54+ tests pass
- [ ] WorldSnapshot has changedBits[8] field
- [ ] Server emits delta snapshots when client's acked tick matches baseline
- [ ] Client correctly reconstructs full snapshot from delta + baseline
- [ ] Bandwidth measurably reduced (qualitative — manual log of avg snapshot size)
