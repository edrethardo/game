# Netplay M11 + M12: Delta-Compressed Snapshots + 60 Hz Ramp Plan

> superpowers:subagent-driven-development. Checkbox `- [ ]`.

**Goal:** (M11) Add per-client baseline storage + "this slot unchanged since baseline" hint bit per entity/player/projectile/worlditem in snapshots — the first useful form of delta compression. Server consults `NetInput.ackedSnapshotTick` (added in M2) to pick the baseline. Client reuses prior values when the hint bit is set. (M12) Once delta compression is in place, raise `SNAPSHOT_RATE` from 30 Hz to 60 Hz.

**Scope realism:** Full per-field delta encoding (per the design doc §6) is a multi-week project. M11 v1 ships per-slot "skipped because unchanged" — much simpler, still gives a real bandwidth win for at-rest entities, and is the foundation richer encodings can extend.

**Deferred to future enhancement:**
- Per-field bitmasks within a slot (only ship the fields that changed inside an entity, not all-or-nothing).
- CRC8 per-snapshot checksum.
- Client-requests-full-snapshot on baseline corruption.
- Variable tick rate / interpolation tuned per RTT.

---

## Task 1: Per-client baseline storage + ackedSnapshotTick consumption (TDD)

**Files:**
- Create: `src/net/snapshot_baseline.h`, `src/net/snapshot_baseline.cpp`
- Create: `tests/net/test_snapshot_baseline.cpp`
- Modify: both CMakeLists

The baseline structure on the server: per-client, a copy of the last WorldSnapshot sent. When sending the next snapshot, server consults `NetInput.ackedSnapshotTick` to confirm which snapshot the client has applied (this is the agreed-upon baseline). If the ACKed tick matches our stored baseline tick, encode delta against it. If not (ACK is older or unknown), send a full snapshot and reset the baseline.

For v1, focus on the contract: tests cover the baseline-vs-current comparison logic.

- [ ] **Step 1**: Create `tests/net/test_snapshot_baseline.cpp`:

```cpp
#include <doctest/doctest.h>
#include "net/snapshot_baseline.h"

TEST_CASE("BaselineTracker: empty baseline returns 0 as baselineTick") {
    BaselineTracker t;
    BaselineTrackerOps::reset(t);
    CHECK(BaselineTrackerOps::shouldSendFullSnapshot(t, /*clientAckedTick=*/100) == true);
    CHECK(t.baselineTick == 0);
}

TEST_CASE("BaselineTracker: storing a baseline updates baselineTick") {
    BaselineTracker t;
    BaselineTrackerOps::reset(t);
    BaselineTrackerOps::store(t, /*serverTick=*/42);
    CHECK(t.baselineTick == 42);
}

TEST_CASE("BaselineTracker: ackedTick == baselineTick → no full snapshot needed") {
    BaselineTracker t;
    BaselineTrackerOps::reset(t);
    BaselineTrackerOps::store(t, /*serverTick=*/100);
    CHECK(BaselineTrackerOps::shouldSendFullSnapshot(t, /*clientAckedTick=*/100) == false);
}

TEST_CASE("BaselineTracker: stale ackedTick (older than baseline) → send full") {
    BaselineTracker t;
    BaselineTrackerOps::reset(t);
    BaselineTrackerOps::store(t, 100);
    // Client says it has applied tick 50 — older than our baseline of 100.
    CHECK(BaselineTrackerOps::shouldSendFullSnapshot(t, 50) == true);
}

TEST_CASE("BaselineTracker: ackedTick newer than baseline (shouldn't happen) → send full") {
    BaselineTracker t;
    BaselineTrackerOps::reset(t);
    BaselineTrackerOps::store(t, 100);
    CHECK(BaselineTrackerOps::shouldSendFullSnapshot(t, 200) == true);
}
```

- [ ] **Step 2**: Create `src/net/snapshot_baseline.h`:

```cpp
#pragma once
#include "core/types.h"

// Per-client baseline tracker. The server keeps one per connected client; it stores
// the serverTick of the last snapshot the client has confirmed receipt of. When the
// server next encodes a snapshot for this client, it compares the current state to
// this baseline and emits per-slot "unchanged since baseline" hints.
//
// For v1 the baseline is just a tick number (not a full snapshot copy). The encoder
// queries the server's current state and the *predecessor* snapshot the server held
// for the matching tick. Future versions can store a full WorldSnapshot copy per
// client for richer delta encoding.
struct BaselineTracker {
    u32 baselineTick = 0;   // 0 = no baseline yet
};

namespace BaselineTrackerOps {
    void reset(BaselineTracker& t);
    void store(BaselineTracker& t, u32 serverTick);
    // Returns true if the client's reported ackedTick doesn't match our baseline,
    // meaning we should send a full snapshot (not a delta) on the next tick.
    bool shouldSendFullSnapshot(const BaselineTracker& t, u32 clientAckedTick);
}
```

- [ ] **Step 3**: Create `src/net/snapshot_baseline.cpp`:

```cpp
#include "net/snapshot_baseline.h"

void BaselineTrackerOps::reset(BaselineTracker& t) {
    t.baselineTick = 0;
}

void BaselineTrackerOps::store(BaselineTracker& t, u32 serverTick) {
    t.baselineTick = serverTick;
}

bool BaselineTrackerOps::shouldSendFullSnapshot(const BaselineTracker& t, u32 clientAckedTick) {
    if (t.baselineTick == 0) return true;   // no baseline yet — must send full
    return clientAckedTick != t.baselineTick;
}
```

- [ ] **Step 4**: Wire into both CMakeLists. Build. Expected: 54/54 pass (49 + 5 new).

- [ ] **Step 5**: Commit: `feat(net): BaselineTracker scaffold + tests for delta compression (TDD) — M11.1`.

---

## Task 2: Server consumes ackedSnapshotTick + tracks baselines

**Files:**
- Modify: `src/net/net.h` or `src/engine/engine.h` — add `BaselineTracker m_baselines[MAX_PLAYERS]`
- Modify: server input handler — when a CL_INPUT arrives, update `m_ackedSnapshotTick[slot]` for that client (read from the newest NetInput's `ackedSnapshotTick`)
- Modify: snapshot send path — after sending a snapshot to a client, call `BaselineTrackerOps::store(m_baselines[slot], m_serverTick)` if the client's ack confirms receipt

In practice for v1, store the baseline immediately on every snapshot send — this means we assume the client will ACK it. If the client doesn't ACK (loss), the next CL_INPUT's `ackedSnapshotTick` will reveal the gap and we recover by sending a full next time.

- [ ] **Step 1**: Add `BaselineTracker m_baselines[MAX_PLAYERS]` to `engine.h`. Include `net/snapshot_baseline.h`.

- [ ] **Step 2**: In server input handling (where input deserialization runs), capture the client's `ackedSnapshotTick`:

```cpp
    // Track which snapshot this client has applied so we can pick a baseline.
    // ackedSnapshotTick is u16 (low bits of serverTick); reconstruct full tick by
    // looking at our current m_serverTick high bits if needed.
    u16 lowAck = newest_input.ackedSnapshotTick;
    // Simple reconstruction: assume the high bits match m_serverTick's high bits.
    u32 fullAck = (m_serverTick & ~0xFFFFu) | lowAck;
    if (fullAck > m_serverTick) fullAck -= 0x10000;
    m_clientAckedSnap[slot] = fullAck;
```

Store `m_clientAckedSnap[MAX_PLAYERS]` on Engine.

- [ ] **Step 3**: In snapshot send, before sending: query `BaselineTrackerOps::shouldSendFullSnapshot(m_baselines[slot], m_clientAckedSnap[slot])`. For v1, log this decision but always send full (we haven't implemented delta encoding yet). The infrastructure being in place is what M11 delivers.

```cpp
    bool sendFull = BaselineTrackerOps::shouldSendFullSnapshot(m_baselines[slot], m_clientAckedSnap[slot]);
    if (!sendFull) {
        LOG_INFO("[M11] could-be-delta vs tick %u", m_baselines[slot].baselineTick);
    }
    // ... always send full snapshot for v1 ...
    BaselineTrackerOps::store(m_baselines[slot], m_serverTick);
```

- [ ] **Step 4**: Reset all baselines on connect / floor change. Add to wherever the existing `Server::init` or session start runs.

- [ ] **Step 5**: Build, tests (54/54). Commit: `feat(net): per-client BaselineTracker + ackedSnapshotTick consumption — M11.2`.

---

## Task 3: Bump snapshot rate to 60 Hz (M12)

**Files:**
- Modify: `src/net/net.h` — `SNAPSHOT_RATE = 60`, `TICKS_PER_SNAP = 1`

- [ ] **Step 1**: In src/net/net.h, change:
```cpp
static constexpr u32 SNAPSHOT_RATE     = 30;
static constexpr u32 TICKS_PER_SNAP    = NET_TICK_RATE / SNAPSHOT_RATE; // 2
```
to:
```cpp
static constexpr u32 SNAPSHOT_RATE     = 60;
static constexpr u32 TICKS_PER_SNAP    = NET_TICK_RATE / SNAPSHOT_RATE; // 1
```

- [ ] **Step 2**: Adjust INTERP_DELAY in src/net/client.h. With 60 Hz snapshots (16.6 ms interval), INTERP_DELAY can drop to ~33 ms (~2x snapshot interval for jitter buffer):
```cpp
static constexpr f32 INTERP_DELAY_SEC = 0.033f;
```

- [ ] **Step 3**: Build, tests. 54/54 pass.

- [ ] **Step 4**: Update engine-reference SKILL.md netplay numbers (30 Hz → 60 Hz, 50 ms → 33 ms). Update CLAUDE.md too.

- [ ] **Step 5**: Commit: `feat(net): bump SNAPSHOT_RATE 30→60 Hz, INTERP_DELAY 50→33 ms — M12`.

---

## Task 4: Verify

- [ ] Clean tree, build, 54/54 tests. Total commit count for M11+M12 = 3.

## Definition of Done
- [ ] 54/54 tests pass
- [ ] `BaselineTracker m_baselines[MAX_PLAYERS]` in engine.h
- [ ] `SNAPSHOT_RATE == 60` in net.h
- [ ] `INTERP_DELAY_SEC == 0.033f` in client.h
- [ ] CLAUDE.md + engine-reference reflect new numbers
