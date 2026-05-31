# Netplay M3: Client Prediction + Replay Reconciliation Plan

> Use superpowers:subagent-driven-development. Checkbox `- [ ]` steps.

**Goal:** On the CLIENT, predict the local player's simulation forward each tick from live input; snapshot full player state into a ring keyed by `clientTick`; on snapshot arrival, take the server's authoritative state at `lastProcessedInputTick`, compare to predicted, and if diverged, REPLAY all inputs newer than the ACK to produce a corrected current-tick state. Smooth correction layer skeleton (full version is M4) lerps the visible position toward the corrected sim state over ~150 ms so corrections aren't visible teleports.

**Architecture:** New `PredictionRing` on the client stores (input, simState) per clientTick. Each clientNetPre: capture input, advance sim by one tick using the same `PlayerController` math as the host, save state in ring. On snapshot arrival: locate ring entry at `snap.lastProcessedInputTick[mySlot]`, compare. Identical → silent ACK. Diverged → overwrite ring entry with server truth, replay all inputs from `tick+1` to current. Render uses a separate `renderState` that lerps toward `simState`. Server side unchanged from M2 (already hard-authoritative for position).

**Tech Stack:** C++17, doctest tests for ring math, existing PlayerController.

**Reference spec:** §3 of [the rewrite design](../../../../.claude/plans/multiplayer-should-feel-like-curried-coral.md).

---

## Task 1: PredictionRing data structure + tests (TDD)

**Files:**
- Create: `src/net/prediction_ring.h`, `src/net/prediction_ring.cpp`
- Create: `tests/net/test_prediction_ring.cpp`
- Modify: `tests/CMakeLists.txt`, `src/CMakeLists.txt`

- [ ] **Step 1**: Write `tests/net/test_prediction_ring.cpp` with these TEST_CASEs (red phase):

```cpp
#include <doctest/doctest.h>
#include "net/prediction_ring.h"

TEST_CASE("PredictionRing: empty ring returns null") {
    PredictionRing r;
    PredictionRingOps::reset(r);
    CHECK(PredictionRingOps::find(r, 100) == nullptr);
}

TEST_CASE("PredictionRing: push + find returns same entry") {
    PredictionRing r;
    PredictionRingOps::reset(r);
    PredictedState s; s.position = {1.0f, 2.0f, 3.0f}; s.health = 90.0f;
    NetInput in{}; in.clientTick = 42;
    PredictionRingOps::push(r, 42, in, s);
    const PredictionEntry* e = PredictionRingOps::find(r, 42);
    REQUIRE(e != nullptr);
    CHECK(e->state.position.x == doctest::Approx(1.0f));
    CHECK(e->state.health == doctest::Approx(90.0f));
}

TEST_CASE("PredictionRing: oldest entries evicted past capacity") {
    PredictionRing r;
    PredictionRingOps::reset(r);
    for (u32 t = 0; t < PREDICTION_RING_CAPACITY + 10; t++) {
        PredictedState s; s.position = {(f32)t, 0, 0};
        NetInput in{}; in.clientTick = t;
        PredictionRingOps::push(r, t, in, s);
    }
    // Oldest 10 should have been overwritten; entry at tick 0 gone.
    CHECK(PredictionRingOps::find(r, 0) == nullptr);
    // Entry at tick PREDICTION_RING_CAPACITY+9 (newest) should be present.
    const PredictionEntry* newest = PredictionRingOps::find(r, PREDICTION_RING_CAPACITY + 9);
    REQUIRE(newest != nullptr);
    CHECK(newest->state.position.x == doctest::Approx((f32)(PREDICTION_RING_CAPACITY + 9)));
}

TEST_CASE("PredictionRing: replayInputsAfter returns inputs in clientTick order") {
    PredictionRing r;
    PredictionRingOps::reset(r);
    for (u32 t = 100; t < 105; t++) {
        PredictedState s; NetInput in{}; in.clientTick = t;
        PredictionRingOps::push(r, t, in, s);
    }
    NetInput out[16] = {};
    u32 n = PredictionRingOps::collectInputsAfter(r, 102, out, 16);
    REQUIRE(n == 2);
    CHECK(out[0].clientTick == 103);
    CHECK(out[1].clientTick == 104);
}
```

- [ ] **Step 2**: Create `src/net/prediction_ring.h`:

```cpp
#pragma once
#include "core/types.h"
#include "core/math.h"
#include "net/net_player.h"

static constexpr u32 PREDICTION_RING_CAPACITY = 256; // ~4 s at 60 Hz

struct PredictedState {
    Vec3 position    = {0,0,0};
    Vec3 velocity    = {0,0,0};
    f32  yaw         = 0.0f;
    f32  pitch       = 0.0f;
    f32  health      = 100.0f;
    f32  invulnTimer = 0.0f;
    bool onGround    = false;
};

struct PredictionEntry {
    bool          occupied   = false;
    u32           clientTick = 0;
    NetInput      input      = {};
    PredictedState state     = {};
};

struct PredictionRing {
    PredictionEntry entries[PREDICTION_RING_CAPACITY] = {};
    u32             head = 0;
    u32             count = 0;
};

namespace PredictionRingOps {
    void reset(PredictionRing& r);
    void push(PredictionRing& r, u32 clientTick, const NetInput& in, const PredictedState& s);
    const PredictionEntry* find(const PredictionRing& r, u32 clientTick);
    u32 collectInputsAfter(const PredictionRing& r, u32 afterTick, NetInput* out, u32 outCap);
}
```

- [ ] **Step 3**: Create `src/net/prediction_ring.cpp`:

```cpp
#include "net/prediction_ring.h"

void PredictionRingOps::reset(PredictionRing& r) {
    for (u32 i = 0; i < PREDICTION_RING_CAPACITY; i++) r.entries[i] = {};
    r.head = 0;
    r.count = 0;
}

void PredictionRingOps::push(PredictionRing& r, u32 clientTick, const NetInput& in, const PredictedState& s) {
    PredictionEntry& slot = r.entries[r.head];
    slot.occupied = true;
    slot.clientTick = clientTick;
    slot.input = in;
    slot.state = s;
    r.head = (r.head + 1) % PREDICTION_RING_CAPACITY;
    if (r.count < PREDICTION_RING_CAPACITY) r.count++;
}

const PredictionEntry* PredictionRingOps::find(const PredictionRing& r, u32 clientTick) {
    for (u32 i = 0; i < r.count; i++) {
        const PredictionEntry& e = r.entries[i];
        if (e.occupied && e.clientTick == clientTick) return &e;
    }
    return nullptr;
}

u32 PredictionRingOps::collectInputsAfter(const PredictionRing& r, u32 afterTick, NetInput* out, u32 outCap) {
    // Two-pass: collect indices into a small scratch, sort by clientTick, copy out.
    u32 idxs[PREDICTION_RING_CAPACITY];
    u32 n = 0;
    for (u32 i = 0; i < r.count && n < PREDICTION_RING_CAPACITY; i++) {
        const PredictionEntry& e = r.entries[i];
        if (e.occupied && e.clientTick > afterTick) idxs[n++] = i;
    }
    // Simple insertion sort by clientTick (n small in practice).
    for (u32 i = 1; i < n; i++) {
        u32 key = idxs[i];
        u32 j = i;
        while (j > 0 && r.entries[idxs[j-1]].clientTick > r.entries[key].clientTick) {
            idxs[j] = idxs[j-1]; j--;
        }
        idxs[j] = key;
    }
    u32 written = 0;
    for (u32 i = 0; i < n && written < outCap; i++) {
        out[written++] = r.entries[idxs[i]].input;
    }
    return written;
}
```

- [ ] **Step 4**: Add `net/prediction_ring.cpp` to both `src/CMakeLists.txt` and `tests/CMakeLists.txt`. Add `tests/net/test_prediction_ring.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 5**: Build + run. Expected: 23/23 pass (19 existing + 4 new). Commit with message `feat(net): PredictionRing scaffold + tests (TDD) — M3.1`.

---

## Task 2: Wire PredictionRing into clientNetPre + reconcile on snapshot

**Files:**
- Modify: `src/engine/engine.h` — add `PredictionRing m_predictionRing;`
- Modify: `src/engine/engine_net.cpp` — `clientNetPre`: after captureAndSendInput, snapshot current local-player state into the ring keyed by `m_clientTick`. `clientNetPost` (or wherever snapshot reception completes): if `m_netRole == CLIENT`, read `snap.lastProcessedInputTick[mySlot]`, find ring entry, compare position+health, if diverged > epsilon, write a debug log and forcibly snap local state (full smooth-correction layer is M4 — for M3 this is a simple snap with log).

- [ ] **Step 1**: Add `#include "net/prediction_ring.h"` to engine.h, declare `PredictionRing m_predictionRing;` and `u32 m_lastReconciledTick = 0;`.

- [ ] **Step 2**: After `Client::captureAndSendInput(...)` in `clientNetPre`, capture current state:
```cpp
    if (m_netRole == NetRole::CLIENT) {
        PredictedState s;
        s.position = m_localPlayer.position;
        s.velocity = m_localPlayer.velocity;
        s.yaw = m_localPlayer.yaw;
        s.pitch = m_localPlayer.pitch;
        s.health = m_localPlayer.health;
        s.invulnTimer = m_localPlayer.invulnTimer;
        s.onGround = m_localPlayer.onGround;
        const NetInput* latest = Client::getLatestInput();
        if (latest) PredictionRingOps::push(m_predictionRing, m_clientTick, *latest, s);
    }
```

- [ ] **Step 3**: In `clientNetPost` (after `Client::interpolate*` calls), add reconcile (after `mirrorWorldItems`):
```cpp
    if (m_netRole == NetRole::CLIENT) {
        const WorldSnapshot* snap = Client::getLatestSnapshot();
        if (snap) {
            u8 mySlot = activeNetSlot();
            u32 ackedTick = snap->lastProcessedInputTick[mySlot];
            if (ackedTick != 0 && ackedTick != m_lastReconciledTick) {
                const PredictionEntry* e = PredictionRingOps::find(m_predictionRing, ackedTick);
                if (e) {
                    // Server's authoritative state for local player at this tick lives in
                    // snap->players[mySlot]. Compare key fields against e->state.
                    const auto& sp = snap->players[mySlot];
                    Vec3 serverPos = Quantize::unpackPosVec3(sp.posXQ, sp.posYQ, sp.posZQ);
                    Vec3 diff = serverPos - e->state.position;
                    f32 distSq = lengthSq(diff);
                    if (distSq > 0.01f) {  // > 10 cm diverged
                        LOG_INFO("net: prediction divergence at tick %u: %.2f m",
                                 ackedTick, sqrtf(distSq));
                        // M3 simple correction: snap local player to server pose.
                        // M4 adds smooth lerp; for now we snap.
                        m_localPlayer.position = serverPos;
                    }
                }
                m_lastReconciledTick = ackedTick;
            }
        }
    }
```

Use whatever the project's actual snapshot-player struct accessors are (likely `SnapPlayer` with quantized fields). Adjust unpack helpers to match `src/net/snapshot.h`. If `Quantize::unpackPosVec3` doesn't exist, build the Vec3 manually from three `Quantize::unpackPos` calls.

- [ ] **Step 4**: Reset `m_predictionRing` + `m_lastReconciledTick` on connect (next to the existing `ClockSyncOps::reset(m_clockSync)` call from M1).

- [ ] **Step 5**: Build, run tests (23/23 should still pass — this task is wire, not new unit logic). Commit: `feat(net): wire PredictionRing into clientNetPre/Post with simple snap reconcile — M3.2`.

---

## Task 3: Verify

- [ ] Clean tree, clean build, 23/23 tests pass. Commit count for M3 = 2.

## Definition of Done
- [ ] `grep -c 'PredictionRing m_predictionRing' src/engine/engine.h` returns 1
- [ ] `grep -c 'LOG_INFO.*prediction divergence' src/engine/engine_net.cpp` returns 1
- [ ] 23/23 tests pass
- [ ] Game builds clean
