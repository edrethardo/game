# Netplay M0: Tidy Current State Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Commit the in-flight "giant netplay fix campaign" work in coherent topic-based commits, mark surfaces that conflict with the rewrite (client-authoritative position, manual fire-retransmit ring, predicted-ghost fire model) as deprecated in code, and update CLAUDE.md + engine-reference to reflect the M0 baseline so M1 starts from a clean foundation.

**Architecture:** No code semantics change in M0 (the in-flight code stays as-is). M0 only reorganizes the working tree into ~5 commits by topic and adds deprecation markers + doc updates. Conflict surfaces are reversed in their natural future milestone (M3 reverts client-auth position; M10 replaces manual fire retransmit with reliable channel; M10/M11 reworks the predicted-ghost model). End state: clean working tree (`git status` shows no changes), build green, two-process co-op smoke functional.

**Tech Stack:** git (history surgery only — no rebase/force-push), cmake (Debug build), C++17, bash (`tools/run_coop_local.sh` for smoke).

**Reference spec:** [/home/aaron/.claude/plans/multiplayer-should-feel-like-curried-coral.md](../../../../.claude/plans/multiplayer-should-feel-like-curried-coral.md) — the rewrite design doc this milestone is preparing for. Read its "Migration Plan → Milestone 0" section before executing.

---

## Pre-flight Notes

**Why this is a refactor plan, not a TDD plan.** M0 does not change code semantics — it reorganizes uncommitted work into commits. The "verification" gates are (a) build still compiles, (b) two-process co-op smoke still functions, (c) `git status` ends clean. There is nothing new to write a unit test for. Future milestones (M1+) will be TDD.

**No force-push, no rebase.** All commits are forward additions on `master`. The campaign baseline commit `b3ea205` stays as-is.

**Conflict surfaces deferred, not deleted.** The design spec lists client-authoritative position, manual fire-retransmit ring, and predicted-ghost fire model as "discard or rework". For M0 they are kept in place (removing them now would regress the game with no replacement) but **explicitly marked deprecated in comments** so future readers see the rewrite plan. Actual replacement happens in M3 (position) and M10/M11 (fire).

**File grouping rationale:** Most uncommitted files split cleanly by topic:

| Topic | Files |
|---|---|
| Tooling | `tools/build_assets.py`, `tools/gen_mesh.py`, `tools/gen_skin.py`, `tools/run_coop_local.sh`, `.claude/skills/create-class-skin/` |
| Per-class visual identity | `assets/materials.json`, `assets/textures/player_combat_engineer_skin_42.png`, `src/engine/engine_init_assets.cpp`, `src/engine/engine_render_world.cpp` |
| Continue-Join save flow | `src/engine/engine_menu.cpp` |
| Netplay campaign | All remaining `src/` files (engine_combat.cpp, engine_net.cpp, engine_update.cpp, engine.cpp, engine.h, engine_init.cpp, engine_render.cpp, engine_render_entities.cpp, engine_startgame.cpp, engine_update_player.cpp, game/item.h, game/projectile.h, net/*) |
| Deprecation markers + docs | `src/net/net_player.h` (deprecation block), `CLAUDE.md`, `.claude/skills/engine-reference/SKILL.md` |

Per-class visual identity touches `snapshot.h/cpp` for the `playerClass` wire bit, but those files also carry netplay campaign changes (clientTickLow, attackAnimQ, etc.), so they ride in the netplay commit — that's where they're most coherent.

---

## Task 1: Pre-flight Build & Smoke

**Files:** none (verification only)

- [ ] **Step 1: Confirm we are on master with the campaign baseline as HEAD**

Run:
```bash
git rev-parse --abbrev-ref HEAD && git log --oneline -1
```

Expected output:
```
master
b102492 add logs to gitignore.
```

If on a different branch or different HEAD, stop and reconcile with the user before proceeding.

- [ ] **Step 2: Build the binary**

Run:
```bash
cmake --build build 2>&1 | tail -20
```

Expected: build succeeds (last line is something like `[100%] Built target DungeonEngine`). If it fails, fix the underlying error before M0 — the plan assumes a green starting point.

- [ ] **Step 3: Two-process co-op smoke (baseline)**

This is a manual smoke. Open two terminals.

Terminal 1 (host):
```bash
./build/dungeon_game
```
In-game: choose Host, start a new game.

Terminal 2 (client):
```bash
./build/dungeon_game
```
In-game: choose Join → 127.0.0.1, connect.

Verify both windows:
- Both players spawn in the same dungeon
- Movement on either side updates both views
- Firing a weapon shows projectile on both sides
- Hitting an enemy updates HP / kills on both sides

If anything is visibly broken, stop and report before proceeding — M0 should not regress functional behavior.

(Note: `tools/run_coop_local.sh` is available as a helper script — currently untracked — but the baseline smoke is easier from two manual terminals.)

- [ ] **Step 4: Take a snapshot of the diff size for later comparison**

Run:
```bash
git diff HEAD --stat | tail -1 && git status --short | wc -l
```

Record the output — at the end of M0 the wc-l output must be `0` (clean tree) and the diff-stat must read `no changes`.

Expected starting point (approximately):
```
 28 files changed, 4098 insertions(+), 311 deletions(-)
38
```

---

## Task 2: Flatten the Index

**Files:** none (git index operation only)

- [ ] **Step 1: Unstage anything currently in the index**

Some files are already staged (`MM` in `git status -s`). To make subsequent topic-based `git add` clean, unstage everything first so all modified content is in the working tree only.

Run:
```bash
git reset
```

Expected output:
```
Unstaged changes after reset:
M	.claude/skills/engine-reference/SKILL.md
...
```
(followed by ~25 lines)

- [ ] **Step 2: Confirm nothing is staged**

Run:
```bash
git diff --cached --stat
```

Expected output: empty (no staged changes).

- [ ] **Step 3: Confirm working tree has the full diff intact**

Run:
```bash
git diff HEAD --stat | tail -1
```

Expected: same diff size as recorded in Task 1 Step 4 (~28 files, ~4098 insertions).

---

## Task 3: Commit Tooling

**Files:**
- Add (untracked): `.claude/skills/create-class-skin/`
- Stage (modified or new): `tools/build_assets.py`, `tools/gen_mesh.py`, `tools/gen_skin.py`, `tools/run_coop_local.sh`

- [ ] **Step 1: Stage tooling files**

Run:
```bash
git add tools/build_assets.py tools/gen_mesh.py tools/gen_skin.py tools/run_coop_local.sh .claude/skills/create-class-skin/
```

- [ ] **Step 2: Verify only tooling is staged**

Run:
```bash
git diff --cached --stat
```

Expected output (file list — counts approximate):
```
 .claude/skills/create-class-skin/SKILL.md |   ??
 tools/build_assets.py                     |   22
 tools/gen_mesh.py                         | 1199
 tools/gen_skin.py                         | 1064
 tools/run_coop_local.sh                   |  102
 N files changed, ~2400+ insertions(+)
```

(`SKILL.md` count is whatever the new skill ships with — exact value not important; the point is no `src/` or `assets/` files should appear.)

- [ ] **Step 3: Commit tooling**

Run:
```bash
git commit -m "$(cat <<'EOF'
tools: add mesh/skin asset generators and co-op smoke script

- gen_mesh.py / gen_skin.py: Python tools to generate low-poly voxel
  meshes and per-class skins (used by the new create-class-skin skill
  and by future enemy/boss asset work).
- build_assets.py: wire the new generators into the asset pipeline.
- run_coop_local.sh: launch a host+client pair locally and tee logs
  to logs/host.log and logs/client.log — the standing test harness
  for the netplay rewrite (see netplay rewrite design doc).
- create-class-skin skill: documents the asset path for player class
  skins; complements the existing create-enemy / create-boss skills.

Part of netplay M0 baseline cleanup.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 4: Verify commit**

Run:
```bash
git log --oneline -1 && git status --short | wc -l
```

Expected:
- Top line: a hash + the commit subject "tools: add mesh/skin asset generators and co-op smoke script"
- wc-l value: smaller than the starting count (most tools/* and the create-class-skin dir gone)

---

## Task 4: Commit Per-Class Visual Identity

**Files:**
- Stage: `assets/materials.json`, `assets/textures/player_combat_engineer_skin_42.png`, `src/engine/engine_init_assets.cpp`, `src/engine/engine_render_world.cpp`

- [ ] **Step 1: Stage per-class visual files**

Run:
```bash
git add assets/materials.json assets/textures/player_combat_engineer_skin_42.png src/engine/engine_init_assets.cpp src/engine/engine_render_world.cpp
```

- [ ] **Step 2: Confirm scope**

Run:
```bash
git diff --cached --stat
```

Expected: only the 4 listed files staged. No `src/net/*`, no `src/engine/engine.cpp`, no `tools/*`. Counts approximately:
```
 assets/materials.json                              | 45 +++
 assets/textures/player_combat_engineer_skin_42.png |  Bin (binary file)
 src/engine/engine_init_assets.cpp                  | 13 ++
 src/engine/engine_render_world.cpp                 | 57 +++++++++-------
 4 files changed, ~115 insertions(+), 22 deletions(-)
```

- [ ] **Step 3: Commit per-class visual identity**

Run:
```bash
git commit -m "$(cat <<'EOF'
render: per-class player visuals (mesh + skin material)

Resolve the player visual from playerClass instead of always rendering
the generic "human" mesh — so a Warrior, Wanderer, Combat Engineer, etc.
look distinct from each other in the world (host, split-screen, and
on remotes). engine_render_world.cpp:235 picks classByte from either
NetPlayer.playerClass (host) or m_renderInterp.playerClass (CLIENT),
falls back to "human" + "human_skin" if either lookup misses so an
unbuilt asset doesn't render as the magenta default cube.

Adds the engineer skin texture and the supporting material entries.
engine_init_assets.cpp wires the new materials. The playerClass wire
bit itself is in the netplay campaign commit (it shares snapshot.h
with other netplay changes).

Part of netplay M0 baseline cleanup.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 4: Verify commit**

Run:
```bash
git log --oneline -2 && git status --short | grep -E '^(M |A | M)' | wc -l
```

Expected: top commit is the per-class visual commit; remaining-changes count is smaller than after Task 3.

---

## Task 5: Commit Continue-Join Save Flow

**Files:**
- Stage: `src/engine/engine_menu.cpp`

- [ ] **Step 1: Stage the menu file**

Run:
```bash
git add src/engine/engine_menu.cpp
```

- [ ] **Step 2: Confirm scope**

Run:
```bash
git diff --cached --stat
```

Expected:
```
 src/engine/engine_menu.cpp | 52 ++++++++++++++++++++++++++++++----
 1 file changed, 46 insertions(+), 6 deletions(-)
```

- [ ] **Step 3: Commit Continue-Join**

Run:
```bash
git commit -m "$(cat <<'EOF'
menu: support Continue-Join (join a host with a loaded save)

Previously Continue and Host/Join were mutually exclusive — picking
Join always routed through New Game's class-select then sent
CL_INVENTORY_SYNC blank. Continue-Join keeps the class from the save
and pushes the loaded inventory to the host post-SV_JOIN_ACCEPT
(m_clientLoadedFromSave gates startGame's inventory wipe + starter
kit).

Also fixes a netRole leak on MENU_BACK from the chooser — backing
out without confirm used to leave m_netRole as SERVER/CLIENT and
that role bled into the next action.

Part of netplay M0 baseline cleanup.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 4: Verify commit**

Run:
```bash
git log --oneline -3
```

Expected: top three commits are netplay-m0 commits in order: continue-join, per-class visuals, tools.

---

## Task 6: Commit Netplay Campaign Wrap-Up

**Files:**
- Stage: all remaining modified files in `src/` and the engine-reference skill (will be edited in Task 8, so leave it for now).

This is the largest commit — it wraps up Phases 1–4 of the netplay campaign. The exact list:

- `src/engine/engine.cpp`
- `src/engine/engine.h`
- `src/engine/engine_combat.cpp`
- `src/engine/engine_init.cpp`
- `src/engine/engine_net.cpp`
- `src/engine/engine_render.cpp`
- `src/engine/engine_render_entities.cpp`
- `src/engine/engine_startgame.cpp`
- `src/engine/engine_update.cpp`
- `src/engine/engine_update_player.cpp`
- `src/game/item.h`
- `src/game/projectile.h`
- `src/net/client.cpp`
- `src/net/client.h`
- `src/net/net.cpp`
- `src/net/net.h`
- `src/net/packet.h`
- `src/net/snapshot.cpp`
- `src/net/snapshot.h`

- [ ] **Step 1: Stage netplay campaign files**

Run:
```bash
git add \
  src/engine/engine.cpp \
  src/engine/engine.h \
  src/engine/engine_combat.cpp \
  src/engine/engine_init.cpp \
  src/engine/engine_net.cpp \
  src/engine/engine_render.cpp \
  src/engine/engine_render_entities.cpp \
  src/engine/engine_startgame.cpp \
  src/engine/engine_update.cpp \
  src/engine/engine_update_player.cpp \
  src/game/item.h \
  src/game/projectile.h \
  src/net/client.cpp \
  src/net/client.h \
  src/net/net.cpp \
  src/net/net.h \
  src/net/packet.h \
  src/net/snapshot.cpp \
  src/net/snapshot.h
```

- [ ] **Step 2: Confirm scope**

Run:
```bash
git diff --cached --stat
```

Expected: only the 19 listed files staged, no others. Approximate count:
```
 19 files changed, ~1300 insertions(+), ~280 deletions(-)
```

- [ ] **Step 3: Verify only deprecation markers + docs remain in the working tree**

Run:
```bash
git status --short
```

Expected output:
```
 M .claude/skills/engine-reference/SKILL.md
```

(That's the only file expected to remain unstaged. If anything else shows up — e.g., a forgotten `src/` file — stop and triage before committing.)

- [ ] **Step 4: Commit the campaign wrap-up**

Run:
```bash
git commit -m "$(cat <<'EOF'
net: wrap up netplay campaign Phases 1-4 (M0 baseline)

Finalizes the in-flight netplay work as a checkpoint baseline before
the from-scratch rewrite begins (see design doc in
~/.claude/plans/multiplayer-should-feel-like-curried-coral.md).

What lands here:

Phase 1 — Reliable-ish fire dispatch:
  - CL_FIRE_WEAPON unreliable packet (origin/yaw/pitch + clientTick)
  - Client-side manual retransmit ring (s_clientFireTx) for ~3 ticks
  - Server-side dedup ring (s_fireDedupRing) per slot
  Replaced in M10 by reliable channel; deprecation marker added in
  Task 7 of this M0 plan.

Phase 2 — Client-side prediction of hit feedback:
  - Predicted melee/hitscan hit query runs against m_renderInterp.entities
    (what the player sees) on CLIENT, spawning impact FX immediately
  - Predicted projectile collision against interpolated pool with a
    50ms grace window before the ghost can self-despawn
  - Damage numbers ride SV_EVENT::DAMAGE_NUMBER (reliable)

Phase 3 (scaffold) — Lag compensation history ring:
  - s_entHistory[MAX_ENTITIES][LAG_COMP_HISTORY_TICKS] structures
  - pushEntityHistory() pushed every TICKS_PER_SNAP server tick
  - beginLagComp/endLagComp/computeLagCompTicks API stubs
  Wired up to hit resolution in M5.

Phase 4 — Smoother remote interp:
  - SNAPSHOT_RATE 20→30 Hz, INTERP_DELAY 100→50 ms
  - Forward extrapolation up to 120 ms when past newest snapshot
  - Entity animTimer / item bobTimer ticked locally on CLIENT
  - SnapEntity.attackAnimQ for remote attack-swing visibility
  - Entity slot-recycle guard via enemyTypeId match

N4 — Ghost-sim removal:
  - AI / projectiles / entity timers / world items skipped on CLIENT
  - Predicted projectile ghost (Projectile.predicted/clientTick) still
    ticks locally for collision-vs-interp pool prediction
  - Movement collision and minimap source use m_renderInterp.entities
    on CLIENT (no more "blocked by dead ghost")

Floor descent rewrite:
  - CL_REQUEST_DESCEND packet (any client can request)
  - Server-side validation (proximity, alive, boss-dead)
  - Shared triggerFloorDescent() flow for host and remote
  - SV_LEVEL_SEED broadcasts the next floor's RNG seed

Wire additions: clientTickLow on SnapProjectile, attackAnimQ /
animFlags / bossStatus / halfExtents on SnapEntity, playerClass on
SnapPlayer, item ownership/exclusivity timers on SnapWorldItem.

What is INTENTIONALLY kept-for-now-but-deprecated (see Task 7 marker
commit, and the rewrite design doc):
  - Client-authoritative position model (posXQ/Y/Z on NetInput) —
    reversed in M3 when client-side prediction + replay reconciliation
    lands.
  - Manual fire-retransmit ring — replaced in M10 by ENet reliable
    channel.
  - Predicted-ghost fire model (Projectile.predicted/clientTick +
    match-despawn pass) — reworked in M10/M11 into a unified
    prediction model where the server-spawned projectile is born at
    the lag-comp tick and the client's predicted projectile IS the
    canonical one until reconciled.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 5: Verify commit**

Run:
```bash
git log --oneline -4 && git status --short
```

Expected:
- Top commit is the campaign wrap-up
- The four commits below are the M0 trail
- `git status` shows only `.claude/skills/engine-reference/SKILL.md` remaining

---

## Task 7: Mark Deprecated Subsystems In Code

**Files:**
- Modify: `src/net/net_player.h` (deprecation block above `NetInput`)
- Modify: `src/engine/engine_combat.cpp` (deprecation comment above `s_clientFireTx` / fire retransmit machinery)

This task adds clear "DEPRECATED — replaced in MN" markers so a future reader sees the rewrite plan. No behavior changes.

- [ ] **Step 1: Read net_player.h to find the exact text to anchor the deprecation block**

Run:
```bash
sed -n '9,33p' src/net/net_player.h
```

Expected output: the comment block above `struct NetInput` describing the absolute-quantized + client-authoritative-position model.

- [ ] **Step 2: Replace the comment block above NetInput with a version that flags client-auth position as deprecated**

Use the Edit tool on `src/net/net_player.h`:

old_string:
```
// Input as received from a client (or captured locally for listen server host).
//
// Aim and position are sent as ABSOLUTE quantized values rather than deltas (the
// historic mouseDeltaX/Y design). Deltas were lossy under UDP loss: a dropped CL_INPUT
// permanently dropped its mouse delta and the server's yaw drifted behind the client's
// live camera ("shoot where I'm not aiming"). Absolutes are idempotent — a dropped
// packet only delays the next sync; nothing is lost. Same byte count for yaw/pitch.
//
// Position is also absolute (posXQ/Y/Z) — the server snaps the remote NetPlayer to
// this value with a max-delta sanity clamp instead of running its own moveAndSlide.
// Co-op trust model: the client is authoritative for player position, the server is
// authoritative for combat / HP / loot. A cheating client can only cheat their own
// movement (visible to themselves) — they can't fake damage or steal kills.
```

new_string:
```
// Input as received from a client (or captured locally for listen server host).
//
// Aim is sent as ABSOLUTE quantized values rather than deltas (the historic
// mouseDeltaX/Y design). Deltas were lossy under UDP loss: a dropped CL_INPUT
// permanently dropped its mouse delta and the server's yaw drifted behind the
// client's live camera ("shoot where I'm not aiming"). Absolutes are idempotent —
// a dropped packet only delays the next sync; nothing is lost. Same byte count
// for yaw/pitch.
//
// DEPRECATED (M3, rewrite design doc): posXQ/Y/Z carry a "trust the client"
// position which the server snaps onto NetPlayer with a 4× speed sanity clamp.
// The full rewrite reverses this: server simulates movement from moveFlags +
// yaw via PlayerController, client predicts locally and replays inputs forward
// on snapshot reconcile. This struct will lose the position fields in M3 and
// the input pipeline will gain a clientTick / ackedSnapshotTick header for the
// new prediction model. Do NOT add new readers of posXQ/Y/Z; new code should
// assume the server is authoritative for position.
```

- [ ] **Step 3: Find the fire-retransmit ring declaration in engine_combat.cpp**

Run:
```bash
grep -n 's_clientFireTx\|FIRE_TX_REPEATS\|s_fireDedupRing' src/engine/engine_combat.cpp | head -10
```

Expected: a small number of hits naming the relevant statics. Note the line numbers — these anchor the next edit.

- [ ] **Step 4: Add a deprecation block above the s_clientFireTx declaration**

The exact location depends on what grep returns (the staged campaign code may have moved). Use the Edit tool on `src/engine/engine_combat.cpp`, anchored on whatever line is the first declaration of `s_clientFireTx`. Add a comment block immediately above the declaration.

Block to add (place above the static):
```
// DEPRECATED (M10, rewrite design doc): manual unreliable+retransmit ring
// reimplementing reliable transport in userspace. The rewrite replaces this
// with ENet's reliable channel for CL_FIRE_WEAPON (smaller wire footprint,
// ENet handles backoff). Kept for now because reliable channel scaffolding
// arrives in M10 — removing the retransmit before M10 lands would regress
// fire delivery under UDP loss. Do not extend this pattern to new packet
// types.
```

If `grep` shows multiple declarations (struct, instance, plus surrounding helpers), put the block above the first one only. The Edit must produce a unique match — include enough surrounding context (the actual declaration line plus 1-2 lines around it) in the `old_string`.

- [ ] **Step 5: Find the predicted-ghost fire model in engine_combat.cpp / projectile.h**

Run:
```bash
grep -n 'predicted\|clientTick' src/game/projectile.h
```

Expected: the lines declaring `bool predicted`, `u32 clientTick`, `f32 predictedLife` in the `Projectile` struct.

- [ ] **Step 6: Add a deprecation block above the Projectile prediction fields**

Use the Edit tool on `src/game/projectile.h`. Anchor on the existing fields (whatever exact form they have) and put this comment above them:

```
// DEPRECATED (M10/M11, rewrite design doc): the "predicted ghost" fire
// model where the client spawns a local-only Projectile with predicted=true
// and matches it to an authoritative server projectile via clientTickLow.
// The rewrite unifies this: the server spawns the projectile at the lag-comp
// tick (so it's born where the client launched it), and the client's
// predicted projectile IS the canonical one until reconciliation arrives.
// `predicted` and `clientTick` will be replaced by a single source-of-truth
// projectile flagged with its origin (server-authoritative vs unconfirmed).
// Do not add new readers of `predicted` outside the existing match-despawn
// path.
```

- [ ] **Step 7: Build to verify the comment additions didn't break anything**

Run:
```bash
cmake --build build 2>&1 | tail -5
```

Expected: still builds clean. (Comment-only edits can't break compilation, but verify in case an Edit accidentally hit code.)

- [ ] **Step 8: Stage and commit the deprecation markers**

Run:
```bash
git add src/net/net_player.h src/engine/engine_combat.cpp src/game/projectile.h
git diff --cached --stat
```

Expected: 3 files, comment-only changes.

Then:
```bash
git commit -m "$(cat <<'EOF'
net: mark M3/M10/M11 deprecation surfaces in code

Flags three subsystems that the netplay rewrite (see
~/.claude/plans/multiplayer-should-feel-like-curried-coral.md)
replaces in their respective milestones:

  - net_player.h NetInput.posXQ/Y/Z + the "trust client position"
    co-op trust model — reversed in M3 by client prediction + replay
    reconciliation against a hard-authoritative server.
  - engine_combat.cpp s_clientFireTx manual retransmit ring —
    replaced in M10 by ENet's native reliable channel for
    CL_FIRE_WEAPON.
  - projectile.h Projectile.predicted/clientTick predicted-ghost
    fire model — reworked in M10/M11 into a single source-of-truth
    projectile with origin authority tracking.

Behavior unchanged. Comment-only commit so a reader who walks into
these files knows they are scaffolding, not the canonical design.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 9: Verify commit**

Run:
```bash
git log --oneline -5 && git diff HEAD~1 HEAD --stat
```

Expected: top commit is the deprecation markers; stat shows only comment-line additions across the 3 files.

---

## Task 8: Update CLAUDE.md and engine-reference Skill

**Files:**
- Modify: `CLAUDE.md` (add reference to rewrite design doc)
- Modify: `.claude/skills/engine-reference/SKILL.md` (update netplay section to reflect M0 state + point to rewrite)

- [ ] **Step 1: Read CLAUDE.md's Architecture section to find where to add the rewrite-doc pointer**

Run:
```bash
grep -n 'Authoritative server\|Split-screen\|Knowledge skills' CLAUDE.md
```

Expected: a few line numbers. The pointer to the rewrite doc belongs near the "Authoritative server" paragraph in the Architecture section.

- [ ] **Step 2: Add a one-line pointer to the rewrite design doc**

Use the Edit tool on `CLAUDE.md`. Find the paragraph ending with `Singleplayer (`NetRole::NONE`) is just the same loop without packets. (Tick/snapshot/wire details: `engine-reference`.)` and add a new paragraph immediately after:

old_string:
```
**Authoritative server.** Listen-server model: host runs the full simulation in `serverUpdate` and is also player slot 0. Clients send `NetInput` packets at 60 Hz, server broadcasts `WorldSnapshot` at 20 Hz (every 3 ticks). Clients run prediction + reconciliation on the local player (`Client::reconcile`) and interpolate remote players/entities/projectiles with a 100 ms delay. Singleplayer (`NetRole::NONE`) is just the same loop without packets. (Tick/snapshot/wire details: `engine-reference`.)
```

new_string:
```
**Authoritative server.** Listen-server model: host runs the full simulation in `serverUpdate` and is also player slot 0. Clients send `NetInput` packets at 60 Hz, server broadcasts `WorldSnapshot` at 30 Hz (every 2 ticks). Clients run prediction + reconciliation on the local player (`Client::reconcile`) and interpolate remote players/entities/projectiles with a 50 ms delay. Singleplayer (`NetRole::NONE`) is just the same loop without packets. (Tick/snapshot/wire details: `engine-reference`.)

**Netplay rewrite in progress.** The netplay stack is being rewritten end-to-end to feel like singleplayer over the internet (target: 2–4 player co-op at ≤100 ms RTT, Quake/Hellgate-pace action). The design is at `~/.claude/plans/multiplayer-should-feel-like-curried-coral.md` and breaks the work into 14 milestones (M0–M14). Conflict surfaces left in the current code are flagged with `DEPRECATED (Mx, rewrite design doc)` comments — do NOT extend those patterns in new code. The rewrite reverses the current "client-authoritative position" model back to server-authoritative + client prediction + lag compensation.
```

- [ ] **Step 3: Read engine-reference SKILL.md's netplay section to find the line(s) referring to client-authoritative position or the old snapshot rate**

Run:
```bash
grep -n 'client-authoritative\|SNAPSHOT_RATE\|posXQ\|trust.*client\|INTERP_DELAY' .claude/skills/engine-reference/SKILL.md
```

Expected: a small number of hits. The lines to update are whichever document the "trust client position" model or stale numbers (20 Hz snapshots / 100 ms interp delay).

- [ ] **Step 4: Update the engine-reference netplay section**

Use the Edit tool on `.claude/skills/engine-reference/SKILL.md` against whatever the grep returned. The edits:

For any line that says "client-authoritative for player position" or similar trust-client framing, replace it with a sentence noting the campaign baseline still uses it but the rewrite reverses it:
```
Player position is currently client-authoritative (NetInput.posXQ/Y/Z; server snaps with 4× speed clamp). This is being reversed in M3 of the netplay rewrite — see ~/.claude/plans/multiplayer-should-feel-like-curried-coral.md. New code must not depend on this model.
```

For any reference to the old snapshot rate (20 Hz) or interp delay (100 ms), update to current (30 Hz / 50 ms) so the doc matches reality post-Phase 4.

Exact edits depend on what's in the file — keep them minimal and tied to the specific stale phrases grep surfaced. If grep returns nothing relevant, skip Step 4.

- [ ] **Step 5: Stage and commit docs**

Run:
```bash
git add CLAUDE.md .claude/skills/engine-reference/SKILL.md
git diff --cached --stat
```

Expected: 2 files, modest insertion counts.

Then:
```bash
git commit -m "$(cat <<'EOF'
docs: point CLAUDE.md + engine-reference to netplay rewrite design

CLAUDE.md gets a paragraph in the Architecture section noting the
rewrite is in flight, where the design lives, and that DEPRECATED
markers in code indicate replacement plans (don't extend those
patterns).

engine-reference SKILL.md netplay section updated for the M0 baseline
numbers (snapshots 30 Hz, interp delay 50 ms) and flags
client-authoritative position as M3 rework-pending so a future reader
of the engine-reference doesn't take the trust-client model as
canonical.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 6: Verify**

Run:
```bash
git log --oneline -6 && git status --short
```

Expected: top of log is the docs commit; `git status --short` is empty (clean tree).

---

## Task 9: Final Build & Smoke Verification

**Files:** none (verification only)

- [ ] **Step 1: Confirm working tree is clean**

Run:
```bash
git status --short | wc -l
```

Expected: `0`

If not 0, identify what's still uncommitted and decide whether it belongs to a topic above (re-stage + amend) or is genuinely new untracked work (separate commit or `.gitignore` entry).

- [ ] **Step 2: Confirm M0 commit trail**

Run:
```bash
git log --oneline master ^b102492
```

Expected: 5–6 commits in this order (top = newest):
```
<hash> docs: point CLAUDE.md + engine-reference to netplay rewrite design
<hash> net: mark M3/M10/M11 deprecation surfaces in code
<hash> net: wrap up netplay campaign Phases 1-4 (M0 baseline)
<hash> menu: support Continue-Join (join a host with a loaded save)
<hash> render: per-class player visuals (mesh + skin material)
<hash> tools: add mesh/skin asset generators and co-op smoke script
```

(Order may vary if topics were committed in a different sequence — the count is what matters.)

- [ ] **Step 3: Clean build from scratch (sanity check)**

Run:
```bash
cmake --build build 2>&1 | tail -10
```

Expected: builds clean. If anything fails, the working tree's "logically grouped" commits introduced a build break that wasn't visible before — fix and amend the relevant commit.

- [ ] **Step 4: Two-process co-op smoke (post-baseline)**

Repeat the manual smoke from Task 1 Step 3. Both players must:
- Spawn in the same dungeon
- See each other move smoothly
- See fire / projectiles on each other's screens
- See damage / kills on enemies on both screens
- Be able to pick up loot, descend floors, die / respawn

Any regression vs the Task 1 baseline means a commit in Tasks 3–7 broke something — bisect commits, identify, fix, and create a NEW commit (do not `--amend` the prior commit; this user prefers new commits over amends per their stored preferences).

- [ ] **Step 5: Record final state**

Run:
```bash
git log --oneline -8 && git status --short
```

Expected:
- 8 lines of log (the 5–6 M0 commits + the prior `b102492`/`b3ea205` baseline)
- Empty `git status --short`

M0 is complete. Working tree is clean, M0 baseline is committed in coherent topic-based commits, deprecation surfaces are flagged for M3/M10/M11, and docs point to the rewrite design.

---

## What This Plan Does Not Do

- **Does not change runtime behavior.** Every commit is a "carry forward" of in-flight work + comment updates. The game plays the same after M0 as before.
- **Does not revert client-authoritative position.** Comments now say it's deprecated; the actual reversal happens in M3.
- **Does not replace the manual fire-retransmit ring.** Replaced in M10.
- **Does not rework the predicted-ghost fire model.** Reworked in M10/M11.
- **Does not add tests.** The rewrite design's verification flow runs at the end of each subsequent milestone (M1+), not at M0.

## Definition of Done

- [ ] `git status --short` returns empty
- [ ] `git log master ^b102492 --oneline | wc -l` returns 5 or 6
- [ ] `cmake --build build` succeeds clean
- [ ] Two-process co-op smoke functions equivalently to the Task 1 baseline
- [ ] `grep -rn 'DEPRECATED.*rewrite design doc' src/` returns at least 3 matches (one per flagged subsystem)
- [ ] CLAUDE.md mentions the rewrite design doc path
