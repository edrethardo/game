# Balance Lab — player power vs enemy power per floor

**Date:** 2026-07-22
**Status:** Approved (brainstormed with Aaron; report-first, targets chosen later)

## Problem

Every balance pass so far (the HP compound rate, the damage slope, the per-tier difficulty
bumps, the +50% roster change) was hand-solved: pick a number, reason about three anchor
floors in a comment, ship it. The comments in `game_constants.h` already speak in the units
that matter — "an 11-hit grind, not 23", "compounding damage would one-shot the player" —
but nothing computes those numbers, so nobody knows what the curves actually look like
across all 150 effective floors, and a content change (a roster buff, an affix range edit)
can silently bend them.

The proposal (Aaron's): compute **effective damage, spell damage and effective health from
typical equipment at each floor** and put that against **enemy damage and health per floor**,
so balancing is done against measured curves instead of feel.

## Decisions (made during brainstorm)

| Question | Decision |
|---|---|
| Deliverable | A **repeatable balance model**, committed and re-run on every future pass — not a one-off analysis |
| Player archetypes | The full **3×3 build grid** (Magic/Melee/Ranged × Tanky/Moderate/Glass) — 9 curves |
| Caster damage | **Total output = weapon DPS + cast DPS** for every column (casters swing their wand too) |
| Balance targets | **Report first**: build the tool, read the curves, then pick target bands together; bands become CI assertions in phase 2 |
| Enemy scope | **Trash + bosses** (champions deferred) |
| Approach | **C++ balance lab linked against the real engine code** (rejected: a Python re-implementation, which would drift exactly the way the pre-fix loot scorer did; deferred: empirical bot simulation as later validation) |

## Architecture

```
assets/config JSON  ──►  tests/balance/balance_lab.{h,cpp}  ──►  CSV report
(real loaders)           (linked into dungeon_tests only)        (env-gated dump)
                         per (difficulty, floor 1..50, cell 0..8):     │
                           typical gear → player power → enemy         ▼
                           curves → metrics                    tools/balance_chart.py
                                                               (CSV → one HTML chart page)
                                                                       │
                                            phase 2: chosen target bands become
                                            doctest REQUIREs — CI goes red when a
                                            change knocks a floor out of band
```

Everything left of the CSV links **real engine code** — `ItemGen::generateItem`,
`BuildScore::score`, `Inventory::getEffectiveWeapon` / `getEffectiveMaxHealth` /
`spellDamageFlat` / `spellDamagePct`, `SkillSystem::computeCooldownTicks`,
`GameConst::floorHealthMult` / `floorDamageMult` / `difficultyDamageBump`, and the real
JSON defs via the real loaders (located with `DUNGEON_REPO_ROOT`, the
`test_ai_preference.cpp` pattern). No formula is re-implemented anywhere; a content change
flows into the report automatically. The lab is **never linked into the game binary**.

### Shared helper extraction (rides along)

The sustained-DPS reduction (attack-speed-divided cooldown, CDR on the swing, the
`shots*cd + reload` clip cycle) currently lives only inside `build_score.h` — a mirror of
`getEffectiveWeapon`'s output shape that has already drifted once (the three 2026-07-22
loot-scoring fix commits). It moves to a small pure header, `src/game/weapon_dps.h`, that
`build_score.h` and the lab both include. One copy of that math, regression-pinned (see
Testing).

## The typical-equipment model

For a given `(difficulty, floor F, build cell)`:

1. **Loot exposure window.** The simulated player has seen the drops from floors
   `max(1, F−3) … F` — you wear gear from the last few floors, not just this one. Per
   floor the model generates **K = 12 drops** (grounded: a floor spawns ~25–35 enemies;
   `LOOT_DROP_CHANCE` is 40% + 1%/level capped at 70% → ~12 items is the early-floor
   yield, conservative later).
2. **Real generation.** Every drop goes through the real `ItemGen::generateItem` at that
   floor's `enemyLevel` (`floor + difficulty*50`) — real rarity windows, real affix rolls,
   real level multipliers.
3. **Gear selection.** Best-per-slot by `BuildScore::score` under the cell — the same
   brain Auto Loot & Equip uses — equipped into a real `PlayerInventory`. "Typical
   equipment" literally means *what auto-equip would wear from typical drops*.
4. **Monte Carlo.** **T = 200 seeded trials**; the report carries **p50** as the
   typical-player line plus a **p10–p90 band** (unlucky vs lucky). The seed is fixed, so
   the CSV is bit-identical run-to-run and diffable across balance changes.

K, the window length, T and the seed are named constants in the lab, documented as
**model parameters, not engine truth**.

## Player power (all real engine functions)

- **Weapon DPS** — `Inventory::getEffectiveWeapon` on the equipped loadout, reduced to
  sustained DPS by the shared `weapon_dps.h` helper.
- **Cast DPS** — `(skill.damage + spellDamageFlat) × (1 + spellDamagePct/100) /
  (cooldown × (1 − CDR))` from `skills.json` defs, using one **representative class per
  column**: Sorcerer (Magic), Warrior (Melee), Marksman (Ranged) — declared constants,
  swappable. **Total DPS = weapon DPS + cast DPS** for every column.
- **Effective HP** — `getEffectiveMaxHealth` on the representative class's base, divided
  through the real armor reduction → EHP in the same units as enemy damage.
- **Sustain** — regen / life-on-hit / lifesteal reported as a separate HP/s column, NOT
  folded into EHP (folding needs a fight-length assumption; keeping it separate keeps EHP
  honest).

## Enemy power

- **Trash:** per effective floor, the tier roster the spawner would actually use
  (`collectTierDefs`, same floor→tier mapping as `spawnFloorEnemies`); the **median**
  enemy's HP and damage after `floorHealthMult(F)` and
  `floorDamageMult(F) × difficultyDamageBump(d)` — the exact spawn-time code path. Roster
  min/max ride along so a tier with a tanky outlier is visible.
- **Incoming DPS:** damage ÷ `attackCooldown` (sustained), plus the raw per-hit number.
- **Bosses:** every `bosses.json` entry curved on its milestone floor with the same
  multipliers — one row per boss per difficulty.

## Metrics and report

CSV rows keyed `(difficulty, floor, cell)`; columns:

- Player: weapon DPS, cast DPS, total DPS, EHP, sustain — each as p10/p50/p90.
- Enemy: median/min/max HP, per-hit damage, sustained DPS; boss HP/hit where present.
- Derived: **TTK-trash** (median HP ÷ p50 total DPS), **TTK-boss**, **hits-to-die**
  (EHP ÷ per-hit), **seconds-to-die** (EHP ÷ enemy DPS).

The dump is a doctest case gated on the **`BALANCE_REPORT=<path>` env var** — normal test
runs skip it and only run the model's sanity pins, so CI stays fast.

`tools/balance_chart.py` renders the CSV into one HTML page: TTK curve, hits/seconds-to-die
curve, raw power curves (log Y — Hell reaches ~300× base HP), all on effective floor 1–150
with the difficulty seams at 50/100 marked (the seams are where entry cliffs will show),
9-cell curve selection with the p10–p90 band on the active cell, boss markers, and a
**worst-offenders table** (top floors by floor-over-floor discontinuity, and by band
violation once targets exist).

## Phase 2: target bands in CI

Ships report-only first. After reading the curves together we pick target bands per
difficulty (in TTK / hits-to-die units). Bands become named constants in the test file,
each with a comment recording why the number was chosen (the `game_constants.h`
discipline), asserted with `REQUIRE` + tolerance over every floor × cell. Bands check
**p50 only** — the luck band stays informational, because asserting on percentile extremes
is flaky-by-design.

## Error handling

- JSON loads are `REQUIRE`d to succeed before any modeling; a missing/renamed config is a
  loud test failure, not a silent zero-row report.
- A build cell whose gear selection can't fill the weapon slot (family gate starved by the
  drop table) fails a sanity pin — that IS a balance bug worth failing on.
- `BALANCE_REPORT` pointing at an unwritable path fails the dump case with the path in the
  message.

## Testing the lab itself

- **Extraction regression:** for the shipped weapon table, `weapon_dps.h` must reproduce
  the sustained-DPS values `build_score.h` produced before the extraction;
  `test_build_score.cpp` keeps passing untouched.
- **Sanity pins (seeded, deterministic):** enemy HP/damage strictly rise with effective
  floor; typical-gear p50 power rises floor-over-floor (plateaus allowed); every
  (floor, cell) produces a full loadout; CSV write smoke test.
- All pins run in the normal `dungeon_tests` suite (CI already executes it on Linux+macOS).

## Out of scope

- Champion multipliers (deferred; add a column later if wanted).
- Empirical bot-sim validation (possible follow-up project on the `--bot-walk` rig).
- Folding healing into EHP; PvP/arena balance; XP/leveling curves.

## Docs to update on landing

- **CLAUDE.md** — short "Balance lab" paragraph (what it is, how to run a report).
- **engine-how-to** — recipe: run a balance report, read the chart page, add/adjust a
  target band.
- Model parameters (K=12, 4-floor window, T=200, representative classes, seed) documented
  at their definitions as model assumptions, not engine truth.
