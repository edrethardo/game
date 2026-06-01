# Prediction Parity C2 — Server-Side Dodge-Roll Movement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the authoritative server reproduce the Wanderer dodge roll's 4 m / 0.5 s movement on `INPUT_EX_DODGE`, so `np.position` on the server tracks the client's predicted `m_localPlayer.position` to floating-point noise across the entire roll — eliminating the ~13 cm/tick (~4 m total) sim-level divergence that the R10 render-offset cap currently masks.

**Architecture:** Reuse the existing `DodgeState` struct as a new field on `NetPlayer`. Extend `PlayerController::updateNetPlayerFromInput` so it (a) starts a roll on the `INPUT_EX_DODGE` press edge using `np.yaw` + `input.moveFlags` (bit-equal copy of the host derivation at `player.cpp:142-153`) and (b) overrides `np.velocity.{x,z}` to `rollDirection × ROLL_SPEED=8.0f` while `ds.rolling` is true. The override sits at the end of `updateNetPlayerFromInput`, AFTER `applyMovement` populated WASD velocity but BEFORE the caller's per-input `Collision::moveAndSlide` integrates position — so the rolled velocity is what actually moves the player. `rollTimer` / `cooldownTimer` decay for active remote slots is co-located with the existing R9 remote-skill-cooldown loop at the top of `serverNetPre`. No wire format change.

**Tech Stack:** C++17, doctest for unit tests, ENet for transport. Source under `src/`, tests under `tests/`. Build via `cmake --build build`. Manual co-op smoke for verification — `player.cpp` is not currently linked into `dungeon_tests` so this pass adds no unit coverage.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/net/net_player.h` | NetPlayer authoritative state struct | Add `DodgeState dodgeState;` field |
| `src/game/player.cpp` | `updateNetPlayerFromInput` — applies a `NetInput` to a `NetPlayer` | Add dodge-start block + per-tick velocity override |
| `src/engine/engine_net.cpp` | `serverNetPre` — drives all remote-player simulation per server tick | Tick `rollTimer` / `cooldownTimer` for active remote slots inside the existing R9 cooldown loop |

No new files. No JSON. No protocol bumps. `DodgeState` is reused verbatim from `src/game/player.h:12-25`.

---

## Background Anchors (read once before starting)

- **Spec:** [docs/superpowers/specs/2026-06-01-prediction-parity-design.md](../specs/2026-06-01-prediction-parity-design.md) — full design, especially the "Commit 2" section.
- **Host dodge start logic (verbatim copy target):** `src/game/player.cpp:131-171` — `PlayerController::update`'s dodge-press branch. The `cosY/sinY → flatFwd/flatRight → dir from WASD → normalize` sequence at lines 142-153 is what the server must reproduce.
- **Host dodge tick (velocity override pattern):** `src/game/player.cpp:188-222` — the `ROLL_SPEED=8.0f` override and the `rollTimer`/`cooldownTimer` decay model.
- **Existing server dodge stub:** `src/game/player.cpp:287-289` — today's `INPUT_EX_DODGE` handler that ONLY sets `np.invulnTimer = 0.3f`. The new dodge-start block replaces this stub (and folds the invuln-arm into it).
- **R3 per-input integration:** `src/engine/engine_net.cpp:152-183` — the `updateNetPlayerFromInput` call is immediately followed by a per-input `Collision::moveAndSlide` that integrates `np.position` from `np.velocity`. The dodge override must run inside `updateNetPlayerFromInput` (i.e. BEFORE that `moveAndSlide` call) so the rolled velocity is what gets integrated.
- **R9 remote-cooldown tick loop:** `src/engine/engine_net.cpp:85-99` — the existing per-remote-slot cooldown decay loop. The new `rollTimer`/`cooldownTimer` decay extends this loop.

---

## Task 1: Add `DodgeState` field to `NetPlayer`

**Files:**
- Modify: `src/net/net_player.h` (around line 95-130, in the `NetPlayer` struct body, near the other Wanderer / class-passive mirrors)

- [ ] **Step 1.1: Add `dodgeState` field on `NetPlayer`**

Open `src/net/net_player.h`. Find the existing block of Wanderer / class-passive server-only mirrors that ends with the `markSpeedTimers[20]` line (around line 118). Insert the new field directly after it, before the `// Per-player equipment passives` comment.

The block currently looks like (around line 110-122):

```cpp
    // Wanderer mark-prey state (death-preamble follow-up batch): server-only mirror so a remote
    // Wanderer's Shadow Dance / mark-spread / speed stacks credit the *remote*, not the host
    // (m_localPlayer swap alias). Mirrors Player::{shadowDanceTimer,markTimer,markSpeedStacks,
    // markSpeedTimers}; not on the wire.
    f32  shadowDanceTimer  = 0.0f;
    f32  markTimer         = 0.0f;
    u8   markSpeedStacks   = 0;
    f32  markSpeedTimers[20] = {};
    // Per-player equipment passives (read from inventory each tick)
    SkillId weaponProc     = SkillId::NONE;
```

Insert immediately after the `markSpeedTimers[20] = {};` line:

```cpp
    // C2: Wanderer dodge-roll state — server-authoritative mirror of Player::dodgeState. Reuses
    // the host struct so PlayerController::updateNetPlayerFromInput can run the same rollTimer /
    // cooldownTimer / rollDirection bookkeeping that PlayerController::update does on the host.
    // Not on the wire — the client computes its own copy from the same input stream, and the
    // server proves parity by re-driving np.position with the same ROLL_SPEED × dt vector.
    DodgeState dodgeState;
```

- [ ] **Step 1.2: Include `game/player.h` so `DodgeState` is visible**

`net_player.h` does not currently include `game/player.h`. Add the include alongside the existing includes near the top of the file. After this line:

```cpp
#include "game/weapon.h"
#include "game/item.h"
```

insert:

```cpp
#include "game/player.h"     // C2: DodgeState struct (reused on NetPlayer)
```

If the build complains about a cyclic include (the existing forward decls `struct EntityHandle; struct NetInput; struct NetPlayer;` at the top of `player.h` suggest the cycle is already avoided), confirm with the build step below — `player.h` deliberately does NOT include `net_player.h`, so the one-way include is safe.

- [ ] **Step 1.3: Build the project**

Run:

```bash
cmake --build build
```

Expected: build succeeds. If the include order produces a redefinition or cyclic-include error, move the new `#include "game/player.h"` to be the FIRST include in `net_player.h` (before `core/types.h`) — `player.h` already includes both `core/types.h` and `core/math.h` itself, so duplicate-include guards will handle it.

- [ ] **Step 1.4: Run the existing test suite**

Run:

```bash
./build/tests/dungeon_tests --no-version
```

Expected: 82/82 passing. This task is a pure data-field addition; no test changes expected.

- [ ] **Step 1.5: Commit**

```bash
git add src/net/net_player.h
git commit -m "$(cat <<'EOF'
feat(net): add DodgeState field on NetPlayer for C2 prediction parity

Server-only mirror of Player::dodgeState so updateNetPlayerFromInput can
run the same rollTimer/cooldownTimer/rollDirection bookkeeping as the host
path. Not on the wire — clients re-derive from input stream.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Reproduce the dodge start + velocity override in `updateNetPlayerFromInput`

**Files:**
- Modify: `src/game/player.cpp:281-290` — replace the existing 3-line invuln-only stub with the full dodge-start + active-roll override

The existing stub at lines 281-290 is:

```cpp
    // Server-side Wanderer dodge: grant i-frames for the roll duration.
    // Full dodge movement is client-predicted; server only needs to track invulnerability.
    // Replayed during reconcile too: setting invulnTimer = 0.3 if it's currently <=0 is
    // idempotent for the same input, and re-arming here keeps the local view honest after
    // reconcile snaps invulnTimer back to the (older) snapshot value — otherwise a freshly
    // predicted dodge briefly looks like it had no i-frames until the server's ack catches up.
    if ((input.extFlags & INPUT_EX_DODGE) && np.invulnTimer <= 0.0f) {
        np.invulnTimer = 0.3f;
    }
}
```

The post-condition for this task is that the server applies the SAME 4 m / 0.5 s roll the client predicts. The override must (a) run AFTER `applyMovement` has populated `np.velocity` from WASD (so it overwrites, not co-exists), and (b) finish BEFORE the function returns — because the caller (`serverNetPre`) integrates `np.position` from `np.velocity` via `Collision::moveAndSlide` on the very next line of `serverNetPre` (engine_net.cpp:178).

- [ ] **Step 2.1: Replace the dodge stub with the full start + override block**

In `src/game/player.cpp`, delete the existing stub at lines 281-289 and write in its place:

```cpp
    // C2 — Server-side Wanderer dodge: reproduce the host's start logic + velocity override so
    // np.position tracks the client's predicted m_localPlayer.position to fp noise across the
    // roll. Verbatim copy of PlayerController::update's dodge branch at player.cpp:131-167 +
    // the active-roll override at 188-217, but driven from NetInput fields instead of Input::.
    // The active-roll override MUST run after applyMovement (above) so it OVERWRITES the WASD-
    // driven velocity, and MUST land before this function returns — serverNetPre runs
    // Collision::moveAndSlide immediately after this call (engine_net.cpp:178) and integrates
    // np.position from np.velocity, so the rolled velocity is what produces the 4 m of motion.
    // rollTimer / cooldownTimer decay lives in serverNetPre's R9 remote-cooldown loop (Task 3)
    // because it's a per-tick operation, not a per-input one — a remote drains multiple inputs
    // per server tick when their packet rate hiccups (engine_net.cpp:129-184), and decaying
    // here would double-tick the timer.
    DodgeState& ds = np.dodgeState;

    // Start dodge on the press edge — mirrors player.cpp:131-167. Replayed inputs during
    // reconcile are idempotent: the guard !ds.rolling skips re-starts within the same roll
    // window, and cooldownTimer > 0 blocks immediate re-starts after the roll ends. Both
    // guards match the host path exactly.
    if ((input.extFlags & INPUT_EX_DODGE)
        && !ds.rolling
        && ds.cooldownTimer <= 0.0f
        && !np.isDead) {
        f32 cosY = cosf(np.yaw);
        f32 sinY = sinf(np.yaw);
        Vec3 flatFwd   = normalize(Vec3{-sinY, 0.0f, -cosY});
        Vec3 flatRight = normalize(cross(flatFwd, {0.0f, 1.0f, 0.0f}));

        Vec3 dir = {0, 0, 0};
        if (input.moveFlags & INPUT_FORWARD)  dir += flatFwd;
        if (input.moveFlags & INPUT_BACKWARD) dir -= flatFwd;
        if (input.moveFlags & INPUT_RIGHT)    dir += flatRight;
        if (input.moveFlags & INPUT_LEFT)     dir -= flatRight;
        if (lengthSq(dir) < 0.001f) dir = flatFwd; // no WASD → roll forward
        ds.rollDirection = normalize(dir);
        ds.rolling       = true;
        ds.rollTimer     = 0.5f;
        np.invulnTimer   = 0.3f; // i-frames for first 60% of the roll (host parity, was the old stub)
    }

    // Active roll: override horizontal velocity to a constant ROLL_SPEED in the rolled
    // direction. Mirrors player.cpp:188-194. Y velocity is left alone so gravity / jumping
    // unaffected — same as the host path. rollTimer decay happens in serverNetPre's tick
    // loop (Task 3), so this branch just maintains the override every input drained this
    // server tick — if a remote drained 3 inputs in one tick, each runs moveAndSlide with
    // velocity = rollDirection × ROLL_SPEED and produces ROLL_SPEED × dt of motion, identical
    // to the host integrating once per gameUpdate at dt=1/60.
    if (ds.rolling) {
        constexpr f32 ROLL_SPEED = 8.0f; // 4 m over 0.5 s — matches player.cpp:192
        np.velocity.x = ds.rollDirection.x * ROLL_SPEED;
        np.velocity.z = ds.rollDirection.z * ROLL_SPEED;
    }
}
```

- [ ] **Step 2.2: Build the project**

Run:

```bash
cmake --build build
```

Expected: build succeeds.

- [ ] **Step 2.3: Run the existing test suite**

Run:

```bash
./build/tests/dungeon_tests --no-version
```

Expected: 82/82 passing. (`player.cpp` is not in the test link list — these changes can't break tests, only build.)

- [ ] **Step 2.4: Commit**

```bash
git add src/game/player.cpp
git commit -m "$(cat <<'EOF'
fix(net): apply Wanderer dodge-roll movement on the authoritative server

updateNetPlayerFromInput now reproduces the host's rollDirection derivation
from np.yaw + input.moveFlags on the INPUT_EX_DODGE press edge, then
overrides np.velocity.{x,z} to rollDirection * ROLL_SPEED=8 for the roll's
duration. Override runs after applyMovement (overwriting WASD) and before
serverNetPre's per-input Collision::moveAndSlide integrates the 4 m of
motion — closing the ~13 cm/tick sim-divergence the R10 render-offset cap
was masking.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Tick `rollTimer` / `cooldownTimer` per server tick

**Files:**
- Modify: `src/engine/engine_net.cpp:89-99` — extend the existing R9 remote-cooldown loop with `dodgeState` decay

The existing R9 loop is at engine_net.cpp:89-99:

```cpp
    // R9: drain remote-player skill cooldowns each server tick so the gate set by
    // SkillSystem::tryActivate (cooldownTimer = def->cooldown on success) actually
    // counts down. Host's own slot is ticked by tickSkillCooldowns in gameUpdate
    // (with split-screen alias swap), so this loop only covers remote net slots.
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (i == m_localPlayerIndex || !m_players[i].active) continue;
        for (u32 s = 0; s < 4; s++) {
            f32& cd = m_classSkillStatesNet[i][s].cooldownTimer;
            if (cd > 0.0f) { cd -= dt; if (cd < 0.0f) cd = 0.0f; }
        }
        f32& bcd = m_bootSkillStates[i].cooldownTimer;
        if (bcd > 0.0f) { bcd -= dt; if (bcd < 0.0f) bcd = 0.0f; }
        f32& hcd = m_helmetSkillStates[i].cooldownTimer;
        if (hcd > 0.0f) { hcd -= dt; if (hcd < 0.0f) hcd = 0.0f; }
    }
```

- [ ] **Step 3.1: Append the dodge timer decay inside the existing loop**

Inside the existing `for (u32 i ...)` loop body, after the `hcd` block but BEFORE the closing brace of the for-loop, add the dodge tick. The block becomes:

```cpp
    // R9: drain remote-player skill cooldowns each server tick so the gate set by
    // SkillSystem::tryActivate (cooldownTimer = def->cooldown on success) actually
    // counts down. Host's own slot is ticked by tickSkillCooldowns in gameUpdate
    // (with split-screen alias swap), so this loop only covers remote net slots.
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (i == m_localPlayerIndex || !m_players[i].active) continue;
        for (u32 s = 0; s < 4; s++) {
            f32& cd = m_classSkillStatesNet[i][s].cooldownTimer;
            if (cd > 0.0f) { cd -= dt; if (cd < 0.0f) cd = 0.0f; }
        }
        f32& bcd = m_bootSkillStates[i].cooldownTimer;
        if (bcd > 0.0f) { bcd -= dt; if (bcd < 0.0f) bcd = 0.0f; }
        f32& hcd = m_helmetSkillStates[i].cooldownTimer;
        if (hcd > 0.0f) { hcd -= dt; if (hcd < 0.0f) hcd = 0.0f; }

        // C2: Wanderer dodge-roll timers — same per-tick cadence as host's
        // PlayerController::update at player.cpp:196-221. End-of-roll resets rolling=false
        // and arms the 1.0 s cooldown; updateNetPlayerFromInput's start-gate
        // (cooldownTimer <= 0.0f) then blocks restarts during that window. Co-located
        // here (not in updateNetPlayerFromInput) so the timer decays exactly once per
        // server tick — drained inputs can fire updateNetPlayerFromInput multiple times
        // per tick (engine_net.cpp:129-184), and decaying inside that loop would
        // burn the 0.5 s roll in 1-2 server ticks under jittered packet arrival.
        DodgeState& ds = m_players[i].dodgeState;
        if (ds.rolling) {
            ds.rollTimer -= dt;
            if (ds.rollTimer <= 0.0f) {
                ds.rolling       = false;
                ds.rollTimer     = 0.0f;
                ds.cooldownTimer = 1.0f;
            }
        } else if (ds.cooldownTimer > 0.0f) {
            ds.cooldownTimer -= dt;
            if (ds.cooldownTimer < 0.0f) ds.cooldownTimer = 0.0f;
        }
    }
```

- [ ] **Step 3.2: Build the project**

Run:

```bash
cmake --build build
```

Expected: build succeeds.

- [ ] **Step 3.3: Run the existing test suite**

Run:

```bash
./build/tests/dungeon_tests --no-version
```

Expected: 82/82 passing.

- [ ] **Step 3.4: Commit**

```bash
git add src/engine/engine_net.cpp
git commit -m "$(cat <<'EOF'
fix(net): tick remote NetPlayer dodgeState timers in serverNetPre

Decays rollTimer / cooldownTimer for active remote slots inside the
existing R9 remote-skill-cooldown loop, so the dodge ends after 0.5 s and
the 1.0 s cooldown gate blocks restarts — matching the host's
PlayerController::update cadence. Co-located with R9 so all per-remote
per-tick state advancement lives in one place.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Verification

Once all three tasks are committed:

- [ ] **Step V.1: Full rebuild + test pass**

```bash
cmake --build build
./build/tests/dungeon_tests --no-version
```

Expected: build clean, **82/82 tests passing**. No new tests added this pass — `player.cpp` and `engine_net.cpp` aren't linked into `dungeon_tests` (see the test-framework spec at `docs/superpowers/specs/2026-05-31-test-framework-design.md`). Future work blocked on the `dungeon_core` refactor described there will add real unit coverage for `updateNetPlayerFromInput`.

- [ ] **Step V.2: Co-op dodge-roll smoke test (the spec's primary verification)**

Manual procedure, taken straight from the spec's Verification section §3:

1. Launch a host: `./build/dungeon_game`. Pick **Host Game**. Start a run as the Wanderer.
2. On a second machine (or second build dir), launch `./build/dungeon_game` and **Join Game** at the host's IP.
3. From the **client**, press **Shift** to fire the Wanderer dodge roll. Try forward, back, strafe-left, strafe-right, and diagonal directions; repeat several times so the 1 s cooldown is exercised.
4. Toggle the net-graph (F9 by default — see `engine-reference` debug keys if uncertain) on both ends and watch the `divergences=` counter.

**Expected:**
- The client's local view smoothly travels the full 4 m in the chosen direction (no R10 render-offset cap saturation visible behind the camera).
- The host's view of the remote client shows the same 4 m roll — no laggy "remote player teleports forward at the end of the roll" snap.
- Per-second `divergences=` count during the roll window drops markedly compared to before this commit. (Pre-C2 baseline: roughly one divergence per tick during the 30 ticks of a roll. Post-C2 target: near zero except for incidental floating-point noise.)

**Failure modes to watch for:**
- *Host shows remote roll, then snaps back at the end* — `cooldownTimer` is not decaying. Check Task 3's R9-loop edit; the `i == m_localPlayerIndex` guard skips the host's own slot (correct — host's `Player` ticks `cooldownTimer` in `PlayerController::update`), but if you accidentally also skip on `np.isDead` outside the loop body, remotes will never decay either.
- *Remote rolls in the wrong direction* — `np.yaw` is being read before the `Quantize::unpackAngle` call earlier in `updateNetPlayerFromInput` (player.cpp:268). The new block must sit AFTER that assignment (it does, per the spec's "after applyMovement at line 270-279" placement).
- *Remote rolls but doesn't move* — the override is running but `Collision::moveAndSlide` isn't seeing the new velocity. Re-check that the override block is INSIDE `updateNetPlayerFromInput` (not after the closing brace of the function), so it lands on `np.velocity` before the caller in `serverNetPre:178` reads it.

- [ ] **Step V.3: Cross-check with C1 if both shipped**

If Commit 1 (status-effect parity, sibling plan) has also landed, run the spec's §4 combined check: client gets slowed AND frozen by a stacked hit, then attempts to dodge. The dodge should still produce the full 4 m motion (dodge override bypasses `effectiveSpeed` entirely — the host path does the same at player.cpp:188-194). If freeze somehow gates the dodge, that's a parity bug in Task 2 — re-read it; the override is unconditional on `ds.rolling`.

---

## Commit Message Summary (for reference)

Three commits, all tagged `fix(net):` matching the recent netplay-rewrite log style (see `git log --oneline -20` for surrounding tone — `fix(net):`-prefixed messages dominate the recent history):

1. `feat(net): add DodgeState field on NetPlayer for C2 prediction parity` (Task 1)
2. `fix(net): apply Wanderer dodge-roll movement on the authoritative server` (Task 2)
3. `fix(net): tick remote NetPlayer dodgeState timers in serverNetPre` (Task 3)

All three carry the standard `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>` trailer.

---

## What's intentionally NOT in this plan

- **No unit tests.** `player.cpp` and `engine_net.cpp` are not linked into `dungeon_tests`. The spec explicitly accepts manual verification for this pass (see Spec §"Test coverage" under "Commit 2"). Forward-only TDD policy (CLAUDE.md / `docs/superpowers/specs/2026-05-31-test-framework-design.md`) means we don't backfill the suite to cover existing combat / net code paths.
- **No wire format change.** `INPUT_EX_DODGE`, `input.yawQ`, and `input.moveFlags` already carry everything needed (spec §"Mechanism"). No bump of PROTOCOL_VERSION, no edits to packet (de)serialization.
- **No host-path edits.** `PlayerController::update` (host/client path) stays as-is. Task 2 only touches `updateNetPlayerFromInput`.
- **No `Client::reconcile` change.** The client already predicts the dodge locally; once the server's `np.position` is in parity, reconcile naturally agrees instead of snapping.
- **C1 (status-effect parity) is a sibling plan**, not part of this one. Don't touch `freezeTimer` / `slowTimer` logic here.
