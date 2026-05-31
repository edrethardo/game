# Netplay Phase 2 D5+D6: Fake-Latency + Net-Graph Overlay Plan

> superpowers:subagent-driven-development. Extends M14.

**Goals:**
- **D5**: Add a fake-latency delay queue to packet send paths so manual smoke testing can simulate ~50–150 ms internet latency on a localhost connection. Adds `m_netFakeLatencyMs` cvar (already declared in M14 but unused).
- **D6**: Add an on-screen net-graph overlay (toggle: F9). Shows rolling 1 Hz numbers — RTT, snapshot rate, divergence count, fake-loss/latency settings.

---

## D5: Fake-latency queue

**Files:**
- Modify: `src/net/net.cpp` — add a tiny delay queue: `struct DelayedPacket { f64 deliverAtSec; u8 reliable; u8 target; ... bytes ... }`. On send, if `m_netFakeLatencyMs > 0`, enqueue with `deliverAtSec = now + ms/1000`. Each net update tick, pop+deliver any packets whose `deliverAtSec <= now`.

Queue size cap (~256 entries) — drop newest on overflow.

- [ ] **Step 1**: Add a static delay-queue in `src/net/net.cpp`:

```cpp
// D5 — fake-latency simulation. Queues outgoing packets and delivers them after
// configurable delay. Enabled when m_netFakeLatencyMs > 0 (read via accessor).
static constexpr u32 NET_DELAY_QUEUE_SIZE = 256;
struct DelayedPacket {
    f64 deliverAtSec = 0.0;
    bool reliable = false;
    ENetPeer* peer = nullptr;   // nullptr = broadcast / sendToServer
    u8 sendToServer = 0;        // 1 if this was a client→server send
    u32 size = 0;
    u8 data[MAX_PACKET_SIZE];
};
static DelayedPacket s_delayQueue[NET_DELAY_QUEUE_SIZE];
static u32 s_delayQueueCount = 0;

// Tick: pop+deliver any packets whose deliverAtSec <= now.
void Net::pumpDelayQueue();
```

- [ ] **Step 2**: Modify `Net::sendToServer` and `Net::sendToPeer` to check fake-latency. If `m_netFakeLatencyMs > 0`, enqueue:

```cpp
    extern Engine* s_engineForNet;
    u32 lat = s_engineForNet ? s_engineForNet->m_netFakeLatencyMs : 0;
    if (lat > 0 && s_delayQueueCount < NET_DELAY_QUEUE_SIZE) {
        DelayedPacket& q = s_delayQueue[s_delayQueueCount++];
        q.deliverAtSec = Clock::getElapsedSeconds() + lat / 1000.0;
        q.reliable = reliable;
        q.peer = nullptr;
        q.sendToServer = 1;
        q.size = size;
        std::memcpy(q.data, data, size);
        return;
    }
    // ... existing immediate-send code ...
```

- [ ] **Step 3**: `Net::pumpDelayQueue()` implementation:
```cpp
void Net::pumpDelayQueue() {
    f64 now = Clock::getElapsedSeconds();
    u32 write = 0;
    for (u32 read = 0; read < s_delayQueueCount; read++) {
        DelayedPacket& p = s_delayQueue[read];
        if (p.deliverAtSec <= now) {
            // Deliver — call the original send path with bypass flag, OR just inline
            // the ENet send code here.
            if (p.sendToServer) {
                // ENet client→server send: existing internal helper or duplicate the
                // ENetPeer send code (look at sendToServer's body).
                // For simplicity, set a thread-local bypass flag and re-invoke
                // sendToServer.
            }
        } else {
            if (write != read) s_delayQueue[write] = p;
            write++;
        }
    }
    s_delayQueueCount = write;
}
```

The cleanest implementation: refactor `sendToServer`/`sendToPeer` so they have an internal `sendImmediately` helper that the queue can call without re-triggering the delay enqueue. Read the current implementations to figure out the minimum refactor.

- [ ] **Step 4**: Call `Net::pumpDelayQueue()` once per frame on both client and server net-tick paths. Add the call near the top of clientNetPre (so delayed-from-server packets get delivered first) and at the top of the server's polling loop.

- [ ] **Step 5**: Build, 54/54 tests pass. Commit: `feat(net): fake-latency delay queue for manual smoke testing (D5)`.

---

## D6: Net-graph on-screen overlay

**Files:**
- Modify: `src/engine/engine_hud.cpp` (or wherever HUD overlays render) — add a small text panel
- Modify: input dispatch — add F9 keybind to toggle

Display (when enabled):
```
NET:  rtt=42.3ms  est=1234.5  div=2/s  loss=5%  lat=80ms
SNAP: 60Hz  in=12  loss=0  baseline=t=1230
INPUT: 60Hz  ack=t=1232  ring=4
```

For v1, just a single line of text in the top-left corner showing RTT / divergence count / fake loss / fake latency. The "rolling counts" are over a 1-second window.

- [ ] **Step 1**: Add `bool m_netGraphVisible = false;` to engine.h.

- [ ] **Step 2**: Find F9 keybind dispatch. Look at how F-keys are mapped:
```bash
grep -n 'F8\|F9\|F10\|SDL_SCANCODE_F9' src/platform/input.cpp src/engine/ -r 2>&1 | head -10
```
Find the existing pattern (likely an `Input::isKeyPressed(SDL_SCANCODE_F8)` check somewhere in engine_update.cpp or engine_menu.cpp).

Add a toggle:
```cpp
    if (Input::isKeyPressed(SDL_SCANCODE_F9)) {
        m_netGraphVisible = !m_netGraphVisible;
    }
```

- [ ] **Step 3**: Render the overlay. Find existing HUD text-rendering helper (likely `drawText` or similar in src/renderer/font.cpp or src/engine/engine_hud.cpp). In the HUD pass, if `m_netGraphVisible && m_netRole == NetRole::CLIENT`:

```cpp
    char buf[256];
    snprintf(buf, sizeof(buf),
        "NET: rtt=%.1fms est=%.0f div=%u loss=%u%% lat=%ums",
        m_clockSync.oneWayTripMs * 2.0f, m_clockSync.serverTickEst,
        m_divergenceCount, m_netFakeLossPct, m_netFakeLatencyMs);
    drawText(buf, 10, 10, ...);   // top-left, white, small font
```

If the existing text-render API is awkward, fall back to a LOG_INFO each frame the overlay is enabled (it'll log to stderr; not as good as on-screen but documents the milestone).

- [ ] **Step 4**: Build, 54/54 tests pass. Commit: `feat(net): F9 net-graph overlay (D6)`.

---

## Verify

- [ ] Clean tree, build, 54/54 tests.

## Definition of Done
- [ ] 54/54 tests pass
- [ ] Fake-latency queue functional (verified by logic test or LOG)
- [ ] F9 toggle exists; net-graph shows on-screen (or logs each frame if rendering is too tangled)
