# Close the Remaining Client/Server Prediction Asymmetries

A targeted netcode improvement: bring the client's local `PlayerController::update`
and the server's `PlayerController::updateNetPlayerFromInput` into exact parity
for the two motion paths that still diverge — **freeze/slow status effects**
and **dodge roll**. Also redefine freeze as a heavy slow (0.15×) rather than a
hard stop, so the visual feel matches "barely moving" instead of "rooted".

## Context

The netplay rewrite (master plan: `~/.claude/plans/multiplayer-should-feel-like-curried-coral.md`)
has shipped through rounds R3–R10. Continuous shake is gone. The R10 render-
offset cap masks the residual visible jitter, but the underlying **sim-level
divergence** between client prediction and server authority still fires on two
known events:

1. **Freeze (and slow) on the local player.** The server's `updateNetPlayerFromInput`
   ([player.cpp:240-248](src/game/player.cpp#L240-L248)) checks both `slowTimer`
   and `freezeTimer`. The client's `PlayerController::update`
   ([player.cpp:74-225](src/game/player.cpp#L74-L225)) checks `slowTimer`
   (line 120) but **not** `freezeTimer`. When an enemy projectile freezes the
   local player, the client keeps walking forward while the server halts —
   ~10 cm/tick divergence, every reconcile crosses the 10 cm threshold.

2. **Dodge roll movement.** The client's `PlayerController::update` overrides
   velocity to `rollDirection × ROLL_SPEED=8 m/s` for 0.5 s during a roll
   ([player.cpp:188-217](src/game/player.cpp#L188-L217)). The server only sets
   `np.invulnTimer = 0.3f` on `INPUT_EX_DODGE` ([player.cpp:287-289](src/game/player.cpp#L287-L289))
   — it never moves the player. ~13 cm/tick divergence over a 30-tick dodge,
   totalling ~4 m if uncorrected. The R10 cap stops the visual oscillation
   but the sim still snaps every reconcile.

The intended outcome: identical inputs → identical positions. The R10 render-
offset cap then has near-zero work to do, and combat movement feels exactly as
crisp as singleplayer.

A secondary, gameplay-design change ships in the same pass: **freeze becomes a
0.15× speed multiplier** (85% reduction) rather than a hard `effectiveSpeed = 0`
stop. Multiplicative with slow, just like soul-harvest/adrenaline buffs are.
Frozen players visibly crawl instead of being rooted in place; stacking with
slow produces 0.06× (a deliberate "you got hit by two debuffs" punishment) but
is otherwise consistent with how every other speed modifier in the system
compounds.

## Goals & Non-Goals

**Goals:**
- Local sim respects `freezeTimer`. Single host code path used by client
  prediction, host singleplayer, and host couch-co-op all behave the same.
- Server applies the dodge roll's 4 m movement on `INPUT_EX_DODGE` so
  `np.position` matches the client's predicted `m_localPlayer.position` to
  floating-point noise across the roll.
- Freeze redefined as `0.15× speed multiplier`, applied consistently on both
  client and server. Slow stays at `0.4×`.

**Non-goals:**
- Predicting status-effect application on incoming-projectile impact (today's
  D3.2 path predicts HP/visuals but not freeze/slow). Needs wire format
  changes for `onHitEffect`/`onHitDuration`; deferred.
- Reliable server confirm/reject for these predictions. Same reconcile-via-
  next-snapshot path the rest of the system uses; the closed asymmetries
  remove the *reason* divergences fire in the first place.
- Refactoring `PlayerController::update` and `updateNetPlayerFromInput` into
  a single shared function. They've diverged organically (host reads
  `Input::*`, server reads `NetInput`); a unified path is a bigger refactor.

## Design

### Commit 1 — Status-effect parity (freeze + slow speed multipliers)

**Files modified:**
- [src/game/player.cpp](src/game/player.cpp) — `PlayerController::update`
  (host/client path) AND `PlayerController::updateNetPlayerFromInput`
  (server path). Both add (or change) the freeze multiplier to 0.15×.

**Host/client path** ([player.cpp:102-123](src/game/player.cpp#L102-L123)):

Add after the slow block:
```cpp
if (player.freezeTimer > 0.0f) {
    effectiveSpeed *= 0.15f; // R12: heavy crawl, not a hard stop
}
```

The slow block at line 120-123 stays as-is (`effectiveSpeed *= 0.4f`).

**Server path** ([player.cpp:240-248](src/game/player.cpp#L240-L248)):

Replace the existing `if (np.freezeTimer > 0.0f) effectiveSpeed = 0.0f;` with
the same multiplicative form:

```cpp
if (np.freezeTimer > 0.0f) {
    effectiveSpeed *= 0.15f; // R12: heavy crawl, mirrors host path
}
```

This is the *only* gameplay change in C1. The slow check at line 240-244 stays
intact (it already correctly multiplies by 0.4× and defers timer decay to
`PlayerController::update`).

**Wire format:** no change. `freezeTimer` already ships on the wire and is
adopted by `Client::reconcile` ([client.cpp:382](src/net/client.cpp#L382)).

**Test coverage:** the freeze/slow speed checks aren't currently linked into
`dungeon_tests` (player.cpp not in the suite). Manual verification only this
pass; full coverage requires the `dungeon_core` library refactor noted in
[docs/superpowers/specs/2026-05-31-test-framework-design.md](docs/superpowers/specs/2026-05-31-test-framework-design.md).

### Commit 2 — Server-side dodge-roll movement

**Files modified:**
- [src/net/net_player.h](src/net/net_player.h) — add a single
  `DodgeState dodgeState;` field on `NetPlayer`. Reuses the existing
  `DodgeState` struct from [player.h:12-16](src/game/player.h#L12-L16);
  keeps server and client field names identical so future shared-function
  refactors stay easy.
- [src/game/player.cpp](src/game/player.cpp) — `updateNetPlayerFromInput`
  extended to start the roll on `INPUT_EX_DODGE` and override velocity each
  tick during the roll. Mirrors the client logic at [player.cpp:131-222](src/game/player.cpp#L131-L222)
  exactly.
- [src/engine/engine_net.cpp](src/engine/engine_net.cpp) — `serverNetPre`
  ticks `rollTimer` / `cooldownTimer` for active remote NetPlayers inside
  the same loop that already drains R9's per-remote skill cooldowns
  (lines around 85-100, just before the per-input loop).

**Mechanism:**

The wire already carries everything needed — no protocol changes.

- `INPUT_EX_DODGE` is the press event ([net_player.h:50](src/net/net_player.h#L50)).
- `input.yawQ` carries the yaw at the moment of dodge.
- `input.moveFlags` carries the W/A/S/D bits at the moment of dodge.

The server reproduces the client's `rollDirection` derivation
([player.cpp:142-153](src/game/player.cpp#L142-L153)) using `np.yaw` (just
unpacked from `input.yawQ` earlier in the same function call at line 268) and
the WASD bits from `input.moveFlags`. Result: bit-equal direction vector.

**Sequence inside `updateNetPlayerFromInput`** (after the existing
`applyMovement` call at line 270-279):

```cpp
DodgeState& ds = np.dodgeState;

// Start dodge on the press edge — mirror player.cpp:135-167.
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
    if (lengthSq(dir) < 0.001f) dir = flatFwd;
    ds.rollDirection = normalize(dir);
    ds.rolling       = true;
    ds.rollTimer     = 0.5f;
    np.invulnTimer   = 0.3f; // already set today by the existing block at L287
}

// Active roll — override velocity to constant ROLL_SPEED in the rolled direction.
// Mirrors the client's tick block at player.cpp:188-217. Runs BEFORE the per-input
// moveAndSlide loop integrates np.position (b524abe), so the same ROLL_SPEED * dt
// distance lands on the server that the client predicted.
if (ds.rolling) {
    constexpr f32 ROLL_SPEED = 8.0f;
    np.velocity.x = ds.rollDirection.x * ROLL_SPEED;
    np.velocity.z = ds.rollDirection.z * ROLL_SPEED;
}
```

The `applyMovement` call at the existing line 270-279 still runs first, so
WASD-driven motion populates the velocity. The dodge override above then
**overwrites** the horizontal components for the duration of the roll —
identical to the client's pattern at player.cpp:188-217 (where dodge runs
after applyMovement in `PlayerController::update`).

**Per-tick decay** alongside the R9 remote-cooldown loop at the top of
`serverNetPre`:

```cpp
for (u32 i = 0; i < MAX_PLAYERS; i++) {
    if (i == m_localPlayerIndex || !m_players[i].active) continue;
    DodgeState& ds = m_players[i].dodgeState;
    if (ds.rolling) {
        ds.rollTimer -= dt;
        if (ds.rollTimer <= 0.0f) {
            ds.rolling      = false;
            ds.rollTimer    = 0.0f;
            ds.cooldownTimer = 1.0f;
        }
    } else if (ds.cooldownTimer > 0.0f) {
        ds.cooldownTimer -= dt;
        if (ds.cooldownTimer < 0.0f) ds.cooldownTimer = 0.0f;
    }
}
```

Co-locating this with the R9 remote-cooldown tick loop keeps all
per-remote-player state advancement in one place.

**Wire format:** no change. The roll's existence is implied from
`INPUT_EX_DODGE` press events; subsequent direction/timer state stays
server-local (the client computes its own copy from the same inputs).

**Test coverage:** same caveat as C1 — `player.cpp` isn't linked into
`dungeon_tests`. Verification by manual co-op smoke (see Verification
section). A future doctest harness for `updateNetPlayerFromInput` could
drive synthetic inputs through 30 ticks of dodge and assert
`np.position` advances by ~4 m.

### What gameplay changes for the player

Freeze becomes a heavy slow (85% reduction) instead of a root. Frozen players
can still reposition, but extremely slowly — about 0.6 m/s at base move
speed. Visually clear "I'm frozen" feedback (player crawls), no rage from
being locked in place during a 0.5 s freeze. Stacks multiplicatively with
slow (frozen + slowed = 0.06× ≈ 0.24 m/s, a deliberately harsh "stop running
into ice mages while bleeding" penalty).

Dodge roll on remote clients (from the host's perspective, and from other
clients' perspectives via snapshot) now travels the full 4 m on the server —
no more "client's avatar slides while server thinks they didn't move." All
spectators see the rolling player move correctly through obstacles and arrive
at the rolled destination.

## Critical files

- [src/game/player.cpp](src/game/player.cpp) — both `PlayerController::update`
  (C1) and `updateNetPlayerFromInput` (C1 + C2).
- [src/net/net_player.h](src/net/net_player.h) — add `DodgeState` to
  `NetPlayer`.
- [src/engine/engine_net.cpp](src/engine/engine_net.cpp) — per-tick rollTimer
  / cooldownTimer decay alongside the R9 remote-skill-cooldown loop in
  `serverNetPre`.

Existing utilities reused (no edits):

- [src/game/player.h](src/game/player.h) — `DodgeState` struct already
  defined for the host-local Player; the new NetPlayer field reuses the
  same struct.
- [src/net/net_player.h](src/net/net_player.h) — `INPUT_EX_DODGE` bit
  already on the wire (line 50); `input.moveFlags` carries
  `INPUT_FORWARD`/`BACKWARD`/`LEFT`/`RIGHT` (lines used by both client and
  server paths today).
- [src/core/math.h](src/core/math.h) — `Vec3`/`cross`/`normalize` for the
  rollDirection calculation.

## Verification

1. `cmake --build build && ./build/tests/dungeon_tests --no-version` — all
   82 tests still pass; this pass adds no new tests because the touched
   files aren't yet in the suite.
2. **Freeze smoke (C1):**
   - Sorcerer host casts Frozen Orb at the client; client gets the freeze
     debuff applied via reconcile.
   - **Expected:** client's local view crawls at 0.15× speed while held
     forward, no longer fully halted. R10 net-graph `divergences=` count
     stays near zero during the freeze window (vs the prior every-tick
     reconcile snap).
3. **Dodge-roll smoke (C2):**
   - Wanderer client presses Shift to dodge. Both client and host see
     the avatar roll 4 m in the chosen direction.
   - **Expected:** the client's local view smoothly travels the full 4 m
     (no R10 cap saturation behind the camera). The host's view of the
     remote client shows the same 4 m roll (no laggy "remote player
     teleports forward" at the end of the roll). Net-graph
     `divergences=` per second drops markedly compared to before.
4. **Combined:** client gets slowed AND frozen by a stacked enemy hit.
   **Expected:** crawl at ~0.06× base move speed, both timers decay
   independently per snapshot. Once one wears off, speed jumps to the
   remaining multiplier.

## Deferred follow-ups (not this pass)

- **Status-effect prediction on incoming-projectile impact.** Extend the
  D3.2 path at [engine_update.cpp:605-657](src/engine/engine_update.cpp#L605-L657)
  to also apply `onHitEffect`/`onHitDuration` on predicted impact. Needs
  these fields on the wire (`SnapProjectile` extension).
- **Reliable `SV_DODGE_RESULT` / `SV_FREEZE_APPLIED` events.** Currently
  the client predicts and the next snapshot's reconcile fixes any drift.
  Reliable confirms would smooth edge cases (e.g., client predicted a
  dodge but the server's `cooldownTimer` says no). Same path as the
  deferred `SV_SKILL_RESULT` from R9.
- **Refactor `PlayerController::update` and `updateNetPlayerFromInput`
  into a single shared function** operating on raw pos/vel/yaw refs.
  Would eliminate this whole class of "remember to mirror the host fix
  into the server path" maintenance burden. Out of scope here.
- **`dungeon_core` library refactor** so `player.cpp` / `collision.cpp`
  link into `dungeon_tests`. Enables real unit coverage of these
  changes. See [docs/superpowers/specs/2026-05-31-test-framework-design.md](docs/superpowers/specs/2026-05-31-test-framework-design.md).
