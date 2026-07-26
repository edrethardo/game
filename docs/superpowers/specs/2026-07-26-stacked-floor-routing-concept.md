# Concept: Stacked-Floor Routing for Squishy Autoplay Builds

**Status:** PARTIALLY IMPLEMENTED (VHALL climb pass, 2026-07-26) — see §9
**Date:** 2026-07-26
**Author:** Autoplay investigation (freeze/fight-balance pass)

**Goal:** make Autoplay's ranged and caster builds reliably TRAVERSE the two vertical
floor styles — `VERTICAL_HALL` (climb UP to an upper-story exit) and `FOUR_STORY` /
"The Descent" (drop DOWN through clean holes) — the way the Warrior already does, without
regressing the "fight through floors, don't run to the exit" balance just tuned in `7ee372a`.

---

## 1. The two known problems (measured)

Both were captured live with the per-tick freeze diagnostic during the freeze investigation
(commits `7f38bd0` → `7ee372a`). Neither is the flat-floor livelock that pass already fixed.

### 1a. VHALL climb-roam
On a VERTICAL_HALL floor whose exit is on the **upper balcony** (`floorDoorPos.y > 1.5`), a
squishy build roams **both stories** (`pos.y` cycling 0 ↔ 3 m), briefly touches the exit
(`d2d` = 2 m) and then wanders back out to `d2d` = 63 m — never holding the balcony long
enough to descend. `bull=1`, `flow=1.00` the whole time: it HAS a valid two-story heading,
it just can't commit to the climb.

### 1b. FOUR_STORY return-lift bounce
On a Descent floor a ranged build descends a story or two, then a **return-lift jump pad**
flings it back up: `pos.y` cycles **9 → 6 → 3 → 0 → 9** for minutes. Measured floor-9 dwell:
189 samples on story 6, 34 on story 9, reaches L0 26 times but never stays. Root: on that
seed the story's **clean holes are severed from the landing pocket by pads**, so the descent
field's tier-2 recovery routes the bot ONTO a pad → launched → re-descend → severed → loop.

### Why the Warrior clears both and squishy builds don't
The discriminator is **movement discipline under combat**, not gear tier per se:

- The Warrior CLOSES distance to enemies. Enemies are scattered across the floor, so closing
  on them naturally walks the bot into new areas — and on a Descent floor, into clean holes;
  on a VHALL floor, up the ramp after a chased target. It also tanks the swarm, so nothing
  knocks it off the route.
- Ranged/caster builds KITE — they back away in a small, predictable arc. They don't explore,
  so they never stumble into the reachable clean hole / never get pushed up the ramp, and the
  swarm's knockback + the bot's own kite jog keep shoving it off the vertical route.

**Common thread:** the bot has a valid vertical heading (the VHallField / DescentField) but
**cannot COMMIT to the vertical objective under combat disruption.** The flat-floor bull
solved exactly this shape for the horizontal case ("commit to the door, punch through"); the
vertical case has no equivalent.

---

## 2. Design principle (carried over from the fight-balance pass)

Whatever we add must preserve the `7ee372a` balance:

1. **Fighting is the default.** A vertical-commit remedy is a LAST RESORT, armed only on a
   genuine livelock (the existing 16 s exit-progress window: no damage AND no exit approach),
   and released the moment combat is viable again (a kill).
2. **Dying is fine; freezing is not.** The remedy may be aggressive (jump, dodge, ignore
   fire) — the goal is to keep converging on the vertical objective, not to survive perfectly.
3. **No level-shape or wire change unless it buys robustness the driver can't.** Prefer a
   driver-only fix; escalate to level-gen only where the geometry itself is the problem.

---

## 3. Concept A — the **VERTICAL COMMIT** (a stacked-floor twin of the exit bull)

A single new remedy, symmetric with `m_autoplayExitBull`, that fires on stacked floors when
the same 16 s livelock window trips. Where the flat bull commits to walking the exit-XZ
heading, the vertical commit commits to the **story-changing route toward the exit** and
holds it through combat until the bot's story actually changes in the right direction.

### A.1 VHALL (climb up)
State to reuse: the existing `m_autoplayVhPortal` / `m_autoplayVhClimbing` / `m_autoplayVhCrossed`
machinery and the `VHallField`. On the commit:

- **Heading:** `v.flowDir` from the `VHallField` (already routes ground → ramp → balcony → door),
  OR beeline to the recorded **void jump-pad** if one is closer than the ramp (the pad is the
  fastest, most disruption-proof climb — one launch and you're up).
- **Discipline while committed:**
  - Suppress FIGHT movement (kite/strafe) — the climb owns the body, like the flat bull.
  - Keep the existing **fall veto** (`wouldFall`) on every WASD component (already present for
    VHALL upper-exit) so a commit can never walk the bot off the rim it just climbed.
  - Force the **climb-assist jump** (`m_autoplayVhClimbing`) continuously while below the exit
    height and on/approaching the ramp — the hop is what carries a jostled body up the narrow
    2-wide graduated slab.
  - Keep firing at LOS targets (a caster can still chip the swarm while climbing) but never
    let firing pull the aim off the climb heading.
- **Release:** when `feetOnStory(exitStory)` AND within the ramp-top / balcony XZ (the existing
  `m_autoplayVhCrossed` latch) — then hand back to the normal on-balcony beeline-to-door.
  Also release on a kill (fight is viable again) UNLESS still below the exit story (don't drop
  a half-finished climb to chase a straggler — that IS the roam).

### A.2 FOUR_STORY (drop down)
The vertical objective is the **nearest CLEAN drop hole on the current story**, then a
committed drop through it. On the commit:

- **Target:** `dropHoleCandidates()` already returns clean-first, nearest-first holes. Pick the
  nearest CLEAN one reachable in the descent field's TIER 1 (pad-free). If tier 1 can't reach
  any clean hole from the current pocket (the severed case), fall through to Concept B / B'
  below rather than routing onto a pad.
- **Heading:** the tier-1 DescentField toward that hole (never the tier-2 pad recovery while
  committed — routing onto a pad is the bug).
- **Discipline:** suppress FIGHT movement; keep the **body-aware pad veto** on every component
  so the commit itself never clips a return lift; walk onto the hole cell and let physics drop
  the bot (the drop IS the descent).
- **Release:** on `commitBotStory` reporting a story below the one we committed from (we
  dropped) — re-arm for the next story. Or on a kill while NOT mid-drop.

### A.3 Shared plumbing
- One latch `m_autoplayVertBull`, armed by the same 16 s window as the flat bull but gated to
  stacked floors; one release-on-kill rule; the two bodies (climb / drop) selected by layout.
- The flat `m_autoplayExitBull` stays for flat floors; the two are mutually exclusive by layout,
  so no interaction.

**Pros:** driver-only, no wire/save/level change; reuses proven machinery (VHallField, DescentField,
climb-assist jump, body-aware veto, fall veto); inherits the fight-balance gating for free.
**Cons:** VHALL A.1 should robustly fix climb-roam. FOUR_STORY A.2 only fixes the **non-severed**
case — a truly severed pocket (no clean hole reachable pad-free) still has nowhere to commit to,
which is Concept B's job.

---

## 4. Concept B — FOUR_STORY level-gen: **guarantee clean-hole reachability per pocket**

The deepest FOUR_STORY failure is geometric: on some seeds a story's clean holes are cut off
from a landing pocket by pad cells, so there is NO pad-free descent from that pocket — the bot
MUST use a return lift, which bounces it. Concept A can't route around geometry that doesn't
exist.

**Fix at the generator** (`world/level_gen.cpp` `carveFourStory`): after punching holes and
placing pads, run a per-story BFS (pad-excluded, same `routable` as the DescentField) from the
set of clean holes; any open cell NOT reached is a severed pocket. For each severed pocket,
either (a) convert its nearest padded hole to a clean one (remove the return lift beneath it),
or (b) punch one guaranteed clean hole inside the pocket. Pin it with a test in
`tests/world/test_four_story.cpp`: **from every open cell on every story (except L0), a clean
drop hole is reachable without crossing a `CELL_JUMPPAD`.**

**Pros:** removes the root cause; makes the DescentField's tier-1 always sufficient, so the
tier-2 pad recovery (the bounce source) never fires; seed-built, so **no wire/save change**.
**Cons:** touches level geometry (needs the reachability test + a visual sanity pass); a new
clean hole slightly lowers the "make you hunt for the way down" difficulty on deep stories
(mitigate by only guaranteeing ONE per severed pocket, not flooding).

This is the recommended durable fix for 1b; Concept A.2 is the driver-side belt-and-suspenders
for any pocket the guarantee somehow misses.

---

## 5. Concept C — VHALL: prefer the **void jump-pad** as the primary climb

Complementary to A.1. VHALL records a void `CELL_JUMPPAD` that flings the player a full story
up. A pad launch is far more disruption-proof than the narrow ramp (one grounded frame and
you're airborne, above the swarm). Concept: when the exit is upper AND a void pad is reachable,
route to the pad FIRST (beeline through the hazard veto), ride the launch, and let the
VHallField take over on the upper story. The ramp becomes the fallback when no pad is reachable.
Cheap, driver-only, and it sidesteps the "knocked off the narrow ramp" failure mode entirely.
(The current code already has a `climbingViaPad` path for FOUR_STORY-style recorded pads;
VHALL records the pad as geometry but not in `DungeonResult::jumpPads[]` — either record it, or
scan the grid for the void pad as the earlier VHALL pad-climb attempt did.)

---

## 6. Recommendation & phasing

1. **Phase 1 (driver, low risk): Concept A.1 + Concept C** — the VHALL climb commit + void-pad
   preference. This should close 1a outright; VHALL is purely a "commit to the climb" problem.
2. **Phase 2 (level-gen, durable): Concept B** — the FOUR_STORY clean-hole reachability
   guarantee + its test. This removes the geometric root of 1b.
3. **Phase 3 (driver, belt-and-suspenders): Concept A.2** — the FOUR_STORY descent commit, for
   any residual severed pocket, and to make squishy builds drop decisively rather than kite near
   a hole.

Phase 1 and Phase 2 are independent and can land in either order; Phase 3 rides on both.

---

## 7. Test / verification plan

- **Unit:** Concept B gets a `test_four_story.cpp` invariant (clean hole reachable pad-free from
  every open cell, all seeds × both grid sizes). Concept A's commit/release predicates should be
  extracted pure where possible (a `vertBullShouldRelease(...)` mirror) and unit-tested like the
  existing autoplay policy helpers.
- **Live (the rig we already have):** paired fresh Sorcerer + Marksman runs forced onto each
  style (`--vhall`, `--fourstory`), with the freeze diagnostic re-enabled, measuring: story
  trajectory (does it converge up / down?), time-on-floor, and — critically — the fight-balance
  numbers from §2 (bull/commit-active % should stay low, kills healthy). Success = both squishy
  builds finish a forced stacked floor in comparable time to the Warrior, with commit-active a
  low single-digit %.
- **Benchmark:** a 1 h 3-build soak (the one running now is the pre-concept baseline) after each
  phase, comparing max effective floor and per-floor dwell on x9 / VHALL floors.

---

## 8. Risks

- **VHALL fall veto vs. commit:** the commit forces jumps + suppresses the fall veto's usual
  producers; must keep the veto authoritative so a jump can't launch the bot off the rim. The
  veto is already per-component and authoritative — the commit must route THROUGH it, not around.
- **Concept B difficulty drift:** guaranteeing a clean hole per pocket slightly eases the deepest
  stories. Keep it to one-per-severed-pocket and re-check the `wall bulk` / hunt-for-the-way-down
  feel.
- **Release-on-kill mid-climb/drop:** must NOT release while below the exit story (VHALL) or
  mid-drop (FOUR_STORY), or the bot abandons a half-finished traversal to chase a straggler —
  which is the roam we're removing. Gate the kill-release on "already past the vertical objective".
- **Interaction with the flat bull:** keep them layout-exclusive; never let both latch.

---

## 9. What actually landed (VHALL climb pass, 2026-07-26)

Concept A.1 (VHALL vertical commit) shipped, plus four supporting fixes found by tracing live
`--autoplay --vhall` runs (fresh Sorcerer AND the geared paladin save_04, which isolates nav from
combat). All are gated to **VHALL floors whose exit is UPPER** (`floorDoorPos.y > 1.5`) so no other
floor type can regress. Nothing touched the wire or save format (driver-only, transient state).

1. **Airborne fall-veto carve-out** (`engine_autoplay.cpp`, the per-component veto now gated on
   `m_localPlayer.onGround`). `wouldFall` reads `feetY` at the jump apex, so while AIRBORNE every
   neighbour cell resolves far below the feet and all four directions veto at once — the commit's jump
   pulse became a POGO in place (measured: committed bot stuck at d2d≈40 m, `on=0`, `fwd=0`). The veto's
   real job is stopping a GROUNDED step off a ledge; lifting it in the air lets the bot steer toward a
   landing, and an airborne drift off an edge is acceptable per the "dying is fine, freezing is not" rule.
2. **Ramp-proximity hop gate** (`m_autoplayVhOnRamp`). The climb hop was gated only on `pos.y < 1.5`,
   true across the whole flat void, so the bot bunny-hopped the entire approach ("bunnyhopping while
   approaching the pad doesn't work"). Now the hop fires only once the bot is on a ramp slab; it WALKS
   grounded to the ramp / onto a void pad and only pogos up the risers.
3. **Fight-while-committing** (the `m_autoplayVhCommit` body). The commit used to clobber the intent to a
   bare walk+fire and the bot "just [ran] for the exit without a care" and died. It now KEEPS the brain's
   combat decisions (aim / fire / dodge / block / class skills / potion) and only OVERRIDES the WASD feet
   toward the exit (faceAndGo decomposition), so the bot fights its way out. Measured: **deaths 0**,
   combat active on **52-70%** of commit ticks.
4. **On-slab centreline anti-drift** (`Autoplay::rampApproachDir` / `rampSegDistXZ`, `autoplay_nav.h`,
   unit-tested). The story-aware VHallField mounts the ramp, but the eased-aim walk drifts off the narrow
   2-wide slab and slides back ("94% airborne, never crests"). Adding `rampApproachDir` ONLY once the bot
   is confirmed on a slab (`pos.y > 0.5`), centring on the nearest ramp, makes it climb onto a balcony on
   **every** run (`max_pos.y` ~3.0, was 0). Using it (or a RouteField) to ROUTE to the ramp instead
   regressed every time — it trapped the bot on the ground under the slab, or stalled the mount entirely.

**Measured state (geared paladin, combat removed as a factor, 90 s forced upper-exit runs):** the bot
climbs onto a balcony every run, fights back, never freezes, never dies — and **descends ~25-33%**.

### 9a. STILL OPEN — the upper-story crossing (the residual)

The bot climbs *a* balcony reliably, but the VHallField (shortest-path, door-seeded) often picks a
NON-exit ramp, landing it on the wrong balcony. To reach the exit balcony it must then cross a
**catwalk** — and one of the two is BROKEN with a 2-cell jump gap the fall veto refuses to let it jump —
so it falls into the void, lands on the ground under the door, and re-climbs (measured: near the door
`pos.y=0`, `d2d` 3-7 m, oscillating). It descends only when it happens to climb the EXIT-serving ramp
directly (that ramp's top IS on the exit balcony — a convex slab it walks straight across, no catwalk).

Two independent fixes would close this; both need a design step, not a heuristic tweak:
- **Vault the broken catwalk.** The enemy AI already has `StoryNav::planVault` (a tested "drop one cell
  ahead + same-height landing within `VAULT_MAX_CELLS`" probe) driving enemy gap-vaults. Give the bot the
  same: when the committed heading crosses a 1-2 cell gap with a same-height landing, JUMP it instead of
  letting the fall veto refuse the step. This lets it cross the broken catwalk to the exit balcony.
- **Bias the mount onto the EXIT ramp.** Forcing the exit ramp with a 2-D field (RouteField to its foot)
  broke the story-aware mount every time tried. A clean fix needs a TWO-STORY field variant that seeds a
  ramp preference, or the VHallField taught to tie-break its ground→ramp choice toward the exit ramp.
