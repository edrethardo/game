# Netplay M1: Clock Sync + Tick Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the client a stable estimate of server time (`t_server_est`) via a connect-time handshake and per-snapshot refinement, separate the client's local tick (`m_clientTick`) from any server-time concept, and rename the `NetInput.tick` wire field to `NetInput.clientTick` so the prediction work in M2/M3 can build on coherent timing primitives.

**Architecture:** New `ClockSync` subsystem on the client tracks a smoothed estimate of the server's current tick. Two new packet types (`CL_TIME_PING` / `SV_TIME_PONG`) bootstrap the estimate at connection (3 rapid pings to absorb single-packet outliers). Every incoming snapshot's `serverTick` field then refines the estimate via a P controller (gain ~0.1). The client maintains its own monotonic `m_clientTick` for stamping outgoing inputs; reads of "what does the server think the time is" route through `ClockSync` instead of the conflated `m_serverTick`. No prediction or replay logic in M1 — this is foundation only.

**Tech Stack:** C++17, ENet (existing transport), `Clock::getElapsedSeconds` for wall time, the existing PacketReader/Writer helpers.

**Reference spec:** [/home/aaron/.claude/plans/multiplayer-should-feel-like-curried-coral.md](../../../../.claude/plans/multiplayer-should-feel-like-curried-coral.md) — §1 ("Clock & Tick Model") and Migration Plan → Milestone 1.

---

## Pre-flight Notes

**No test framework in the project.** Verification per task is `cmake --build build` (clean build). End-of-milestone verification adds a manual two-process co-op smoke (deferred to the user — Claude can't drive a GUI).

**Scope boundaries.** M1 only delivers the *estimator* and the *clientTick stamping*. It does NOT:
- Apply `t_server_est` to render-tick scheduling — `computeInterpPair` continues using `Clock::getElapsedSeconds() - INTERP_DELAY_SEC` for now. (Tightening renderTime to use `t_server_est` is part of M3-M4.)
- Apply `t_server_est` to input send-tick scheduling — inputs continue sending at the current tick rate. (Send-tick scheduling lands in M2's input-pipeline rewrite.)
- Change snapshot rate or interp delay (already 30 Hz / 50 ms post-M0 baseline).
- Touch lag compensation (M5).

Doing only the foundation in M1 means the engine still ships and plays after M1 lands — no half-built state where rendering or hit detection waits on incomplete clock logic.

**Conflict with the deprecated NetInput.posXQ/Y/Z.** Those fields are flagged for removal in M3. M1 does not touch them. Future M3 plan removes them in one atomic change.

**Where m_serverTick is used today.** A grep at plan-writing time showed the field is touched in `engine.h:152` (declaration), `engine_net.cpp:63, 70, 489, 506, 510, 533, 541` (server tick increment + snapshot stamping + input stamping), and `client.cpp:214` (a comment noting the host resets it on descent). M1 introduces a clear split: server tick handling stays unchanged on the host's SERVER role; client (CLIENT role) gets `m_clientTick` for its own monotonic counter, and `ClockSync` for the estimate of remote server time.

---

## Task 1: Add ClockSync Class Skeleton

**Files:**
- Create: `src/net/clock_sync.h`
- Create: `src/net/clock_sync.cpp`
- Modify: `CMakeLists.txt` (only if it explicitly lists source files; if it globs `src/**/*.cpp`, no change needed)

- [ ] **Step 1: Check the CMakeLists.txt source-list pattern**

Run:
```bash
grep -n 'src/net' CMakeLists.txt | head
```
If output shows individual file names (e.g., `src/net/client.cpp`), you'll need to add `src/net/clock_sync.cpp` to the list. If it shows a glob pattern (`src/**/*.cpp` or similar), no edit needed.

- [ ] **Step 2: Create the header**

Write `src/net/clock_sync.h` with this content:

```cpp
#pragma once

#include "core/types.h"

// Client-side estimate of the server's current simulation tick + wall-clock time.
// Foundation for the netplay rewrite (M1 in the rewrite design doc) — the prediction
// + replay reconciliation work in M3 needs to know *when* a snapshot represents,
// not just *what it carries*.
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
    bool   bootstrapped     = false;   // true once at least one pong has arrived
    f64    serverTickEst    = 0.0;     // smoothed estimate of server's current tick at `lastUpdateClockSec`
    f64    lastUpdateClockSec = 0.0;   // local Clock::getElapsedSeconds() when serverTickEst was last refreshed
    f32    oneWayTripMs     = 30.0f;   // smoothed half-RTT (defaults to a benign guess pre-handshake)
    u32    pongsReceived    = 0;       // diagnostic
    u32    snapshotsApplied = 0;       // diagnostic

    // Smoothing gains (tuned conservatively — raise if estimate is too sluggish in testing)
    static constexpr f32 SNAP_GAIN     = 0.1f;   // P controller gain when ingesting snapshot.serverTick
    static constexpr f32 OWT_GAIN      = 0.2f;   // EMA gain for one-way-trip
    static constexpr f64 LARGE_DELTA   = 6.0;    // ticks: jump (don't smooth) if estimate is this far off
};

namespace ClockSyncOps {
    // Reset to pristine state — call on disconnect / before reconnect.
    void reset(ClockSync& cs);

    // Process a pong reply. `clientSentMs` is what we stamped on our ping;
    // `serverTickAtRecv` is the server tick when it processed the ping;
    // `pongRecvNowSec` is local Clock::getElapsedSeconds() when we received the pong.
    // Computes RTT, half-RTT (oneWayTripMs), and seeds serverTickEst.
    void onPongReceived(ClockSync& cs,
                        u32 clientSentMs,
                        u32 serverTickAtRecv,
                        f64 pongRecvNowSec);

    // Refine the estimate using the serverTick stamp on an incoming snapshot.
    // Uses the P controller to converge without phase-jumping. Skips refinement
    // until bootstrapped (first pong has set a baseline).
    void onSnapshotReceived(ClockSync& cs, u32 snapServerTick, f64 recvNowSec);

    // Current estimate of the server's tick at "now" (local clock).
    // Returns 0.0 if not yet bootstrapped.
    f64 currentServerTickEst(const ClockSync& cs, f64 nowSec);

    // Convenience: integer (floor) tick estimate.
    u32 currentServerTickEstU32(const ClockSync& cs, f64 nowSec);
}
```

- [ ] **Step 3: Create the implementation**

Write `src/net/clock_sync.cpp` with this content:

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
static constexpr f32 TICK_DT_SEC = 1.0f / 60.0f;
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
    // We can derive RTT only if we know our own clock at send-time — clientSentMs is
    // what we stamped on the outgoing ping, captured from the same local clock now
    // sampled as pongRecvNowSec*1000. The math doesn't need server's local wall time.
    const u32 nowMs = static_cast<u32>(pongRecvNowSec * 1000.0);
    // u32 subtraction is wrap-safe; if our session approaches 2^32 ms (~49 days) we
    // have bigger problems than netcode drift.
    const u32 rttMs = nowMs - clientSentMs;
    const f32 newOwt = static_cast<f32>(rttMs) * 0.5f;

    if (!cs.bootstrapped) {
        cs.oneWayTripMs  = newOwt;
        cs.bootstrapped  = true;
    } else {
        // EMA smooth — handshake sends 3 pings rapidly, this averages them.
        cs.oneWayTripMs += OWT_GAIN * (newOwt - cs.oneWayTripMs);
    }

    // Server processed the ping `oneWayTripMs` ago (their stamp was at recv). So
    // by *now*, the server has advanced (oneWayTripMs / 1000) * 60 ticks past
    // serverTickAtRecv. That's the moment-of-pong-arrival estimate.
    const f64 serverTickAtPongArrival =
        static_cast<f64>(serverTickAtRecv) + (cs.oneWayTripMs / 1000.0) * TICKS_PER_SEC;

    cs.serverTickEst      = serverTickAtPongArrival;
    cs.lastUpdateClockSec = pongRecvNowSec;
    cs.pongsReceived++;
}

void ClockSyncOps::onSnapshotReceived(ClockSync& cs, u32 snapServerTick, f64 recvNowSec) {
    if (!cs.bootstrapped) {
        // No baseline yet — seed directly. This shouldn't happen in normal play (handshake
        // runs before snapshots start streaming) but covers the case where the very first
        // snapshot beats the pong reply.
        cs.serverTickEst      = static_cast<f64>(snapServerTick) +
                                (cs.oneWayTripMs / 1000.0) * TICKS_PER_SEC;
        cs.lastUpdateClockSec = recvNowSec;
        cs.bootstrapped       = true;
        cs.snapshotsApplied++;
        return;
    }

    // What we would predict the server tick to be *now* based on prior state.
    const f64 elapsedSinceUpdate = recvNowSec - cs.lastUpdateClockSec;
    const f64 predictedNow       = cs.serverTickEst + elapsedSinceUpdate * TICKS_PER_SEC;

    // Where snapshot says the server was when it was sent. We don't know exact send
    // time, but the snapshot just left the server `oneWayTripMs` ago, so server is now
    // approximately snapServerTick + oneWayTripMs/1000 * 60 ticks ahead.
    const f64 observed = static_cast<f64>(snapServerTick) +
                         (cs.oneWayTripMs / 1000.0) * TICKS_PER_SEC;

    const f64 delta = observed - predictedNow;
    if (delta > LARGE_DELTA || delta < -LARGE_DELTA) {
        // Big jump — phase change (host restarted floor → m_serverTick=0, network
        // hiccup, etc.). Snap rather than smooth so we don't lag the estimate for
        // seconds waiting for the P controller to catch up.
        cs.serverTickEst = observed;
    } else {
        // Small drift — smooth via P controller. predictedNow + delta is the target;
        // we move serverTickEst forward by elapsedSinceUpdate's worth of ticks *and*
        // close some fraction of `delta`.
        cs.serverTickEst = predictedNow + SNAP_GAIN * delta;
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

- [ ] **Step 4: Update CMakeLists.txt if needed**

If Step 1 showed individual files listed, add `src/net/clock_sync.cpp` to the list following the existing pattern. If the glob covers it, skip.

- [ ] **Step 5: Build to verify it compiles**

Run:
```bash
cmake --build build 2>&1 | tail -10
```
Expected: clean build. If you get an error about an unused parameter or include, fix it inline. If you get a duplicate-symbol or missing-symbol, the CMakeLists wasn't updated correctly.

- [ ] **Step 6: Commit**

Run:
```bash
git add src/net/clock_sync.h src/net/clock_sync.cpp
# Only stage CMakeLists.txt if you actually modified it:
git diff --cached --stat
```
If CMakeLists was touched, `git add CMakeLists.txt` too. Then:

```bash
git commit -m "$(cat <<'EOF'
net: add ClockSync skeleton for server-time estimate (M1.1)

New subsystem on the client: tracks a smoothed estimate of the server's
current simulation tick. Operates on f64 to absorb session-length drift
without losing precision. Two refinement paths: onPongReceived from a
CL_TIME_PING / SV_TIME_PONG handshake (wired next task) and
onSnapshotReceived feeding snapshot.serverTick through a P controller
(SNAP_GAIN=0.1). Big deltas (>6 ticks) snap rather than smooth so a
host floor-descent reset doesn't lag the estimate for seconds.

No callers yet — pure data type + math. Wired into the engine in M1
follow-up tasks (handshake, snapshot hook, NetInput rename).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Add CL_TIME_PING / SV_TIME_PONG Packet Types

**Files:**
- Modify: `src/net/net.h` — add the new packet types to `NetPacketType` enum
- (No serializer files — payloads are tiny; we'll inline read/write in Tasks 3-4)

- [ ] **Step 1: Locate the NetPacketType enum**

Run:
```bash
grep -n 'enum.*NetPacketType\|CL_INPUT\|CL_REQUEST_DESCEND\|SV_LEVEL_SEED' src/net/net.h | head -10
```
The enum is in `src/net/net.h`. Note its style (whether it uses `enum class` / `enum struct`, the underlying type, the existing CL_* and SV_* assignments).

- [ ] **Step 2: Add CL_TIME_PING and SV_TIME_PONG values**

Use the Edit tool. Find the line that defines the next-available CL_* and SV_* enum values (look at the highest existing assignments — e.g. CL_REQUEST_DESCEND=0x07, CL_FIRE_WEAPON=0x08, SV_LEVEL_SEED=0x15). Add the new values with the next free numbers — likely:

```cpp
    CL_TIME_PING       = 0x09,   // 8-byte payload: u32 clientTimeMs (echoed back as-is)
    SV_TIME_PONG       = 0x16,   // 12-byte payload: u32 clientTimeMs + u32 serverTick + u32 serverTimeMs
```

Slot them in next to their existing CL_*/SV_* neighbors. Do NOT renumber existing values — wire compatibility with running clients (and, for now, with the M0 baseline tree) depends on stable enum integers.

If actual enum values differ from the example above (e.g., the next free CL_* is 0x09, the next free SV_* is 0x17), use the actual free numbers. Verify by re-grep after the edit:

```bash
grep -n 'CL_TIME_PING\|SV_TIME_PONG' src/net/net.h
```
Should show your two new lines.

- [ ] **Step 3: Build to verify the enum compiles**

```bash
cmake --build build 2>&1 | tail -5
```
Expected: clean. Adding an enum value should not break anything else.

- [ ] **Step 4: Commit**

```bash
git add src/net/net.h
git commit -m "$(cat <<'EOF'
net: add CL_TIME_PING / SV_TIME_PONG packet types (M1.2)

Wire types for the clock-sync handshake. Payloads:
  CL_TIME_PING:  u32 clientTimeMs (4 B)
  SV_TIME_PONG:  u32 clientTimeMs + u32 serverTick + u32 serverTimeMs (12 B)

Server echoes the client's stamped time as-is so the client can compute
RTT without needing the server's wall clock — clientTimeMs is round-tripped
unchanged.

Handlers wired in M1.3 (client send) and M1.4 (server handle / client
recv).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Server Handles CL_TIME_PING; Wire Handshake on Client Connect

**Files:**
- Modify: `src/net/server.cpp` — add a CL_TIME_PING handler in the existing packet dispatch
- Modify: `src/engine/engine_net.cpp` — invoke the handshake from `clientNetPre` (or whatever the connect path is)
- Modify: `src/engine/engine.h` — add `ClockSync m_clockSync` member; `f64 m_lastPingSentSec`; `u32 m_pingsSent`

- [ ] **Step 1: Locate the server's packet dispatch**

Run:
```bash
grep -n 'CL_INPUT\|CL_FIRE_WEAPON\|CL_REQUEST_DESCEND' src/net/server.cpp | head -10
```
Find the switch/if-chain that dispatches by `NetPacketType`. That's where the new handler goes.

- [ ] **Step 2: Add the CL_TIME_PING handler in server.cpp**

Use the Edit tool to add a new case alongside the existing CL_* handlers. The handler reads the client's stamped ms, samples `serverTick` and the server's current wall clock, and writes a SV_TIME_PONG back to the same peer.

Sketch (adapt to the actual file's helper names — PacketReader / PacketWriter / send-back style):

```cpp
case NetPacketType::CL_TIME_PING: {
    // Echo back immediately with serverTick + serverTimeMs. Use the same channel as
    // CL_INPUT (unreliable, unsequenced) — the client sends 3 pings to absorb a
    // dropped reply, and a stale pong is benign (the client validates clientTimeMs
    // matches one it sent recently).
    if (size < 4) { LOG_WARN("net: short CL_TIME_PING (%u bytes)", size); break; }
    PacketReader r(data, size);
    const u32 clientTimeMs = r.readU32();
    const u32 serverTick   = g_engine->serverTickNow();    // accessor — see Step 4
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

If the server already has a way to access `m_serverTick` directly (e.g., as a static, or via a passed argument), use that instead of `g_engine->serverTickNow()`. If not, add a small accessor on `Engine`:

```cpp
// in engine.h, public section:
u32 serverTickNow() const { return m_serverTick; }
```

- [ ] **Step 3: Add m_clockSync + handshake state members to Engine**

Edit `src/engine/engine.h`. In the `Engine` class (locate via `grep -n 'm_serverTick' src/engine/engine.h`), near the existing `m_serverTick` declaration, add:

```cpp
    // Clock-sync subsystem (CLIENT role) — see src/net/clock_sync.h. The host (SERVER
    // role) has direct access to its m_serverTick so it does not consult m_clockSync.
    ClockSync m_clockSync;
    f64       m_lastPingSentSec = 0.0;   // last time we sent a CL_TIME_PING
    u32       m_pingsSent       = 0;     // count sent so far in this connection; reset on disconnect
```

Add `#include "net/clock_sync.h"` at the top of engine.h if not already pulled in by a downstream header.

- [ ] **Step 4: Reset m_clockSync on connection / disconnection**

Edit `src/engine/engine.cpp` (or wherever the client connect / disconnect handlers live). Find the spot that runs on `Net::connectToServer` success and on disconnect. Add:

- On connect: `ClockSyncOps::reset(m_clockSync); m_pingsSent = 0; m_lastPingSentSec = 0.0;`
- On disconnect: `ClockSyncOps::reset(m_clockSync); m_pingsSent = 0; m_lastPingSentSec = 0.0;`

If you can't readily find a clean connect callback, the safest fallback is to reset at the top of `startGame()` for the CLIENT role.

- [ ] **Step 5: Send handshake pings from clientNetPre**

Edit `src/engine/engine_net.cpp`. In the `Engine::clientNetPre` function (near where it currently does `Client::captureAndSendInput`), insert handshake-ping logic BEFORE the input capture:

```cpp
    // Clock-sync handshake — send 3 CL_TIME_PINGs ~10 ms apart immediately after
    // connection, then stop. Snapshot-driven refinement (ClockSyncOps::onSnapshotReceived,
    // wired in M1.5) takes over from there.
    constexpr u32 HANDSHAKE_PING_COUNT  = 3;
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

- [ ] **Step 6: Build**

```bash
cmake --build build 2>&1 | tail -10
```
Expected: clean. If you get an "undefined reference to" for `ClockSyncOps::reset`, the cpp file isn't being compiled — check CMakeLists.

- [ ] **Step 7: Commit**

```bash
git add src/net/server.cpp src/engine/engine.h src/engine/engine.cpp src/engine/engine_net.cpp
git commit -m "$(cat <<'EOF'
net: wire CL_TIME_PING handshake on client connect (M1.3)

Client sends 3 CL_TIME_PINGs ~10 ms apart at the top of each clientNetPre
once a connection is up, until 3 are sent. Server handler echoes back a
SV_TIME_PONG carrying the client's stamped time (round-tripped unchanged)
plus serverTick and serverTimeMs.

ClockSync is reset on every connect/disconnect so a reconnect bootstraps
cleanly. m_pingsSent / m_lastPingSentSec carry the handshake state on
Engine.

Server access to serverTick exposed via a small inline accessor
(serverTickNow) — preferable to global state and keeps the engine.h
surface honest.

Client handling of SV_TIME_PONG (calling ClockSyncOps::onPongReceived) is
M1.4.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Client Handles SV_TIME_PONG

**Files:**
- Modify: `src/net/client.cpp` — add `handleTimePong` and route SV_TIME_PONG into it; wire ClockSync via a small public Client API (or, simpler, pass `Engine::m_clockSync` in by reference)

There are two ways to expose `m_clockSync` to the client packet handler:
- (a) Pass a `ClockSync&` as a parameter to whatever function processes incoming packets.
- (b) Add a Client-level static pointer to the engine's clock sync (set on init).

Option (a) is cleaner; pick it if the existing Client API already takes Engine state by reference. Option (b) matches the existing pattern if Client::receiveSnapshot already operates on file-scope statics.

- [ ] **Step 1: Look at how incoming packets are dispatched on the client**

Run:
```bash
grep -n 'CL_INPUT\|SV_SNAPSHOT\|SV_EVENT\|SV_LEVEL_SEED\|case NetPacketType' src/net/client.cpp src/engine/engine_net.cpp | head -20
```
This will reveal where the SV_* dispatch happens. It might be in `Net::poll`, in `Engine::clientNetPre`, or in `src/net/net.cpp`.

- [ ] **Step 2: Add Client::handleTimePong**

If the dispatch is in client.cpp, add a new function `Client::handleTimePong(const u8* data, u32 size, ClockSync& cs)` that:

```cpp
void Client::handleTimePong(const u8* data, u32 size, ClockSync& cs) {
    if (size < 12) { LOG_WARN("net: short SV_TIME_PONG (%u bytes)", size); return; }
    PacketReader r(data, size);
    const u32 clientTimeMs    = r.readU32();
    const u32 serverTick      = r.readU32();
    const u32 serverTimeMs    = r.readU32();
    (void)serverTimeMs;   // currently unused; reserved if we later add wall-time diagnostics
    const f64 recvNowSec      = Clock::getElapsedSeconds();
    ClockSyncOps::onPongReceived(cs, clientTimeMs, serverTick, recvNowSec);
}
```

Add a matching declaration in `src/net/client.h`:
```cpp
    void handleTimePong(const u8* data, u32 size, ClockSync& cs);
```

(If using Option (b), drop the ClockSync& parameter and have handleTimePong reference a file-scope `s_clockSync` set by an init call.)

- [ ] **Step 3: Wire the dispatch**

In the packet-dispatch site you found in Step 1, add a case for `SV_TIME_PONG`:

```cpp
case NetPacketType::SV_TIME_PONG:
    Client::handleTimePong(data, size, engine->m_clockSync);
    break;
```

The exact way you reach `engine->m_clockSync` from the dispatch site depends on the existing call convention. If the dispatch site already has access to the engine instance, use it directly. If not, pass it down as a parameter or use the file-scope pointer approach.

- [ ] **Step 4: Build**

```bash
cmake --build build 2>&1 | tail -10
```
Expected: clean.

- [ ] **Step 5: Add a one-line diagnostic log**

To make manual smoke easy, log when the handshake bootstraps. In `handleTimePong`, after `ClockSyncOps::onPongReceived(...)`, add:

```cpp
    if (cs.pongsReceived <= 3) {
        LOG_INFO("net: clock-sync pong #%u — oneWayTripMs=%.1f serverTickEst=%.1f",
                 cs.pongsReceived, cs.oneWayTripMs, cs.serverTickEst);
    }
```

- [ ] **Step 6: Commit**

```bash
git add src/net/client.cpp src/net/client.h src/engine/engine_net.cpp
# Or whichever dispatch site you actually modified.
git diff --cached --stat
git commit -m "$(cat <<'EOF'
net: client handles SV_TIME_PONG, seeds ClockSync (M1.4)

Each incoming SV_TIME_PONG arrives in Client::handleTimePong, which
unpacks the round-tripped clientTimeMs + the server's tick/wall-time
stamps and feeds them into ClockSyncOps::onPongReceived. After the 3
handshake replies arrive, oneWayTripMs is the EMA of three samples
(absorbs a single packet outlier) and serverTickEst is seeded.

Diagnostic log fires on the first 3 pongs so manual smoke can confirm
the handshake completed and what the estimate looks like.

Snapshot-driven refinement is M1.5.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Hook ClockSync Into Snapshot Reception

**Files:**
- Modify: `src/net/client.cpp` — `Client::receiveSnapshot` calls `ClockSyncOps::onSnapshotReceived` once the snapshot is successfully deserialized

- [ ] **Step 1: Locate Client::receiveSnapshot**

Already mapped to `src/net/client.cpp` around line 181 (per the M0-era exploration). Verify:
```bash
grep -n 'void Client::receiveSnapshot' src/net/client.cpp
```

- [ ] **Step 2: Add the hook**

The receiveSnapshot signature needs the ClockSync reference. Two options:
- Add `ClockSync&` parameter (mirrors handleTimePong above)
- Use the same Engine accessor pattern you set up in Task 4

Pick whichever fits the existing pattern. Then, right after `Snapshot::deserialize(snap, data, size)` returns true and you have `snap.serverTick`, add:

```cpp
    ClockSyncOps::onSnapshotReceived(cs, snap.serverTick, Clock::getElapsedSeconds());
```

- [ ] **Step 3: Update the receiveSnapshot caller**

Wherever `receiveSnapshot` is called (probably also in `engine_net.cpp` or in `net.cpp`'s packet dispatch), pass the new ClockSync& through.

- [ ] **Step 4: Add a low-frequency diagnostic log**

Every 30 snapshots (~1 second at 30 Hz), log the current state:

```cpp
    static u32 s_logCounter = 0;
    if ((s_logCounter++ % 30) == 0) {
        LOG_INFO("net: clock-sync serverTickEst=%.1f wireServerTick=%u oneWayTripMs=%.1f",
                 cs.serverTickEst, snap.serverTick, cs.oneWayTripMs);
    }
```

This makes drift / jitter visible during smoke testing.

- [ ] **Step 5: Build**

```bash
cmake --build build 2>&1 | tail -10
```
Expected: clean.

- [ ] **Step 6: Commit**

```bash
git add src/net/client.cpp src/net/client.h src/engine/engine_net.cpp
git diff --cached --stat
git commit -m "$(cat <<'EOF'
net: refine ClockSync from snapshot serverTick (M1.5)

Each Client::receiveSnapshot now calls ClockSyncOps::onSnapshotReceived
with snap.serverTick. The P controller (gain 0.1) smooths transient
jitter; large deltas (>6 ticks — e.g., host floor-descent reset of
m_serverTick=0) snap rather than smooth so the estimate doesn't lag
the host for seconds.

Throttled diagnostic log (1 Hz) prints estimate / wire tick / smoothed
oneWayTripMs so manual smoke can eyeball convergence.

NetInput.tick → clientTick rename is M1.6 (will pull from a new
client-monotonic counter rather than reusing m_serverTick).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Rename NetInput.tick → NetInput.clientTick, Add Engine::m_clientTick

**Files:**
- Modify: `src/net/net_player.h` — rename the wire field
- Modify: `src/net/client.cpp` — update `captureAndSendInput` writes
- Modify: `src/net/server.cpp` (and possibly `src/net/snapshot.cpp` / `src/net/packet.h`) — update reads
- Modify: `src/engine/engine.h` — add `u32 m_clientTick`
- Modify: `src/engine/engine_net.cpp` — increment m_clientTick in clientNetPre, use it for input stamping

- [ ] **Step 1: Locate NetInput.tick references**

Run:
```bash
grep -rn 'NetInput\|\.tick\|->tick\|input\.tick\|input->tick' src/net/ src/engine/ src/game/ | grep -v 'static_cast\|tickTimer\|tickRate\|burnTimer\|//.*tick' | head -40
```

(The grep is noisy because "tick" appears in many contexts — focus on NetInput-related hits.) Make a list of the actual touch points. Expect: the struct declaration (`NetInput.tick`), the InputRingBuffer logic, the client capture/send, the server handler, possibly a few diagnostic logs.

- [ ] **Step 2: Rename the wire field**

In `src/net/net_player.h`, change:
```cpp
    u32 tick;           // which server tick this input is for
```
to:
```cpp
    u32 clientTick;     // monotonic client-local sim tick (M1) — server uses for input
                        // ring buffer ordering and (in M2) for lastProcessedInputTick echo.
```

- [ ] **Step 3: Update writers and readers**

Use the Edit tool (NOT sed — we need to be precise) to rename all `tick` references on `NetInput` instances. There will be a handful: capture, serialize write (e.g., `w.writeU32(s_latestInput.tick)`), deserialize read on the server, ring buffer ordering checks in net_player.h, any diagnostic logs.

After each edit, the file should still compile.

- [ ] **Step 4: Add Engine::m_clientTick**

In `src/engine/engine.h`, near the existing `m_serverTick` declaration, add:
```cpp
    // Client-local monotonic sim tick. Drives NetInput.clientTick (M1). Independent of
    // m_serverTick (which is the host-only server tick on SERVER role). On CLIENT role,
    // m_serverTick is currently still incremented for backward-compat with prediction
    // paths that will be reworked in M3 — read m_clockSync.currentServerTickEst for any
    // "what does the server think the time is" question.
    u32 m_clientTick = 0;
```

- [ ] **Step 5: Increment m_clientTick in clientNetPre and use it for input stamping**

In `src/engine/engine_net.cpp` `Engine::clientNetPre`, near the existing `m_serverTick++` line, add:
```cpp
    m_clientTick++;
```
Then change the `Client::captureAndSendInput` call to pass `m_clientTick` instead of `m_serverTick`:
```cpp
    Client::captureAndSendInput(m_localPlayer, m_clientTick, ws.currentWeapon, m_activeClassSkill);
```

The function signature already names the argument `clientTick` (see client.h:31), so the rename is internally consistent.

- [ ] **Step 6: Build**

```bash
cmake --build build 2>&1 | tail -10
```
Expected: clean. If you missed a renamed reference somewhere, you'll get an "no member named 'tick' in NetInput" error — fix and rebuild.

- [ ] **Step 7: Commit**

```bash
git add src/net/net_player.h src/net/client.cpp src/net/server.cpp src/engine/engine.h src/engine/engine_net.cpp
# Plus any other files you renamed in
git diff --cached --stat
git commit -m "$(cat <<'EOF'
net: rename NetInput.tick → clientTick, add Engine::m_clientTick (M1.6)

Two changes that look cosmetic but pry apart two previously-conflated
concepts:

  1. Wire field rename — NetInput.tick is now NetInput.clientTick.
     Same bytes on the wire (still 4 B at the same offset), but the
     name no longer suggests it's the server's tick. The server still
     reads it the same way (input-ring ordering by clientTick).

  2. m_clientTick on Engine — a monotonic counter incremented per
     clientNetPre tick. This now drives the wire stamp; m_serverTick
     no longer flows out as "the tick the client thinks it's at".

Together: client publishes its own time; server publishes its own
time (snapshot.serverTick); the client-side ClockSync (added in M1.1)
maps between them when needed.

m_serverTick on the client is still incremented for now (prediction-
adjacent code reads it); M1.7 audits those readers and routes the
"server time" reads through ClockSync.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Audit Remaining m_serverTick Reads on Client

**Files:**
- Modify: `src/net/client.cpp` (probable), `src/engine/engine_combat.cpp` (probable — predicted projectile clientTick), maybe others — wherever client-side code reads `m_serverTick` and *meant* "what does the server think the time is"

- [ ] **Step 1: Inventory remaining m_serverTick reads on the CLIENT branch**

Run:
```bash
grep -rn 'm_serverTick' src/ | grep -v 'src/engine/engine_net.cpp:.*m_serverTick++\|server.cpp' | head -30
```

Look for each hit. Categorize:
- **Server-side or shared:** the read is gated by `if (m_netRole == SERVER)` or in code that only the host runs. Leave it.
- **Predicted-fire clientTick:** stamps `Projectile.clientTick` on a predicted ghost. This currently uses `m_serverTick` but semantically wants "the client's monotonic tick at fire time" (so the server can match it via SnapProjectile.clientTickLow). Update to `m_clientTick`.
- **Floor-reset hack:** `client.cpp:214` comment about resetting m_serverTick on descent. This was an artifact of the old conflation; ClockSync now handles the descent reset via its LARGE_DELTA snap-rather-than-smooth path. Investigate whether the reset logic at descent is still needed at all.
- **Other:** if anything else reads m_serverTick on the CLIENT to mean "server time", replace with `ClockSyncOps::currentServerTickEstU32(m_clockSync, Clock::getElapsedSeconds())`.

- [ ] **Step 2: Update predicted-fire clientTick stamping**

In `src/engine/engine_combat.cpp`, find where predicted projectiles are stamped (look for `predicted = true` and `clientTick = m_serverTick` or similar). Change `m_serverTick` to `m_clientTick`.

Run:
```bash
grep -n 'predicted\s*=\s*true\|\.clientTick\s*=' src/engine/engine_combat.cpp | head -10
```
Use Edit tool on each match.

- [ ] **Step 3: Verify the descent-reset path**

Read `src/net/client.cpp` around line 214 (the comment about the host resetting m_serverTick=0 on descent). Decide:
- If the reset is needed to keep client interp from showing weird state across the level transition, keep it but document that ClockSync now handles the temporal side via its LARGE_DELTA snap.
- If it's dead code with ClockSync in place, remove the special case.

If unclear, leave it alone for this milestone and add a TODO comment referencing M3 (when client-side prediction lands and this code path will be revisited).

- [ ] **Step 4: Build**

```bash
cmake --build build 2>&1 | tail -10
```
Expected: clean.

- [ ] **Step 5: Commit**

```bash
git add -u src/
git diff --cached --stat
git commit -m "$(cat <<'EOF'
net: route client m_serverTick reads through ClockSync where appropriate (M1.7)

Audit of remaining client-side m_serverTick references after the
NetInput rename in M1.6. Reclassified:

  - Predicted-projectile clientTick stamping: now reads m_clientTick.
    The matching key on the wire (SnapProjectile.clientTickLow) was
    already low-16-bits of "the client's tick at fire time" — calling
    it that explicitly cleans up the conceptual model.

  - Descent reset on the client (client.cpp:214 era comment): ClockSync's
    LARGE_DELTA snap-rather-than-smooth handles the host-side
    m_serverTick=0 reset cleanly. [Replace this bullet with one of:
    "Reset path kept, comment updated to reference ClockSync handling"
    OR "Reset path removed — ClockSync absorbs the descent transition"
    OR "Left untouched with a TODO referencing M3" — based on what you
    decided in Step 3.]

  - All other client m_serverTick reads are either on the SERVER role
    (host) or in code that's being reworked in M3 anyway.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Final Build + Manual Smoke (Deferred to User)

**Files:** none (verification only)

- [ ] **Step 1: Confirm clean tree**

```bash
git status --short
```
Expected: empty.

- [ ] **Step 2: Confirm M1 commit trail**

```bash
git log --oneline 67a7ccf..HEAD
```
Expected: 7 commits (one per task above, M1.1 → M1.7). All on master.

- [ ] **Step 3: Clean build from scratch**

```bash
cmake --build build 2>&1 | tail -10
```
Expected: clean.

- [ ] **Step 4: Manual two-process co-op smoke (USER, optional)**

This is deferred to the user — Claude can't drive the GUI. Instructions:

Terminal 1: `./build/dungeon_game` → Host → New Game. Watch the log for:
```
net: clock-sync pong #1 — oneWayTripMs=0.5 serverTickEst=...
net: clock-sync pong #2 — ...
net: clock-sync pong #3 — ...
```
on the joining-client terminal once a client connects.

Terminal 2: `./build/dungeon_game` → Join → 127.0.0.1. Connect. The throttled 1 Hz `net: clock-sync serverTickEst=...` log should appear and the values should be **stable** (drifting smoothly, not jumping) once the handshake completes.

If `serverTickEst` is wildly off from the host's actual tick (visible by reading host log), there's a clock-sync bug — investigate before declaring M1 done.

**Manual smoke pass conditions:**
- Both players spawn, move, fire, kill enemies, descend floors as before (no regression).
- Client log shows 3 pongs received post-connect.
- 1 Hz `serverTickEst` log on client converges to "current host serverTick" within ~5 snapshots (~150 ms) and stays within ±2 ticks of it thereafter.

If smoke fails, the symptom usually points at which subtask broke — check the diagnostic logs first.

---

## What This Plan Does Not Do

- **Does not apply `t_server_est` to render-tick scheduling.** `computeInterpPair` still uses `Clock::getElapsedSeconds() - INTERP_DELAY_SEC`. Tightening that to use `t_server_est` is M3/M4.
- **Does not change snapshot rate or interp delay.** Stays at the M0 baseline (30 Hz / 50 ms).
- **Does not implement the rolling input window.** That's M2.
- **Does not implement replay reconciliation.** That's M3.
- **Does not wire lag compensation.** That's M5.

## Definition of Done

- [ ] `git status --short` empty
- [ ] `git log 67a7ccf..HEAD --oneline | wc -l` returns 7
- [ ] `cmake --build build` succeeds clean
- [ ] `src/net/clock_sync.h` and `src/net/clock_sync.cpp` exist and are referenced
- [ ] `grep -c 'CL_TIME_PING\|SV_TIME_PONG' src/net/net.h` returns ≥2
- [ ] `grep -c 'ClockSync m_clockSync' src/engine/engine.h` returns 1
- [ ] `grep -c 'u32 m_clientTick' src/engine/engine.h` returns 1
- [ ] `grep -c 'NetInput.*\.clientTick' src/` returns ≥3 (struct + writer + reader)
- [ ] Manual smoke pass (user-driven, optional) — clock-sync log lines appear and converge
