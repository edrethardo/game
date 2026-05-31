# Netplay M1 (TDD): Clock Sync + Tick Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the client a stable estimate of server time (`t_server_est`) via a connect-time handshake and per-snapshot refinement, separate the client's local tick (`m_clientTick`) from any server-time concept, and rename the `NetInput.tick` wire field to `NetInput.clientTick` so the prediction work in M2/M3 can build on coherent timing primitives — implemented **test-first** using the doctest framework landed in the test-framework v1 milestone.

**Architecture:** New `ClockSync` subsystem on the client tracks a smoothed estimate of the server's current tick. Two new packet types (`CL_TIME_PING` / `SV_TIME_PONG`) bootstrap the estimate at connection (3 rapid pings to absorb single-packet outliers). Every incoming snapshot's `serverTick` field then refines the estimate via a P controller (gain ~0.1). The client maintains its own monotonic `m_clientTick` for stamping outgoing inputs. The pure-math parts (the ClockSync state machine + the SV_TIME_PONG byte-buffer parsing) are unit-tested via doctest; the wire-handshake and field-rename parts are verified by build + manual smoke.

**Tech Stack:** C++17, ENet (existing transport), `Clock::getElapsedSeconds` for wall time, existing PacketReader/Writer helpers, doctest (header at `external/doctest/doctest.h`) for the unit-test parts.

**Reference spec:** [/home/aaron/.claude/plans/multiplayer-should-feel-like-curried-coral.md](../../../../.claude/plans/multiplayer-should-feel-like-curried-coral.md) — §1 ("Clock & Tick Model") and Migration Plan → Milestone 1. Test framework spec: [docs/superpowers/specs/2026-05-31-test-framework-design.md](../specs/2026-05-31-test-framework-design.md).

---

## Pre-flight Notes

**TDD discipline.** For unit-testable subsystems (ClockSync state machine, SV_TIME_PONG decode), the pattern is: write the failing test → see it fail in `dungeon_tests` → write minimal code → see it pass → commit. The plan groups all ClockSync tests into one initial commit (with the class as a compile-only stub), then a second commit makes them pass. Pong-decode follows the same pattern in its own task.

**Not everything is unit-testable.** The wire-handshake (sending CL_TIME_PING on connect, server echoing) involves real network state and ENet peers — that gets manual smoke. The NetInput field rename is a mechanical refactor — the build succeeding IS the test. The m_serverTick audit produces no new behavior — same deal.

**Scope boundaries (unchanged from pre-TDD draft).** M1 only delivers the *estimator* and the *clientTick stamping*. It does NOT:
- Apply `t_server_est` to render-tick scheduling — that's M3/M4.
- Implement the rolling input window — that's M2.
- Touch lag compensation — that's M5.

**Tree state when starting:** Master is clean. Test framework lands at HEAD (4 commits ago: doctest vendored, tests/ skeleton, CMake hook, CLAUDE.md docs).

**Where m_serverTick is used today.** A grep at plan-writing time showed the field is touched in `engine.h:152` (declaration), `engine_net.cpp:63, 70, 489, 506, 510, 533, 541` (server tick increment + snapshot stamping + input stamping), and `client.cpp:214` (a comment noting the host resets it on descent). M1 introduces a clear split: server tick handling stays unchanged on the host's SERVER role; client (CLIENT role) gets `m_clientTick` for its own monotonic counter, and `ClockSync` for the estimate of remote server time.

---

## Task 1: ClockSync Test Suite (Failing) + Stub Header

**Files:**
- Create: `tests/net/test_clock_sync.cpp` — all the test cases for ClockSync, written before the class exists
- Create: `src/net/clock_sync.h` — minimal stub so the tests compile (struct + namespace declarations)
- Modify: `tests/CMakeLists.txt` — add the new test file + `src/net/clock_sync.cpp` (which doesn't exist yet — link error expected)

This task ends with the tests **compiling and failing at link time** (or failing at runtime once we add a stub .cpp). That's the red phase.

- [ ] **Step 1: Create the stub header**

Write `src/net/clock_sync.h`:

```cpp
#pragma once

#include "core/types.h"

// Client-side estimate of the server's current simulation tick + wall-clock time.
// Foundation for the netplay rewrite (M1) — the prediction + replay reconciliation
// work in M3 needs to know *when* a snapshot represents, not just *what it carries*.
//
// Bootstrapped at connect by 3 CL_TIME_PING ↔ SV_TIME_PONG round-trips; refined every
// time a WorldSnapshot arrives by feeding `snap.serverTick` through a P controller
// (gain ~0.1) so transient jitter doesn't phase-jump the estimate.
//
// All times are doubles internally because we accumulate over a session.
//
// Threading: single-threaded — the net layer drives this from the same callback
// that delivers packets, on the main thread. No locking.
struct ClockSync {
    bool   bootstrapped       = false;
    f64    serverTickEst      = 0.0;
    f64    lastUpdateClockSec = 0.0;
    f32    oneWayTripMs       = 30.0f;
    u32    pongsReceived      = 0;
    u32    snapshotsApplied   = 0;

    static constexpr f32 SNAP_GAIN   = 0.1f;
    static constexpr f32 OWT_GAIN    = 0.2f;
    static constexpr f64 LARGE_DELTA = 6.0;
};

namespace ClockSyncOps {
    void reset(ClockSync& cs);

    void onPongReceived(ClockSync& cs,
                        u32 clientSentMs,
                        u32 serverTickAtRecv,
                        f64 pongRecvNowSec);

    void onSnapshotReceived(ClockSync& cs, u32 snapServerTick, f64 recvNowSec);

    f64 currentServerTickEst(const ClockSync& cs, f64 nowSec);
    u32 currentServerTickEstU32(const ClockSync& cs, f64 nowSec);
}
```

- [ ] **Step 2: Create the test file**

Write `tests/net/test_clock_sync.cpp`:

```cpp
// test_clock_sync.cpp — TDD spec for the ClockSync state machine. Each TEST_CASE
// describes one behavior the type must exhibit; tests are written before the
// implementation. Pure math + state — no network, no engine globals.

#include <doctest/doctest.h>
#include "net/clock_sync.h"

TEST_CASE("ClockSync: reset returns to pristine state") {
    ClockSync cs;
    cs.bootstrapped = true;
    cs.serverTickEst = 12345.0;
    cs.oneWayTripMs = 99.0f;
    cs.pongsReceived = 7;
    ClockSyncOps::reset(cs);
    CHECK(cs.bootstrapped == false);
    CHECK(cs.serverTickEst == 0.0);
    CHECK(cs.oneWayTripMs == doctest::Approx(30.0f));
    CHECK(cs.pongsReceived == 0);
    CHECK(cs.snapshotsApplied == 0);
}

TEST_CASE("ClockSync: first pong bootstraps the estimate") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    // We sent the ping at 100 ms (clientSentMs=100), server stamped its tick at
    // recv = 200, the pong arrived back at 130 ms (pongRecvNowSec=0.130). RTT is
    // 30 ms, so oneWayTripMs is 15 ms.
    ClockSyncOps::onPongReceived(cs, /*clientSentMs=*/100,
                                     /*serverTickAtRecv=*/200,
                                     /*pongRecvNowSec=*/0.130);
    CHECK(cs.bootstrapped == true);
    CHECK(cs.pongsReceived == 1);
    CHECK(cs.oneWayTripMs == doctest::Approx(15.0f));
    // serverTickAtPongArrival = 200 + (15/1000)*60 = 200.9 ticks.
    CHECK(cs.serverTickEst == doctest::Approx(200.9));
    CHECK(cs.lastUpdateClockSec == doctest::Approx(0.130));
}

TEST_CASE("ClockSync: subsequent pongs EMA-smooth oneWayTripMs") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    ClockSyncOps::onPongReceived(cs, 100, 200, 0.130);   // RTT 30 → OWT 15
    ClockSyncOps::onPongReceived(cs, 200, 210, 0.250);   // sent@200ms, recv@250ms, RTT 50 → OWT 25
    // After EMA(OWT_GAIN=0.2): OWT = 15 + 0.2*(25 - 15) = 17.0
    CHECK(cs.oneWayTripMs == doctest::Approx(17.0f));
    CHECK(cs.pongsReceived == 2);
}

TEST_CASE("ClockSync: currentServerTickEst projects forward by wall time") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    ClockSyncOps::onPongReceived(cs, 100, 600, 0.130);
    // Right at recv time: estimate equals serverTickEst (≈600.9 — recomputed for OWT 15).
    CHECK(ClockSyncOps::currentServerTickEst(cs, 0.130) == doctest::Approx(600.9));
    // 100 ms later (0.230 s): 0.1 s * 60 = 6 ticks ahead → 606.9.
    CHECK(ClockSyncOps::currentServerTickEst(cs, 0.230) == doctest::Approx(606.9));
    // 1 second later: +60 ticks → 660.9.
    CHECK(ClockSyncOps::currentServerTickEst(cs, 1.130) == doctest::Approx(660.9));
}

TEST_CASE("ClockSync: currentServerTickEst returns 0 before bootstrap") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    CHECK(ClockSyncOps::currentServerTickEst(cs, 0.0) == 0.0);
    CHECK(ClockSyncOps::currentServerTickEstU32(cs, 99.0) == 0u);
}

TEST_CASE("ClockSync: currentServerTickEstU32 floors fractional ticks") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    ClockSyncOps::onPongReceived(cs, 100, 500, 0.130);
    // est ≈ 500.9 at recv time → U32 floor is 500.
    CHECK(ClockSyncOps::currentServerTickEstU32(cs, 0.130) == 500u);
}

TEST_CASE("ClockSync: onSnapshotReceived seeds when not bootstrapped") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    // Snapshot arrives before any pong — should bootstrap from snapshot's serverTick.
    // OWT is still the default 30 ms, so estimate becomes 1000 + (30/1000)*60 = 1001.8.
    ClockSyncOps::onSnapshotReceived(cs, /*snapServerTick=*/1000, /*recvNowSec=*/0.500);
    CHECK(cs.bootstrapped == true);
    CHECK(cs.serverTickEst == doctest::Approx(1001.8));
    CHECK(cs.snapshotsApplied == 1);
}

TEST_CASE("ClockSync: onSnapshotReceived smooths small drift via P controller") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    ClockSyncOps::onPongReceived(cs, 100, 1000, 0.130);  // serverTickEst ≈ 1000.9 at t=0.130
    // 100 ms later (recvNowSec=0.230), a snapshot arrives claiming serverTick=1007.
    // Predicted now: 1000.9 + 0.1*60 = 1006.9. Observed: 1007 + 0.015*60 = 1007.9. Delta: 1.0.
    // After P controller (gain 0.1): est = 1006.9 + 0.1 * 1.0 = 1007.0.
    ClockSyncOps::onSnapshotReceived(cs, /*snapServerTick=*/1007, /*recvNowSec=*/0.230);
    CHECK(cs.serverTickEst == doctest::Approx(1007.0));
    CHECK(cs.snapshotsApplied == 1);
}

TEST_CASE("ClockSync: onSnapshotReceived snaps on large delta (host floor reset)") {
    ClockSync cs;
    ClockSyncOps::reset(cs);
    ClockSyncOps::onPongReceived(cs, 100, 5000, 0.130);   // serverTickEst ≈ 5000.9
    // 50 ms later, host has just descended and reset m_serverTick = 0. Next snapshot
    // carries snapServerTick = 3 (a few ticks past reset). Delta vs predicted is
    // huge → should SNAP rather than smooth.
    ClockSyncOps::onSnapshotReceived(cs, /*snapServerTick=*/3, /*recvNowSec=*/0.180);
    // After snap: estimate = observed = 3 + (15/1000)*60 = 3.9
    CHECK(cs.serverTickEst == doctest::Approx(3.9));
}
```

- [ ] **Step 3: Update `tests/CMakeLists.txt` to compile the new test file and (eventually) link against clock_sync.cpp**

Use the Edit tool. The current source list is:
```
add_executable(dungeon_tests
    test_main.cpp
    sanity_test.cpp
)
```

Replace with:
```
add_executable(dungeon_tests
    test_main.cpp
    sanity_test.cpp
    net/test_clock_sync.cpp
    ${CMAKE_SOURCE_DIR}/src/net/clock_sync.cpp
)
```

`tests/net/` directory needs to exist before CMake can find the file:

```bash
mkdir -p tests/net
```

(Step 2 wrote the test file there, but only `mkdir -p tests/net` happened implicitly via the Write tool. If `ls tests/net/` shows the test file, you're fine.)

- [ ] **Step 4: Try to build — expect link failure (clock_sync.cpp doesn't exist yet)**

```bash
cmake -B build 2>&1 | tail -5
cmake --build build --target dungeon_tests 2>&1 | tail -20
```

Expected: CMake configure succeeds (file is listed); the **build fails** when linking `dungeon_tests` because none of the `ClockSyncOps::*` functions are defined anywhere. Capture the error — it should mention undefined references to `ClockSyncOps::reset`, `ClockSyncOps::onPongReceived`, etc.

If you get a *compile* error instead (e.g., `unknown type 'f64'`), the stub header is wrong — check that `#include "core/types.h"` pulls in the f64/u32 aliases on the test target's include path. The `target_include_directories` in tests/CMakeLists.txt already adds `${CMAKE_SOURCE_DIR}/src`, so the include should resolve.

- [ ] **Step 5: Create a placeholder clock_sync.cpp so we get a runtime test failure instead of link failure**

This step is small — a no-op .cpp that defines the functions with broken stubs. We want the tests to *compile and link* but then *fail at runtime*. Write `src/net/clock_sync.cpp`:

```cpp
// clock_sync.cpp — TDD red phase: stubs that compile but do nothing useful. The
// actual math lands in Task 2 to make the test_clock_sync.cpp expectations pass.

#include "net/clock_sync.h"
#include "core/types.h"

void ClockSyncOps::reset(ClockSync& /*cs*/) {
    // Intentional no-op — tests will fail until Task 2 lands the real reset.
}

void ClockSyncOps::onPongReceived(ClockSync& /*cs*/,
                                  u32 /*clientSentMs*/,
                                  u32 /*serverTickAtRecv*/,
                                  f64 /*pongRecvNowSec*/) {
    // Intentional no-op — Task 2 implements.
}

void ClockSyncOps::onSnapshotReceived(ClockSync& /*cs*/,
                                      u32 /*snapServerTick*/,
                                      f64 /*recvNowSec*/) {
    // Intentional no-op — Task 2 implements.
}

f64 ClockSyncOps::currentServerTickEst(const ClockSync& /*cs*/, f64 /*nowSec*/) {
    return 0.0;  // wrong — Task 2 implements.
}

u32 ClockSyncOps::currentServerTickEstU32(const ClockSync& /*cs*/, f64 /*nowSec*/) {
    return 0u;  // wrong — Task 2 implements.
}
```

- [ ] **Step 6: Rebuild and run tests — expect runtime failures**

```bash
cmake --build build --target dungeon_tests 2>&1 | tail -5
./build/tests/dungeon_tests 2>&1 | tail -30
```

Expected: build succeeds (link now resolves), test run fails with multiple FAILED cases. doctest will report which expressions failed and what the actual vs expected values were. You'll see things like:
```
test cases:  X |  Y passed | Z failed
```

with Z > 0. The "framework smoke" sanity tests from Task 2 of the test-framework plan still pass; the 9 new ClockSync tests fail.

If the run actually succeeds (0 failures), the stubs accidentally returned correct values — re-check Step 5's stub returns are wrong (return 0.0, not the expected values).

- [ ] **Step 7: Commit the red phase**

```bash
git add tests/net/test_clock_sync.cpp src/net/clock_sync.h src/net/clock_sync.cpp tests/CMakeLists.txt
git diff --cached --stat
git commit -m "$(cat <<'EOF'
test(net): ClockSync test suite + stub (TDD red phase) — M1.1

Adds tests/net/test_clock_sync.cpp with 9 TEST_CASEs covering:
  - reset() returns to pristine state
  - first pong bootstraps estimate from RTT + serverTick
  - subsequent pongs EMA-smooth oneWayTripMs (gain 0.2)
  - currentServerTickEst projects forward by wall time
  - currentServerTickEst returns 0 before bootstrap
  - currentServerTickEstU32 floors fractional ticks
  - onSnapshotReceived seeds when not bootstrapped
  - onSnapshotReceived smooths small drift via P controller (gain 0.1)
  - onSnapshotReceived snaps on large delta (>6 ticks — host floor reset)

Stub src/net/clock_sync.{h,cpp} compiles + links so tests run, but ops
are no-ops returning wrong values — every ClockSync test fails by
design. Task 2 (M1.2) lands the real impl to turn them green.

tests/CMakeLists.txt extends the dungeon_tests source list to pick up
the new test file and (the still-stubbed) clock_sync.cpp.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Implement ClockSync to Pass All Tests (TDD Green)

**Files:**
- Modify: `src/net/clock_sync.cpp` — replace stub bodies with the real implementation

- [ ] **Step 1: Replace the stub clock_sync.cpp with the real implementation**

Use the Write tool to replace the entire file:

```cpp
// clock_sync.cpp — client-side estimate of server simulation tick + wall time.
// See clock_sync.h for the contract; this file is the math. Pinned to 60 Hz tick
// (TICK_DT = 1/60 s) because the project's simulation runs at that rate — if the
// tick rate ever changes, surface it as a parameter rather than hardcoding.
//
// All four operations (reset, onPongReceived, onSnapshotReceived, currentServerTickEst)
// are O(1). No allocation, no logging in hot paths — caller can log diagnostics at
// will from outside.

#include "net/clock_sync.h"
#include "core/types.h"

// Tick rate is shared with the rest of the engine (NET_TICK_RATE in net.h is 60).
// Duplicating the constant here keeps clock_sync.cpp independent of net.h's broader
// surface — only the type aliases are needed.
static constexpr f32 TICKS_PER_SEC = 60.0f;

void ClockSyncOps::reset(ClockSync& cs) {
    cs.bootstrapped       = false;
    cs.serverTickEst      = 0.0;
    cs.lastUpdateClockSec = 0.0;
    cs.oneWayTripMs       = 30.0f;
    cs.pongsReceived      = 0;
    cs.snapshotsApplied   = 0;
}

void ClockSyncOps::onPongReceived(ClockSync& cs,
                                  u32 clientSentMs,
                                  u32 serverTickAtRecv,
                                  f64 pongRecvNowSec) {
    const u32 nowMs = static_cast<u32>(pongRecvNowSec * 1000.0);
    const u32 rttMs = nowMs - clientSentMs;
    const f32 newOwt = static_cast<f32>(rttMs) * 0.5f;

    if (!cs.bootstrapped) {
        cs.oneWayTripMs = newOwt;
        cs.bootstrapped = true;
    } else {
        cs.oneWayTripMs += ClockSync::OWT_GAIN * (newOwt - cs.oneWayTripMs);
    }

    const f64 serverTickAtPongArrival =
        static_cast<f64>(serverTickAtRecv) + (cs.oneWayTripMs / 1000.0) * TICKS_PER_SEC;

    cs.serverTickEst      = serverTickAtPongArrival;
    cs.lastUpdateClockSec = pongRecvNowSec;
    cs.pongsReceived++;
}

void ClockSyncOps::onSnapshotReceived(ClockSync& cs, u32 snapServerTick, f64 recvNowSec) {
    if (!cs.bootstrapped) {
        cs.serverTickEst      = static_cast<f64>(snapServerTick) +
                                (cs.oneWayTripMs / 1000.0) * TICKS_PER_SEC;
        cs.lastUpdateClockSec = recvNowSec;
        cs.bootstrapped       = true;
        cs.snapshotsApplied++;
        return;
    }

    const f64 elapsedSinceUpdate = recvNowSec - cs.lastUpdateClockSec;
    const f64 predictedNow       = cs.serverTickEst + elapsedSinceUpdate * TICKS_PER_SEC;

    const f64 observed = static_cast<f64>(snapServerTick) +
                         (cs.oneWayTripMs / 1000.0) * TICKS_PER_SEC;

    const f64 delta = observed - predictedNow;
    if (delta > ClockSync::LARGE_DELTA || delta < -ClockSync::LARGE_DELTA) {
        // Big jump — phase change (host restarted floor → m_serverTick=0, network
        // hiccup, etc.). Snap rather than smooth so we don't lag the estimate for
        // seconds waiting for the P controller to catch up.
        cs.serverTickEst = observed;
    } else {
        cs.serverTickEst = predictedNow + ClockSync::SNAP_GAIN * delta;
    }
    cs.lastUpdateClockSec = recvNowSec;
    cs.snapshotsApplied++;
}

f64 ClockSyncOps::currentServerTickEst(const ClockSync& cs, f64 nowSec) {
    if (!cs.bootstrapped) return 0.0;
    const f64 elapsed = nowSec - cs.lastUpdateClockSec;
    return cs.serverTickEst + elapsed * TICKS_PER_SEC;
}

u32 ClockSyncOps::currentServerTickEstU32(const ClockSync& cs, f64 nowSec) {
    const f64 est = currentServerTickEst(cs, nowSec);
    if (est <= 0.0) return 0;
    return static_cast<u32>(est);
}
```

- [ ] **Step 2: Build and run tests — expect ALL to pass**

```bash
cmake --build build --target dungeon_tests 2>&1 | tail -5
./build/tests/dungeon_tests 2>&1 | tail -10
```

Expected: 12 cases total (3 sanity + 9 ClockSync), 12 passed, 0 failed. doctest's footer says `Status: SUCCESS!`.

If any case still fails, read the doctest output — it shows file:line + the decomposed expression. The arithmetic in this file follows the test cases exactly; mismatch means a typo. Don't proceed to commit until green.

- [ ] **Step 3: Build the game (sanity — make sure adding clock_sync.cpp didn't break anything else)**

The game binary doesn't link clock_sync.cpp yet (that wiring lands in Task 3+), but compiling the file with the rest of the project is a smoke check.

```bash
cmake --build build 2>&1 | tail -3
```
Expected: `Built target DungeonEngine` and `Built target dungeon_tests`, no errors.

- [ ] **Step 4: Commit the green phase**

```bash
git add src/net/clock_sync.cpp
git diff --cached --stat
git commit -m "$(cat <<'EOF'
feat(net): ClockSync impl — all 9 tests green (TDD green) — M1.2

Replaces the no-op stubs with the real math:

  - reset(): zero out all fields, oneWayTripMs back to the 30 ms default.
  - onPongReceived(): compute RTT from clientSentMs round-trip, halve to
    OWT, EMA-smooth (gain 0.2) after first pong, project serverTick at
    pong arrival forward by OWT.
  - onSnapshotReceived(): if not bootstrapped, seed; else P-controller
    smooth (gain 0.1) toward the observed serverTick + OWT projection.
    Large deltas (>6 ticks, e.g. host floor-descent reset) snap rather
    than smooth.
  - currentServerTickEst(): project serverTickEst forward by elapsed
    wall time × TICKS_PER_SEC. Returns 0 if not bootstrapped.
  - currentServerTickEstU32(): floor to u32.

12/12 dungeon_tests cases pass (3 sanity + 9 ClockSync). Game binary
still builds clean.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Add CL_TIME_PING / SV_TIME_PONG Packet Types

**Files:**
- Modify: `src/net/net.h` — add new packet types

(No tests — enum addition. Wire-format roundtrip gets tested in Task 5 via the SV_TIME_PONG decode.)

- [ ] **Step 1: Locate the NetPacketType enum**

```bash
grep -n 'enum.*NetPacketType\|CL_INPUT\|CL_REQUEST_DESCEND\|SV_LEVEL_SEED' src/net/net.h | head -10
```

Note the enum style and the existing CL_*/SV_* assignments. Pick the next free values per the project's convention (likely CL_TIME_PING=0x09 if 0x08 is the last CL_*, and SV_TIME_PONG = next free SV_*).

- [ ] **Step 2: Add the new enum values via Edit tool**

Locate the existing CL_* and SV_* enum members. Insert (preserving alignment):

```cpp
    CL_TIME_PING       = 0x09,   // 4-byte payload: u32 clientTimeMs (echoed by SV_TIME_PONG)
```
alongside the other CL_* members, and:
```cpp
    SV_TIME_PONG       = 0x16,   // 12-byte payload: u32 clientTimeMs + u32 serverTick + u32 serverTimeMs
```
alongside the other SV_* members. If 0x09 or 0x16 collide with existing values, pick the next free numbers and document.

- [ ] **Step 3: Build to confirm enum compiles**

```bash
cmake --build build 2>&1 | tail -5
```
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add src/net/net.h
git commit -m "$(cat <<'EOF'
net: add CL_TIME_PING / SV_TIME_PONG packet types — M1.3

Wire types for the clock-sync handshake. Payloads:
  CL_TIME_PING:  u32 clientTimeMs (4 B)
  SV_TIME_PONG:  u32 clientTimeMs + u32 serverTick + u32 serverTimeMs (12 B)

Server echoes the client's stamped time as-is so the client can compute
RTT without needing the server's wall clock — clientTimeMs is round-
tripped unchanged.

Server-side handler in M1.4. Client-side decoder + ClockSync routing
in M1.5 (TDD against synthesized bytes).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Server Handles CL_TIME_PING, Client Sends Handshake

**Files:**
- Modify: `src/net/server.cpp` — add CL_TIME_PING dispatch case
- Modify: `src/engine/engine.h` — add `ClockSync m_clockSync; f64 m_lastPingSentSec; u32 m_pingsSent;` members + `serverTickNow()` accessor
- Modify: `src/engine/engine.cpp` — reset m_clockSync on connect/disconnect
- Modify: `src/engine/engine_net.cpp` — send handshake pings from clientNetPre

This task is **not unit-tested** — it operates on live ENet peers. Verification is build + (deferred) manual smoke. The ClockSyncOps::* functions involved are already covered by the Task 1+2 tests; this task only wires the call sites.

- [ ] **Step 1: Add ClockSync members + accessor to Engine**

In `src/engine/engine.h`, find the line `u32 m_serverTick = 0;` (around line 152) and add immediately after it:

```cpp
    // Clock-sync subsystem (CLIENT role) — see src/net/clock_sync.h. The host
    // (SERVER role) has direct access to its m_serverTick so it does not consult
    // m_clockSync.
    ClockSync m_clockSync;
    f64       m_lastPingSentSec = 0.0;
    u32       m_pingsSent       = 0;
```

In the same file, in the public section near other small inline accessors (look for any existing `const`-returning short methods), add:

```cpp
    u32 serverTickNow() const { return m_serverTick; }
```

Also add `#include "net/clock_sync.h"` to the top of engine.h if not already pulled in.

- [ ] **Step 2: Reset ClockSync on connect / disconnect**

In `src/engine/engine.cpp`, find the spots where a successful connect happens and where a disconnect happens. (Likely in `startGame()` for CLIENT role and in a disconnect callback.) Add at each:

```cpp
    ClockSyncOps::reset(m_clockSync);
    m_pingsSent = 0;
    m_lastPingSentSec = 0.0;
```

If you can't readily find a clean spot, the safest fallback is to call this at the top of whatever function transitions GameState to IN_GAME for a CLIENT role.

- [ ] **Step 3: Send handshake pings from clientNetPre**

In `src/engine/engine_net.cpp`, in `Engine::clientNetPre`, before the existing `Client::captureAndSendInput(...)` call, add:

```cpp
    // Clock-sync handshake — send 3 CL_TIME_PINGs ~10 ms apart immediately after
    // connection, then stop. Snapshot-driven refinement (ClockSyncOps::onSnapshotReceived,
    // wired in M1.6) takes over from there.
    constexpr u32 HANDSHAKE_PING_COUNT       = 3;
    constexpr f64 HANDSHAKE_PING_SPACING_SEC = 0.010;
    const f64 nowSec = Clock::getElapsedSeconds();
    if (m_pingsSent < HANDSHAKE_PING_COUNT &&
        (m_pingsSent == 0 || nowSec - m_lastPingSentSec >= HANDSHAKE_PING_SPACING_SEC)) {
        const u32 clientTimeMs = static_cast<u32>(nowSec * 1000.0);
        PacketWriter w;
        w.writeU8(static_cast<u8>(NetPacketType::CL_TIME_PING));
        w.writeU8(0);
        w.writeU16(0);
        w.writeU32(clientTimeMs);
        Net::sendToServer(w.data, w.cursor, /*reliable=*/false);
        m_lastPingSentSec = nowSec;
        m_pingsSent++;
    }
```

- [ ] **Step 4: Server-side CL_TIME_PING handler**

Locate the packet dispatch in `src/net/server.cpp`:
```bash
grep -n 'CL_INPUT\|CL_FIRE_WEAPON\|CL_REQUEST_DESCEND' src/net/server.cpp | head -5
```

Find the switch/if-chain and add a CL_TIME_PING case. Sketch (adapt to the file's actual helper conventions):

```cpp
case NetPacketType::CL_TIME_PING: {
    if (size < 4) { LOG_WARN("net: short CL_TIME_PING (%u bytes)", size); break; }
    PacketReader r(data, size);
    const u32 clientTimeMs = r.readU32();
    const u32 serverTick   = g_engine->serverTickNow();
    const u32 serverTimeMs = static_cast<u32>(Clock::getElapsedSeconds() * 1000.0);
    PacketWriter w;
    w.writeU8(static_cast<u8>(NetPacketType::SV_TIME_PONG));
    w.writeU8(0);
    w.writeU16(0);
    w.writeU32(clientTimeMs);
    w.writeU32(serverTick);
    w.writeU32(serverTimeMs);
    Net::sendToPeer(peer, w.data, w.cursor, /*reliable=*/false);
    break;
}
```

If `g_engine` isn't already declared as a global in server.cpp, pass the Engine reference into the dispatch the same way other handlers reach it.

- [ ] **Step 5: Build**

```bash
cmake --build build 2>&1 | tail -10
```
Expected: clean. Both `DungeonEngine` and `dungeon_tests` rebuild green (tests are not affected by this task).

If you get linker errors about `g_engine`, look at how existing handlers reach engine state and use the same pattern.

- [ ] **Step 6: Run tests — no regressions**

```bash
./build/tests/dungeon_tests 2>&1 | tail -5
```
Expected: 12/12 pass (no new tests, none broken).

- [ ] **Step 7: Commit**

```bash
git add src/engine/engine.h src/engine/engine.cpp src/engine/engine_net.cpp src/net/server.cpp
git diff --cached --stat
git commit -m "$(cat <<'EOF'
net: wire CL_TIME_PING handshake (server + client) — M1.4

Engine gets m_clockSync (ClockSync), m_lastPingSentSec, m_pingsSent
fields + a small serverTickNow() accessor for use from server.cpp.
ClockSync is reset on connect/disconnect so reconnects bootstrap
cleanly.

clientNetPre sends 3 CL_TIME_PINGs ~10 ms apart at the top of the tick
once connected, until 3 are sent. server.cpp's CL_TIME_PING handler
echoes back a SV_TIME_PONG carrying the client's stamped time
(round-tripped unchanged) plus serverTick and serverTimeMs.

Client-side decoder + ClockSync routing in M1.5 (TDD).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Client Decodes SV_TIME_PONG, Feeds ClockSync (TDD)

**Files:**
- Modify: `tests/net/test_clock_sync.cpp` — append a test that exercises `Client::handleTimePong` with synthesized bytes
- Modify: `src/net/client.h` — declare `handleTimePong`
- Modify: `src/net/client.cpp` — implement `handleTimePong` (red → green)
- Modify: wherever SV_* dispatch happens (likely `src/engine/engine_net.cpp` or `src/net/net.cpp`) — route SV_TIME_PONG into the handler
- Modify: `tests/CMakeLists.txt` — add `src/net/client.cpp` to the test target's source list IF the test needs it (likely yes, since `handleTimePong` will be in client.cpp)

- [ ] **Step 1: Write the failing test**

Append to `tests/net/test_clock_sync.cpp`:

```cpp
// --- SV_TIME_PONG byte-buffer decode tests ---
// These exercise the wire-decode side: a SV_TIME_PONG payload as bytes goes through
// Client::handleTimePong, which unpacks the fields and feeds them to ClockSyncOps.

#include "net/client.h"   // for Client::handleTimePong

TEST_CASE("Client::handleTimePong decodes payload and seeds ClockSync") {
    // Build a 12-byte SV_TIME_PONG payload by hand.
    u8 payload[12];
    // clientTimeMs = 100 (little-endian u32 — matches PacketReader::readU32)
    payload[0] = 100; payload[1] = 0; payload[2] = 0; payload[3] = 0;
    // serverTick = 200
    payload[4] = 200; payload[5] = 0; payload[6] = 0; payload[7] = 0;
    // serverTimeMs = 9999 (ignored by handleTimePong but must be in payload to read)
    payload[8] = 0x0F; payload[9] = 0x27; payload[10] = 0; payload[11] = 0;

    ClockSync cs;
    ClockSyncOps::reset(cs);
    // Pretend we received this payload at pongRecvNowSec=0.130 — same numbers as
    // the "first pong bootstraps" test above, exercising the dispatch + decode path.
    Client::handleTimePong(payload, sizeof(payload), cs, /*pongRecvNowSec=*/0.130);
    CHECK(cs.bootstrapped == true);
    CHECK(cs.pongsReceived == 1);
    CHECK(cs.oneWayTripMs == doctest::Approx(15.0f));
    CHECK(cs.serverTickEst == doctest::Approx(200.9));
}

TEST_CASE("Client::handleTimePong ignores too-short payloads") {
    u8 payload[4] = { 100, 0, 0, 0 };   // 4 bytes — way too small
    ClockSync cs;
    ClockSyncOps::reset(cs);
    Client::handleTimePong(payload, sizeof(payload), cs, 0.130);
    CHECK(cs.bootstrapped == false);
    CHECK(cs.pongsReceived == 0);
}
```

If the project's PacketReader is not endian-portable (or uses a different byte order), adjust the byte layout accordingly. Confirm by reading `src/net/packet.h` — the existing `readU32` is the canonical layout to match.

- [ ] **Step 2: Add Client::handleTimePong declaration in client.h**

Use the Edit tool on `src/net/client.h`. Add this declaration alongside the other `void receive*` / `void handle*` Client functions:

```cpp
    // Decode a SV_TIME_PONG payload and feed it to ClockSync. Caller supplies the
    // wall-clock time of the pong arrival (Clock::getElapsedSeconds at decode time).
    // Short payloads are logged and ignored (cs unchanged).
    void handleTimePong(const u8* data, u32 size, ClockSync& cs, f64 pongRecvNowSec);
```

Add `#include "net/clock_sync.h"` to client.h's includes if not already pulled in.

- [ ] **Step 3: Add `src/net/client.cpp` to the test target so the test can link**

Use the Edit tool on `tests/CMakeLists.txt` to extend the source list:

```
add_executable(dungeon_tests
    test_main.cpp
    sanity_test.cpp
    net/test_clock_sync.cpp
    ${CMAKE_SOURCE_DIR}/src/net/clock_sync.cpp
    ${CMAKE_SOURCE_DIR}/src/net/client.cpp
)
```

**Watch out:** client.cpp likely includes many other engine headers and references many other symbols. If it pulls in cross-file dependencies that don't exist in the test target, you'll get link errors. In that case, you have two options:

A. Move just `handleTimePong` into a small new file `src/net/client_time.cpp` so the test only needs to link that one. (Cleaner — recommended.)

B. Add all of client.cpp's dependencies to the test target source list. (Messier — fast path is to try and see how bad the link error is.)

Try A first. Move the handleTimePong implementation into `src/net/client_time.cpp` (small file: just `#include "net/client.h"` + the function body). Update `src/CMakeLists.txt` (the main game one) to add `net/client_time.cpp` so the game build also picks it up. Then add `src/net/client_time.cpp` to the test source list instead of client.cpp.

- [ ] **Step 4: Build — expect the test to FAIL (function doesn't exist yet)**

```bash
cmake --build build --target dungeon_tests 2>&1 | tail -10
```
Expected: link error mentioning undefined reference to `Client::handleTimePong`. That's the red phase confirmation.

- [ ] **Step 5: Implement handleTimePong**

Decide on file (`src/net/client_time.cpp` if you went with Option A, otherwise `src/net/client.cpp`). Write:

```cpp
// client_time.cpp — clock-sync pong decoder. Split out from client.cpp so the test
// target can link it without pulling in client.cpp's full dependency tree.

#include "net/client.h"
#include "net/packet.h"     // PacketReader
#include "net/clock_sync.h"
#include "core/log.h"

void Client::handleTimePong(const u8* data, u32 size, ClockSync& cs, f64 pongRecvNowSec) {
    if (size < 12) {
        LOG_WARN("net: short SV_TIME_PONG (%u bytes)", size);
        return;
    }
    PacketReader r(data, size);
    const u32 clientTimeMs = r.readU32();
    const u32 serverTick   = r.readU32();
    const u32 serverTimeMs = r.readU32();
    (void)serverTimeMs;   // currently unused; reserved for future wall-time diagnostics
    ClockSyncOps::onPongReceived(cs, clientTimeMs, serverTick, pongRecvNowSec);

    if (cs.pongsReceived <= 3) {
        LOG_INFO("net: clock-sync pong #%u — oneWayTripMs=%.1f serverTickEst=%.1f",
                 cs.pongsReceived, cs.oneWayTripMs, cs.serverTickEst);
    }
}
```

If you put it in `client_time.cpp`, also update `src/CMakeLists.txt` to add `net/client_time.cpp` to the production source list so the game also picks it up.

- [ ] **Step 6: Rebuild — tests pass**

```bash
cmake --build build --target dungeon_tests 2>&1 | tail -5
./build/tests/dungeon_tests 2>&1 | tail -10
```
Expected: 14/14 cases pass (3 sanity + 9 original ClockSync + 2 new handleTimePong).

- [ ] **Step 7: Wire SV_TIME_PONG dispatch on incoming packets**

Locate where the client dispatches received SV_* packets:
```bash
grep -n 'SV_SNAPSHOT\|SV_EVENT\|SV_LEVEL_SEED\|case NetPacketType' src/net/client.cpp src/net/net.cpp src/engine/engine_net.cpp | head -10
```

Add a case for SV_TIME_PONG that calls `Client::handleTimePong(data, size, engine->m_clockSync, Clock::getElapsedSeconds())`. Pass through whatever pattern existing SV_* dispatches use to reach engine state.

- [ ] **Step 8: Build the full game — sanity**

```bash
cmake --build build 2>&1 | tail -5
```
Expected: both targets build clean.

- [ ] **Step 9: Commit**

```bash
git add tests/net/test_clock_sync.cpp tests/CMakeLists.txt src/net/client.h src/net/client_time.cpp src/CMakeLists.txt
# (Plus any other files you touched, e.g. the dispatch site)
git diff --cached --stat
git commit -m "$(cat <<'EOF'
feat(net): Client::handleTimePong decodes SV_TIME_PONG → ClockSync (TDD) — M1.5

Adds 2 new tests in test_clock_sync.cpp synthesizing a 12-byte
SV_TIME_PONG payload and asserting the decoder routes the bytes
into ClockSyncOps::onPongReceived correctly (and ignores short
payloads).

Impl in src/net/client_time.cpp (split from client.cpp so the test
target doesn't have to link all of client.cpp's deps). Picked up
by both DungeonEngine and dungeon_tests.

SV_TIME_PONG dispatch wired into the client's packet receive path.

14/14 tests pass.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Hook ClockSync Into Snapshot Reception

**Files:**
- Modify: `src/net/client.cpp` — `Client::receiveSnapshot` calls `ClockSyncOps::onSnapshotReceived` once the snapshot is deserialized
- Modify: `src/net/client.h` — receiveSnapshot signature gains a `ClockSync&` parameter
- Modify: `src/engine/engine_net.cpp` — pass `m_clockSync` to receiveSnapshot

No new tests — Task 1's `onSnapshotReceived seeds when not bootstrapped` and `onSnapshotReceived smooths small drift` tests already cover the math. This task only wires the call site.

- [ ] **Step 1: Update signature and impl**

Find `void Client::receiveSnapshot(const u8* data, u32 size)` in `src/net/client.cpp` (~line 181) and `src/net/client.h`. Change signature to:

```cpp
void receiveSnapshot(const u8* data, u32 size, ClockSync& cs);
```

In the .cpp, after the snapshot is successfully deserialized (the `if (Snapshot::deserialize(snap, data, size))` branch), add:

```cpp
        ClockSyncOps::onSnapshotReceived(cs, snap.serverTick, Clock::getElapsedSeconds());

        static u32 s_logCounter = 0;
        if ((s_logCounter++ % 30) == 0) {
            LOG_INFO("net: clock-sync serverTickEst=%.1f wireServerTick=%u oneWayTripMs=%.1f",
                     cs.serverTickEst, snap.serverTick, cs.oneWayTripMs);
        }
```

- [ ] **Step 2: Update callers**

Find where `receiveSnapshot` is called (likely in `engine_net.cpp` or `net.cpp`'s packet dispatch). Pass `m_clockSync` through.

- [ ] **Step 3: Build, run tests, build game**

```bash
cmake --build build 2>&1 | tail -5
./build/tests/dungeon_tests 2>&1 | tail -5
```
Expected: both build clean, 14/14 tests still pass.

- [ ] **Step 4: Commit**

```bash
git add src/net/client.cpp src/net/client.h src/engine/engine_net.cpp
git commit -m "$(cat <<'EOF'
net: hook ClockSync into Client::receiveSnapshot — M1.6

receiveSnapshot now takes a ClockSync& and feeds snap.serverTick into
ClockSyncOps::onSnapshotReceived after a successful deserialize. The
P controller (gain 0.1) smooths transient jitter; large deltas (>6
ticks, e.g. host floor-descent reset) snap rather than smooth.

Throttled 1 Hz diagnostic log prints estimate / wire tick / smoothed
oneWayTripMs so manual smoke can eyeball convergence.

No new tests — Task 1's onSnapshotReceived math is already covered
unit-test-wise; this task only wires the existing math into the live
dispatch path.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Rename NetInput.tick → NetInput.clientTick + Add m_clientTick

**Files:**
- Modify: `src/net/net_player.h` — rename `tick` to `clientTick`
- Modify: `src/net/client.cpp`, `src/net/server.cpp`, possibly `src/net/snapshot.cpp` — update reads/writes
- Modify: `src/engine/engine.h` — add `u32 m_clientTick = 0;`
- Modify: `src/engine/engine_net.cpp` — increment m_clientTick in clientNetPre, pass it to captureAndSendInput

No new tests — mechanical rename. The build succeeding is the test.

- [ ] **Step 1: Inventory existing references**

```bash
grep -rn 's_latestInput\.tick\|\.tick = \|input->tick\|input\.tick\|inputs\[' src/net/ src/engine/ | grep -v '//.*tick\|//.*tickRate\|//.*tickTimer\|burnTimer\|tickRate\|tickTimer' | head -20
```

Make a list of every NetInput.tick reference. Sweep them all in one pass.

- [ ] **Step 2: Rename the wire field**

Edit `src/net/net_player.h`. Change:
```cpp
    u32 tick;           // which server tick this input is for
```
to:
```cpp
    u32 clientTick;     // monotonic client-local sim tick (M1) — server uses for input
                        // ring buffer ordering and (in M2) for lastProcessedInputTick echo.
```

- [ ] **Step 3: Update every reader and writer**

Use the Edit tool on each call site identified in Step 1. Replace `.tick` with `.clientTick` on NetInput instances. Be careful not to rename unrelated `tick` references (e.g., `m_serverTick`, `tickTimer`).

InputRingBuffer (also in net_player.h) checks `input.tick <= newest.tick` — rename those too.

- [ ] **Step 4: Add m_clientTick on Engine**

In `src/engine/engine.h`, near `u32 m_serverTick = 0;`, add:

```cpp
    // Client-local monotonic sim tick. Drives NetInput.clientTick (M1). Independent of
    // m_serverTick. On CLIENT role, read m_clockSync.currentServerTickEst for any
    // "what does the server think the time is" question.
    u32 m_clientTick = 0;
```

- [ ] **Step 5: Increment m_clientTick and use it for input stamping**

In `src/engine/engine_net.cpp` `Engine::clientNetPre`, near the existing `m_serverTick++` (around line 533), add:

```cpp
    m_clientTick++;
```

Change the `Client::captureAndSendInput(...)` call to pass `m_clientTick` instead of `m_serverTick`:

```cpp
    Client::captureAndSendInput(m_localPlayer, m_clientTick, ws.currentWeapon, m_activeClassSkill);
```

The function already names its argument `clientTick` (see client.h:31), so the call is now self-consistent.

- [ ] **Step 6: Build, test, game-build**

```bash
cmake --build build 2>&1 | tail -10
./build/tests/dungeon_tests 2>&1 | tail -5
```
Expected: clean build, 14/14 tests still pass. If you get a `no member named 'tick' in NetInput` error, a reference was missed in Step 3.

- [ ] **Step 7: Commit**

```bash
git add -u src/
git diff --cached --stat
git commit -m "$(cat <<'EOF'
net: rename NetInput.tick → clientTick, add Engine::m_clientTick — M1.7

Two changes that look cosmetic but pry apart two previously-conflated
concepts:

  1. Wire field rename — NetInput.tick is now NetInput.clientTick.
     Same bytes on the wire (still 4 B at the same offset). Server
     still reads it the same way (input-ring ordering by clientTick).

  2. m_clientTick on Engine — a monotonic counter incremented per
     clientNetPre tick. This now drives the wire stamp; m_serverTick
     no longer flows out as "the tick the client thinks it's at".

Together: client publishes its own time; server publishes its own
time (snapshot.serverTick); the client-side ClockSync (added in M1.1)
maps between them when needed.

m_serverTick on the client is still incremented for now (prediction-
adjacent code reads it); M1.8 audits those readers and routes the
"server time" reads through ClockSync.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Audit Remaining m_serverTick Reads on Client

**Files:** various (likely `src/net/client.cpp`, `src/engine/engine_combat.cpp`).

No new tests — audit + targeted edits.

- [ ] **Step 1: Inventory remaining m_serverTick reads on the CLIENT branch**

```bash
grep -rn 'm_serverTick' src/ | grep -v 'src/engine/engine_net.cpp:.*m_serverTick++\|server.cpp' | head -30
```

Categorize each hit:
- **Server-side / shared:** leave it.
- **Predicted-fire clientTick:** rewrite to use `m_clientTick`.
- **Floor-reset hack at client.cpp:214:** ClockSync's LARGE_DELTA path handles this now; investigate whether the reset block can be simplified.
- **Other client reads of "server time":** replace with `ClockSyncOps::currentServerTickEstU32(m_clockSync, Clock::getElapsedSeconds())`.

- [ ] **Step 2: Update predicted-fire clientTick stamping**

In `src/engine/engine_combat.cpp`, find where predicted projectiles are stamped:
```bash
grep -n 'predicted\s*=\s*true\|\.clientTick\s*=' src/engine/engine_combat.cpp | head -10
```
Use Edit tool to replace `m_serverTick` with `m_clientTick` in those stamping calls.

- [ ] **Step 3: Verify descent-reset path**

Read `src/net/client.cpp` around line 214. Decide: keep, simplify, or annotate with a TODO referencing M3. If unclear, leave it alone and add a comment:

```cpp
    // TODO(M3): the descent reset semantics here predate ClockSync. With ClockSync's
    // LARGE_DELTA snap, the explicit reset may be redundant. Revisit when client-side
    // prediction lands and the ring-buffer model changes.
```

- [ ] **Step 4: Build, test**

```bash
cmake --build build 2>&1 | tail -5
./build/tests/dungeon_tests 2>&1 | tail -5
```
Expected: clean build, 14/14 still pass.

- [ ] **Step 5: Commit**

```bash
git add -u src/
git commit -m "$(cat <<'EOF'
net: route client m_serverTick reads through ClockSync where appropriate — M1.8

Audit of remaining client-side m_serverTick references after the
NetInput rename in M1.7. Updates:

  - Predicted-projectile clientTick stamping: now reads m_clientTick.
    The matching key on the wire (SnapProjectile.clientTickLow) was
    already low-16-bits of "the client's tick at fire time" — calling
    it that explicitly cleans up the conceptual model.

  - Descent-reset path on the client (client.cpp:214 era): annotated
    with a TODO(M3) since ClockSync's LARGE_DELTA snap-rather-than-smooth
    plausibly subsumes it. Left as-is for this milestone.

  - All other client m_serverTick reads are either on the SERVER role
    (host) or in code that's being reworked in M3 anyway.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Final Build + Test + Smoke (Deferred to User)

**Files:** none (verification only)

- [ ] **Step 1: Confirm clean tree**

```bash
git status --short
```
Expected: empty.

- [ ] **Step 2: Confirm M1 commit trail**

```bash
git log 376f74b..HEAD --oneline
```

(376f74b is the commit that added the plan files; M1 commits land after it.) Expected: 8 commits in order (top = newest):
```
<hash> net: route client m_serverTick reads through ClockSync where appropriate — M1.8
<hash> net: rename NetInput.tick → clientTick, add Engine::m_clientTick — M1.7
<hash> net: hook ClockSync into Client::receiveSnapshot — M1.6
<hash> feat(net): Client::handleTimePong decodes SV_TIME_PONG → ClockSync (TDD) — M1.5
<hash> net: wire CL_TIME_PING handshake (server + client) — M1.4
<hash> net: add CL_TIME_PING / SV_TIME_PONG packet types — M1.3
<hash> feat(net): ClockSync impl — all 9 tests green (TDD green) — M1.2
<hash> test(net): ClockSync test suite + stub (TDD red phase) — M1.1
```

- [ ] **Step 3: Clean build from scratch**

```bash
rm -rf build && cmake -B build 2>&1 | tail -3 && cmake --build build 2>&1 | tail -3
```
Expected: full configure + full build succeed. Both `DungeonEngine` and `dungeon_tests` build clean.

- [ ] **Step 4: Run tests**

```bash
./build/tests/dungeon_tests
```
Expected: 14 cases, all passed. Status: SUCCESS!

- [ ] **Step 5: CTest**

```bash
ctest --test-dir build --output-on-failure
```
Expected: 1/1 passed.

- [ ] **Step 6: Manual two-process co-op smoke (USER, optional)**

Deferred to the user (Claude can't drive the GUI).

Terminal 1: `./build/dungeon_game` → Host → New Game. Once a client connects, watch the client terminal for:
```
net: clock-sync pong #1 — oneWayTripMs=0.5 serverTickEst=...
net: clock-sync pong #2 — ...
net: clock-sync pong #3 — ...
```
followed by 1 Hz `net: clock-sync serverTickEst=... wireServerTick=... oneWayTripMs=...` lines that should converge to within ±2 ticks of the host's `m_serverTick` and stay stable.

Terminal 2: `./build/dungeon_game` → Join → 127.0.0.1.

**Pass conditions:** Both players spawn/move/fire/kill/descend as before (no regression). Client log shows 3 handshake pongs post-connect. 1 Hz serverTickEst log converges within ~5 snapshots (~150 ms) and stays stable.

If smoke fails, the diagnostic logs name which step broke.

---

## What This Plan Does Not Do

- **Does not apply `t_server_est` to render-tick scheduling.** `computeInterpPair` still uses `Clock::getElapsedSeconds() - INTERP_DELAY_SEC`. M3/M4.
- **Does not change snapshot rate or interp delay.** Stays at 30 Hz / 50 ms.
- **Does not implement the rolling input window.** M2.
- **Does not implement replay reconciliation.** M3.
- **Does not wire lag compensation.** M5.

## Definition of Done

- [ ] `git status --short` empty
- [ ] `git log 376f74b..HEAD --oneline | wc -l` returns 8
- [ ] `cmake --build build` succeeds clean
- [ ] `./build/tests/dungeon_tests` → 14 cases / all passed / SUCCESS
- [ ] `src/net/clock_sync.{h,cpp}` exist, referenced in `src/CMakeLists.txt`
- [ ] `grep -c 'TEST_CASE.*ClockSync' tests/net/test_clock_sync.cpp` returns 9 (the original 9; the 2 handleTimePong cases are named `Client::handleTimePong...`)
- [ ] `grep -c 'CL_TIME_PING\|SV_TIME_PONG' src/net/net.h` returns ≥2
- [ ] `grep -c 'ClockSync m_clockSync' src/engine/engine.h` returns 1
- [ ] `grep -c 'u32 m_clientTick' src/engine/engine.h` returns 1
- [ ] `grep -c 'NetInput.*\.clientTick' src/` returns ≥3 (struct + writer + reader)
- [ ] Manual smoke pass (user-driven, optional)
