# Netplay M6: Predicted Enemy Hit Reactions Plan

> superpowers:subagent-driven-development. Checkbox `- [ ]`.

**Goal:** Lock the existing M0-baseline predicted hit FX (already running against `m_renderInterp.entities` on CLIENT melee + hitscan) under a recording structure that M10 can consume. New `PendingHitRing` records (clientTick, targetEntityId) tuples on each predicted hit. M10 (reliable channel cleanup) wires up the consumer: when `SV_DAMAGE_DONE` arrives, it acks predicted entries; mismatches trigger soft FX cleanup.

**This milestone does NOT yet ack predicted hits** — that needs the reliable damage-done event from M10. M6 records; M10 reconciles. M6 is the producer half.

---

## Task 1: PendingHitRing scaffold + tests

**Files:**
- Create: `src/net/pending_hit_ring.h`, `src/net/pending_hit_ring.cpp`
- Create: `tests/net/test_pending_hit_ring.cpp`
- Modify: both CMakeLists

- [ ] **Step 1**: Create `tests/net/test_pending_hit_ring.cpp` with 4 TEST_CASEs (red phase):

```cpp
#include <doctest/doctest.h>
#include "net/pending_hit_ring.h"

TEST_CASE("PendingHitRing: empty ring has zero pending") {
    PendingHitRing r;
    PendingHitRingOps::reset(r);
    CHECK(r.count == 0);
}

TEST_CASE("PendingHitRing: record adds an entry") {
    PendingHitRing r;
    PendingHitRingOps::reset(r);
    PendingHitRingOps::record(r, 100, 5, 0);  // clientTick=100, entityIdx=5, isPlayer=0
    CHECK(r.count == 1);
    CHECK(r.entries[0].clientTick == 100);
    CHECK(r.entries[0].targetIdx == 5);
}

TEST_CASE("PendingHitRing: ack matches and clears entry") {
    PendingHitRing r;
    PendingHitRingOps::reset(r);
    PendingHitRingOps::record(r, 100, 5, 0);
    PendingHitRingOps::record(r, 101, 7, 0);
    bool acked = PendingHitRingOps::ack(r, 100, 5);
    CHECK(acked == true);
    // entry 0 removed, entry 1 still present
    CHECK(r.count == 1);
    CHECK(r.entries[0].clientTick == 101);
}

TEST_CASE("PendingHitRing: ack returns false on miss") {
    PendingHitRing r;
    PendingHitRingOps::reset(r);
    PendingHitRingOps::record(r, 100, 5, 0);
    CHECK(PendingHitRingOps::ack(r, 999, 5) == false);
    CHECK(r.count == 1);
}

TEST_CASE("PendingHitRing: expireOlderThan removes stale entries") {
    PendingHitRing r;
    PendingHitRingOps::reset(r);
    PendingHitRingOps::record(r, 50, 1, 0);
    PendingHitRingOps::record(r, 100, 2, 0);
    PendingHitRingOps::record(r, 150, 3, 0);
    PendingHitRingOps::expireOlderThan(r, 100);
    CHECK(r.count == 2);  // 50 removed, 100 and 150 remain
}
```

- [ ] **Step 2**: Create `src/net/pending_hit_ring.h`:

```cpp
#pragma once
#include "core/types.h"

static constexpr u32 PENDING_HIT_RING_CAPACITY = 64;

struct PendingHit {
    u32 clientTick = 0;     // the client tick on which the predicted hit fired
    u16 targetIdx  = 0;     // entity slot index OR player slot (see isPlayer)
    u8  isPlayer   = 0;     // 0 = entity, 1 = remote player target
    u8  acked      = 0;     // 1 if server confirmed, marked for cleanup
};

struct PendingHitRing {
    PendingHit entries[PENDING_HIT_RING_CAPACITY] = {};
    u32        count = 0;
};

namespace PendingHitRingOps {
    void reset(PendingHitRing& r);
    void record(PendingHitRing& r, u32 clientTick, u16 targetIdx, u8 isPlayer);
    // Returns true if a matching entry was found and removed. M10 calls this on
    // SV_DAMAGE_DONE arrival.
    bool ack(PendingHitRing& r, u32 clientTick, u16 targetIdx);
    // Drop entries with clientTick < cutoff (kept compact). Call ~1 Hz to bound
    // memory if a server stops sending acks.
    void expireOlderThan(PendingHitRing& r, u32 cutoffClientTick);
}
```

- [ ] **Step 3**: Create `src/net/pending_hit_ring.cpp`:

```cpp
#include "net/pending_hit_ring.h"

void PendingHitRingOps::reset(PendingHitRing& r) {
    for (u32 i = 0; i < PENDING_HIT_RING_CAPACITY; i++) r.entries[i] = {};
    r.count = 0;
}

void PendingHitRingOps::record(PendingHitRing& r, u32 clientTick, u16 targetIdx, u8 isPlayer) {
    if (r.count >= PENDING_HIT_RING_CAPACITY) {
        // Drop oldest (shift down).
        for (u32 i = 1; i < PENDING_HIT_RING_CAPACITY; i++) r.entries[i-1] = r.entries[i];
        r.entries[PENDING_HIT_RING_CAPACITY - 1] = {};
        r.count = PENDING_HIT_RING_CAPACITY - 1;
    }
    PendingHit& e = r.entries[r.count];
    e.clientTick = clientTick;
    e.targetIdx  = targetIdx;
    e.isPlayer   = isPlayer;
    e.acked      = 0;
    r.count++;
}

bool PendingHitRingOps::ack(PendingHitRing& r, u32 clientTick, u16 targetIdx) {
    for (u32 i = 0; i < r.count; i++) {
        PendingHit& e = r.entries[i];
        if (e.clientTick == clientTick && e.targetIdx == targetIdx) {
            // shift down
            for (u32 j = i + 1; j < r.count; j++) r.entries[j-1] = r.entries[j];
            r.entries[r.count - 1] = {};
            r.count--;
            return true;
        }
    }
    return false;
}

void PendingHitRingOps::expireOlderThan(PendingHitRing& r, u32 cutoffClientTick) {
    u32 write = 0;
    for (u32 read = 0; read < r.count; read++) {
        if (r.entries[read].clientTick >= cutoffClientTick) {
            if (write != read) r.entries[write] = r.entries[read];
            write++;
        }
    }
    for (u32 i = write; i < r.count; i++) r.entries[i] = {};
    r.count = write;
}
```

- [ ] **Step 4**: Wire into CMake. Build, run. Expected: 36/36 pass (31 + 5 new).

- [ ] **Step 5**: Commit: `feat(net): PendingHitRing scaffold + tests (TDD) — M6.1`.

---

## Task 2: Record predicted hits in client-side melee + hitscan

**Files:**
- Modify: `src/engine/engine.h` — add `PendingHitRing m_pendingHits;`
- Modify: `src/engine/engine_combat.cpp` — call `PendingHitRingOps::record` in each predicted-hit branch

- [ ] **Step 1**: Add to engine.h:
```cpp
    PendingHitRing m_pendingHits;
```
With `#include "net/pending_hit_ring.h"`.

- [ ] **Step 2**: In `src/engine/engine_combat.cpp`, find the Phase 2 melee prediction (around line 327) where the loop iterates `hits[]` and spawns impact FX on `m_renderInterp.entities`. After spawning the FX for each predicted hit, record:

```cpp
        PendingHitRingOps::record(m_pendingHits, m_clientTick, static_cast<u16>(hits[i]), 0);
```

Same in the hitscan branch (around line 418): after `CombatQuery::raycast(...)` returns a hit, record:

```cpp
        PendingHitRingOps::record(m_pendingHits, m_clientTick, static_cast<u16>(hit.entityHandle), 0);
```

(Use the actual hit-entity field name — adapt per code structure.)

- [ ] **Step 3**: Reset on connect. Same spot as ClockSync / PredictionRing / RenderOffset reset in `engine_startgame.cpp`. Add `PendingHitRingOps::reset(m_pendingHits);`.

- [ ] **Step 4**: Build, tests (36/36). Commit: `feat(net): record predicted hits into PendingHitRing — M6.2`.

The ring is consumed by M10's SV_DAMAGE_DONE handler. For now entries accumulate and are pruned by `expireOlderThan` (which we don't yet call — M10 will).

---

## Task 3: Verify

- [ ] Clean build, 36/36 tests. M6 = 2 commits.

## Definition of Done
- [ ] 36/36 tests pass
- [ ] `PendingHitRing m_pendingHits` exists in engine.h
- [ ] `PendingHitRingOps::record` called in melee + hitscan CLIENT branches
