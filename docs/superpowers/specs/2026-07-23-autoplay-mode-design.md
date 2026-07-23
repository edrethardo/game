# Autoplay — the game plays itself

**Date:** 2026-07-23
**Status:** Approved (brainstormed with Aaron)

## Problem / idea

A new game mode, **Autoplay**, selected in the main menu instead of Singleplayer: pick a
character and the game plays itself — runs the floors, fights enemies, descends floor to
floor. The player can open the inventory to change the Auto-Loot build at any time, and can
grab control whenever they like; the bot takes back over after ~2 s of no player input.

## Decisions (made during brainstorm)

| Question | Decision |
|---|---|
| Purpose | A **real idle/AFK play mode** — loot, XP and saves are all real, same as singleplayer |
| On death | **Auto-respawn and keep farming** (the safe Respawn branch; never the destructive Reload-save) |
| Bosses | **Fight everything** — bosses are just harder floors; die-and-retry is acceptable |
| End of run | **Keep farming**: the engine's own difficulty ladder (Normal→Nightmare→Hell at floor 50) carries it; a cleared hero re-enters via Free-Play (deepest unlocked difficulty, floor 1) and loops |
| Entry points | **Menu mode only** — no in-run toggle for normal SP runs |
| Build doctrine | The bot doesn't just wear the build — it **plays** it: the 3×3 cell parameterizes combat behavior and positioning (Aaron's addition) |
| Architecture | **A: input-layer bot + brain module** (rejected: extending the wire-level `--bot-walk`, which is dead in SP and demonstrably rots; rejected: entity-style direct control, which bypasses dodge/CC/credit systems) |

## Architecture

Two units with one interface between them:

1. **The brain** — a new `autoplay` module (pure decision core + thin engine glue), ticked
   once per sim tick for the bot-controlled lane. It always emits the same three things:
   a move vector, desired yaw/pitch, and a set of held/pressed `GameAction`s.
2. **The control seam** — a small synthetic-input overlay at the input-abstraction layer
   that makes those intents indistinguishable from human input, plus direct
   `player.yaw/pitch` writes for aim (camera and — if ever networked — wire aim follow
   automatically, since the wire packs absolute angles).

Because the bot IS input, every existing player system runs unchanged: jump assist, dodge
i-frames, CC/stun suppression, the skill-cast preamble (unlock gating, energy copy, scaling
stamps), kill credit / loot attribution ambient statics, and interact arbitration. This is
the load-bearing property of the design; any shortcut that drives the player below the
input layer re-creates the `--bot-walk` fire bit (set on the wire, consumed by nothing).

### Verified engine facts the design leans on

- **SP consumes no `NetInput`** — `captureLocalInput` never runs under `NetRole::NONE`, so
  the wire-level `--bot-walk` seam is dead in singleplayer (it actively freezes the player).
  The action layer is the only seam covering all scattered consumers (fire, dodge inside
  `PlayerController::update`, block, skills, potion, interact, reload).
- **The SP world keeps running with the inventory open** (only the local player is frozen
  via `gameplayInputFrozen()`); the pause menu and character-inspect screen hard-freeze SP.
- **A per-floor BFS flow field toward the exit already exists**
  (`LevelGridSystem::buildFlowField` / `flowDirection`) and friendly NPCs already walk it;
  `Pathfinder::findPath` is capped at 256 closed cells — short detours only, never
  whole-floor routes.
- **Boss floors place the exit inside the boss arena** and seal it until the boss dies
  (`floorHasBoss && floorBossAlive()`), so "walk to the exit" naturally becomes the fight.
- Descent is a one-tick request (`m_descendRequested`) consumed by `updateFloorDoor`, which
  owns the boss gate / demo intercept / autosave chain — the bot sets the request flag
  inside the 2 m (full-3D) door radius and never calls `triggerFloorDescent` directly.
- **Difficulty laddering is automatic** in `triggerFloorDescent` (floor 50 → next
  difficulty, floor 1); Hell 50 → credits → VICTORY → town is the only stopping point.
- Auto Loot & Equip (`autoMode`/`buildCell`, vacuum, auto-equip, bag pruning, build grid
  UI) already solves the entire gear game; the build grid is pad-reachable and re-gears on
  the spot.

## Mode entry & flow

- **Main menu row "Autoplay"** below Single Player (hidden in the demo build). The menu's
  five hand-mirrored sites (labels/colors/count, mouse hit-test count, `maxSel`, action
  switch, `demoActionMap`) are one atomic edit — a documented drift trap.
- Selecting it sets a session-only **`m_menu.autoplay`** intent flag (the `m_menu.arena`
  pattern; cleared on every main-menu confirm) and re-enters the normal chain:
  New/Continue → save slot → class select. The play-style chooser (subState 23) is
  **skipped**: Autoplay force-sets `autoMode = 1` (Auto Loot is the bot's gear brain) and
  keeps the character's persisted `buildCell` (new characters: `DEFAULT_BUILD_CELL`). No
  new menu subStates (0–24 are taken; 25 stays free).
- The solo-start site consumes the flag: starts exactly like Singleplayer (same
  `startGame` / Continue / town routing) and arms the bot for lane 0.
- **Nothing persists**: no save-format change; a save written during autoplay is a normal
  SP save, freely continued in plain Singleplayer (and vice versa — an autoplay session on
  a Continue character is just another way to play it).
- Dev door **`--autoplay`** following the `--autoloot` pattern; combinable with `--vhall`,
  `--fourstory`, `--lava` for traversal testing.

## The control seam

- **Synthetic actions:** the overlay lets the bot hold/press real `GameAction`s with the
  same one-pressed-edge-per-render-frame semantics human presses have (mirroring
  `consumePressedState`); a bot **movement vector** merges where stick input merges; aim is
  written to `player.yaw/pitch` before `PlayerController::update`. All bot state is
  **per-lane** from day one (v1 arms lane 0 only); any new per-lane Engine field joins the
  `LOCAL_PLAYER_SWAP_FIELDS` X-macro.
- **Takeover:** a new small `Input` API exposes the per-frame human-activity booleans the
  device-glyph system already computes internally (kbm + per-pad, threshold-filtered:
  stick deadzone 0.5, mouse ≥ 6 px Manhattan — so pad drift and Steam Deck trackpad noise
  can't wrestle the bot). Real gameplay-device activity → the player has control **that
  same frame**; a 2.0 s no-activity timer hands control back. The transition frame
  consumes pending look deltas so a mouse nudge can't yank the aim mid-handoff.
- **UI interactions never count as takeover**: opening/browsing the inventory, pause, ESC,
  and menu-nav keys don't grab control — the bot keeps playing underneath. (This is what
  makes "change the build while it fights" work.)
- **Freeze carve-out:** `gameplayInputFrozen()` exists to stop human UI clicks leaking
  into gameplay; while the bot holds control, bot-sourced input bypasses those gates (the
  SP world already keeps running with the inventory open). The pause menu and
  character-inspect screen still hard-freeze everything, bot included.
- **Stun/CC correctness for free:** because bot actions flow through the same consumers,
  the existing stun gates suppress the bot exactly as they suppress a human.

## The brain

A priority-ordered state machine; every state emits (move, aim, actions). The decision
core is pure (grid/entity/player data in, intents out) so it unit-tests without the engine
— the `BuildScore`/`CrowdControl` pattern.

**States (priority order):** SURVIVE → FIGHT → LOOT-SETTLE → TRAVEL → DESCEND; special
cases BOSS-FIGHT (a FIGHT flavor), RESPAWN, STUCK-RECOVER.

- **Hazard veto (always on, all states):** every steering intent is checked against lava
  cells, un-landable gaps and drop holes (`StoryNav::planVault` + lava lookahead) — kiting
  can never back the bot into a lake or off a balcony. Jump decisions check active
  slows/freezes first (jump reach scales with effective speed).
- **SURVIVE:** potion via the synthetic action at the doctrine's threshold; detour to
  health/energy globes (3 m walk-over pickups) when low; disengage per doctrine.
- **FIGHT:** targets via the existing combat queries (they already filter dead / friendly
  / props / burrowed). Projectile aim through `LeadAssist::interceptTime` (exact
  closed-form intercept, already unit-tested — deliberately not the enemies' cruder
  one-step lead); fire confirmed by ray test (`rayNearestEntity`) so the crosshair is
  actually on the target; melee aims yaw only (the cone is horizontal). Skills cast
  through the synthetic rail so the unlock/energy/scaling preamble runs unmodified.
  Reload handled; synthetic dodges assert the desired WASD direction on the same tick as
  the press (dodge direction derives from held movement keys). Shrines activated
  (strict upside); chests opened, accepting mimic ambushes as fights the doctrine can
  take; the loot goblin chased only by Ranged/Magic doctrines (1.35× speed check); **The
  Source portal on Hell 50 is hard-avoided**.
- **LOOT-SETTLE:** a short post-combat beat letting the auto-loot vacuum collect drops
  before moving on — until no `worthPickingUp` item remains in the vacuum radius, capped at
  ~3 s (the vacuum + auto-equip + pruning are entirely existing systems).
- **TRAVEL:** flow-field descent (`flowDirection`), disambiguating the zero-vector
  (at-exit vs unreachable — the raw flow byte distinguishes them); local A* detours with
  `bodyRadius` for blocked headings; per-style story layer:
  - **VERTICAL_HALL:** exit balcony at y=3 m — climb the serving ramp from
    `dungeon.portals[]` (diagonal corner); never attempt the broken catwalk (its 2-cell gap
    needs full run speed; the ramp route is strictly safer).
  - **FOUR_STORY:** spawn L3 → exit L0; pick drop holes from `dungeon.dropHoles[]`
    filtered by the bot's current story (1-cell holes jumped over when crossing, entered
    when descending; ≥2-cell holes are committed falls); jump pads fire involuntarily on
    contact — pad cells are modeled as launches, and a bad fall recovers via recorded pads.
  - **Hellforge (lava):** flow field and A* treat lava as walkable — the brain does not:
    lava cells are walls to steering except the stepping-stone causeways and 1-cell veins,
    which are jumped at speed.
  - Sentinel worlds (town/arena/Source chamber) have flow fields pointing at their
    centres, not an exit — the bot only ever drives normal dungeon floors and idles
    elsewhere.
- **DESCEND:** inside the 2 m full-3D door radius, set the descend request (never a
  synthesized tap — the interact arbitration would grab loot forever with drops in reach;
  never `triggerFloorDescent` directly — it would skip the boss gate and autosave chain).
- **BOSS-FIGHT:** no special pathing — the exit's flow field walks into the sealed boss
  arena; FIGHT handles the rest; the door unseals on kill via the existing gate.
- **RESPAWN:** on `GAME_OVER`, wait a short visible beat (~1.5 s), then drive the safe
  Respawn branch (full heal, floor entrance, enemies already walked home). Never
  Reload-save (destructive: wipes session source shards).
- **STUCK-RECOVER:** friendly-NPC-style stuck detection (no progress over a window →
  re-path; persistent → cell-center recovery).

### Build doctrine (the 3×3 cell plays differently)

Doctrine = one small parameter table keyed by build cell (engagement band,
commit/disengage thresholds, potion %, dodge/block policy, cover usage) + per-column
behaviors. Switching the cell in the inventory re-gears (existing) AND swaps doctrine live.

| | Magic (col 0) | Melee (col 1) | Ranged (col 2) |
|---|---|---|---|
| **Tanky** (potion 35%, block often, retreat rarely) | Mid-range anchor: casts on cooldown, holds ground, blocks between casts | Face-tank: commits into packs, keeps enemies in the front cone, fights chokepoints/doorways | Standing gunline: holds ~0.8× range, blocks during reloads instead of running |
| **Moderate** (potion 50%, balanced) | Cast rotation + wand pokes, strafes for LOS | Commits but disengages when surrounded (>3 in melee arc) | Classic kite: 0.55–1.0× range band, strafes with LOS (the enemy keep-away policy, inverted) |
| **Glass Cannon** (potion 60%, dodge early, never get touched) | Max-range artillery: casts from cover edges, breaks LOS on cooldowns, dodges anything closing | Hit-and-run: dodge in, burst, dodge out; never stands in a pack | Hard kite at max range: cover-cell reloads (`findCoverCell`), flees multiple closers, prefers high ground on stacked floors |

Spatial primitives (all existing): width-aware LOS, cover-cell + flank-cell grid queries,
the flow field for repositioning, `StoryNav` for vertical play.

## UI & feedback

- **"AUTO" strap** on the floor indicator (which already carries mode labels like
  "Arena"); flips to "MANUAL · Ns" with the resume countdown while the player holds
  control. Lives in the normal HUD branch → F10 hide-HUD and pause-hide come free.
- Bot milestones (respawned, descended, doctrine/build switched) announce via the existing
  chat line, like auto-equip does.
- No new bindings, screens, or panels; the build grid / inventory / pause surfaces are
  reused untouched, so Switch pad-reachability holds by construction.

## Error handling

- Unreachable-exit states (bad fall into a sealed pocket, flow byte 0xFF) → STUCK-RECOVER;
  if recovery fails persistently, the bot idles and announces it in chat rather than
  thrashing (the player can grab control; this is also the honest failure mode).
- The bot never acts in sentinel worlds (town/arena/Source chamber) or non-`IN_GAME`
  states except the RESPAWN branch in `GAME_OVER` and post-credits re-entry.
- A cleared hero with no dungeon to farm re-enters via Free-Play (deepest unlocked
  difficulty, floor 1); if that flow is unavailable for any reason, autoplay parks in town
  and says so.

## Testing

- **Pure-core doctest units:** doctrine selection per cell; hazard veto on synthetic grids
  (lava lakes, gaps, drop holes — steering intents must be vetoed); takeover/resume state
  machine (instant grab, 2.0 s handback, UI keys never grab); target-selection policy;
  descend gating (never requests descent while the boss lives; never touches the Source
  portal); input-overlay edge semantics (one pressed-edge per render frame).
- **Live acceptance (dev doors):** `--autoplay` alone and with `--vhall` / `--fourstory` /
  `--lava`; measured pass: the bot completes floors 1–10 unattended (including the floor-5
  and floor-10 bosses), one Descent floor, and one Hellforge floor.
- Full suite stays green; no existing test weakened.

## Out of scope (v1)

- Multiplayer/couch bot lanes (the seam is per-lane-ready; no wire work now).
- An in-run autoplay toggle for normal SP runs.
- Sim-speed / fast-forward.
- Balance-lab metrics emission — noted as the natural follow-up: this bot IS the empirical
  rig the balance-lab spec deferred, but v1 ships as a game feature, not an instrument.
- Arena/town autoplay (sentinel worlds pause the bot by design).

## Docs to update on landing

- CLAUDE.md: an Autoplay paragraph (mode flag pattern, the control seam, the freeze
  carve-out, per-lane rule).
- `engine-reference`: the new Input activity API + any new constants; debug-keys table if
  a debug toggle is added.
- `engine-how-to`: pitfalls (menu five-site mirror, interact arbitration vs descend
  request, flow-field zero-vector ambiguity, lava-is-walkable trap).
