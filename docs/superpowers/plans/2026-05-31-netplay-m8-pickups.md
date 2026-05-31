# Netplay M8: Predicted Pickups Plan

> superpowers:subagent-driven-development. Checkbox `- [ ]`.

**Goal:** Visually predict the removal of a world item the moment the client sends `CL_PICKUP_ITEM`, instead of waiting ~RTT/2 for the next snapshot to mirror it away. Server-side validation is unchanged: if rejected, the next snapshot still contains the item and `mirrorWorldItems` brings it back. Inventory addition stays server-driven (snapshot-mirrored) to avoid duplication risk.

**Scope minimization:** v1 predicts the WORLD ITEM disappearance only. The inventory "add" stays snapshot-driven — the existing ~80 ms lag on seeing the new item in your bag is acceptable for v1. Full inventory prediction can be a future enhancement.

---

## Task 1: PendingPickupRing scaffold + tests

**Files:**
- Create: `src/net/pending_pickup_ring.h`, `src/net/pending_pickup_ring.cpp`
- Create: `tests/net/test_pending_pickup_ring.cpp`
- Modify: both CMakeLists

- [ ] **Step 1**: Create `tests/net/test_pending_pickup_ring.cpp`:

```cpp
#include <doctest/doctest.h>
#include "net/pending_pickup_ring.h"

TEST_CASE("PendingPickupRing: empty") {
    PendingPickupRing r;
    PendingPickupRingOps::reset(r);
    CHECK(r.count == 0);
    CHECK(PendingPickupRingOps::isPending(r, 42) == false);
}

TEST_CASE("PendingPickupRing: record and isPending") {
    PendingPickupRing r;
    PendingPickupRingOps::reset(r);
    PendingPickupRingOps::record(r, 100, 42);
    CHECK(r.count == 1);
    CHECK(PendingPickupRingOps::isPending(r, 42) == true);
    CHECK(PendingPickupRingOps::isPending(r, 999) == false);
}

TEST_CASE("PendingPickupRing: ack removes entry") {
    PendingPickupRing r;
    PendingPickupRingOps::reset(r);
    PendingPickupRingOps::record(r, 100, 42);
    PendingPickupRingOps::record(r, 101, 43);
    CHECK(PendingPickupRingOps::ack(r, 42) == true);
    CHECK(r.count == 1);
    CHECK(PendingPickupRingOps::isPending(r, 42) == false);
    CHECK(PendingPickupRingOps::isPending(r, 43) == true);
}

TEST_CASE("PendingPickupRing: expireOlderThan") {
    PendingPickupRing r;
    PendingPickupRingOps::reset(r);
    PendingPickupRingOps::record(r, 50, 1);
    PendingPickupRingOps::record(r, 150, 2);
    PendingPickupRingOps::expireOlderThan(r, 100);
    CHECK(r.count == 1);
    CHECK(PendingPickupRingOps::isPending(r, 1) == false);
    CHECK(PendingPickupRingOps::isPending(r, 2) == true);
}
```

- [ ] **Step 2**: Create `src/net/pending_pickup_ring.h`:

```cpp
#pragma once
#include "core/types.h"

static constexpr u32 PENDING_PICKUP_RING_CAPACITY = 16;

struct PendingPickup {
    u32 clientTick = 0;
    u32 itemUid    = 0;
};

struct PendingPickupRing {
    PendingPickup entries[PENDING_PICKUP_RING_CAPACITY] = {};
    u32           count = 0;
};

namespace PendingPickupRingOps {
    void reset(PendingPickupRing& r);
    void record(PendingPickupRing& r, u32 clientTick, u32 itemUid);
    bool isPending(const PendingPickupRing& r, u32 itemUid);
    bool ack(PendingPickupRing& r, u32 itemUid);
    void expireOlderThan(PendingPickupRing& r, u32 cutoffClientTick);
}
```

- [ ] **Step 3**: Create `src/net/pending_pickup_ring.cpp`:

```cpp
#include "net/pending_pickup_ring.h"

void PendingPickupRingOps::reset(PendingPickupRing& r) {
    for (u32 i = 0; i < PENDING_PICKUP_RING_CAPACITY; i++) r.entries[i] = {};
    r.count = 0;
}

void PendingPickupRingOps::record(PendingPickupRing& r, u32 clientTick, u32 itemUid) {
    if (r.count >= PENDING_PICKUP_RING_CAPACITY) {
        for (u32 i = 1; i < PENDING_PICKUP_RING_CAPACITY; i++) r.entries[i-1] = r.entries[i];
        r.count = PENDING_PICKUP_RING_CAPACITY - 1;
    }
    PendingPickup& e = r.entries[r.count];
    e.clientTick = clientTick;
    e.itemUid    = itemUid;
    r.count++;
}

bool PendingPickupRingOps::isPending(const PendingPickupRing& r, u32 itemUid) {
    for (u32 i = 0; i < r.count; i++) {
        if (r.entries[i].itemUid == itemUid) return true;
    }
    return false;
}

bool PendingPickupRingOps::ack(PendingPickupRing& r, u32 itemUid) {
    for (u32 i = 0; i < r.count; i++) {
        if (r.entries[i].itemUid == itemUid) {
            for (u32 j = i + 1; j < r.count; j++) r.entries[j-1] = r.entries[j];
            r.entries[r.count - 1] = {};
            r.count--;
            return true;
        }
    }
    return false;
}

void PendingPickupRingOps::expireOlderThan(PendingPickupRing& r, u32 cutoffClientTick) {
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

- [ ] **Step 4**: Wire into CMakeLists. Build. 45/45 pass (41 + 4).

- [ ] **Step 5**: Commit: `feat(net): PendingPickupRing scaffold + tests (TDD) — M8.1`.

---

## Task 2: Predict pickup on CL_PICKUP_ITEM send

**Files:**
- Modify: `src/engine/engine.h` — add `PendingPickupRing m_pendingPickups;`
- Modify: `src/engine/engine_update.cpp` — at the CL_PICKUP_ITEM send site (around line 1070), record + locally hide the item; in mirrorWorldItems consumers, ack any pending entries when the snapshot confirms the item is gone.

- [ ] **Step 1**: Add to engine.h:
```cpp
    PendingPickupRing m_pendingPickups;
```

- [ ] **Step 2**: At the CL_PICKUP_ITEM send site in engine_update.cpp (around 1070): immediately after the send, find the local world-item with that uid and deactivate it locally:

```cpp
    // Local prediction: hide the item until snapshot confirms or restores.
    PendingPickupRingOps::record(m_pendingPickups, m_clientTick, uid);
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = m_worldItems.items[i];
        if (wi.active && wi.uid == uid) {
            wi.active = false;
            break;
        }
    }
```

Adjust field names to the project's actual WorldItem struct. Look at WorldItemPool / WorldItem in src/game/world_item.h or similar.

- [ ] **Step 3**: Bound the ring with `expireOlderThan`. In `clientNetPost` (the same place ClockSync/PredictionRing run): after `mirrorWorldItems`, if any pending entry now has its item gone from the snapshot (or has aged out), drop it:

Actually, the reconcile is implicit:
- `Client::mirrorWorldItems` rewrites `m_worldItems` from the snapshot each frame.
- If server picked up the item, snapshot lacks it → mirror leaves it absent (no change from our local hide).
- If server rejected, snapshot still contains it → mirror activates it → item reappears visually. Good.
- The PendingPickupRing entry is just a flag for "I'm waiting on this", currently unused by the reconcile path itself. To keep the ring from growing on UDP loss, call:

```cpp
    // Bound: drop entries older than ~2 seconds (120 ticks at 60 Hz).
    if (m_clientTick > 120) {
        PendingPickupRingOps::expireOlderThan(m_pendingPickups, m_clientTick - 120);
    }
```

at the bottom of clientNetPost.

- [ ] **Step 4**: Reset on connect (same spot as the other rings in engine_startgame.cpp). Add `PendingPickupRingOps::reset(m_pendingPickups);`.

- [ ] **Step 5**: Build, 45/45 tests still pass. Commit: `feat(net): predict world-item disappearance on pickup send — M8.2`.

The smooth fade-in on reject (item reappears) is currently a hard re-activate by mirrorWorldItems. M13 (smooth correction layer full) adds a fade-in animation. M8 is the structural foundation.

---

## Task 3: Verify

- [ ] Clean tree, build, 45/45 tests. M8 = 2 commits.

## Definition of Done
- [ ] 45/45 tests pass
- [ ] `PendingPickupRing m_pendingPickups` in engine.h
- [ ] Local item-deactivate happens at the CL_PICKUP_ITEM send site
- [ ] Inventory add NOT predicted in v1 (snapshot-driven)
