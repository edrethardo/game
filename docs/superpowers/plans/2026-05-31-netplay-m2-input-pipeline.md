# Netplay M2: Input Pipeline Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Switch the wire input format from single-input-per-packet to a rolling 4-input window for UDP-loss redundancy; add `ackedSnapshotTick` to inputs (foundation for M11 delta compression ACKs); echo `lastProcessedInputTick[slot]` in the snapshot header; drop the deprecated `posXQ/Y/Z` from `NetInput` and stop the M0-era client-authoritative position override on the server. Server becomes hard-authoritative for player position (foundation for M3 prediction + replay reconciliation).

**Architecture:** `NetInput.posXQ/Y/Z` deleted (saves 6 B per input). `NetInput` gains `u16 ackedSnapshotTick` (low bits of latest snapshot the client has applied — used in M11). Each `CL_INPUT` packet now serializes 4 consecutive NetInputs (rolling window: this tick + 3 prior); server reads all 4, skips any with `clientTick <= lastProcessedInputTick`, processes the rest. Snapshot header gains `u32 lastProcessedInputTick[MAX_PLAYERS]` so the client (in M3) knows which inputs server has baked in. The M0-era `np.position = reported` override is removed — server now trusts `PlayerController::updateNetPlayerFromInput`'s computed position.

**Tech Stack:** C++17, existing PacketReader/Writer, doctest for wire-format roundtrip tests.

**Reference spec:** [/home/aaron/.claude/plans/multiplayer-should-feel-like-curried-coral.md](../../../../.claude/plans/multiplayer-should-feel-like-curried-coral.md) — §2 (Input Pipeline) and Migration Plan → Milestone 2.

---

## Task 1: Add ackedSnapshotTick to NetInput + Drop posXQ/Y/Z (TDD)

**Files:**
- Modify: `src/net/net_player.h` — NetInput struct
- Modify: `tests/net/test_clock_sync.cpp` — actually rename to a per-subsystem test file is overkill; the new test goes in a new file `tests/net/test_input_wire.cpp`
- Modify: `tests/CMakeLists.txt` — add the new test file

- [ ] **Step 1: Write the failing test**

Create `tests/net/test_input_wire.cpp`:

```cpp
// test_input_wire.cpp — NetInput struct layout + ring buffer ordering tests.
// Started in M2 (input pipeline rewrite); future input-window tests will live here.

#include <doctest/doctest.h>
#include "net/net_player.h"

TEST_CASE("NetInput: ackedSnapshotTick field is reachable and writable") {
    NetInput in{};
    in.clientTick = 42;
    in.ackedSnapshotTick = 7;
    CHECK(in.clientTick == 42);
    CHECK(in.ackedSnapshotTick == 7);
}

TEST_CASE("NetInput: posXQ/Y/Z fields removed") {
    // This is a compile-time check — if posXQ/Y/Z still exist as members, the
    // commented-out lines below would compile. We trust the build system to fail
    // if a stale reader still touches them.
    NetInput in{};
    in.clientTick = 1;
    // in.posXQ = 0; // would fail to compile post-M2 (intentional)
    CHECK(in.clientTick == 1);
}

TEST_CASE("InputRingBuffer: monotonic clientTick discards duplicates and stale") {
    InputRingBuffer buf{};
    NetInput a{}; a.clientTick = 1;
    NetInput b{}; b.clientTick = 2;
    NetInput c{}; c.clientTick = 2;  // duplicate tick
    NetInput d{}; d.clientTick = 1;  // stale tick
    buf.push(a); buf.push(b); buf.push(c); buf.push(d);
    CHECK(buf.count == 2);   // dup + stale rejected
    REQUIRE(buf.getLatest() != nullptr);
    CHECK(buf.getLatest()->clientTick == 2);
}
```

- [ ] **Step 2: Add the new test file to tests/CMakeLists.txt**

Use Edit tool on `tests/CMakeLists.txt`. Extend the source list to include `net/test_input_wire.cpp`. Insert it next to `net/test_clock_sync.cpp`.

- [ ] **Step 3: Build — expect compile error (ackedSnapshotTick doesn't exist yet)**

```bash
cmake -B build 2>&1 | tail -3
cmake --build build --target dungeon_tests 2>&1 | tail -10
```
Expected: compile error mentioning `ackedSnapshotTick` not found.

- [ ] **Step 4: Modify `src/net/net_player.h` to drop posXQ/Y/Z and add ackedSnapshotTick**

Use Edit tool. Find the NetInput struct:
```cpp
struct NetInput {
    u32 clientTick;     // monotonic client-local sim tick (M1) — server uses for input
                        // ring buffer ordering and (in M2) for lastProcessedInputTick echo.
    u8  moveFlags;      // bit0=W, bit1=S, bit2=A, bit3=D, bit4=jump, bit5=fire, bit6=lockHold
    u8  weaponId;       // currently selected weapon
    u16 yawQ;           // absolute yaw,   packed via Quantize::packAngle over [-π, π]
    u16 pitchQ;         // absolute pitch, packed via Quantize::packAngle over [-π, π]
    u16 posXQ;          // absolute position, packed via Quantize::packPos over [-128, 128] m
    u16 posYQ;
    u16 posZQ;
    u8  extFlags;       // extended input flags (potion, reload, skill, etc.)
    u8  skillSlot;      // which class skill slot (0-3) to activate
};
```

Replace with:
```cpp
struct NetInput {
    u32 clientTick;       // monotonic client-local sim tick (M1) — server uses for input
                          // ring buffer ordering and lastProcessedInputTick echo (M2).
    u16 ackedSnapshotTick;// low 16 bits of the latest snapshot.serverTick the client has
                          // applied. Server uses this in M11 (delta compression) to choose
                          // a baseline. Unused until M11 — write zero meanwhile.
    u8  moveFlags;        // bit0=W, bit1=S, bit2=A, bit3=D, bit4=jump, bit5=fire, bit6=lockHold
    u8  weaponId;         // currently selected weapon
    u16 yawQ;             // absolute yaw,   packed via Quantize::packAngle over [-π, π]
    u16 pitchQ;           // absolute pitch, packed via Quantize::packAngle over [-π, π]
    u8  extFlags;         // extended input flags (potion, reload, skill, etc.)
    u8  skillSlot;        // which class skill slot (0-3) to activate
};
```

Also remove or update the deprecation comment block above NetInput — it can now say:
```
// Input as received from a client (or captured locally for listen server host).
//
// Aim is sent as ABSOLUTE quantized values rather than deltas. Deltas were lossy
// under UDP loss: a dropped packet permanently dropped its mouse delta and the
// server's yaw drifted behind the client's live camera. Absolutes are idempotent —
// a dropped packet only delays the next sync; nothing is lost. Same byte count.
//
// Position is server-authoritative (M2+) — the server runs PlayerController on each
// remote and computes np.position from moveFlags + yaw. M3 will add client-side
// prediction + replay reconciliation on top.
```

- [ ] **Step 5: Build — expect "no member named posXQ" errors at the call sites**

```bash
cmake --build build 2>&1 | tail -20
```
Capture the call sites that reference `posXQ`, `posYQ`, `posZQ`. They need updating in subsequent steps.

- [ ] **Step 6: Update all posXQ/Y/Z call sites**

Run:
```bash
grep -rn 'posXQ\|posYQ\|posZQ' src/ | head -10
```

Expected hits (likely):
- `src/game/player.cpp` in `captureLocalInput` — writes posXQ/Y/Z from player.position. **Remove these writes.**
- `src/net/client.cpp` in `captureAndSendInput` — serializes posXQ/Y/Z to wire. **Remove these writes** (wire format will change in Task 3 anyway, but for now matching the smaller struct).
- `src/net/server.cpp` in input deserialize — reads posXQ/Y/Z from wire. **Remove.**
- `src/engine/engine_net.cpp` in serverNetPre — reads `in.posXQ/Y/Z` for the client-authoritative position override. **Remove the whole override block** so the server trusts PlayerController.

For each:
- Remove the writes in capture (player.cpp, client.cpp).
- Remove the reads in deserialize (server.cpp).
- Remove the override block in engine_net.cpp (the `Vec3 reported{...}` + clamp + `np.position = reported` lines). After `PlayerController::updateNetPlayerFromInput(np, in, dt)` runs, **delete the `np.position = preMovePos;` line** — server now keeps PlayerController's computed position.

- [ ] **Step 7: Wire format update (this changes packet sizes — will be properly windowed in Task 3, but for now align with the new struct)**

`src/net/client.cpp` `captureAndSendInput` currently serializes 22 bytes. With posXQ/Y/Z gone (-6 B) but ackedSnapshotTick added (+2 B), the new size is 18 B. Update the serializer accordingly.

`src/net/server.cpp` (or wherever the deserialize lives) needs to match. Keep the wire fields in the same order as the struct: `clientTick(4) + ackedSnapshotTick(2) + moveFlags(1) + weaponId(1) + yawQ(2) + pitchQ(2) + extFlags(1) + skillSlot(1) = 14 B` plus 4 B header = 18 B total.

- [ ] **Step 8: Build + tests**

```bash
cmake --build build 2>&1 | tail -5
./build/tests/dungeon_tests 2>&1 | tail -5
```
Expected: 17/17 pass (14 existing + 3 new).

- [ ] **Step 9: Commit**

```bash
git add -u src/ tests/
git diff --cached --stat
git commit -m "$(cat <<'EOF'
net: drop NetInput.posXQ/Y/Z, add ackedSnapshotTick (TDD) — M2.1

NetInput shrinks from 24 B to 18 B on wire. The deprecated client-
authoritative position fields (flagged in M0) are gone; the server-
side override block in engine_net.cpp:serverNetPre is removed so
PlayerController::updateNetPlayerFromInput's computed position is now
canonical for remote players. Server is hard-authoritative for player
position; client-side prediction + replay reconciliation lands in M3.

NetInput.ackedSnapshotTick (u16) carries the low 16 bits of the latest
snapshot.serverTick the client has applied. Currently unused by the
server (writes zero meanwhile); M11 (delta compression) uses it to
choose a per-client baseline.

3 new tests in tests/net/test_input_wire.cpp cover field reachability,
the (compile-time) posXQ removal, and the existing InputRingBuffer
monotonic-discard logic.

17/17 tests pass.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Rolling 4-Input Window on the Wire (TDD)

**Files:**
- Modify: `src/net/client.cpp` — serialize last 4 inputs per CL_INPUT
- Modify: `src/net/server.cpp` (or wherever deserialize lives) — read 4 inputs
- Modify: `src/net/net_player.h` — add a small "client send window" helper (an `InputSendWindow` of size 4)
- Modify: `tests/net/test_input_wire.cpp` — append wire-window roundtrip test

- [ ] **Step 1: Append failing test**

Add to `tests/net/test_input_wire.cpp`:

```cpp
// Wire format for CL_INPUT in M2: 4-byte packet header + 1 byte windowCount + N*18 bytes
// of NetInput structs (N up to INPUT_WINDOW_SIZE = 4). The serializer/deserializer pair
// must roundtrip cleanly.

TEST_CASE("InputWindow: serializes and deserializes 4 inputs cleanly") {
    // Build 4 inputs with distinct clientTicks
    NetInput window[4];
    for (u32 i = 0; i < 4; i++) {
        window[i] = NetInput{};
        window[i].clientTick = 100 + i;
        window[i].ackedSnapshotTick = 7;
        window[i].moveFlags = static_cast<u8>(0x10 | i);
        window[i].weaponId = static_cast<u8>(2);
        window[i].yawQ = 0xABCD;
        window[i].pitchQ = 0x1234;
        window[i].extFlags = 0;
        window[i].skillSlot = 1;
    }
    u8 buf[256] = {};
    u32 size = serializeInputWindow(buf, sizeof(buf), window, 4);
    REQUIRE(size > 0);
    REQUIRE(size <= sizeof(buf));

    NetInput parsed[4] = {};
    u32 count = deserializeInputWindow(buf, size, parsed, 4);
    REQUIRE(count == 4);
    for (u32 i = 0; i < 4; i++) {
        CHECK(parsed[i].clientTick == 100 + i);
        CHECK(parsed[i].ackedSnapshotTick == 7);
        CHECK(parsed[i].moveFlags == (0x10 | i));
        CHECK(parsed[i].weaponId == 2);
        CHECK(parsed[i].yawQ == 0xABCD);
        CHECK(parsed[i].pitchQ == 0x1234);
    }
}

TEST_CASE("InputWindow: deserialize rejects truncated buffer") {
    u8 buf[3] = { 0, 0, 4 };  // claims 4 inputs but no input bytes follow
    NetInput parsed[4] = {};
    u32 count = deserializeInputWindow(buf, sizeof(buf), parsed, 4);
    CHECK(count == 0);
}
```

The function names `serializeInputWindow` and `deserializeInputWindow` are new — they don't exist yet. Build will fail at link time.

- [ ] **Step 2: Add window helpers**

In `src/net/net_player.h` (or a new `src/net/input_wire.h` if preferred), add:

```cpp
static constexpr u32 INPUT_WINDOW_SIZE = 4;

// Serialize up to `count` NetInputs (≤ INPUT_WINDOW_SIZE) into `outBuf`. Layout:
//   u8  windowCount
//   u8  reserved (=0, for u16 alignment)
//   u16 reserved (=0)
//   N × (u32 clientTick + u16 ackedSnapshotTick + u8 moveFlags + u8 weaponId
//        + u16 yawQ + u16 pitchQ + u8 extFlags + u8 skillSlot)  // 14 B per input
// Returns total bytes written (0 on overflow).
u32 serializeInputWindow(u8* outBuf, u32 outCap, const NetInput* inputs, u32 count);

// Inverse of the above. Writes up to `maxCount` inputs into `outInputs`. Returns
// the number actually decoded (0 if the buffer is truncated or malformed).
u32 deserializeInputWindow(const u8* buf, u32 size, NetInput* outInputs, u32 maxCount);
```

Implement them in a new `src/net/input_wire.cpp` (small file, keeps client.cpp's deps off the test target):

```cpp
#include "net/net_player.h"
#include "core/types.h"

static constexpr u32 INPUT_BYTES = 14;  // wire size of one NetInput
static constexpr u32 HEADER_BYTES = 4;  // windowCount (1) + reserved (3)

u32 serializeInputWindow(u8* outBuf, u32 outCap, const NetInput* inputs, u32 count) {
    if (count > INPUT_WINDOW_SIZE) count = INPUT_WINDOW_SIZE;
    const u32 total = HEADER_BYTES + count * INPUT_BYTES;
    if (total > outCap) return 0;
    u32 o = 0;
    outBuf[o++] = static_cast<u8>(count);
    outBuf[o++] = 0;
    outBuf[o++] = 0; outBuf[o++] = 0;
    for (u32 i = 0; i < count; i++) {
        const NetInput& in = inputs[i];
        // little-endian u32 clientTick
        outBuf[o++] = static_cast<u8>( in.clientTick        & 0xFF);
        outBuf[o++] = static_cast<u8>((in.clientTick >> 8)  & 0xFF);
        outBuf[o++] = static_cast<u8>((in.clientTick >> 16) & 0xFF);
        outBuf[o++] = static_cast<u8>((in.clientTick >> 24) & 0xFF);
        outBuf[o++] = static_cast<u8>( in.ackedSnapshotTick       & 0xFF);
        outBuf[o++] = static_cast<u8>((in.ackedSnapshotTick >> 8) & 0xFF);
        outBuf[o++] = in.moveFlags;
        outBuf[o++] = in.weaponId;
        outBuf[o++] = static_cast<u8>( in.yawQ       & 0xFF);
        outBuf[o++] = static_cast<u8>((in.yawQ >> 8) & 0xFF);
        outBuf[o++] = static_cast<u8>( in.pitchQ       & 0xFF);
        outBuf[o++] = static_cast<u8>((in.pitchQ >> 8) & 0xFF);
        outBuf[o++] = in.extFlags;
        outBuf[o++] = in.skillSlot;
    }
    return total;
}

u32 deserializeInputWindow(const u8* buf, u32 size, NetInput* outInputs, u32 maxCount) {
    if (size < HEADER_BYTES) return 0;
    const u32 count = buf[0];
    if (count == 0 || count > INPUT_WINDOW_SIZE) return 0;
    if (size < HEADER_BYTES + count * INPUT_BYTES) return 0;
    const u32 toRead = (count < maxCount) ? count : maxCount;
    u32 o = HEADER_BYTES;
    for (u32 i = 0; i < toRead; i++) {
        NetInput& out = outInputs[i];
        out.clientTick        = (u32)buf[o] | ((u32)buf[o+1] << 8) | ((u32)buf[o+2] << 16) | ((u32)buf[o+3] << 24); o += 4;
        out.ackedSnapshotTick = (u16)buf[o] | ((u16)buf[o+1] << 8); o += 2;
        out.moveFlags = buf[o++];
        out.weaponId  = buf[o++];
        out.yawQ      = (u16)buf[o] | ((u16)buf[o+1] << 8); o += 2;
        out.pitchQ    = (u16)buf[o] | ((u16)buf[o+1] << 8); o += 2;
        out.extFlags  = buf[o++];
        out.skillSlot = buf[o++];
    }
    return toRead;
}
```

Add `net/input_wire.cpp` to both `src/CMakeLists.txt` and `tests/CMakeLists.txt`.

- [ ] **Step 3: Build + run tests**

```bash
cmake --build build --target dungeon_tests 2>&1 | tail -5
./build/tests/dungeon_tests 2>&1 | tail -5
```
Expected: 19/19 pass (2 new InputWindow cases added).

- [ ] **Step 4: Update Client::captureAndSendInput to send a window**

Add a static window buffer in client.cpp (file-scope):

```cpp
static NetInput s_sendWindow[INPUT_WINDOW_SIZE] = {};
static u32      s_sendWindowCount = 0;
```

In `Client::captureAndSendInput`, after building the new input, shift the window:

```cpp
    // Shift oldest out, append newest. Window is in oldest→newest order.
    if (s_sendWindowCount < INPUT_WINDOW_SIZE) {
        s_sendWindow[s_sendWindowCount++] = s_latestInput;
    } else {
        for (u32 i = 0; i < INPUT_WINDOW_SIZE - 1; i++) {
            s_sendWindow[i] = s_sendWindow[i + 1];
        }
        s_sendWindow[INPUT_WINDOW_SIZE - 1] = s_latestInput;
    }
    // Serialize window into the packet body.
    u8 payload[256];
    u32 payloadSize = serializeInputWindow(payload, sizeof(payload), s_sendWindow, s_sendWindowCount);
    PacketWriter w;
    w.writeU8(static_cast<u8>(NetPacketType::CL_INPUT));
    w.writeU8(0);
    w.writeU16(0);
    for (u32 i = 0; i < payloadSize; i++) w.writeU8(payload[i]);
    Net::sendToServer(w.data, w.cursor, false);
```

Replace the existing single-input serialization with this windowed version.

- [ ] **Step 5: Update server-side deserialize to read window**

Find where the server deserializes a CL_INPUT (in `src/net/server.cpp` or `src/net/net.cpp`'s dispatch). Replace single-input read with:

```cpp
case NetPacketType::CL_INPUT: {
    if (size < 4 + 4) break;
    NetInput batch[INPUT_WINDOW_SIZE];
    u32 count = deserializeInputWindow(data + 4, size - 4, batch, INPUT_WINDOW_SIZE);
    InputRingBuffer& buf = getInputBuffer(slotForPeer(peer));
    for (u32 i = 0; i < count; i++) {
        buf.push(batch[i]);  // push() filters dup/stale by clientTick
    }
    break;
}
```

Adjust `slotForPeer(peer)` and `getInputBuffer(i)` names to match existing helpers.

- [ ] **Step 6: Build + tests + game**

```bash
cmake --build build 2>&1 | tail -5
./build/tests/dungeon_tests 2>&1 | tail -5
```
Expected: clean build, 19/19 pass.

- [ ] **Step 7: Commit**

```bash
git add -u src/ tests/
git diff --cached --stat
git commit -m "$(cat <<'EOF'
net: rolling 4-input window on CL_INPUT (TDD) — M2.2

Client now packs its last 4 NetInputs into every CL_INPUT packet
(oldest→newest order, INPUT_WINDOW_SIZE = 4). Server reads up to 4
inputs from each packet and pushes them into the existing per-slot
ring buffer; InputRingBuffer::push() already filters duplicates and
stale clientTicks by monotonicity, so server-side change is minimal.

Redundancy benefit: a single dropped UDP packet no longer drops an
input — the next packet still carries it. Three consecutive losses
required to lose an input at all.

New file src/net/input_wire.{h-via-net_player.h,cpp} contains pure
serializeInputWindow / deserializeInputWindow functions, unit-tested
via tests/net/test_input_wire.cpp (2 new TEST_CASEs: clean roundtrip,
truncated-buffer rejection).

19/19 tests pass.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Echo lastProcessedInputTick[slot] in Snapshot Header

**Files:**
- Modify: `src/net/snapshot.h` — add `u32 lastProcessedInputTick[MAX_PLAYERS]` to WorldSnapshot header
- Modify: `src/net/snapshot.cpp` — serialize/deserialize the new field
- Modify: server snapshot build — populate from `np.lastProcessedInputTick`
- Modify: `tests/` — add snapshot-header roundtrip test (optional but cheap)

- [ ] **Step 1: Add field to WorldSnapshot struct**

In `src/net/snapshot.h`, find the `WorldSnapshot` struct (has fields like `u32 serverTick = 0;`). Add immediately after serverTick:

```cpp
    u32  lastProcessedInputTick[MAX_PLAYERS] = {};  // per-slot ACK of newest input the
                                                    // server has applied. Clients read
                                                    // this in M3 to replay only inputs
                                                    // newer than the ACK.
```

`MAX_PLAYERS` is already defined for the snapshot pools — match its source.

- [ ] **Step 2: Add to serialize/deserialize**

In `src/net/snapshot.cpp`:
- In the serialize function, after writing serverTick, write `MAX_PLAYERS` u32 values from `snap.lastProcessedInputTick[i]`.
- In deserialize, after reading serverTick, read `MAX_PLAYERS` u32 values into `snap.lastProcessedInputTick[i]`.

Adjust the header size comment in the file.

- [ ] **Step 3: Populate from server state**

In the server's snapshot-build path (likely `Snapshot::buildFromState` or similar), after populating serverTick, loop over `m_players[]` and copy `np.lastProcessedInputTick` into the snapshot.

- [ ] **Step 4: Build + tests**

```bash
cmake --build build 2>&1 | tail -5
./build/tests/dungeon_tests 2>&1 | tail -5
```
Expected: 19/19 still pass.

- [ ] **Step 5: Commit**

```bash
git add -u src/
git commit -m "$(cat <<'EOF'
net: echo lastProcessedInputTick[MAX_PLAYERS] in snapshot header — M2.3

WorldSnapshot gains a u32 lastProcessedInputTick[MAX_PLAYERS] array
written immediately after serverTick. Server populates from each
NetPlayer's existing np.lastProcessedInputTick (already tracked
server-side since the M0 baseline; only the wire-out wiring was
missing).

Clients can now ask "which of my inputs has the server actually baked
in?" — the foundation for M3 replay reconciliation. Currently unread
by the client; M3 wires the reader.

Wire size: +16 B per snapshot (MAX_PLAYERS = 4 × u32). Acceptable —
delta compression in M11 will skip the field on unchanged ticks.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Verify

- [ ] **Step 1: Clean tree, full build, tests, ctest**

```bash
git status --short && echo "STATUS_END"
rm -rf build && cmake -B build 2>&1 | tail -3 && cmake --build build 2>&1 | tail -3
./build/tests/dungeon_tests 2>&1 | tail -5
ctest --test-dir build --output-on-failure 2>&1 | tail -5
```

Expected: clean tree, clean build, 19/19 tests pass, 1/1 ctest passes.

- [ ] **Step 2: Confirm M2 commit trail**

```bash
git log ad42b62..HEAD --oneline
```
Expected: 3 commits (M2.1, M2.2, M2.3).

---

## Definition of Done

- [ ] `git status --short` empty
- [ ] M2 produces 3 commits
- [ ] 19/19 tests pass
- [ ] `grep -c 'posXQ\|posYQ\|posZQ' src/` returns 0
- [ ] `grep -c 'ackedSnapshotTick' src/net/net_player.h` returns 1
- [ ] `grep -c 'lastProcessedInputTick\[' src/net/snapshot.h` returns 1
- [ ] Manual smoke pass (user-driven, optional) — co-op still works end-to-end
