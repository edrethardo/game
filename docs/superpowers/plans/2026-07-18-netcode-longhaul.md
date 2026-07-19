# Netcode Long-Haul (Germany↔New Zealand) — Implementation Plan

> **For agentic workers:** subagent-driven, per-task commits on branch `feat/netcode-longhaul`.
> Steps use checkbox syntax.

**Goal:** Make the game playable across a ~150 ms one-way / ~300 ms RTT link with jitter and light
loss (Germany↔New Zealand), for BOTH co-op PvE and arena PvP.

**Approach (measure-first + core fix, user-approved scope):** The netplay stack is already strong —
trust-client position (your own movement never rubber-bands regardless of RTT), an adaptive jitter
buffer, 8-input-window redundancy, lag comp anchored to the acked snapshot, reconcile smoothing,
delta compression, input coasting. The DE↔NZ problems are (a) we can't reproduce the condition
locally because the soak rig adds only *constant* latency — no jitter, so the adaptive buffer never
engages — and (b) one constant, `MAX_INTERP_DELAY_MS = 150`, caps the jitter buffer AND the
server-side lag-comp rewind, clipping on long-haul jitter spikes → remote players/enemies stutter.
Fix: add a jitter knob to the rig, then raise that one ceiling (it lifts the jitter buffer, the
movement rewind, and PvE+PvP hit-reg together — all three read it via `LagComp::rewindTicks`).

**Investigation facts (verified):**
- Fake latency: single choke `enqueueDelayed` (`net.cpp:165`), applied to every send both directions.
- `MAX_INTERP_DELAY_MS`/`DEFAULT_INTERP_DELAY_MS` live in `lag_comp.h`; `client.h`/`client.cpp` derive
  the client's applied delay from them, so client-applied delay and server-trusted rewind can't drift.
- Fire/hit rewind (`engine_combat.cpp:1048-1051`) and movement rewind (`engine_net.cpp:199,261`) BOTH
  use the wire-stamped `in.interpDelayMs` via `LagComp::rewindTicks`/`targetTick` — so raising the cap
  serves PvE and PvP hit-reg. Server pose history `LAG_COMP_HISTORY_TICKS = 64` (1067 ms).
- Client snapshot ring `SNAP_BUFFER_SIZE = 32` (533 ms). At 250 ms interp = 15 ticks behind newest;
  oldest sample is 31 ticks behind → 16 ticks (267 ms) of bracketing headroom. Sufficient, unchanged.
- `u8` holds 250 (< 255). Trust-client reconcile only snaps on >5 m teleports — untouched.

**Tech stack:** C++17, doctest (`tests/net/`), CMake. No new deps. No wire/save format change (these
are local cvars + a client-derived delay already carried on the wire in the existing `interpDelayMs`
byte — no new field, no `PROTOCOL_VERSION` bump).

---

### Task 1: `--net-jitter <ms>` soak-rig knob (Phase 0 — makes DE↔NZ measurable)

**Files:**
- Modify: `src/net/net.h` (setter decl, after `setFakeLatencyMs` ~line 566)
- Modify: `src/net/net.cpp` (cvar + setter + apply in `enqueueDelayed`)
- Modify: `src/engine/launch_options.h` (`netJitterMs` field, after `netLatencyMs` line 60)
- Modify: `src/engine/launch_options.cpp` (parse + help)
- Modify: `src/engine/engine_launch.cpp` (member copy + log)
- Modify: `src/engine/engine_net.cpp` (push cvar each frame beside `setFakeLatencyMs`, lines 74 & 1087)
- Modify: `src/engine/engine.h` (`m_netFakeJitterMs` member beside `m_netFakeLatencyMs`)

No unit test (rand-based cvar + CLI, mirrors `--net-latency` exactly); verified by build + a headless
`--help`/parse smoke and the startup log line.

- [ ] **Step 1: net.cpp — cvar + apply jitter in the delay-queue enqueue.**

After `static u32 s_fakeLatencyMs = 0;` (net.cpp ~line 106) add:

```cpp
// Long-haul rig: fake JITTER — set via Net::setFakeJitterMs(). Adds a per-packet random delay in
// [0, s_fakeJitterMs] ms ON TOP of the latency floor, so inter-arrival time varies the way a real
// long-haul link does (and, because the added delay varies per packet, packets naturally REORDER).
// This is what actually exercises the client's adaptive jitter buffer — constant latency leaves it
// at its floor. Applied at the same single choke as the latency (enqueueDelayed).
static u32 s_fakeJitterMs = 0;
```

In `enqueueDelayed`, replace the `deliverAtSec` line (net.cpp:165):

```cpp
    // Latency is the floor; jitter is a per-packet [0, s_fakeJitterMs] ms spread on top (rand() like
    // shouldDropPacket). Varying it per packet reproduces long-haul reordering — the delivery pump
    // drains by deliverAt regardless of enqueue order, so an earlier-scheduled packet can overtake.
    const u32 jitterMs = (s_fakeJitterMs > 0) ? static_cast<u32>(rand() % (s_fakeJitterMs + 1)) : 0u;
    e.deliverAtSec = Clock::getElapsedSeconds() + (s_fakeLatencyMs + jitterMs) * 0.001;
```

At the bottom of net.cpp, after `void Net::setFakeLatencyMs(u32 ms) { s_fakeLatencyMs = ms; }`
(line 1534) add:

```cpp
void Net::setFakeJitterMs(u32 ms) { s_fakeJitterMs = ms; }
```

- [ ] **Step 2: net.h — declare the setter** after the `setFakeLatencyMs` declaration (~line 566):

```cpp
    // Long-haul rig: fake jitter cvar. Per-packet random [0, ms] delay added on top of the fake
    // latency (net.cpp), so the client's adaptive interp buffer is actually exercised. Pushed each
    // frame from serverNetPre/clientNetPre like the latency/loss cvars.
    void setFakeJitterMs(u32 ms);
```

- [ ] **Step 3: launch_options.h — field** after `netLatencyMs` (line 60):

```cpp
    u32 netJitterMs  = 0;                      // --net-jitter <0-500>: per-packet [0,ms] jitter on top of latency
```

- [ ] **Step 4: launch_options.cpp — parse + help.** In the help block after the `--net-latency`
line (line 68) add:

```cpp
    LOG_INFO("  --net-jitter <ms>      add per-packet [0,ms] jitter on top of latency (0-500)");
```

In the arg loop, after the `--net-latency` `else if` block (ends ~line 169) add:

```cpp
        } else if (ieq(a, "--net-jitter")) {
            const char* v = (i + 1 < argc) ? argv[++i] : "";
            long n = parseLong(v, -1);
            if (n < 0 || n > 500) {
                LOG_WARN("--net-jitter expects 0-500 ms (got '%s')", v); opt.valid = false; break;
            }
            opt.netJitterMs = (u32)n;       // adversity harness — not a game-jump directive
```

(Use the SAME numeric-parse helper the `--net-latency` branch uses — read that branch and mirror it;
if it uses an inline `atoi`/`strtol`, mirror that exactly instead of `parseLong`.)

- [ ] **Step 5: engine.h — member** beside `m_netFakeLatencyMs` (grep it):

```cpp
    u32 m_netFakeJitterMs = 0;   // --net-jitter: pushed into Net:: each frame (serverNetPre/clientNetPre)
```

- [ ] **Step 6: engine_launch.cpp — copy + log.** After `m_netFakeLatencyMs = opt.netLatencyMs;`
(line 67):

```cpp
    m_netFakeJitterMs  = opt.netJitterMs;
```

Extend the adversity log condition + message (lines 68-71) to include jitter:

```cpp
    if (opt.netLossPct > 0 || opt.netLatencyMs > 0 || opt.netJitterMs > 0)
        LOG_INFO("Launch: NET ADVERSITY ON — %u%% loss, +%ums one-way latency, +/-%ums jitter (net-graph: F9)",
                 (u32)opt.netLossPct, opt.netLatencyMs, opt.netJitterMs);
```

- [ ] **Step 7: engine_net.cpp — push the cvar each frame.** Beside BOTH `Net::setFakeLatencyMs(m_netFakeLatencyMs);`
call sites (line 74 in serverNetPre, line 1087 in clientNetPre) add on the next line:

```cpp
    Net::setFakeJitterMs(m_netFakeJitterMs);
```

- [ ] **Step 8: Build + smoke.**

```bash
cmake --build build 2>&1 | tail -5
./build/tests/dungeon_tests 2>&1 | tail -2      # unchanged count, still green
./build/dungeon_game --help 2>&1 | grep -i jitter   # help line present
```
Expected: clean build, tests green, help shows `--net-jitter`. (If `--help` isn't a supported flag,
skip that line — the build + the parse mirroring is the gate.)

- [ ] **Step 9: Commit.**

```bash
git add src/net/net.h src/net/net.cpp src/engine/launch_options.h src/engine/launch_options.cpp \
        src/engine/engine_launch.cpp src/engine/engine_net.cpp src/engine/engine.h
git commit -m "feat(net): --net-jitter soak-rig knob — per-packet jitter to exercise the adaptive interp buffer

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Raise the interp/rewind ceiling for long-haul (Phase 1 — the core fix)

**Files:**
- Modify: `src/net/lag_comp.h` (`MAX_INTERP_DELAY_MS` 150→250 + comment)
- Test: `tests/net/test_lag_comp.cpp` (pin the new ceiling), `tests/net/test_interp_delay.cpp`
  (adaptive curve reaches the deeper target)

The ONLY production change is the one constant. Everything downstream (client applied delay via
`client.cpp`'s `MAX_INTERP_DELAY_SEC`, the server movement + fire rewind via `LagComp::rewindTicks`)
derives from it, so the single edit lifts all three ceilings in lockstep. `SNAP_BUFFER_SIZE` (32 =
533 ms) and `LAG_COMP_HISTORY_TICKS` (64 = 1067 ms) both have headroom for a 250 ms (15-tick) rewind
plus half-RTT — verified in the plan header; NO change needed, but the test asserts the arithmetic.

- [ ] **Step 1: Write/extend the failing tests.**

In `tests/net/test_lag_comp.cpp` (READ it first for the fixture/idiom), add:

```cpp
TEST_CASE("LagComp: interp ceiling raised to 250 ms for long-haul (DE<->NZ)") {
    // The cap is the trust ceiling AND the jitter-buffer max. 200 ms (a real DE<->NZ buffer under
    // jitter) must now pass through, where the old 150 ms cap clamped it.
    CHECK(LagComp::sanitize(200) == 200);
    CHECK(LagComp::sanitize(250) == 250);
    CHECK(LagComp::sanitize(255) == 250);   // above the cap clamps to the cap
    CHECK(LagComp::sanitize(0)   == LagComp::DEFAULT_INTERP_DELAY_MS);  // absent -> baseline
    CHECK(LagComp::toWireMs(0.25f) == 250);  // 250 ms round-trips through the wire byte
    CHECK(LagComp::toWireMs(0.30f) == 250);  // clamped

    // rewindTicks is fractional: 250 ms * 0.06 ticks/ms = 15 ticks. Must fit the server's
    // 64-tick pose history even stacked on a 300 ms-RTT half-RTT (9 ticks): 15 + 9 = 24 < 64.
    CHECK(LagComp::rewindTicks(250) == doctest::Approx(15.0f));
    CHECK(15.0f + 9.0f < 64.0f);
}
```

In `tests/net/test_interp_delay.cpp` (READ it first), add:

```cpp
TEST_CASE("InterpDelay: adaptive buffer can now widen past the old 150 ms cap") {
    const f32 base = 0.033f;   // 33 ms
    const f32 maxd = 0.250f;   // NEW long-haul cap (was 0.150)
    // Sustained high arrival jitter (~90 ms smoothed) drives the target above the OLD cap.
    f32 delay = base;
    for (int i = 0; i < 400; i++)
        delay = computeInterpDelay(delay, 0.090f, base, maxd);
    CHECK(delay > 0.150f);                    // would have been pinned at 0.150 before
    CHECK(delay <= 0.250f + 1e-4f);           // never exceeds the new cap
    // And it still floors at base when the link is calm.
    f32 calm = 0.100f;
    for (int i = 0; i < 400; i++) calm = computeInterpDelay(calm, 0.0f, base, maxd);
    CHECK(calm == doctest::Approx(base));
}
```

- [ ] **Step 2: Run to verify failure.**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*interp ceiling*,*widen past*"
```
Expected: FAIL — `sanitize(200)` returns 150, `toWireMs(0.25)` returns 150, and the interp-delay case
tops out at 0.150.

- [ ] **Step 3: Raise the ceiling.** In `src/net/lag_comp.h`, change `MAX_INTERP_DELAY_MS` (line 51)
from `150` to `250` and replace its comment with:

```cpp
// Upper bound on how wide the client's jitter buffer may grow — and the server's rewind trust
// ceiling. Raised 150 -> 250 ms for long-haul play (Germany<->New Zealand, ~150 ms one-way + jitter):
// the adaptive buffer must be allowed to ride out jitter spikes past 150 ms or remote players/enemies
// stutter. This is ALSO the max the server will rewind for a client's lag-comp (movement + fire), so
// a malicious client can claim at most 250 ms of rewind — bounded, and legitimate at this distance.
// Fits the 64-tick server pose history (250 ms = 15 ticks, + half-RTT ~9 ticks << 64) and the 32-slot
// client snapshot ring (533 ms, render time sits 15 ticks behind the 31-tick-deep oldest sample).
inline constexpr u8 MAX_INTERP_DELAY_MS = 250;
```

- [ ] **Step 4: Run to verify pass.**

```bash
cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*interp ceiling*,*widen past*"
```
Expected: PASS. Then the FULL suite (existing clock/lag/interp tests must stay green): `./build/tests/dungeon_tests`.

- [ ] **Step 5: Commit.**

```bash
git add src/net/lag_comp.h tests/net/test_lag_comp.cpp tests/net/test_interp_delay.cpp
git commit -m "feat(net): raise interp/lag-comp ceiling 150->250ms for long-haul (jitter buffer + rewind)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Validate + document (Phase 1 close-out)

**Files:**
- Modify: `CLAUDE.md` (netplay stack paragraph — jitter rig + long-haul envelope)
- Modify: `.claude/skills/engine-reference/SKILL.md` (soak-rig + interp-cap facts)
- Create: `/home/aaron/.claude/projects/-home-aaron-game/memory/project_netcode_soak_rig.md` UPDATE
  (the existing soak-rig memory — add `--net-jitter` + the DE↔NZ envelope + new interp cap)

Full interactive two-process soak needs a display, so the *smoothness* proof is the user's playtest
(like the arena). This task locks the deterministic gains (unit tests), records the validation recipe,
and syncs docs. Do NOT claim a measured smoothness result that wasn't run.

- [ ] **Step 1: Deterministic verification.**

```bash
cmake --build build && ctest --test-dir build --output-on-failure 2>&1 | tail -5
```
Expected: full suite green (previous count + the 2 new cases). If any fail, STOP / report BLOCKED.

Attempt a headless rig smoke (best-effort — may need a display; if it can't open a window, note that
and rely on the build + parse):

```bash
timeout 6 ./build/dungeon_game --host --new sorcerer --net-latency 150 --net-jitter 30 --net-loss 2 --bot-walk 2>&1 | grep -iE "NET ADVERSITY|jitter" | head
```
Expected (if it runs): the startup log shows `NET ADVERSITY ON — 2% loss, +150ms one-way latency,
+/-30ms jitter`. Record the actual output (or "headless: no display, not run").

- [ ] **Step 2: CLAUDE.md — netplay paragraph.** Find the "Netplay stack" paragraph and the sentence
listing the verification rig (`--net-loss <0-90> --net-latency <ms> --bot-walk` + F9 net-graph).
Add `--net-jitter <ms>` to that rig list, and append one sentence:

> For long-haul links (e.g. Germany↔New Zealand, ~150 ms one-way + jitter) the adaptive interp buffer
> and the server lag-comp rewind share one ceiling, `LagComp::MAX_INTERP_DELAY_MS` (250 ms) — raised
> from 150 so the jitter buffer can ride out spikes without stuttering remotes; `--net-jitter`
> reproduces the condition locally (constant `--net-latency` alone never engages the adaptive buffer).

- [ ] **Step 3: engine-reference — soak-rig + cap facts.** In the networking/soak-rig area, add
`--net-jitter <0-500>` beside the other rig flags and note `MAX_INTERP_DELAY_MS` is now 250 (was 150),
governing the jitter buffer AND the movement/fire lag-comp rewind ceiling (both read
`LagComp::rewindTicks(in.interpDelayMs)`), with headroom vs the 64-tick pose history + 32-slot ring.

- [ ] **Step 4: Update the soak-rig memory.** READ
`/home/aaron/.claude/projects/-home-aaron-game/memory/project_netcode_soak_rig.md`, add the
`--net-jitter <ms>` flag, the DE↔NZ envelope (`--net-latency 150 --net-jitter 30 --net-loss 2
--bot-walk`), and the note that the interp/rewind cap is now 250 ms. Keep the existing reference
numbers; append, don't rewrite. Update `MEMORY.md`'s one-line pointer only if the hook changed.

- [ ] **Step 5: Commit.**

```bash
git add CLAUDE.md .claude/skills/engine-reference/SKILL.md docs/superpowers/plans/2026-07-18-netcode-longhaul.md
git commit -m "docs: --net-jitter rig + long-haul interp/rewind ceiling — CLAUDE.md, engine-reference, plan

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```
(The memory file is outside the repo — Write it, it is not part of the git commit.)

---

## Validation recipe for the user (hand off after Task 3)

Local reproduction of the DE↔NZ condition, one machine, two processes:
```bash
./build/dungeon_game --host --new sorcerer --net-latency 150 --net-jitter 30 --net-loss 2 --bot-walk &
./build/dungeon_game --join 127.0.0.1 --net-latency 150 --net-jitter 30 --net-loss 2
```
Press **F9** for the net-graph. Watch the 1 Hz `[NET-GRAPH]` line: `idelay` (interp delay) should now
widen past 150 ms toward ~250 ms under the jitter instead of clamping, and remote-player motion should
stay smooth (no freeze/stutter) with 0 hard snaps. Then the real test: host with your NZ friend.

## Parked (out of this scope, from the findings)
- Continuous clock OWT refinement (frozen after the 3-pong bootstrap) — long-session stability.
- 30 Hz snapshot decoupling + RTT-scaled coast/input-window — cut packet count/loss exposure.
These are the "full long-haul treatment" the user deferred; revisit if the playtest shows residual
issues the cap raise didn't cover.
