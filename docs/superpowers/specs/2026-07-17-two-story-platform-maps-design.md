# Two-Story Platform Maps (walk-under balconies) + Combat Hall Arena — Design

**Goal:** Real two-story map support — a walkable surface you stand on AND walk under — and a
rebuilt arena that uses it: a Quake / Metroid-Prime-Hunters Combat-Hall map whose signature is a
perimeter **sniper balcony** overlooking the pit, with a covered arcade beneath it.

**Why an engine extension:** the grid is a heightfield (`GridCell` = one `floorHeight` + one
`ceilingHeight` per cell). "Stand on it and walk under it" is inexpressible; every raised tier so
far is solid underneath. The fix is one new cell capability — an optional **platform slab** per
cell — threaded through the same three chokes the jump pads proved out (collision, raycast,
mesher), with zero wire and zero save impact.

## Locked decisions (from the user, 2026-07-17)

1. Backbone = **per-cell platform slab** (not a full second grid layer, not a solid-under fake).
2. Arena = **full redesign** (~44×44 Combat Hall), not a wrap of the current 36×36.
3. Balcony access = **corner stairwells AND jump pads** (quiet route + fast route).
4. The **snipe balcony is the point** — open inner edge, clean sightlines into the pit.

## Non-goals (v1)

- N-story maps or independent upper-story wall layouts (walls stay full-height, shared by both
  stories — right for Combat Hall; an additive `upper-solid` flag can come later without rework).
- Dungeon/town generation using platforms (backbone is general; adoption is a separate project).
- Enemy AI on platforms — platforms are PvP-only, the exact pads/ledges policy (enemies keep
  navigating the base floor and simply walk under balconies; AI fields never change meaning).
- Railings (sub-cell geometry) — balcony edges are open, Quake-style.
- World items on platforms (arena spawns none; the item render/pickup floor snaps are parked).

---

## Part 1 — Engine backbone: `CELL_PLATFORM`

### Data model (`world/level_grid.h`)

- New flag: `CELL_PLATFORM = 1 << 5` (bits 6–7 remain free).
- `GridCell` grows 6 → 8 bytes: `u8 platHeight` (slab TOP surface, quarter-units) and
  `u8 platMaterialId`. The grid is rebuilt from the seed on every peer and never serialized —
  **no `SAVE_VERSION` bump, no `PROTOCOL_VERSION` bump, no snapshot change.**
- Fixed thickness `PLATFORM_THICKNESS_Q = 2` (0.5 m). Underside =
  `max(platHeight − 2, floorHeight)` — the clamp lets low stair steps degrade gracefully into
  riser-like geometry with no dead under-space.
- New `LevelGridSystem` helpers (the single choke every consumer uses — never read `platHeight`
  raw outside them):
  - `hasPlatform(grid, x, z)`, `getPlatformTop(...)`, `getPlatformUnderside(...)`
  - `effectiveFloorHeight(grid, x, z, feetY)` — slab top when `feetY ≥ top − STEP_UP_HEIGHT`,
    else base floor. The `STEP_UP_HEIGHT` tolerance is what makes graduated slab **stairs**
    (1-qu = 0.25 m steps) climbable, and is also the anti-cheese gate: a slab more than 0.4 m
    above the feet reads as a wall, exactly like `CELL_LEDGE` (slabs are implicitly ledge-gated —
    you reach them by stairs, a close jump, or a pad; never by walk-up snap).
  - `effectiveCeilingHeight(grid, x, z, feetY)` — slab underside while the body is below the
    slab, else the real `ceilingHeight`.

### Collision (`world/collision.cpp`, BOTH `moveAndSlide` overloads)

- The landing scan and the floor-height snap block switch from `getFloorHeight` to
  `effectiveFloorHeight` keyed on the **pre-move feet Y** — a body falling from above lands on
  the slab top; a body underneath keeps the base floor and can never be snapped up through the
  slab.
- **New: rising ceiling clamp.** Open cells currently have NO ceiling collision at all (the
  0.8 m jump apex could never reach one). With a 3.0 m slab and a 17 m/s pad launch this becomes
  mandatory: when rising, if the head (`feetY + PLAYER_HEIGHT`) would cross
  `effectiveCeilingHeight`, clamp position and zero `velocity.y` (the under-balcony head-bonk).
- XZ axes: the slab walk-up gate joins `overlapsLedgeAbove` in the axis block checks (a slab top
  > `STEP_UP_HEIGHT` above the feet blocks the step like a wall).
- Jump pads compose unchanged; authoring rule: **no pads ON the balcony** (a 3.0 m + 3.6 m apex
  launch would clear the outer wall).
- Replication: this is deterministic geometry inside the one function every movement path
  funnels through (local predict, server remote drain, reconcile replay) — the jump-pad story
  verbatim. `posY` + `onGround` are already snapshotted; **no wire change.**

### Raycast (`world/raycast.cpp` — one function, 24 call sites inherit it)

- `tryFloorCeil` gains the two slab planes: a downward ray crossing the slab **top** from above
  hits with normal +Y; an upward ray crossing the **underside** from below hits with normal −Y
  (same in-cell XZ containment test as the existing floor/ceiling checks; a ray starting under
  the slab gets a negative-t top crossing and correctly ignores it).
- **Rim hits:** when the DDA advances into a slab cell and the ray's Y at the crossing lies
  within `[underside, top]`, return a hit with `lastNormal` (the slab edge seen from the side).
- This one change makes hitscan weapons, PvP LOS (`combat_query.cpp:125`), projectile flight
  (`projectile.cpp:237`, meteors in `engine_update.cpp:1409`), every dash/teleport skill, the
  HUD aim probes, and enemy LOS all respect balcony floors — you cannot shoot through the slab,
  and balcony-edge shots into the pit stay clear.

### Direct height consumers that switch to `effectiveFloorHeight` (v1, PvP-relevant)

- `game/teleport_dest.cpp:76` — dash/teleport destination Y must resolve at the **victim's feet
  Y**, so closing skills (Shadow Dance etc.) land ON the balcony beside a balcony target, not
  under it.
- Ground-targeted AoE impact Y (meteor landing, ground pools) — resolve at the target's story.
  The implementation plan locates the exact sites in `updateMeteors`/skill code.
- Parked (dungeon adoption, not v1): world-item render snap (`engine_render_world.cpp:89`) and
  pickup floor logic; enemy-AI floor reads (`enemy_ai.cpp:80,209`) stay base-floor by design.
- Splash damage is radial distance and already crosses tiers today (crown vs pit) — unchanged.

### Mesher (`world/level_mesh.cpp`)

Slab cells emit, following the riser-face pattern (owner draws each shared face once):
- top quad (+Y, `platMaterialId`), underside quad (−Y, only when underside > base floor),
- rim quads toward each neighbour lacking a slab overlapping the same band (`wallMaterialId`);
  adjacent slabs at equal height share no face; a 1-qu stair neighbour emits just the sliver.
- Section AABB `expand` covers all slab verts (frustum-culling safe).

### Tests (`tests/world/`, doctest)

- Collision: falling body lands on slab top; body walks beneath; rising head clamps at the
  underside (pad launch under balcony included); ground body is never snapped up through a slab;
  slab > STEP_UP blocks XZ like a ledge; graduated slab stairs climb; slab walk-off falls.
- Raycast: down hits top from above; up hits underside from below; horizontal ray in the band
  hits the rim; horizontal ray below passes under; above passes over; balcony-edge shot into an
  open pit hits the pit floor, not the slab.
- Mesher output is verified visually (no unit surface).

---

## Part 2 — The Combat Hall arena (`engine_arena.cpp` rebuild)

- **44×44**, cellSize 1.0 (9 mesh sections — well inside `MAX_LEVEL_SECTIONS`). Open sky kept;
  interior `ceilingHeight` 20 (5 m walls) so a balcony jump (3.0 + 0.8 = 3.8 m) cannot clear.
- **Perimeter sniper balcony:** the 2-cell band one cell in from each wall, slab top 12 qu
  (3.0 m), underside 2.5 m (0.7 m headroom over a 1.8 m body). Open inner edge — drop off or
  fire into the pit anywhere along it.
- **Arcade** beneath the balcony: covered perimeter flank route, out of balcony sightlines.
  Full-height solid **columns** every ~6 cells along the balcony's inner-edge row — arcade cover
  below, pillars to strafe around above, and the visual "supports" that sell the structure.
- **4 corner stairwells:** L-shaped switchback slab stairs (12 × 0.25 m steps), arcade → balcony.
- **Jump pads:** the 4 pit pads repositioned to arc onto the balcony inner edge (the fast, loud,
  exposed route); the tower launch-ring and its crown role unchanged.
- **Center:** tower 6×6 @ 1.5 m, crown 2×2 @ **3.0 m — level with the balcony**, so crown and
  balcony snipe lines duel each other across the map; 4 cardinal 2-lane ramps as today.
- **Spawns:** `kArenaPads` move into the arcade near each corner stairwell, tucked behind a
  column — you respawn in cover, out of balcony LOS, with stairs and pit both reachable. The
  farthest-pad respawn logic is untouched.
- **Symmetry:** every placement invariant under x→43−x, z→43−z and the diagonal swap — no corner
  favored.
- Progression firewall unchanged: no XP, no loot, no saves in-arena.

## Verification

1. Unit suite green (both new test files), both build configs clean.
2. SP smoke: climb stairs, snipe from the balcony, walk the arcade beneath, pad-arc up, head-bonk
   under the slab, drop off the edge.
3. Co-op (the real test): host + client — balcony movement with **no rubber-band** (reconcile
   replays the same slab collision), cross-story shots blocked/clear identically on both screens,
   pad→balcony arc replicates, `--net-loss 15 --net-latency 100` soak shows 0 hard snaps.
4. Perf: 44×44 with balcony geometry stays within the 300–500 draw-call budget (slab quads join
   existing section submeshes — no new draw calls, only more triangles).

## Risks / parked

- **Balcony camping** is a balance risk; the knobs are column spacing, stairwell count, and the
  crown duel line. Needs a playtest, not a green suite.
- **Upper-story walls over open ground floor** (real 2-story dungeon rooms) need the additive
  `upper-solid` extension later; nothing in this design blocks it.
- **Dungeon-gen adoption** (bridges, galleries in `level_gen.cpp`) is deliberately out of scope;
  when it lands it must also convert the parked consumers (world-item snaps, flying-enemy vs
  underside).
