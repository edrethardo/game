# Prediction-Parity C1: Status-Effect Speed Parity (Freeze + Slow) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the freeze-related client/server prediction asymmetry by making `freezeTimer` apply a 0.15× speed multiplier on **both** motion paths in `src/game/player.cpp` (host/client `PlayerController::update` AND server `PlayerController::updateNetPlayerFromInput`), replacing the server's hard `effectiveSpeed = 0.0f` stop.

**Architecture:** Two surgical edits in one file. The host path currently ignores `freezeTimer` entirely (so a frozen local player keeps walking, diverging from the server every tick). The server path currently zeros speed (so frozen remote players are rooted). Both paths converge on `effectiveSpeed *= 0.15f`, mirroring how the existing 0.4× slow check and every other speed modifier (soul harvest, adrenaline, deflect, mark) already compound. No wire-format change — `freezeTimer` already ships on the snapshot and is adopted by `Client::reconcile`.

**Tech Stack:** C++17, doctest (tests stay green at 82/82), `cmake` + `ninja`/`make` desktop build. No new dependencies.

**Scope boundary:** This is Commit 1 only. Commit 2 (server-side dodge-roll movement on `NetPlayer`) is being written by a parallel agent and lands in a separate commit — do **not** touch `src/net/net_player.h`, `src/engine/engine_net.cpp`, or the dodge-roll blocks in `player.cpp` (lines 131-171, 187-222, 281-289).

---

## File Structure

**Modified (single file):**
- `src/game/player.cpp`
  - `PlayerController::update` (lines 74-225) — insert a freeze multiplier block immediately after the existing slow block (lines 120-123).
  - `PlayerController::updateNetPlayerFromInput` (lines 230-290) — rewrite the existing freeze block (lines 245-248) from `effectiveSpeed = 0.0f;` to `effectiveSpeed *= 0.15f;`.

**Not modified:** the wire (no protocol change), `Client::reconcile` (already adopts `freezeTimer`), the slow blocks on either path (already correct at 0.4×).

---

## Test Coverage Note (read before starting)

`src/game/player.cpp` is **not** currently linked into `dungeon_tests` (per CLAUDE.md: "tests/ mirrors src/" but `player.cpp` is not in the `add_executable(dungeon_tests ...)` source list). The `dungeon_core` library refactor that would let us cover this path is tracked separately in `docs/superpowers/specs/2026-05-31-test-framework-design.md` and is **out of scope here.**

Consequence for this plan:
- **No new doctest case is added.** Adding a `TEST_CASE` that links `player.cpp` would expand the test-binary source list significantly (the rest of `game/` would need to come along for symbols to resolve) and is the wrong batch for a 2-line gameplay tweak.
- **Verification is manual co-op smoke** (procedure in the Verification section below), plus the existing 82/82 suite must remain green to confirm we didn't regress anything that *is* covered (snapshot quantization, clock sync, input ring, prediction ring, etc.).

This caveat is explicitly acknowledged in the spec at `docs/superpowers/specs/2026-06-01-prediction-parity-design.md` lines 105-108.

---

## Task 1: Apply freeze multiplier on the host/client path

**Files:**
- Modify: `src/game/player.cpp:120-123` (insert immediately after the existing slow block inside `PlayerController::update`)

- [ ] **Step 1: Read the existing slow block to confirm context**

Open `src/game/player.cpp` and locate lines 120-123. They should currently read:

```cpp
    if (player.slowTimer > 0.0f) {
        effectiveSpeed *= 0.4f; // 60% slow
        player.slowTimer -= dt;
    }
```

If they don't match exactly (e.g., comments differ, a refactor landed), stop and re-read the spec — the line numbers and surrounding modifiers (soul harvest at 104-106, adrenaline at 108-110, deflect at 112-114, mark at 116-118) anchor the insertion point.

- [ ] **Step 2: Insert the freeze multiplier directly after the slow block**

After line 123 (`}`), and **before** the existing `// Merge keyboard + left stick for movement` comment at line 125, add:

```cpp
    // C1 (prediction-parity): freeze is a heavy crawl, not a hard stop. Mirrors the
    // server's updateNetPlayerFromInput so client prediction and authoritative sim
    // produce identical positions during a freeze window. Stacks multiplicatively
    // with slow (frozen + slowed = 0.06× = ~0.24 m/s).
    if (player.freezeTimer > 0.0f) {
        effectiveSpeed *= 0.15f;
    }
```

The block deliberately does **not** decay `freezeTimer` here. Timer decay is owned by the authoritative-snapshot path (`Client::reconcile` adopts the server's value each snapshot); decaying it locally would double-decrement under reconcile replay — the same pitfall the slow block's neighboring comment at lines 240-244 of `updateNetPlayerFromInput` already warns about.

- [ ] **Step 3: Build incrementally**

Run: `cmake --build build`
Expected: clean build, no warnings on the changed file. If the build is stale, prime it with `cmake -B build -DCMAKE_BUILD_TYPE=Debug` first.

- [ ] **Step 4: Run the test suite**

Run: `./build/tests/dungeon_tests --no-version`
Expected: `[doctest] Status: SUCCESS!` with `82 | 82 passed`. Anything else means a covered subsystem regressed — stop and investigate.

---

## Task 2: Replace the hard freeze stop on the server path with the same 0.15× multiplier

**Files:**
- Modify: `src/game/player.cpp:245-248` (inside `PlayerController::updateNetPlayerFromInput`)

- [ ] **Step 1: Locate and verify the existing server-side freeze block**

Lines 245-248 should currently read:

```cpp
    // Freeze stops all movement
    if (np.freezeTimer > 0.0f) {
        effectiveSpeed = 0.0f;
    }
```

If the comment or the `0.0f` differ, re-read — Commit 2 may have landed first in your branch and you're about to clobber its work. (Per the parallel-agent note in the header: C2 doesn't touch this block, so if it differs, something is off.)

- [ ] **Step 2: Replace with the multiplicative form**

Change those four lines to:

```cpp
    // C1 (prediction-parity): freeze is a heavy crawl, mirrors PlayerController::update's
    // host path. Was previously effectiveSpeed = 0.0f (hard root), but that fought the
    // client's now-also-multiplicative prediction and produced a per-tick ~10 cm
    // divergence whenever a freeze landed on the local player.
    if (np.freezeTimer > 0.0f) {
        effectiveSpeed *= 0.15f;
    }
```

The slow block immediately above (lines 240-244) stays untouched — it's already `effectiveSpeed *= 0.4f;` and already correctly defers timer decay.

- [ ] **Step 3: Build**

Run: `cmake --build build`
Expected: clean build. The change is a value swap on an existing statement, so no signature shifts, no header touches.

- [ ] **Step 4: Run the test suite again**

Run: `./build/tests/dungeon_tests --no-version`
Expected: `82 | 82 passed`. Same justification as Task 1.

---

## Task 3: Verify the change end-to-end (manual co-op smoke)

**Files:** none modified.

This is the verification gate before commit. The spec at lines 256-278 prescribes the procedure; what follows is the executable version.

- [ ] **Step 1: Release build for realistic timing**

Run: `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel`
Expected: optimized binary at `./build-rel/dungeon_game`. (Debug is fine too, but freeze duration is short — release timing makes the crawl visually obvious.)

- [ ] **Step 2: Launch two instances on the local machine**

Terminal A (host):
```bash
./build-rel/dungeon_game
```
Pick "Host Game" from the menu, choose Sorcerer.

Terminal B (client):
```bash
./build-rel/dungeon_game
```
Pick "Join Game", enter `127.0.0.1`, choose any class (Wanderer is convenient for the dodge cross-check, though dodge isn't fixed until C2).

- [ ] **Step 3: Freeze smoke — host freezes client**

- On the host (Sorcerer), cast Frozen Orb at the client. The Frozen Orb skill is on the legendary-granted skill bar; if the host doesn't have it equipped, spawn one via the debug item-give key (see `engine-reference` skill for current bindings) or just shoot any enemy projectile that applies freeze.
- While the client is frozen (visible HUD debuff icon + blue tint on the avatar), hold W on the client.

**Expected:**
- Client avatar **visibly crawls forward** at ~0.6 m/s (= 4 m/s base × 0.15). Before C1, the client would walk at full speed locally then snap backward each reconcile (root-causing the every-tick divergence).
- Toggle the net-graph overlay (F-key, see in-game debug keys in `engine-reference`). The `divergences=` counter should stay flat or grow by ≤1 across the entire freeze window. Before C1 it incremented every tick (every 16 ms).

If the client still moves at full speed → Task 1's edit didn't land (or build was stale).
If the client is fully rooted → Task 2's edit didn't land (or you're testing on a stale binary).

- [ ] **Step 4: Slow + freeze stacking check**

Same setup. Get the client both slowed (e.g., walk through a slow puddle / boss cleaver hit) AND frozen at the same time. Hold W.

**Expected:** crawl is dramatically slower — roughly 0.06× base move speed (≈0.24 m/s). Visually it's "barely moving." When the slow wears off (timer ticks to 0), speed jumps to 0.15× base. When freeze wears off, full speed returns (modulo any other modifiers).

This confirms the multiplicative stacking property the spec calls out at lines 41-43.

- [ ] **Step 5: Reverse direction — client freezes host**

Less critical but worth a sanity check: host plays Wanderer, client plays Sorcerer, client freezes host. The host runs the same `PlayerController::update` path that the client did in Step 3 (it's the singleplayer/host path), so the expected behavior is identical: host avatar crawls at 0.15× while frozen. Pass = same behavior on both ends.

- [ ] **Step 6: Final unit-test pass**

Run: `cmake --build build && ./build/tests/dungeon_tests --no-version`
Expected: `82 | 82 passed`. (Yes, this is the third time — it's cheap, and it guards against the "I edited the wrong line in the wrong build directory" failure mode.)

---

## Task 4: Commit

- [ ] **Step 1: Stage only `src/game/player.cpp`**

Run:
```bash
git add src/game/player.cpp
git status
```
Expected: exactly one modified file (`src/game/player.cpp`). If anything else is staged (test files, build artifacts, the C2 plan's territory), unstage it — this commit is C1 only.

- [ ] **Step 2: Verify the diff is minimal**

Run:
```bash
git diff --cached src/game/player.cpp
```
Expected: two hunks. Hunk 1 inserts a 6-line freeze block inside `PlayerController::update` after the slow block. Hunk 2 replaces the body of the existing freeze block inside `updateNetPlayerFromInput` (4 lines in → 6 lines out, with the multiplicative form and the new comment). No incidental whitespace churn, no untouched-line drift.

- [ ] **Step 3: Commit**

Use the project's `fix(net): …` tone (see `git log --oneline -20` — every recent netcode fix follows this shape):

```bash
git commit -m "fix(net): freeze applies 0.15× speed mult on both client and server paths"
```

The longer-form rationale lives in the spec and inline comments; the project's recent commits keep the subject terse (≤72 chars) and skip body paragraphs for surgical fixes. Match that.

- [ ] **Step 4: Confirm the commit landed**

Run: `git log --oneline -3`
Expected: top entry is your new commit, second is `873d96e docs: prediction-parity spec (close freeze + dodge-roll asymmetries)`. The C2 commit (server-side dodge roll) will land **after** yours in time order — that's fine, the two commits are independent.

---

## Verification (summary)

Pulled together for the executing agent to tick off as a single checklist:

- [ ] `cmake --build build` succeeds after Task 1 and after Task 2 (no warnings introduced on the changed file).
- [ ] `./build/tests/dungeon_tests --no-version` reports `82 | 82 passed` after both edits.
- [ ] Two-instance localhost co-op: frozen client crawls forward at ~0.15× (visible motion, not rooted).
- [ ] Net-graph `divergences=` counter stays flat across a freeze window (no per-tick reconcile snap).
- [ ] Freeze + slow stack to ~0.06× base move speed (barely moving).
- [ ] Symmetric: client-freezes-host produces the same crawl on the host's local view.
- [ ] `git diff --cached src/game/player.cpp` shows exactly two minimal hunks, both inside the two functions named in this plan.

---

## Commit message

```
fix(net): freeze applies 0.15× speed mult on both client and server paths
```

Rationale (kept out of the commit body to match recent project tone — every `fix(net): …` commit since `b524abe` is a single-line subject):
- Closes the per-tick ~10 cm divergence between `PlayerController::update` (host/client) and `PlayerController::updateNetPlayerFromInput` (server) whenever `freezeTimer > 0`.
- Redefines freeze as a heavy slow (85% reduction) instead of a hard root; stacks multiplicatively with slow (0.4×) for a combined 0.06×.
- No wire-format change — `freezeTimer` already shipped on `WorldSnapshot` and is already adopted by `Client::reconcile`.

---

## Out of scope (do not do in this commit)

- Predicting status-effect *application* on incoming projectile impact (D3.2 path). Needs new wire fields on `SnapProjectile`. Deferred per spec §"Deferred follow-ups".
- Reliable `SV_FREEZE_APPLIED` confirm event. Same rationale — current reconcile-via-next-snapshot path handles it.
- Refactoring `update` + `updateNetPlayerFromInput` into a shared function. Real cleanup but a bigger refactor; the spec explicitly excludes it.
- Anything in `src/net/net_player.h`, `src/engine/engine_net.cpp`, or the dodge-roll blocks in `player.cpp`. That's Commit 2's territory and is being written by a parallel agent.
- Adding a doctest case for the freeze multiplier. `player.cpp` isn't linked into `dungeon_tests`; covering this properly needs the `dungeon_core` library refactor (tracked at `docs/superpowers/specs/2026-05-31-test-framework-design.md`).
