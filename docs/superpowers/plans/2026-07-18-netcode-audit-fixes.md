# Netcode Audit Fixes (DE↔NZ long-haul, round 2) — Implementation Plan

> Subagent-driven on branch `feat/netcode-longhaul`, per-task commits authorized.
> Source: a measured 100 s soak at the DE↔NZ envelope (150 ms + ±30 ms jitter + 2% loss both
> ways, rtt=383 ms measured) plus three adversarial code audits (timing / hit-reg / bandwidth).

**Measured baseline (pre-fix soak):** 0 hard snaps; idelay 56–69 ms (healthy); div ~14/100 s all
≤0.33 m, none near enemies; BUT server delta baseline age 21–38 ticks vs a 32-deep ring →
delta=75–88% (12–25% full-snapshot fallback) → ~19–21 KB/s instead of ~9.

## Confirmed findings → fixes (all verified against code by independent auditors)

| # | Finding | Fix | Files |
|---|---|---|---|
| 1 | Burst outages pollute the jitter EMA (a 380 ms gap read as "jitter" inflates interp delay 50–150 ms for hundreds of ms after the outage ends) | Clamp the inter-arrival delta fed to the EMA at 3× nominal (an outage is not jitter) — pure, tested | `net/interp_delay.h`, `tests/net/test_interp_delay.cpp` |
| 2 | Input redundancy window (8 = 133 ms) < coast (250 ms): ticks 9–15 of any burst are unrecoverable → coast-guess-then-truth rubber-band | `INPUT_WINDOW_SIZE` 8 → 15 (window ≈ coast); wire-range change ⇒ `PROTOCOL_VERSION` 21 → 22 | `net/net_player.h`, `net/net.h`, `tests/net/test_input_wire.cpp` |
| 3 | Server delta-baseline ring (32 ticks = 533 ms) too shallow at 300+ ms RTT — measured 12–25% full-snap fallback; client ring must match or deltas name evicted baselines | `SNAP_HISTORY_DEPTH` and `SNAP_BUFFER_SIZE` 32 → 64 (memory-only, ~1 MB/side) | `engine/engine.h`, `net/client.h` |
| 4 | PvE fire rewind clamp 15 ticks < RTT/2+interp (18–24) exactly when jitter widens the buffer → 50–150 ms uncompensated vs moving enemies; 1 m fire-origin clamp < 1.5 m legit displacement at run speed × RTT/2 | `LAG_COMP_MAX_REWIND_TICKS` 15 → 24 (history is 64); origin clamp 1 m → 2 m | `engine/engine_combat.cpp` |
| 5 | **No PvP player-pose lag comp at all** — every `Combat::pvp*` query reads the victim's present-tick position; at 300 ms a strafing player is 3–4 m (5–7 body widths) from where the firer aimed. Arena PvP functionally broken at distance | Per-slot player pose history (reuse the tested `LagCompRing`); `beginLagComp`/`endLagComp` additionally swap/restore the PvP view positions (position-only; HP/block/corpse stay present-time — the entity pattern exactly) | `engine/engine_combat.cpp`, `engine/engine.h` |
| 6 | No outbound flush after update → queued sends wait for next frame's `enet_host_service` (~16.7 ms/direction, ~33 ms hidden RTT); delay-queue oversized/full path silently DROPS packets (incl. reliable) — the "caller sends immediately" comment is false at all 7 call sites (latent: unreachable only because MAX_SNAPSHOT_SIZE == DELAY_MAX_PAYLOAD) | `Net::flush()` (enet_host_flush) called after the fixed-step loop; oversized/full fall through to the matching immediate send; `static_assert(MAX_SNAPSHOT_SIZE <= DELAY_MAX_PAYLOAD)` | `net/net.h`, `net/net.cpp`, `engine/engine.cpp` |
| 7 | Docs/validation drift | Doc sync (PROTOCOL 22, new constants, rig notes, ClockSync latent-trap comment) + post-fix soak re-measure (bage < 64, delta ~100%, outage-guard behavior) | CLAUDE.md, engine-reference, `net/clock_sync.h` |

## Parked (reported to the user, not implemented)
- **Perfect-block/dodge favor-the-defender**: at 300 ms the 200 ms perfect window is reactively
  impossible (see-late 150–250 ms + raise-travel 150 ms); needs a design decision (client-timestamped
  block evaluation or RTT-scaled window).
- **30 Hz snapshot mode** (`TICKS_PER_SNAP=2`): halves host uplink AND doubles ring span; +16.7 ms
  remote latency. Not needed for 2-player DE↔NZ; landmine documented (`SNAP_NOMINAL_INTERVAL_SEC`
  must become 1/SNAPSHOT_RATE).
- **Reliable-retransmit rig mode**: fake loss never models the ~RTO reliable respike, so soaks
  under-test reliable-event latency (fires/pickups/kill-feed at +600 ms on a real lost packet).
- **ClockSync**: diagnostic-only (no functional consumer); frozen 3-pong OWT becomes a live bug the
  moment `currentServerTickEst` gains a real caller — warning comment added in Task 7.

## Verification
Full suite green after each task; post-fix soak at the same envelope: expect bage ≤ ~24 t against a
64-ring (0% fallback), delta ≈ 100%, bandwidth toward ~9–12 KB/s, idelay still 56–69 ms steady and
recovering promptly after an injected outage; 0 hard snaps throughout. PvP rewind verified by review
(no PvP soak rig exists — live arena playtest remains the user's).
