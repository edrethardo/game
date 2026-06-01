# Netplay D7.3: Real Per-Pool-Index Delta Encoding Plan

> superpowers:subagent-driven-development. Builds on D7.1 (slot-equality helpers) + D7.2 (baseline storage + isFullSnapshot flag).

**Goal:** Implement actual delta encoding using the per-pool-index identity already shipped on every Snap* record (`SnapPlayer.slotIndex`, `SnapEntity.poolIndex`, `SnapProjectile.poolIndex`, `SnapWorldItem.slotIndex`). The D7.1 deferral was correct — comparing dense-array positions [0..count-1] doesn't work when buildFromState re-sorts each tick. Comparing by poolIndex is what the records were already designed for.

**Wire format (delta mode, when isFullSnapshot=0):**
- 1 byte: `unchangedPlayersMask` (bits 0..3 set ⇒ "this slot's record is in the baseline, unchanged, omitted from wire")
- 8 bytes: `unchangedEntitiesMask[8]` (64 bits for 64 entity pool slots)
- 8 bytes: `unchangedProjectilesMask[8]` (64 bits — works for 64-slot pools; Switch's smaller pool means trailing bits unused; PC pool of 1024 means this approach only delta-compresses the first 64; FULL snapshot is sent for the rest. Acceptable for v1.)
- 8 bytes: `unchangedWorldItemsMask[8]` (64 bits)
- Then standard `count + N records` for each of players/entities/projectiles/worldItems — but excluding any pool-index whose unchanged bit is set.

The receiver, before reading the dense changed records, copies the unchanged slots from its locally-stored `m_lastAppliedSnap` baseline.

---

## Task 1: Helper functions for pool-index lookup + bitmask ops (TDD)

**Files:**
- Modify: `src/net/snapshot.h` — declare `findPlayerByPoolIndex` etc., bitmask set/get/test
- Modify: `src/net/snapshot.cpp` — implement
- Append: `tests/net/test_snapshot_delta.cpp` — TDD coverage

- [ ] **Step 1**: Add to `tests/net/test_snapshot_delta.cpp`:

```cpp
TEST_CASE("Snapshot::findEntityByPoolIndex returns null on missing") {
    WorldSnapshot s;
    s.entityCount = 0;
    CHECK(Snapshot::findEntityByPoolIndex(s, 5) == nullptr);
}

TEST_CASE("Snapshot::findEntityByPoolIndex returns matching record") {
    WorldSnapshot s;
    s.entityCount = 2;
    s.entities[0].poolIndex = 7;
    s.entities[1].poolIndex = 2;
    REQUIRE(Snapshot::findEntityByPoolIndex(s, 7) != nullptr);
    REQUIRE(Snapshot::findEntityByPoolIndex(s, 7) == &s.entities[0]);
    REQUIRE(Snapshot::findEntityByPoolIndex(s, 2) == &s.entities[1]);
    CHECK(Snapshot::findEntityByPoolIndex(s, 5) == nullptr);
}

TEST_CASE("Snapshot bitmask: set/get 64-bit operations") {
    u8 mask[8] = {};
    Snapshot::setBit64(mask, 0);
    Snapshot::setBit64(mask, 7);
    Snapshot::setBit64(mask, 8);
    Snapshot::setBit64(mask, 63);
    CHECK(Snapshot::getBit64(mask, 0) == true);
    CHECK(Snapshot::getBit64(mask, 7) == true);
    CHECK(Snapshot::getBit64(mask, 8) == true);
    CHECK(Snapshot::getBit64(mask, 63) == true);
    CHECK(Snapshot::getBit64(mask, 1) == false);
    CHECK(Snapshot::getBit64(mask, 32) == false);
    CHECK(Snapshot::getBit64(mask, 64) == false);  // out of range
}
```

- [ ] **Step 2**: Add helpers to snapshot.h:
```cpp
namespace Snapshot {
    // Pool-index lookups
    const SnapPlayer*    findPlayerByPoolIndex(const WorldSnapshot& s, u8 slotIndex);
    const SnapEntity*    findEntityByPoolIndex(const WorldSnapshot& s, u8 poolIndex);
    const SnapProjectile* findProjectileByPoolIndex(const WorldSnapshot& s, u16 poolIndex);
    const SnapWorldItem* findWorldItemByPoolIndex(const WorldSnapshot& s, u8 slotIndex);
    // 64-bit bitmask ops over u8[8]
    void setBit64(u8* mask, u32 bit);
    bool getBit64(const u8* mask, u32 bit);
}
```

- [ ] **Step 3**: Implement in snapshot.cpp using linear scan (counts are small: 4 players, ≤64 entities, ≤64 projectiles, ≤64 world items).

```cpp
const SnapEntity* Snapshot::findEntityByPoolIndex(const WorldSnapshot& s, u8 poolIndex) {
    for (u8 i = 0; i < s.entityCount; i++) {
        if (s.entities[i].poolIndex == poolIndex) return &s.entities[i];
    }
    return nullptr;
}
// ... similar for other three pools ...

void Snapshot::setBit64(u8* mask, u32 bit) {
    if (bit >= 64) return;
    mask[bit / 8] |= (1u << (bit % 8));
}
bool Snapshot::getBit64(const u8* mask, u32 bit) {
    if (bit >= 64) return false;
    return (mask[bit / 8] & (1u << (bit % 8))) != 0;
}
```

- [ ] **Step 4**: Build, expect 67/67 pass (64 + 3 new).

- [ ] **Step 5**: Commit: `feat(net): snapshot pool-index lookups + 64-bit mask helpers (TDD) — D7.3.1`.

---

## Task 2: Delta serialize/deserialize wire format

**Files:**
- Modify: `src/net/snapshot.cpp` — when `isFullSnapshot=0`, use the delta path
- Modify: `src/net/snapshot.h` — declare delta paths
- Append: `tests/net/test_snapshot_delta.cpp` — roundtrip test

- [ ] **Step 1**: Add roundtrip test:

```cpp
TEST_CASE("Snapshot delta roundtrip: unchanged entity is copied from baseline") {
    // Baseline has one entity at poolIndex 5, position (10, 0, 0).
    WorldSnapshot baseline;
    baseline.serverTick = 100;
    baseline.entityCount = 1;
    baseline.entities[0].poolIndex = 5;
    baseline.entities[0].posX = 10;

    // Current is identical (entity hasn't moved).
    WorldSnapshot current = baseline;
    current.serverTick = 101;

    // Encode current as delta against baseline.
    u8 buf[4096] = {};
    u32 size = Snapshot::serializeDelta(buf, sizeof(buf), current, baseline);
    REQUIRE(size > 0);

    // Decode with the baseline as the starting point.
    WorldSnapshot decoded;
    bool ok = Snapshot::deserializeDelta(decoded, buf, size, baseline);
    REQUIRE(ok);

    CHECK(decoded.serverTick == 101);
    CHECK(decoded.entityCount == 1);
    CHECK(decoded.entities[0].poolIndex == 5);
    CHECK(decoded.entities[0].posX == 10);   // copied from baseline
}

TEST_CASE("Snapshot delta roundtrip: changed entity uses new values") {
    WorldSnapshot baseline;
    baseline.serverTick = 100;
    baseline.entityCount = 1;
    baseline.entities[0].poolIndex = 5;
    baseline.entities[0].posX = 10;

    WorldSnapshot current = baseline;
    current.serverTick = 101;
    current.entities[0].posX = 50;   // moved

    u8 buf[4096] = {};
    u32 size = Snapshot::serializeDelta(buf, sizeof(buf), current, baseline);
    REQUIRE(size > 0);

    WorldSnapshot decoded;
    bool ok = Snapshot::deserializeDelta(decoded, buf, size, baseline);
    REQUIRE(ok);

    CHECK(decoded.entities[0].posX == 50);   // new value from delta payload
}

TEST_CASE("Snapshot delta roundtrip: removed entity not present") {
    WorldSnapshot baseline;
    baseline.serverTick = 100;
    baseline.entityCount = 2;
    baseline.entities[0].poolIndex = 5;
    baseline.entities[1].poolIndex = 7;

    WorldSnapshot current = baseline;
    current.serverTick = 101;
    current.entityCount = 1;
    current.entities[0].poolIndex = 7;   // poolIndex 5 dropped
    current.entities[1] = {};            // cleared

    u8 buf[4096] = {};
    u32 size = Snapshot::serializeDelta(buf, sizeof(buf), current, baseline);
    REQUIRE(size > 0);

    WorldSnapshot decoded;
    bool ok = Snapshot::deserializeDelta(decoded, buf, size, baseline);
    REQUIRE(ok);

    CHECK(decoded.entityCount == 1);
    CHECK(decoded.entities[0].poolIndex == 7);
}
```

- [ ] **Step 2**: Implement in `snapshot.cpp`:

```cpp
u32 Snapshot::serializeDelta(u8* outBuf, u32 outCap, const WorldSnapshot& current, const WorldSnapshot& baseline) {
    // Wire format:
    //   - header same as full: u32 serverTick, u32 lastProcessedInputTick[MAX_PLAYERS],
    //     [u8 isFullSnapshot=0]
    //   - then 1 byte unchangedPlayersMask + 8 bytes per other pool
    //   - then dense [count + records] for each pool, excluding unchanged ones

    PacketWriter w(outBuf, outCap);
    w.writeU32(current.serverTick);
    for (u32 i = 0; i < MAX_PLAYERS; i++) w.writeU32(current.lastProcessedInputTick[i]);
    w.writeU8(0);   // isFullSnapshot = 0

    // Compute masks
    u8 unchangedPlayers = 0;
    for (u8 i = 0; i < current.playerCount; i++) {
        u8 slot = current.players[i].slotIndex;
        const SnapPlayer* base = findPlayerByPoolIndex(baseline, slot);
        if (base && std::memcmp(base, &current.players[i], sizeof(SnapPlayer)) == 0) {
            unchangedPlayers |= (1u << slot);
        }
    }
    u8 unchangedEntities[8] = {};
    for (u8 i = 0; i < current.entityCount; i++) {
        u8 pi = current.entities[i].poolIndex;
        const SnapEntity* base = findEntityByPoolIndex(baseline, pi);
        if (base && std::memcmp(base, &current.entities[i], sizeof(SnapEntity)) == 0) {
            setBit64(unchangedEntities, pi);
        }
    }
    u8 unchangedProjectiles[8] = {};
    for (u16 i = 0; i < current.projectileCount; i++) {
        u16 pi = current.projectiles[i].poolIndex;
        if (pi >= 64) continue;   // bitmask only covers first 64 slots; rest always sent
        const SnapProjectile* base = findProjectileByPoolIndex(baseline, pi);
        if (base && std::memcmp(base, &current.projectiles[i], sizeof(SnapProjectile)) == 0) {
            setBit64(unchangedProjectiles, pi);
        }
    }
    u8 unchangedWorldItems[8] = {};
    for (u8 i = 0; i < current.worldItemCount; i++) {
        u8 si = current.worldItems[i].slotIndex;
        const SnapWorldItem* base = findWorldItemByPoolIndex(baseline, si);
        if (base && std::memcmp(base, &current.worldItems[i], sizeof(SnapWorldItem)) == 0) {
            setBit64(unchangedWorldItems, si);
        }
    }

    // Write masks
    w.writeU8(unchangedPlayers);
    for (u32 i = 0; i < 8; i++) w.writeU8(unchangedEntities[i]);
    for (u32 i = 0; i < 8; i++) w.writeU8(unchangedProjectiles[i]);
    for (u32 i = 0; i < 8; i++) w.writeU8(unchangedWorldItems[i]);

    // Write changed records: for each, write its full payload if not in mask
    auto writePlayerIfChanged = [&](const SnapPlayer& p) {
        if (unchangedPlayers & (1u << p.slotIndex)) return false;
        // Inline the existing per-record serializer here; for brevity assume there's
        // already a writePlayer(w, p) helper. If not, copy the bytes from the existing
        // full-serialize function's body.
        writePlayer(w, p);
        return true;
    };
    // Count first, then write — protocol expects count prefix.
    u8 changedPlayers = 0;
    for (u8 i = 0; i < current.playerCount; i++) {
        if (!(unchangedPlayers & (1u << current.players[i].slotIndex))) changedPlayers++;
    }
    w.writeU8(changedPlayers);
    for (u8 i = 0; i < current.playerCount; i++) writePlayerIfChanged(current.players[i]);

    // Repeat for entities, projectiles, worldItems with their own write helpers.
    // ... (same shape) ...

    return w.cursor;
}

bool Snapshot::deserializeDelta(WorldSnapshot& out, const u8* buf, u32 size, const WorldSnapshot& baseline) {
    PacketReader r(buf, size);
    out.serverTick = r.readU32();
    for (u32 i = 0; i < MAX_PLAYERS; i++) out.lastProcessedInputTick[i] = r.readU32();
    u8 isFull = r.readU8();
    if (isFull != 0) return false;   // caller should use deserialize() for full

    u8 unchangedPlayers = r.readU8();
    u8 unchangedEntities[8]; for (u32 i = 0; i < 8; i++) unchangedEntities[i] = r.readU8();
    u8 unchangedProjectiles[8]; for (u32 i = 0; i < 8; i++) unchangedProjectiles[i] = r.readU8();
    u8 unchangedWorldItems[8]; for (u32 i = 0; i < 8; i++) unchangedWorldItems[i] = r.readU8();

    // Start by copying ALL unchanged baseline slots into out.
    out.playerCount = 0;
    for (u8 i = 0; i < baseline.playerCount; i++) {
        u8 slot = baseline.players[i].slotIndex;
        if (unchangedPlayers & (1u << slot)) {
            out.players[out.playerCount++] = baseline.players[i];
        }
    }
    out.entityCount = 0;
    for (u8 i = 0; i < baseline.entityCount; i++) {
        u8 pi = baseline.entities[i].poolIndex;
        if (getBit64(unchangedEntities, pi)) {
            out.entities[out.entityCount++] = baseline.entities[i];
        }
    }
    out.projectileCount = 0;
    for (u16 i = 0; i < baseline.projectileCount; i++) {
        u16 pi = baseline.projectiles[i].poolIndex;
        if (pi < 64 && getBit64(unchangedProjectiles, pi)) {
            out.projectiles[out.projectileCount++] = baseline.projectiles[i];
        }
    }
    out.worldItemCount = 0;
    for (u8 i = 0; i < baseline.worldItemCount; i++) {
        u8 si = baseline.worldItems[i].slotIndex;
        if (getBit64(unchangedWorldItems, si)) {
            out.worldItems[out.worldItemCount++] = baseline.worldItems[i];
        }
    }

    // Then read changed records and append.
    u8 changedPlayers = r.readU8();
    for (u8 i = 0; i < changedPlayers; i++) {
        SnapPlayer p;
        readPlayer(r, p);
        out.players[out.playerCount++] = p;
    }
    // Repeat for entities/projectiles/worldItems.

    return true;
}
```

The helper functions `writePlayer` / `readPlayer` / etc. need to be extracted from the existing serialize/deserialize so they can be called from both the full and delta paths. Pragmatic: refactor in this same commit.

If extracting the per-record serializers is too invasive, fall back to: encode the delta as `full snapshot bytes` minus the unchanged-slot bytes, with bitmask up front telling the decoder which records to skip+copy-from-baseline. This still requires a per-record skip mechanism but doesn't need full helpers.

- [ ] **Step 3**: Wire serializeDelta into the server's snapshot send path. In `engine_net.cpp::serverNetPost`, before calling Server::sendSnapshot, decide per-client:
```cpp
    for (each connected remote slot) {
        if (BaselineTrackerOps::shouldSendFullSnapshot(m_baselines[slot], m_clientAckedSnap[slot])) {
            // Send full snapshot
            Server::sendSnapshotFull(...);  // existing path with isFullSnapshot=1
        } else {
            // Send delta against m_baselineSnap[slot]
            Server::sendSnapshotDelta(..., m_baselineSnap[slot]);
        }
        // Always update baseline after send
        m_baselineSnap[slot] = currentSnap;
    }
```

This requires Server::sendSnapshot to support both modes — refactor accordingly.

- [ ] **Step 4**: Wire deserializeDelta into the client's receive path. In `Client::receiveSnapshot`, peek the isFullSnapshot byte to decide which decoder to call. Pass `m_lastAppliedSnap` as baseline for the delta path.

- [ ] **Step 5**: Build, 70/70 tests pass (67 + 3 new roundtrip).

- [ ] **Step 6**: Commit: `feat(net): per-pool-index slot-delta encoding (D7.3.2)`.

---

## Task 3: Verify + smoke

- [ ] **Step 1**: Build full clean, 70/70 tests pass.

- [ ] **Step 2**: Manual smoke deferred to user (live two-process co-op test). Verifies that:
  - Items, players, entities, projectiles all render correctly via delta-encoded snapshots
  - No visible desync from missed unchanged bits
  - Bandwidth on the wire is measurably lower at steady state (compare snapshot byte counts in the log)

## Definition of Done
- [ ] 70/70 tests pass
- [ ] `Snapshot::serializeDelta` + `deserializeDelta` exist and are called by server/client
- [ ] Per-client `m_baselineSnap` is consulted when not the first snapshot
- [ ] Roundtrip test verifies unchanged/changed/removed slot cases
