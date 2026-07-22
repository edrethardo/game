# Balance Lab Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A repeatable C++ balance model that computes typical-equipment player power (weapon DPS + cast DPS, EHP, sustain) per floor and puts it against enemy/boss curves, emitting a CSV report + HTML chart page.

**Architecture:** Everything links the REAL engine code (`ItemGen::rollItem`, `BuildScore::score`, `Inventory::*`, `GameConst::*`, the real JSON loaders) — no formula is re-implemented. Lab code lives in `tests/balance/`, linked into `dungeon_tests` only. Three tiny single-source extractions ride along (sustained-DPS cycle → `weapon_dps.h`, `armorMitigation` → inline in `combat.h`, `kClassDefs` → `class_defs.cpp`, floor→tier ladder → `enemy_def.h`) so the lab and the engine can never drift.

**Tech Stack:** C++17, doctest (vendored), CMake, Python 3 stdlib (chart renderer, no pip deps).

**Spec:** `docs/superpowers/specs/2026-07-22-balance-lab-design.md`

**Key facts for the implementer** (verified against the tree at plan time):

- `ItemGen::init(u32 seed)` seeds a file-local LCG and resets the uid counter — reseeding gives full determinism. `ItemGen::rollItem(u8 enemyLevel, const ItemDef* defs, u32 defCount, const AffixDef* affixDefs, u32 affixDefCount, Rarity rarityFloor = Rarity::COMMON)` is the whole drop pipeline (rarity → def pick → base-stat scale → affixes). [src/game/item.h:663-675]
- `ItemLoader::loadItemDefs / loadAffixDefs / loadSkillDefs(const char* path, T* defs, u32& count)` [src/game/item.h:656-658]; `EnemyLoader::load(path, EnemyDefTable&)`; `BossLoader::load(path, BossDefTable&)`. `item_loader.cpp`, `item_gen.cpp`, `inventory.cpp` are ALREADY in tests/CMakeLists.txt; `enemy_loader.cpp` / `boss_loader.cpp` are NOT yet.
- `DUNGEON_REPO_ROOT` is a compile definition on dungeon_tests — `EnemyLoader::load(DUNGEON_REPO_ROOT "/assets/config/enemies.json", table)` is the established pattern (see tests/game/test_ai_preference.cpp).
- Enemy spawn scaling (engine_spawn.cpp:1411-1420 and 505-507): `effectiveFloor = rawFloor + difficulty*50`, HP × `GameConst::floorHealthMult(eff)`, damage × `GameConst::floorDamageMult(eff) * GameConst::difficultyDamageBump(difficulty)`. Bosses use the identical mults (engine_spawn.cpp:822-824).
- `Inventory::getEffectiveWeapon(inv, itemDefs, baseWeapon)` returns a final `WeaponDef` — `damage`, `cooldown` (attack-speed + CDR applied, floored 0.05), `clipSize`, `reloadTime` (floored 0.2 when base > 0), `critChance`, `critMult`. `getEffectiveMaxHealth(inv, baseMaxHealth)` = (base + flat incl. per-item bonusHealth) × (1 + pct). On-demand affix sums: `armorRating`, `healthRegenRate`, `lifestealPct`, `spellDamageFlat`, `spellDamagePct`. `inv.bonusCooldownReduction` is already capped 0–0.5 by `recalculateStats`.
- `kClassDefs` (with `baseHealth`, `skills[4]`, `skillUnlockFloor[4]`) is defined in engine_init.cpp:72-143 — NOT linkable into tests until Task 5 extracts it.
- `MAX_ITEM_DEFS` 224, `MAX_AFFIX_DEFS` 32, `MAX_SKILL_DEFS` 64, `MAX_ENEMY_DEFS` 64, `MAX_AFFIXES_PER_ITEM` 4, `MAX_INVENTORY_ITEMS` 24. Empty item ⇔ `defId == 0xFFFF`. `ItemSlot`: WEAPON, OFFHAND, HELMET, ARMOR, BOOTS, RING, GLOVES, COUNT.
- Tests may use STL (`<algorithm>`, `std::sort`) — the no-heap rule is for engine hot paths, not the lab. Lab structs still use fixed arrays where natural.

---

### Task 1: Extract the sustained-DPS cycle formula into `src/game/weapon_dps.h`

The reduction currently lives only inside `BuildScore::score` (build_score.h:151-159) — a mirror that has already drifted once. Extract the cycle math; the scorer and the lab both call it.

**Files:**
- Create: `src/game/weapon_dps.h`
- Modify: `src/game/build_score.h:151-159` (+ include at top)
- Create: `tests/game/test_weapon_dps.cpp`
- Modify: `tests/CMakeLists.txt` (add the test file)

- [ ] **Step 1: Write the failing test**

```cpp
// tests/game/test_weapon_dps.cpp — pins the single-source sustained-DPS cycle formula
// (extracted from build_score.h so the scorer and the balance lab cannot drift apart).
#include "doctest/doctest.h"
#include "game/weapon_dps.h"

TEST_CASE("no-clip weapon: dps = perHit / cooldown") {
    CHECK(WeaponDps::sustained(30.0f, 0.5f, 0.0f, 0.0f) == doctest::Approx(60.0f));
}

TEST_CASE("clip weapon pays the reload cycle") {
    // 8 shots x 12 dmg, 0.35 s between shots, 1.2 s reload: 96 / (2.8 + 1.2) = 24
    CHECK(WeaponDps::sustained(12.0f, 0.35f, 8.0f, 1.2f) == doctest::Approx(24.0f));
}

TEST_CASE("instant reload collapses to the no-clip formula") {
    CHECK(WeaponDps::sustained(12.0f, 0.35f, 8.0f, 0.0f)
          == doctest::Approx(WeaponDps::sustained(12.0f, 0.35f, 0.0f, 0.0f)));
}

TEST_CASE("expected crit multiplier") {
    CHECK(WeaponDps::expectedCritMult(0.05f, 2.0f) == doctest::Approx(1.05f));   // baseline
    CHECK(WeaponDps::expectedCritMult(0.20f, 2.5f) == doctest::Approx(1.30f));   // dagger
}
```

Add to `tests/CMakeLists.txt` after the `game/test_build_score.cpp` line:

```cmake
    game/test_weapon_dps.cpp         # single-source sustained-DPS cycle (scorer + balance lab)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target dungeon_tests 2>&1 | tail -5`
Expected: compile FAILURE — `game/weapon_dps.h: No such file or directory`

- [ ] **Step 3: Create the header**

```cpp
// src/game/weapon_dps.h — THE sustained-DPS cycle formula, single-sourced.
//
// Reduces a weapon's numbers to damage-per-second the way the game actually plays out:
// a clip weapon pays its reload every magazine (shots*cd + reload per cycle), everything
// else is simply perHit / cooldown. BuildScore::score and the balance lab both call this;
// it was extracted from build_score.h because a hand-mirrored copy of this exact math
// drifted once already (the 2026-07-22 loot-scoring fixes).
//
// Caller contract: effCooldown > 0 (the engine floors it at 0.05 in buildWeaponDef; the
// scorer's reconstruction bottoms out at 0.2/1.5). Reload flooring (0.2 s when the base
// reload is nonzero) also stays with the callers — the engine applies it in
// buildWeaponDef, the scorer mirrors it pre-call — because it needs the BASE reload to
// decide, which this pure cycle formula deliberately doesn't see.
#pragma once
#include "core/types.h"

namespace WeaponDps {

// shots < 1 means "no magazine" (melee / energy weapons): plain perHit / cooldown.
inline f32 sustained(f32 perHit, f32 effCooldown, f32 shots, f32 reloadSeconds) {
    if (shots < 1.0f) return perHit / effCooldown;
    return shots * perHit / (shots * effCooldown + reloadSeconds);
}

// Expected-value damage multiplier from crits: 1 + chance*(mult-1). The scorer ignores
// crit (near-constant across weapons); the lab includes it because daggers' 20%/2.5x is
// a real +25% sustained output over the 5%/2.0x baseline.
inline f32 expectedCritMult(f32 critChance, f32 critMult) {
    return 1.0f + critChance * (critMult - 1.0f);
}

} // namespace WeaponDps
```

- [ ] **Step 4: Rewire `build_score.h` to call it**

Add `#include "game/weapon_dps.h"` after the existing includes (line 4). Then replace lines 151-159 (the `f32 dps;` block inside the weapon branch):

```cpp
        f32 dps;
        if (def.baseClipSize > 0) {
            const f32 shots  = static_cast<f32>(def.baseClipSize) * (1.0f + clipPct * 0.01f);
            f32 reload = def.baseReloadTime * (1.0f - reloadPct * 0.01f);
            if (def.baseReloadTime > 0.0f && reload < 0.2f) reload = 0.2f;   // engine floor
            dps = WeaponDps::sustained(perHit, effCd, shots, reload);
        } else {
            dps = WeaponDps::sustained(perHit, effCd, 0.0f, 0.0f);
        }
```

- [ ] **Step 5: Run tests — new ones pass AND the scorer regression stays green**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*sustained*,*crit multiplier*,*reload*" && ./build/tests/dungeon_tests -tc="*build*score*,*BuildScore*,*scorer*"`
Expected: all PASS. If any test_build_score case fails, the rewire changed a value — diff the formula against the original block; the extraction must be value-identical.

- [ ] **Step 6: Full suite + game build, then commit**

Run: `cmake --build build && ./build/tests/dungeon_tests --no-version | tail -3`
Expected: all tests pass, game target still builds.

```bash
git add src/game/weapon_dps.h src/game/build_score.h tests/game/test_weapon_dps.cpp tests/CMakeLists.txt
git commit -m "refactor(game): extract sustained-DPS cycle formula to weapon_dps.h

Single source for the scorer and the upcoming balance lab — the hand-mirrored
copy of this math in build_score.h is the one that drifted before the 2026-07-22
loot-scoring fixes.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Make `Combat::armorMitigation` header-inline

The lab converts armor → EHP through the real mitigation curve, but linking all of combat.cpp into tests drags in the entity/projectile world. The function is 4 pure lines — move it into combat.h as `inline` (single source preserved, testable without combat.cpp).

**Files:**
- Modify: `src/game/combat.h:83` (declaration → inline definition)
- Modify: `src/game/combat.cpp:333-337` (delete the out-of-line body)
- Create: `tests/game/test_armor_mitigation.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/game/test_armor_mitigation.cpp — pins the armor curve the balance lab's EHP
// conversion rides on. Compiles WITHOUT combat.cpp: that is the point of the test —
// the function must be header-inline so tests-only code can use the real curve.
#include "doctest/doctest.h"
#include "game/combat.h"

TEST_CASE("armorMitigation: diminishing returns, 100 armor = 50%, hard cap 80%") {
    CHECK(Combat::armorMitigation(-5.0f)    == doctest::Approx(0.0f));
    CHECK(Combat::armorMitigation(0.0f)     == doctest::Approx(0.0f));
    CHECK(Combat::armorMitigation(100.0f)   == doctest::Approx(0.5f));
    CHECK(Combat::armorMitigation(300.0f)   == doctest::Approx(0.75f));
    CHECK(Combat::armorMitigation(10000.0f) == doctest::Approx(0.80f));   // cap
}
```

CMake: add `game/test_armor_mitigation.cpp` after `game/test_armor_tier.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target dungeon_tests 2>&1 | tail -5`
Expected: LINK failure — `undefined reference to Combat::armorMitigation(float)` (combat.cpp is not linked into dungeon_tests).

- [ ] **Step 3: Move the body**

In `src/game/combat.h`, replace the line `f32 armorMitigation(f32 armor);` (the comment block above it stays) with:

```cpp
    inline f32 armorMitigation(f32 armor) {
        if (armor <= 0.0f) return 0.0f;
        f32 mit = armor / (armor + 100.0f); // diminishing returns: 100 armor = 50%
        return (mit > 0.80f) ? 0.80f : mit;  // hard cap so a stacked build can't become invulnerable
    }
```

In `src/game/combat.cpp`, delete the whole `f32 Combat::armorMitigation(f32 armor) { ... }` definition (lines 333-337).

- [ ] **Step 4: Run tests + full build**

Run: `cmake --build build && ./build/tests/dungeon_tests -tc="*armorMitigation*"`
Expected: PASS; game target links (combat.cpp callers now use the inline).

- [ ] **Step 5: Commit**

```bash
git add src/game/combat.h src/game/combat.cpp tests/game/test_armor_mitigation.cpp tests/CMakeLists.txt
git commit -m "refactor(game): armorMitigation header-inline so tests link the real curve

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Floor→tier single-source + enemy/boss curves (`tests/balance/` scaffolding)

**Files:**
- Modify: `src/game/enemy_def.h` (add `enemyTierForFloor`)
- Modify: `src/engine/engine_startgame.cpp:774-778` (use it)
- Create: `tests/balance/balance_lab.h`
- Create: `tests/balance/balance_lab.cpp`
- Create: `tests/balance/test_balance_lab.cpp`
- Modify: `tests/CMakeLists.txt` (test file + balance_lab.cpp + enemy_loader.cpp + boss_loader.cpp)

- [ ] **Step 1: Write the failing test**

```cpp
// tests/balance/test_balance_lab.cpp — the balance lab: typical-gear player power vs
// enemy curves per floor (spec: docs/superpowers/specs/2026-07-22-balance-lab-design.md).
// This file holds the always-on sanity pins; the BALANCE_REPORT CSV dump case is added
// in a later task.
#include "doctest/doctest.h"
#include "balance/balance_lab.h"
#include "game/enemy_loader.h"
#include "game/boss_loader.h"
#include "game/game_constants.h"
#include <algorithm>

// Shared fixture: the real shipped tables, loaded once (doctest runs cases in one process).
static const EnemyDefTable& enemyTable() {
    static EnemyDefTable t;
    static bool loaded = EnemyLoader::load(DUNGEON_REPO_ROOT "/assets/config/enemies.json", t);
    REQUIRE(loaded);
    return t;
}
static const BossDefTable& bossTable() {
    static BossDefTable t;
    static bool loaded = BossLoader::load(DUNGEON_REPO_ROOT "/assets/config/bosses.json", t);
    REQUIRE(loaded);
    return t;
}

TEST_CASE("enemyTierForFloor matches the spawn ladder at every band boundary") {
    CHECK(enemyTierForFloor(1)  == 1);
    CHECK(enemyTierForFloor(10) == 1);
    CHECK(enemyTierForFloor(11) == 2);
    CHECK(enemyTierForFloor(20) == 2);
    CHECK(enemyTierForFloor(21) == 3);
    CHECK(enemyTierForFloor(30) == 3);
    CHECK(enemyTierForFloor(31) == 4);
    CHECK(enemyTierForFloor(40) == 4);
    CHECK(enemyTierForFloor(41) == 5);
    CHECK(enemyTierForFloor(50) == 5);
}

TEST_CASE("enemy trash curve: multiplier path matches the spawn code exactly") {
    const EnemyDefTable& t = enemyTable();
    // Recompute one point by hand straight from the roster + GameConst — the lab function
    // must agree bit-for-bit (same functions, same order of operations).
    const u8 rawFloor = 25, difficulty = 2;                    // Hell 25 -> effective 125
    const u32 eff = 125;
    const EnemyDef* defs[MAX_ENEMY_DEFS];
    const u32 n = collectTierDefs(t, enemyTierForFloor(rawFloor), defs, MAX_ENEMY_DEFS);
    REQUIRE(n > 0);
    f32 hp[MAX_ENEMY_DEFS];
    for (u32 i = 0; i < n; i++) hp[i] = defs[i]->health * GameConst::floorHealthMult(eff);
    std::sort(hp, hp + n);
    const f32 expectMedian = (n % 2) ? hp[n / 2] : 0.5f * (hp[n / 2 - 1] + hp[n / 2]);

    const BalanceLab::EnemyCurve c = BalanceLab::enemyTrashAt(t, rawFloor, difficulty);
    CHECK(c.hpMedian == doctest::Approx(expectMedian));
    CHECK(c.hpMin    == doctest::Approx(hp[0]));
    CHECK(c.hpMax    == doctest::Approx(hp[n - 1]));
}

TEST_CASE("enemy trash curve rises within every tier band, every difficulty") {
    const EnemyDefTable& t = enemyTable();
    const u8 bands[5][2] = {{1,10},{11,20},{21,30},{31,40},{41,50}};
    for (u8 d = 0; d < 3; d++)
        for (const auto& b : bands)
            for (u8 f = b[0]; f < b[1]; f++) {
                const BalanceLab::EnemyCurve a = BalanceLab::enemyTrashAt(t, f, d);
                const BalanceLab::EnemyCurve n2 = BalanceLab::enemyTrashAt(t, f + 1, d);
                CHECK(n2.hpMedian  > a.hpMedian);    // same roster, bigger multiplier
                CHECK(n2.hitMedian > a.hitMedian);
            }
}

TEST_CASE("boss curve exists on every authored boss floor and scales like the spawner") {
    const BossDefTable& bt = bossTable();
    REQUIRE(bt.count > 0);
    for (u32 i = 0; i < bt.count; i++) {
        const BossDef& bd = bt.defs[i];
        const BalanceLab::BossCurve c = BalanceLab::bossAt(bt, bd.floor, 1);   // Nightmare
        CHECK(c.present);
        const u32 eff = bd.floor + 50u;
        CHECK(c.hp  == doctest::Approx(bd.baseHp  * GameConst::floorHealthMult(eff)));
        CHECK(c.hit == doctest::Approx(bd.baseDmg * GameConst::floorDamageMult(eff)
                                       * GameConst::difficultyDamageBump(1)));
    }
    CHECK_FALSE(BalanceLab::bossAt(bt, 2, 0).present);   // floor 2 has no boss
}
```

CMake — add to the test-file block and to the production block:

```cmake
    balance/test_balance_lab.cpp     # balance lab: enemy/boss curves + typical-gear sanity pins
    balance/balance_lab.cpp          # the lab itself (tests-only; NEVER linked into the game)
```
```cmake
    ${CMAKE_SOURCE_DIR}/src/game/enemy_loader.cpp   # balance lab reads the real roster
    ${CMAKE_SOURCE_DIR}/src/game/boss_loader.cpp    # balance lab reads the real bosses
```

Also add `${CMAKE_CURRENT_SOURCE_DIR}` to `target_include_directories(dungeon_tests PRIVATE ...)` so `#include "balance/balance_lab.h"` resolves.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target dungeon_tests 2>&1 | tail -5`
Expected: compile FAILURE — `balance/balance_lab.h: No such file or directory` (and `enemyTierForFloor` undeclared).

- [ ] **Step 3: Add `enemyTierForFloor` to enemy_def.h and rewire the spawner**

In `src/game/enemy_def.h`, directly above `collectTierDefs`:

```cpp
// RAW floor (1-50, difficulty-independent) -> enemy tier (1-5). Single source: the spawn
// path and the balance lab must agree on which roster a floor draws from, so the ladder
// lives here rather than inline in engine_startgame.
inline u8 enemyTierForFloor(u8 rawFloor) {
    if (rawFloor >= 41) return 5;
    if (rawFloor >= 31) return 4;
    if (rawFloor >= 21) return 3;
    if (rawFloor >= 11) return 2;
    return 1;
}
```

In `src/engine/engine_startgame.cpp`, replace the 5-line ladder (lines 774-778, under the "Determine tier for enemy spawning" comment):

```cpp
    u8 currentTier = enemyTierForFloor(static_cast<u8>(m_level.currentFloor));
```

- [ ] **Step 4: Create `tests/balance/balance_lab.h`**

```cpp
// tests/balance/balance_lab.h — the balance lab: typical-equipment player power vs enemy
// power per floor (spec: docs/superpowers/specs/2026-07-22-balance-lab-design.md).
//
// TESTS-ONLY code: linked into dungeon_tests, never into the game binary. Everything here
// CALLS real engine code (ItemGen / BuildScore / Inventory / GameConst / the JSON defs) —
// the lab holds model structure (windows, trials, percentiles), never game math.
#pragma once
#include "core/types.h"
#include "game/item.h"
#include "game/enemy_def.h"
#include "game/boss_def.h"

namespace BalanceLab {

// --- model parameters: assumptions about a TYPICAL PLAYER, not engine truth ------------------
// A player at floor F wears the best of the drops they saw over the last few floors.
static constexpr u32 DROPS_PER_FLOOR  = 12;   // ~25-35 kills x LOOT_DROP_CHANCE (40%+1%/lvl)
static constexpr u32 WINDOW_FLOORS    = 4;    // gear comes from floors F-3..F
static constexpr u32 TRIALS           = 200;  // Monte-Carlo trials per (difficulty, floor)
static constexpr u32 MAX_WINDOW_DROPS = DROPS_PER_FLOOR * WINDOW_FLOORS;

inline u32 effectiveFloor(u8 rawFloor, u8 difficulty) { return rawFloor + difficulty * 50u; }

// --- enemy side ------------------------------------------------------------------------------
// Post-multiplier trash-roster stats at one (floor, difficulty). Medians are PER-METRIC
// (median HP and median damage may come from different defs) — the report reads each curve
// independently, so a single "median enemy" would just be a worse summary.
struct EnemyCurve {
    f32 hpMedian = 0, hpMin = 0, hpMax = 0;
    f32 hitMedian = 0;    // damage per landed hit
    f32 dpsMedian = 0;    // median over defs of scaledDamage / attackCooldown
};
EnemyCurve enemyTrashAt(const EnemyDefTable& table, u8 rawFloor, u8 difficulty);

struct BossCurve {
    bool present = false;
    const char* name = "";
    f32 hp = 0, hit = 0, dps = 0;
};
BossCurve bossAt(const BossDefTable& table, u8 rawFloor, u8 difficulty);

} // namespace BalanceLab
```

- [ ] **Step 5: Create `tests/balance/balance_lab.cpp`**

```cpp
// tests/balance/balance_lab.cpp — see balance_lab.h. Enemy/boss curves in this task;
// typical-gear Monte Carlo and player power land in later tasks.
#include "balance/balance_lab.h"
#include "game/game_constants.h"
#include <algorithm>

namespace BalanceLab {

static f32 medianOf(f32* v, u32 n) {
    if (n == 0) return 0.0f;
    std::sort(v, v + n);
    return (n % 2) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

EnemyCurve enemyTrashAt(const EnemyDefTable& table, u8 rawFloor, u8 difficulty) {
    EnemyCurve c;
    const EnemyDef* defs[MAX_ENEMY_DEFS];
    const u32 n = collectTierDefs(table, enemyTierForFloor(rawFloor), defs, MAX_ENEMY_DEFS);
    if (n == 0) return c;

    // The exact spawn-time scaling path (engine_spawn.cpp): HP compounds via floorHealthMult,
    // damage is linear floorDamageMult x the per-tier difficulty bump.
    const u32 eff   = effectiveFloor(rawFloor, difficulty);
    const f32 hpMul = GameConst::floorHealthMult(eff);
    const f32 dmMul = GameConst::floorDamageMult(eff) * GameConst::difficultyDamageBump(difficulty);

    f32 hp[MAX_ENEMY_DEFS], hit[MAX_ENEMY_DEFS], dps[MAX_ENEMY_DEFS];
    for (u32 i = 0; i < n; i++) {
        hp[i]  = defs[i]->health * hpMul;
        hit[i] = defs[i]->damage * dmMul;
        dps[i] = (defs[i]->attackCooldown > 0.0f) ? hit[i] / defs[i]->attackCooldown : hit[i];
    }
    c.hpMedian  = medianOf(hp, n);
    c.hpMin     = hp[0];              // medianOf sorted the array in place
    c.hpMax     = hp[n - 1];
    c.hitMedian = medianOf(hit, n);
    c.dpsMedian = medianOf(dps, n);
    return c;
}

BossCurve bossAt(const BossDefTable& table, u8 rawFloor, u8 difficulty) {
    BossCurve c;
    const BossDef* bd = findBossDefByFloor(table, rawFloor);
    if (!bd) return c;
    const u32 eff = effectiveFloor(rawFloor, difficulty);
    c.present = true;
    c.name    = bd->name;
    c.hp      = bd->baseHp  * GameConst::floorHealthMult(eff);
    c.hit     = bd->baseDmg * GameConst::floorDamageMult(eff)
              * GameConst::difficultyDamageBump(difficulty);
    c.dps     = (bd->atkCooldown > 0.0f) ? c.hit / bd->atkCooldown : c.hit;
    return c;
}

} // namespace BalanceLab
```

- [ ] **Step 6: Build + run**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*enemyTierForFloor*,*trash curve*,*boss curve*"`
Expected: PASS. If `enemy_loader.cpp`/`boss_loader.cpp` produce link errors against MaterialSystem symbols, extend `tests/stubs_material.cpp` with the missing no-op stubs (that file exists for exactly this).

Then the FULL build (game + tests) — the engine_startgame rewire must compile: `cmake --build build 2>&1 | tail -3`

- [ ] **Step 7: Commit**

```bash
git add src/game/enemy_def.h src/engine/engine_startgame.cpp tests/balance/ tests/CMakeLists.txt
git commit -m "feat(tests): balance lab scaffolding — enemy/boss curves off the real spawn math

enemyTierForFloor extracted to enemy_def.h so the spawner and the lab share one
floor->tier ladder.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Typical-gear Monte Carlo (drops + selection)

**Files:**
- Modify: `tests/balance/balance_lab.h`
- Modify: `tests/balance/balance_lab.cpp`
- Modify: `tests/balance/test_balance_lab.cpp`

- [ ] **Step 1: Write the failing tests** (append to test_balance_lab.cpp; add the includes)

```cpp
#include "game/build_score.h"
#include <cstring>

static void loadGearTables(ItemDef* items, u32& itemCount, AffixDef* affixes, u32& affixCount) {
    REQUIRE(ItemLoader::loadItemDefs (DUNGEON_REPO_ROOT "/assets/config/items.json",   items,   itemCount));
    REQUIRE(ItemLoader::loadAffixDefs(DUNGEON_REPO_ROOT "/assets/config/affixes.json", affixes, affixCount));
}

TEST_CASE("typical gear: same (floor,difficulty,trial) is bit-identical every run") {
    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    u32 ic = 0, ac = 0; loadGearTables(items, ic, affixes, ac);

    BalanceLab::DropSet a, b;
    BalanceLab::rollWindowDrops(25, 1, 7, items, ic, affixes, ac, a);
    BalanceLab::rollWindowDrops(25, 1, 7, items, ic, affixes, ac, b);
    REQUIRE(a.count == b.count);
    REQUIRE(a.count == BalanceLab::MAX_WINDOW_DROPS);
    CHECK(std::memcmp(a.items, b.items, sizeof(ItemInstance) * a.count) == 0);

    // A different trial must produce a different stream (or the Monte Carlo is a no-op).
    BalanceLab::DropSet c;
    BalanceLab::rollWindowDrops(25, 1, 8, items, ic, affixes, ac, c);
    CHECK(std::memcmp(a.items, c.items, sizeof(ItemInstance) * a.count) != 0);
}

TEST_CASE("typical gear: every build cell fields a weapon from mid-game windows") {
    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    u32 ic = 0, ac = 0; loadGearTables(items, ic, affixes, ac);

    const u8 floors[] = {10, 25, 50};
    for (u8 f : floors)
        for (u8 d = 0; d < 3; d += 2)                    // Normal + Hell
            for (u32 trial = 0; trial < 5; trial++) {
                BalanceLab::DropSet drops;
                BalanceLab::rollWindowDrops(f, d, trial, items, ic, affixes, ac, drops);
                for (u8 cell = 0; cell < 9; cell++) {
                    PlayerInventory inv;
                    BalanceLab::selectLoadout(drops, cell, items, ic, inv);
                    const ItemInstance& w = inv.equipped[static_cast<u32>(ItemSlot::WEAPON)];
                    REQUIRE(w.defId != 0xFFFF);          // family gate starved a column = balance bug
                    CHECK(BuildScore::weaponInFamily(items[w.defId].weaponSubtype,
                                                     BuildScore::buildCol(cell)));
                }
            }
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --target dungeon_tests 2>&1 | tail -5`
Expected: compile FAILURE — `DropSet`, `rollWindowDrops`, `selectLoadout` not members of BalanceLab.

- [ ] **Step 3: Implement** — append to balance_lab.h inside the namespace:

```cpp
// --- typical-equipment Monte Carlo -----------------------------------------------------------
struct DropSet { ItemInstance items[MAX_WINDOW_DROPS]; u32 count = 0; };

// The drops a floor-F player saw: DROPS_PER_FLOOR real ItemGen rolls per window floor, each
// at that floor's own effective level. Reseeds ItemGen from (floor,difficulty,trial) so a
// trial is deterministic and independent of sweep order — and the SAME drops are then shown
// to all nine build cells (the same loot fell; each build just wears it differently).
void rollWindowDrops(u8 rawFloor, u8 difficulty, u32 trial,
                     const ItemDef* defs, u32 defCount,
                     const AffixDef* affixDefs, u32 affixDefCount,
                     DropSet& out);

// Best-of-window per slot under BuildScore for `cell`, equipped into a real PlayerInventory
// via Inventory::equip (so recalculateStats runs and the stat caches are engine-true).
void selectLoadout(const DropSet& drops, u8 cell,
                   const ItemDef* defs, u32 defCount, PlayerInventory& outInv);
```

Append to balance_lab.cpp (add `#include "game/build_score.h"` at the top):

```cpp
// FNV-1a over the trial coordinates: stable, order-independent seeding. ItemGen's LCG maps
// seed->stream 1:1, so distinct trial coords give distinct (if correlated-looking) streams.
static u32 trialSeed(u8 rawFloor, u8 difficulty, u32 trial) {
    u32 h = 2166136261u;
    const u32 parts[3] = {rawFloor, difficulty, trial};
    for (u32 p : parts) { h ^= p; h *= 16777619u; }
    return h ? h : 1u;   // LCG seed 0 is legal but keep it nonzero for hygiene
}

void rollWindowDrops(u8 rawFloor, u8 difficulty, u32 trial,
                     const ItemDef* defs, u32 defCount,
                     const AffixDef* affixDefs, u32 affixDefCount, DropSet& out) {
    out.count = 0;
    ItemGen::init(trialSeed(rawFloor, difficulty, trial));
    const u8 first = (rawFloor > WINDOW_FLOORS - 1)
                   ? static_cast<u8>(rawFloor - (WINDOW_FLOORS - 1)) : 1;
    for (u8 f = first; f <= rawFloor; f++) {
        const u8 lvl = static_cast<u8>(effectiveFloor(f, difficulty));   // max 150, fits u8
        for (u32 k = 0; k < DROPS_PER_FLOOR && out.count < MAX_WINDOW_DROPS; k++)
            out.items[out.count++] = ItemGen::rollItem(lvl, defs, defCount,
                                                       affixDefs, affixDefCount);
    }
}

void selectLoadout(const DropSet& drops, u8 cell,
                   const ItemDef* defs, u32 defCount, PlayerInventory& outInv) {
    outInv = PlayerInventory{};
    for (u32 sl = 0; sl < static_cast<u32>(ItemSlot::COUNT); sl++) {
        s32 bestIdx = -1; f32 bestScore = 0.0f;
        for (u32 i = 0; i < drops.count; i++) {
            const ItemInstance& it = drops.items[i];
            if (it.defId == 0xFFFF || it.defId >= defCount) continue;
            if (static_cast<u32>(defs[it.defId].slot) != sl) continue;
            const f32 s = BuildScore::score(it, defs[it.defId], cell);
            if (s > bestScore) { bestScore = s; bestIdx = static_cast<s32>(i); }
        }
        if (bestIdx < 0) continue;                      // window dropped nothing for this slot
        const s8 bp = Inventory::addToBackpack(outInv, drops.items[bestIdx]);
        if (bp >= 0) Inventory::equip(outInv, static_cast<u8>(bp), defs);
    }
}
```

(Note: `score()` already returns 0 for out-of-family weapons and for pet consumables — pet defs are unrollable so they never appear in a DropSet at all.)

- [ ] **Step 4: Run**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*typical gear*"`
Expected: PASS. If the weapon pin fails for cell col 0 (Magic) it means the shipped drop tables genuinely can't produce a wand in a 48-item mid-game window — that is a real finding: STOP and report it rather than weakening the test.

- [ ] **Step 5: Commit**

```bash
git add tests/balance/
git commit -m "feat(tests): balance lab typical-gear Monte Carlo — real drops, auto-equip selection

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: `kClassDefs` extraction + player power

**Files:**
- Create: `src/game/class_defs.cpp` (table moved verbatim from engine_init.cpp:72-143)
- Modify: `src/engine/engine_init.cpp` (delete the table)
- Modify: `src/CMakeLists.txt` (add `game/class_defs.cpp`)
- Modify: `tests/CMakeLists.txt` (add it too)
- Modify: `tests/balance/balance_lab.h` / `balance_lab.cpp` (powerOf)
- Modify: `tests/balance/test_balance_lab.cpp`

- [ ] **Step 1: Extract the class table (mechanical, no behavior change)**

Create `src/game/class_defs.cpp`:

```cpp
// class_defs.cpp — the player class table (kClassDefs), moved out of engine_init.cpp so
// tests-only code (the balance lab) can link the real class base stats and skill lists
// without dragging in the whole Engine. Declared extern in game/item.h.
#include "game/item.h"

// <<< PASTE engine_init.cpp lines 72-143 HERE VERBATIM — the comment banner
//     "Player class definitions — 8 classes with 4 skills each" plus the whole
//     `const ClassDef kClassDefs[...] = { ... };` initializer. Do not edit values. >>>
```

Then delete those lines from `src/engine/engine_init.cpp`, add `game/class_defs.cpp` to the source list in `src/CMakeLists.txt` (next to the other `game/` entries if any, otherwise after `engine/` block), and add `${CMAKE_SOURCE_DIR}/src/game/class_defs.cpp` to `tests/CMakeLists.txt`.

Run: `cmake --build build 2>&1 | tail -3` — game AND tests both build (duplicate-symbol or missing-symbol failures mean the move was incomplete).

- [ ] **Step 2: Write the failing power tests** (append to test_balance_lab.cpp)

```cpp
#include "game/weapon_dps.h"
#include "game/combat.h"

TEST_CASE("player power: bare crafted sword gives exactly the hand-computable numbers") {
    // Two synthetic defs — index 0 a plain sword, index 1 unused. No affixes anywhere, so
    // every engine conversion (effective weapon, max health, armor) runs on knowns.
    static ItemDef defs[2] = {};
    std::strcpy(defs[0].name, "Lab Sword");
    defs[0].slot = ItemSlot::WEAPON;
    defs[0].weaponType = WeaponType::MELEE;
    defs[0].weaponSubtype = WeaponSubtype::SWORD;
    defs[0].baseDamage = 30.0f;
    defs[0].baseCooldown = 0.5f;

    ItemInstance sword{};
    sword.defId = 0; sword.damage = 30.0f; sword.rarity = Rarity::COMMON;

    PlayerInventory inv{};
    const s8 bp = Inventory::addToBackpack(inv, sword);
    REQUIRE(bp >= 0);
    Inventory::equip(inv, static_cast<u8>(bp), defs);

    static SkillDef noSkills[1] = {};
    const u8 cell = 4;   // Moderate / Melee -> representative class WARRIOR
    const BalanceLab::PlayerPower p = BalanceLab::powerOf(inv, cell, 1, defs, noSkills, 0);

    // sustained(30, 0.5, no clip) = 60; baseline crit 5% x2.0 -> x1.05 = 63.
    CHECK(p.weaponDps == doctest::Approx(63.0f));
    CHECK(p.castDps   == doctest::Approx(0.0f));        // no skill defs passed
    CHECK(p.totalDps  == doctest::Approx(63.0f));
    // No gear health, no armor: EHP == the representative class's base pool.
    const f32 warriorBase = kClassDefs[static_cast<u32>(PlayerClass::WARRIOR)].baseHealth;
    CHECK(p.ehp     == doctest::Approx(warriorBase));
    CHECK(p.sustain == doctest::Approx(0.0f));
}

TEST_CASE("player power: cast DPS uses the class skill list, unlock gating and CDR") {
    static ItemDef defs[1] = {};
    std::strcpy(defs[0].name, "Lab Wand");
    defs[0].slot = ItemSlot::WEAPON;
    defs[0].weaponType = WeaponType::PROJECTILE;
    defs[0].weaponSubtype = WeaponSubtype::WAND;
    defs[0].baseDamage = 10.0f;
    defs[0].baseCooldown = 0.5f;

    ItemInstance wand{};
    wand.defId = 0; wand.damage = 10.0f; wand.rarity = Rarity::COMMON;
    PlayerInventory inv{};
    Inventory::equip(inv, static_cast<u8>(Inventory::addToBackpack(inv, wand)), defs);

    // A synthetic skill def bound to the SORCERER's first skill id: 100 dmg / 2 s cooldown.
    const ClassDef& sorc = kClassDefs[static_cast<u32>(PlayerClass::SORCERER)];
    static SkillDef sd[1] = {};
    sd[0].id = sorc.skills[0];
    sd[0].damage = 100.0f;
    sd[0].cooldown = 2.0f;

    const u8 cell = 0;   // Tanky / Magic -> SORCERER
    // Below the unlock floor: no cast output. At/after it: damage/cooldown = 50 dps.
    const u8 unlock = sorc.skillUnlockFloor[0];
    if (unlock > 1) {
        const BalanceLab::PlayerPower locked =
            BalanceLab::powerOf(inv, cell, static_cast<u8>(unlock - 1), defs, sd, 1);
        CHECK(locked.castDps == doctest::Approx(0.0f));
    }
    const BalanceLab::PlayerPower p = BalanceLab::powerOf(inv, cell, unlock, defs, sd, 1);
    CHECK(p.castDps == doctest::Approx(50.0f));
}
```

Run: `cmake --build build --target dungeon_tests 2>&1 | tail -5`
Expected: compile FAILURE — `PlayerPower` / `powerOf` not declared.

- [ ] **Step 3: Implement** — append to balance_lab.h:

```cpp
// --- player power off a loadout --------------------------------------------------------------
struct PlayerPower { f32 weaponDps = 0, castDps = 0, totalDps = 0, ehp = 0, sustain = 0; };

// Representative class per damage COLUMN — a declared model assumption (spec): the class
// whose base HP and skill list stand in for everyone playing that archetype.
inline PlayerClass columnClass(u8 col) {
    switch (col) {
        case 0:  return PlayerClass::SORCERER;   // Magic
        case 2:  return PlayerClass::MARKSMAN;   // Ranged
        default: return PlayerClass::WARRIOR;    // Melee
    }
}

// All real engine functions: getEffectiveWeapon -> WeaponDps cycle x EV crit; class skills
// gated by unlock floor with gear spell rolls + real CDR; getEffectiveMaxHealth through
// armorMitigation for EHP; regen/life-on-hit/lifesteal as a separate sustain column.
// Deliberately NOT modeled (documented in the spec): skill energy costs, weapon range,
// enemy armor, player skill-aim. rawFloor gates skill unlocks only.
PlayerPower powerOf(const PlayerInventory& inv, u8 cell, u8 rawFloor,
                    const ItemDef* itemDefs,
                    const SkillDef* skillDefs, u32 skillDefCount);
```

Append to balance_lab.cpp (add includes `"game/weapon_dps.h"` and `"game/combat.h"`):

```cpp
PlayerPower powerOf(const PlayerInventory& inv, u8 cell, u8 rawFloor,
                    const ItemDef* itemDefs,
                    const SkillDef* skillDefs, u32 skillDefCount) {
    PlayerPower p;
    const u8 col = BuildScore::buildCol(cell);
    const ClassDef& cls = kClassDefs[static_cast<u32>(columnClass(col))];

    // Weapon DPS: the real effective weapon (attack speed, CDR, clip already applied by
    // buildWeaponDef), the shared cycle formula, expected-value crit.
    const WeaponDef unarmed{};   // fallback only; loadouts always fill the weapon slot
    const WeaponDef w = Inventory::getEffectiveWeapon(inv, itemDefs, unarmed);
    p.weaponDps = WeaponDps::sustained(w.damage, w.cooldown,
                                       static_cast<f32>(w.clipSize), w.reloadTime)
                * WeaponDps::expectedCritMult(w.critChance, w.critMult);

    // Cast DPS: the class's unlocked damage skills. Formula per spec:
    // (damage + spellFlat) * (1 + spellPct/100) / (cooldown * (1 - CDR)).
    const f32 spellFlat = Inventory::spellDamageFlat(inv);
    const f32 spellPct  = Inventory::spellDamagePct(inv);
    const f32 cdr       = inv.bonusCooldownReduction;      // recalculateStats caps at 0.5
    for (u32 s = 0; s < 4; s++) {
        if (cls.skills[s] == SkillId::NONE) continue;
        if (cls.skillUnlockFloor[s] > rawFloor) continue;
        const SkillDef* sd = nullptr;
        for (u32 i = 0; i < skillDefCount; i++)
            if (skillDefs[i].id == cls.skills[s]) { sd = &skillDefs[i]; break; }
        if (!sd || sd->damage <= 0.0f || sd->cooldown <= 0.0f) continue;
        p.castDps += (sd->damage + spellFlat) * (1.0f + spellPct * 0.01f)
                   / (sd->cooldown * (1.0f - cdr));
    }
    p.totalDps = p.weaponDps + p.castDps;

    // EHP: real max health divided through the real armor curve — same units as enemy damage.
    const f32 maxHp = Inventory::getEffectiveMaxHealth(inv, cls.baseHealth);
    p.ehp = maxHp / (1.0f - Combat::armorMitigation(Inventory::armorRating(inv)));

    // Sustain (HP/s), reported beside EHP rather than folded in (spec: folding needs a
    // fight-length assumption). Life-on-hit converts via the weapon's real swing rate.
    const f32 hitRate = (w.cooldown > 0.0f) ? 1.0f / w.cooldown : 0.0f;
    p.sustain = Inventory::healthRegenRate(inv)
              + inv.bonusLifeOnHit * hitRate
              + Inventory::lifestealPct(inv) * 0.01f * p.weaponDps;
    return p;
}
```

- [ ] **Step 4: Run**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*player power*"`
Expected: PASS (and the full suite stays green: `./build/tests/dungeon_tests --no-version | tail -3`).

- [ ] **Step 5: Commit**

```bash
git add src/game/class_defs.cpp src/engine/engine_init.cpp src/CMakeLists.txt tests/
git commit -m "feat(tests): balance lab player power off real engine math; kClassDefs -> class_defs.cpp

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Sweep, percentiles, CSV dump + always-on sanity pins

**Files:**
- Modify: `tests/balance/balance_lab.h` / `balance_lab.cpp`
- Modify: `tests/balance/test_balance_lab.cpp`

- [ ] **Step 1: Write the failing tests** (append; add `#include <cstdio>`, `#include <cstdlib>`)

```cpp
static void loadAllTables(ItemDef* items, u32& ic, AffixDef* affixes, u32& ac,
                          SkillDef* skills, u32& sc) {
    loadGearTables(items, ic, affixes, ac);
    REQUIRE(ItemLoader::loadSkillDefs(DUNGEON_REPO_ROOT "/assets/config/skills.json", skills, sc));
}

TEST_CASE("sweep row: percentiles ordered, TTK metrics consistent with their inputs") {
    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    static SkillDef skills[MAX_SKILL_DEFS];
    u32 ic = 0, ac = 0, sc = 0; loadAllTables(items, ic, affixes, ac, skills, sc);

    BalanceLab::MetricsRow r;
    BalanceLab::computeRow(0, 25, 4, /*trials=*/40, items, ic, affixes, ac, skills, sc,
                           enemyTable(), bossTable(), r);
    CHECK(r.tDps[0] <= r.tDps[1]);            // p10 <= p50
    CHECK(r.tDps[1] <= r.tDps[2]);            // p50 <= p90
    CHECK(r.ehp[0]  <= r.ehp[1]);
    CHECK(r.ttkTrash     == doctest::Approx(r.enemy.hpMedian / r.tDps[1]));
    CHECK(r.hitsToDie    == doctest::Approx(r.ehp[1] / r.enemy.hitMedian));
    CHECK(r.secondsToDie == doctest::Approx(r.ehp[1] / r.enemy.dpsMedian));
}

TEST_CASE("sanity pin: p50 gear power rises up the floor ladder for every cell") {
    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    static SkillDef skills[MAX_SKILL_DEFS];
    u32 ic = 0, ac = 0, sc = 0; loadAllTables(items, ic, affixes, ac, skills, sc);

    const u8 ladder[] = {10, 25, 40, 50};
    for (u8 d = 0; d < 3; d += 2)                       // Normal + Hell
        for (u8 cell = 0; cell < 9; cell += 4) {        // one cell per row/col diagonal
            f32 prevDps = 0, prevEhp = 0;
            for (u8 f : ladder) {
                BalanceLab::MetricsRow r;
                BalanceLab::computeRow(d, f, cell, 40, items, ic, affixes, ac, skills, sc,
                                       enemyTable(), bossTable(), r);
                // 0.98: p50-of-40-trials has noise; a real regression drops far more.
                CHECK(r.tDps[1] >= prevDps * 0.98f);
                CHECK(r.ehp[1]  >= prevEhp * 0.98f);
                prevDps = r.tDps[1]; prevEhp = r.ehp[1];
            }
        }
}

TEST_CASE("CSV smoke: header + one row per cell, parseable floats") {
    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    static SkillDef skills[MAX_SKILL_DEFS];
    u32 ic = 0, ac = 0, sc = 0; loadAllTables(items, ic, affixes, ac, skills, sc);

    const char* path = "balance_smoke.csv";              // build dir cwd
    FILE* fp = std::fopen(path, "w");
    REQUIRE(fp);
    BalanceLab::writeCsvHeader(fp);
    for (u8 cell = 0; cell < 9; cell++) {
        BalanceLab::MetricsRow r;
        BalanceLab::computeRow(0, 10, cell, 10, items, ic, affixes, ac, skills, sc,
                               enemyTable(), bossTable(), r);
        BalanceLab::writeCsvRow(fp, r);
    }
    std::fclose(fp);

    fp = std::fopen(path, "r");
    REQUIRE(fp);
    char line[1024]; u32 lines = 0;
    while (std::fgets(line, sizeof line, fp)) lines++;
    std::fclose(fp);
    std::remove(path);
    CHECK(lines == 10);                                  // header + 9 cells
}

// The report dump. NOT a test: env-gated so normal CI runs skip the ~min-long full sweep.
//   BALANCE_REPORT=/tmp/balance.csv ./build/tests/dungeon_tests -tc="*balance report*"
TEST_CASE("balance report: full sweep CSV when BALANCE_REPORT is set") {
    const char* path = std::getenv("BALANCE_REPORT");
    if (!path) return;

    static ItemDef items[MAX_ITEM_DEFS]; static AffixDef affixes[MAX_AFFIX_DEFS];
    static SkillDef skills[MAX_SKILL_DEFS];
    u32 ic = 0, ac = 0, sc = 0; loadAllTables(items, ic, affixes, ac, skills, sc);

    FILE* fp = std::fopen(path, "w");
    REQUIRE_MESSAGE(fp, "BALANCE_REPORT path not writable: ", path);
    BalanceLab::writeCsvHeader(fp);
    for (u8 d = 0; d < 3; d++)
        for (u8 f = 1; f <= 50; f++)
            for (u8 cell = 0; cell < 9; cell++) {
                BalanceLab::MetricsRow r;
                BalanceLab::computeRow(d, f, cell, BalanceLab::TRIALS,
                                       items, ic, affixes, ac, skills, sc,
                                       enemyTable(), bossTable(), r);
                BalanceLab::writeCsvRow(fp, r);
            }
    std::fclose(fp);
    MESSAGE("balance report written: ", path, " (1350 rows + header)");
}
```

Run to verify compile failure (`MetricsRow` / `computeRow` / `writeCsv*` missing).

- [ ] **Step 2: Implement** — append to balance_lab.h (and add `#include <cstdio>` to its
includes — `writeCsvHeader`/`writeCsvRow` take `FILE*`):

```cpp
// --- the sweep -------------------------------------------------------------------------------
struct MetricsRow {
    u8 difficulty = 0, rawFloor = 0, cell = 0;
    // Player power percentiles [p10, p50, p90] over the trial set.
    f32 wDps[3] = {}, cDps[3] = {}, tDps[3] = {}, ehp[3] = {}, sus[3] = {};
    EnemyCurve enemy;
    BossCurve  boss;
    // Derived from the p50s (the typical player) vs the median enemy.
    f32 ttkTrash = 0, ttkBoss = 0, hitsToDie = 0, secondsToDie = 0;
};

// One report row: `trials` Monte-Carlo trials (same drop stream for every cell — seeding is
// by (difficulty, floor, trial) only), percentiles, enemy/boss curves, derived metrics.
void computeRow(u8 difficulty, u8 rawFloor, u8 cell, u32 trials,
                const ItemDef* items, u32 itemCount,
                const AffixDef* affixes, u32 affixCount,
                const SkillDef* skills, u32 skillCount,
                const EnemyDefTable& enemies, const BossDefTable& bosses,
                MetricsRow& out);

void writeCsvHeader(FILE* fp);
void writeCsvRow(FILE* fp, const MetricsRow& r);
```

Append to balance_lab.cpp (add `#include <cstdio>`):

```cpp
// q in [0,1]; nearest-rank on a sorted copy. n is TRIALS-sized (<=200): stack array is fine.
static f32 percentileOf(const f32* v, u32 n, f32 q) {
    if (n == 0) return 0.0f;
    f32 tmp[TRIALS];
    for (u32 i = 0; i < n; i++) tmp[i] = v[i];
    std::sort(tmp, tmp + n);
    u32 idx = static_cast<u32>(q * static_cast<f32>(n - 1) + 0.5f);
    if (idx >= n) idx = n - 1;
    return tmp[idx];
}

void computeRow(u8 difficulty, u8 rawFloor, u8 cell, u32 trials,
                const ItemDef* items, u32 itemCount,
                const AffixDef* affixes, u32 affixCount,
                const SkillDef* skills, u32 skillCount,
                const EnemyDefTable& enemies, const BossDefTable& bosses,
                MetricsRow& out) {
    out = MetricsRow{};
    out.difficulty = difficulty; out.rawFloor = rawFloor; out.cell = cell;
    if (trials > TRIALS) trials = TRIALS;

    static f32 wv[TRIALS], cv[TRIALS], tv[TRIALS], ev[TRIALS], sv[TRIALS];
    for (u32 t = 0; t < trials; t++) {
        DropSet drops;
        rollWindowDrops(rawFloor, difficulty, t, items, itemCount, affixes, affixCount, drops);
        PlayerInventory inv;
        selectLoadout(drops, cell, items, itemCount, inv);
        const PlayerPower p = powerOf(inv, cell, rawFloor, items, skills, skillCount);
        wv[t] = p.weaponDps; cv[t] = p.castDps; tv[t] = p.totalDps;
        ev[t] = p.ehp;       sv[t] = p.sustain;
    }
    const f32 qs[3] = {0.10f, 0.50f, 0.90f};
    for (u32 i = 0; i < 3; i++) {
        out.wDps[i] = percentileOf(wv, trials, qs[i]);
        out.cDps[i] = percentileOf(cv, trials, qs[i]);
        out.tDps[i] = percentileOf(tv, trials, qs[i]);
        out.ehp[i]  = percentileOf(ev, trials, qs[i]);
        out.sus[i]  = percentileOf(sv, trials, qs[i]);
    }

    out.enemy = enemyTrashAt(enemies, rawFloor, difficulty);
    out.boss  = bossAt(bosses, rawFloor, difficulty);
    if (out.tDps[1] > 0.0f) {
        out.ttkTrash = out.enemy.hpMedian / out.tDps[1];
        if (out.boss.present) out.ttkBoss = out.boss.hp / out.tDps[1];
    }
    if (out.enemy.hitMedian > 0.0f) out.hitsToDie    = out.ehp[1] / out.enemy.hitMedian;
    if (out.enemy.dpsMedian > 0.0f) out.secondsToDie = out.ehp[1] / out.enemy.dpsMedian;
}

void writeCsvHeader(FILE* fp) {
    std::fprintf(fp,
        "difficulty,floor,effFloor,cell,row,col,"
        "wDps10,wDps50,wDps90,cDps10,cDps50,cDps90,tDps10,tDps50,tDps90,"
        "ehp10,ehp50,ehp90,sus10,sus50,sus90,"
        "enHpMed,enHpMin,enHpMax,enHit,enDps,"
        "bossName,bossHp,bossHit,"
        "ttkTrash,ttkBoss,hitsToDie,secondsToDie\n");
}

void writeCsvRow(FILE* fp, const MetricsRow& r) {
    std::fprintf(fp,
        "%u,%u,%u,%u,%u,%u,"
        "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
        "%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,"
        "%.1f,%.1f,%.1f,%.2f,%.2f,"
        "%s,%.1f,%.2f,"
        "%.3f,%.3f,%.3f,%.3f\n",
        r.difficulty, r.rawFloor, effectiveFloor(r.rawFloor, r.difficulty), r.cell,
        BuildScore::buildRow(r.cell), BuildScore::buildCol(r.cell),
        r.wDps[0], r.wDps[1], r.wDps[2], r.cDps[0], r.cDps[1], r.cDps[2],
        r.tDps[0], r.tDps[1], r.tDps[2],
        r.ehp[0], r.ehp[1], r.ehp[2], r.sus[0], r.sus[1], r.sus[2],
        r.enemy.hpMedian, r.enemy.hpMin, r.enemy.hpMax, r.enemy.hitMedian, r.enemy.dpsMedian,
        r.boss.present ? r.boss.name : "", r.boss.hp, r.boss.hit,
        r.ttkTrash, r.ttkBoss, r.hitsToDie, r.secondsToDie);
}
```

- [ ] **Step 3: Run the pins, then time a real dump**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="*sweep row*,*sanity pin*,*CSV smoke*"`
Expected: PASS.

Run: `time BALANCE_REPORT=/tmp/claude-1000/-home-aaron-game/9a6f70a9-a6ef-419b-a3c0-970dc2f6b5cb/scratchpad/balance.csv ./build/tests/dungeon_tests -tc="*balance report*" && wc -l < /tmp/claude-1000/-home-aaron-game/9a6f70a9-a6ef-419b-a3c0-970dc2f6b5cb/scratchpad/balance.csv`
Expected: 1351 lines. If wall time exceeds ~3 min, cut `TRIALS` to 100 (a model parameter, not a contract) and note it in the header comment.

- [ ] **Step 4: Commit**

```bash
git add tests/balance/
git commit -m "feat(tests): balance lab sweep — percentiles, TTK/hits-to-die metrics, env-gated CSV report

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: `tools/balance_chart.py` — CSV → one HTML chart page

Design rules applied (dataviz skill): one unit per axis (TTK, hits, seconds, DPS, HP each get their own chart — never dual-axis); 3 fixed-order categorical series (Magic=blue `#2a78d6`, Melee=orange `#eb6834`, Ranged=aqua `#1baf7a` — validated palette, dark variants `#3987e5`/`#d95926`/`#199e70`); legend + direct labels; text in ink colors, never series colors; hover crosshair tooltip; difficulty seams as reference lines; log Y only on the raw-power charts.

**Files:**
- Create: `tools/balance_chart.py`

- [ ] **Step 1: Write the script**

```python
#!/usr/bin/env python3
"""balance_chart.py — render the balance lab's CSV into one self-contained HTML page.

Usage:  python3 tools/balance_chart.py balance.csv -o balance.html
Input:  the CSV written by the "balance report" doctest case (BALANCE_REPORT=...).
Output: static HTML, no external assets: charts per metric (effective floor 1-150 on X,
        difficulty seams at 50/100), posture selector, p10-p90 band, boss markers,
        worst-offenders table. Pure stdlib so it runs anywhere the repo does.
"""
import argparse, csv, json, sys

COLS = ["Magic", "Melee", "Ranged"]          # fixed categorical order, col index 0/1/2
ROWS = ["Tanky", "Moderate", "Glass Cannon"]

# Charts: (title, y-label, metric key, band keys or None, log-y). One unit per axis.
CHARTS = [
    ("Time-to-kill trash",   "seconds", "ttkTrash",     None,                 False),
    ("Hits to die",          "hits",    "hitsToDie",    None,                 False),
    ("Seconds to die",       "seconds", "secondsToDie", None,                 False),
    ("Player total DPS",     "dmg/s",   "tDps50",       ("tDps10", "tDps90"), True),
    ("Player effective HP",  "HP",      "ehp50",        ("ehp10", "ehp90"),   True),
    ("Enemy HP (median)",    "HP",      "enHpMed",      None,                 True),
]

def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        sys.exit(f"no rows in {path}")
    for r in rows:
        for k, v in r.items():
            if k != "bossName":
                r[k] = float(v) if v else 0.0
    return rows

def build_series(rows):
    """out[postureRow][chartKey][col] = [[effFloor, value], ...] sorted by floor."""
    out = {pr: {} for pr in range(3)}
    bosses = []
    for r in sorted(rows, key=lambda r: r["effFloor"]):
        pr, pc = int(r["row"]), int(r["col"])
        for title, _, key, band, _log in CHARTS:
            d = out[pr].setdefault(key, {0: [], 1: [], 2: []})
            d[pc].append([r["effFloor"], r[key]])
            if band:
                for bk in band:
                    bd = out[pr].setdefault(bk, {0: [], 1: [], 2: []})
                    bd[pc].append([r["effFloor"], r[bk]])
        if r["bossName"] and pr == 1 and pc == 1:      # record each boss once
            bosses.append({"floor": r["effFloor"], "name": r["bossName"],
                           "hp": r["bossHp"], "hit": r["bossHit"]})
    return out, bosses

def worst_offenders(rows, key="ttkTrash", top=10):
    """Largest floor-over-floor jumps in `key` — the 'go fix this first' list."""
    jumps = []
    series = {}
    for r in sorted(rows, key=lambda r: r["effFloor"]):
        k = (int(r["row"]), int(r["col"]))
        prev = series.get(k)
        if prev is not None and prev[1] > 0:
            ratio = r[key] / prev[1] if prev[1] else 0
            jumps.append({"from": prev[0], "to": r["effFloor"],
                          "posture": ROWS[k[0]], "column": COLS[k[1]],
                          "before": round(prev[1], 2), "after": round(r[key], 2),
                          "ratio": round(ratio, 2)})
        series[k] = (r["effFloor"], r[key])
    jumps.sort(key=lambda j: max(j["ratio"], 1 / j["ratio"] if j["ratio"] else 1), reverse=True)
    return jumps[:top]

HTML = """<!doctype html><html><head><meta charset="utf-8">
<title>DungeonEngine Balance Report</title>
<style>
:root { --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --grid:#e4e3df;
        --s1:#2a78d6; --s2:#eb6834; --s3:#1baf7a; }
@media (prefers-color-scheme: dark) {
  :root { --surface:#1a1a19; --ink:#ffffff; --ink2:#c3c2b7; --grid:#33332f;
          --s1:#3987e5; --s2:#d95926; --s3:#199e70; } }
body { background:var(--surface); color:var(--ink);
       font:14px/1.45 system-ui,sans-serif; margin:24px; }
h1 { font-size:20px; } h2 { font-size:15px; margin:18px 0 4px; }
.controls { margin:12px 0; } .controls button { margin-right:6px; padding:4px 12px;
  border:1px solid var(--grid); background:none; color:var(--ink); border-radius:6px;
  cursor:pointer; } .controls button.on { border-color:var(--ink2); font-weight:600; }
.grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(430px,1fr)); gap:18px; }
svg text { fill:var(--ink2); font-size:11px; }
table { border-collapse:collapse; margin-top:8px; }
td,th { border:1px solid var(--grid); padding:3px 9px; font-size:13px; text-align:right; }
th { color:var(--ink2); font-weight:600; } td:nth-child(3),td:nth-child(4){text-align:left}
.tip { position:fixed; pointer-events:none; background:var(--surface); color:var(--ink);
  border:1px solid var(--grid); border-radius:6px; padding:5px 9px; font-size:12px;
  display:none; box-shadow:0 2px 8px rgba(0,0,0,.18); }
</style></head><body>
<h1>Balance Report — player power vs enemy power, effective floor 1–150</h1>
<p style="color:var(--ink2)">Series: <b style="color:var(--s1)">■</b> Magic
 <b style="color:var(--s2)">■</b> Melee <b style="color:var(--s3)">■</b> Ranged —
 p50 lines, p10–p90 band where shown. Dashed rules mark Normal→Nightmare→Hell.</p>
<div class="controls" id="posture"></div>
<div class="grid" id="charts"></div>
<h2>Worst offenders — largest floor-over-floor jumps in TTK</h2>
<table id="off"><tr><th>eff. floor</th><th>→</th><th>posture</th><th>column</th>
<th>before</th><th>after</th><th>×</th></tr></table>
<div class="tip" id="tip"></div>
<script>
const DATA = __DATA__, CHARTS = __CHARTS__, BOSSES = __BOSSES__, OFF = __OFF__;
const SER = ['--s1','--s2','--s3'], NAMES = ['Magic','Melee','Ranged'];
let posture = 1;
const css = v => getComputedStyle(document.documentElement).getPropertyValue(v);

function draw() {
  const host = document.getElementById('charts'); host.innerHTML = '';
  for (const [title, ylab, key, band, logY] of CHARTS) {
    const W = 460, H = 240, L = 52, R = 10, T = 24, B = 30;
    const d = DATA[posture][key];
    let ymin = Infinity, ymax = -Infinity;
    const bandD = band ? band.map(bk => DATA[posture][bk]) : null;
    const scan = bandD ? [d, ...bandD] : [d];
    for (const s of scan) for (const c in s) for (const [,v] of s[c])
      { if (v > 0 || !logY) { ymin = Math.min(ymin, v); ymax = Math.max(ymax, v); } }
    if (logY) { ymin = Math.max(ymin, 1e-3); }
    const X = f => L + (f - 1) / 149 * (W - L - R);
    const Y = v => { if (logY) { const a = Math.log(ymin), b = Math.log(Math.max(ymax, ymin * 1.01));
                     return T + (1 - (Math.log(Math.max(v, ymin)) - a) / (b - a)) * (H - T - B); }
                     return T + (1 - (v - 0) / (ymax || 1)) * (H - T - B); };
    let s = `<h2>${title}</h2><svg viewBox="0 0 ${W} ${H}" data-key="${key}">`;
    for (const sf of [50.5, 100.5])   // difficulty seams, drawn between the two floors
      s += `<line x1="${X(sf)}" y1="${T}" x2="${X(sf)}" y2="${H-B}"
             stroke="${css('--grid')}" stroke-dasharray="4"/>`;
    const ticks = logY ? [ymin, Math.sqrt(ymin*ymax), ymax] : [0, ymax/2, ymax];
    for (const tv of ticks)
      s += `<line x1="${L}" y1="${Y(tv)}" x2="${W-R}" y2="${Y(tv)}" stroke="${css('--grid')}"/>
            <text x="${L-4}" y="${Y(tv)+4}" text-anchor="end">${tv>=100?tv.toFixed(0):tv.toPrecision(3)}</text>`;
    for (const fx of [1, 50, 100, 150])
      s += `<text x="${X(fx)}" y="${H-B+16}" text-anchor="middle">${fx}</text>`;
    s += `<text x="${L}" y="${T-8}">${ylab}</text>`;
    for (const c of [0, 1, 2]) {
      const col = css(SER[c]);
      if (bandD) {
        const up = bandD[1][c], lo = bandD[0][c];
        s += `<path d="M${up.map(p=>X(p[0])+','+Y(p[1])).join('L')}
              L${[...lo].reverse().map(p=>X(p[0])+','+Y(p[1])).join('L')}Z"
              fill="${col}" opacity="0.13" stroke="none"/>`;
      }
      const pts = d[c];
      s += `<path d="M${pts.map(p=>X(p[0])+','+Y(p[1])).join('L')}" fill="none"
             stroke="${col}" stroke-width="2"/>`;
      const last = pts[pts.length-1];
      s += `<text x="${X(last[0])-2}" y="${Y(last[1])-5}" text-anchor="end">${NAMES[c]}</text>`;
    }
    if (key === 'ttkTrash') for (const b of BOSSES)
      s += `<circle cx="${X(b.floor)}" cy="${H-B-6}" r="3.5" fill="${css('--ink2')}">
            <title>${b.name} (floor ${b.floor})</title></circle>`;
    s += '</svg>';
    const div = document.createElement('div'); div.innerHTML = s; host.appendChild(div);
  }
  hover();
}

function hover() {                             // crosshair tooltip: nearest floor readout
  const tip = document.getElementById('tip');
  for (const svg of document.querySelectorAll('svg')) {
    svg.onmousemove = e => {
      const r = svg.getBoundingClientRect(), key = svg.dataset.key;
      const f = Math.max(1, Math.min(150, Math.round((e.clientX-r.left-52*r.width/460) /
                ((r.width-62*r.width/460)) * 149 + 1)));
      const d = DATA[posture][key];
      const vals = [0,1,2].map(c => { const p = d[c].find(p=>p[0]===f);
                                      return p ? p[1].toFixed(2) : '—'; });
      tip.style.display = 'block';
      tip.style.left = (e.clientX+14)+'px'; tip.style.top = (e.clientY+10)+'px';
      tip.innerHTML = `eff. floor <b>${f}</b><br>` +
        vals.map((v,c)=>`<span style="color:${css(SER[c])}">■</span> ${NAMES[c]}: ${v}`).join('<br>');
    };
    svg.onmouseleave = () => tip.style.display = 'none';
  }
}

const pc = document.getElementById('posture');
['Tanky','Moderate','Glass Cannon'].forEach((n, i) => {
  const b = document.createElement('button');
  b.textContent = n; b.className = i === posture ? 'on' : '';
  b.onclick = () => { posture = i;
    pc.querySelectorAll('button').forEach((x,j)=>x.className = j===i?'on':''); draw(); };
  pc.appendChild(b);
});
const off = document.getElementById('off');
for (const j of OFF) off.insertAdjacentHTML('beforeend',
  `<tr><td>${j.from}</td><td>${j.to}</td><td>${j.posture}</td><td>${j.column}</td>
   <td>${j.before}</td><td>${j.after}</td><td>${j.ratio}</td></tr>`);
draw();
</script></body></html>"""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv_path")
    ap.add_argument("-o", "--out", default="balance.html")
    args = ap.parse_args()
    rows = load(args.csv_path)
    data, bosses = build_series(rows)
    html = (HTML.replace("__DATA__", json.dumps(data))
                .replace("__CHARTS__", json.dumps([[t, y, k, b, l] for t, y, k, b, l in CHARTS]))
                .replace("__BOSSES__", json.dumps(bosses))
                .replace("__OFF__", json.dumps(worst_offenders(rows))))
    with open(args.out, "w") as f:
        f.write(html)
    print(f"wrote {args.out} ({len(rows)} rows)")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Generate a real report and render it**

```bash
BALANCE_REPORT=/tmp/claude-1000/-home-aaron-game/9a6f70a9-a6ef-419b-a3c0-970dc2f6b5cb/scratchpad/balance.csv \
  ./build/tests/dungeon_tests -tc="*balance report*"
python3 tools/balance_chart.py \
  /tmp/claude-1000/-home-aaron-game/9a6f70a9-a6ef-419b-a3c0-970dc2f6b5cb/scratchpad/balance.csv \
  -o /tmp/claude-1000/-home-aaron-game/9a6f70a9-a6ef-419b-a3c0-970dc2f6b5cb/scratchpad/balance.html
```

Expected: `wrote .../balance.html (1350 rows)`.

- [ ] **Step 3: LOOK at the output** (dataviz step 7 — the validator can't see layout)

Open the HTML (or screenshot it headlessly) and check: no label collisions, seams at 50/100, band visible, tooltip tracks, offenders table populated. Fix anything broken before committing. Then show the page to Aaron — this is the artifact the whole project exists to produce, and target-band selection (phase 2) starts from it.

- [ ] **Step 4: Commit**

```bash
git add tools/balance_chart.py
git commit -m "feat(tools): balance_chart.py — balance-lab CSV to a one-page HTML curve report

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: Documentation + final verification

**Files:**
- Modify: `CLAUDE.md` (Testing section)
- Modify: `.claude/skills/engine-how-to/SKILL.md` (add the recipe)

- [ ] **Step 1: CLAUDE.md** — append to the **Testing** section:

```markdown
**Balance lab.** `tests/balance/` holds a repeatable balance model (spec:
`docs/superpowers/specs/2026-07-22-balance-lab-design.md`): typical-equipment player power
(Monte-Carlo through the real `ItemGen`/`BuildScore`/`Inventory` code) vs enemy/boss curves
(the real `GameConst` spawn multipliers) per (difficulty, floor, build cell). Always-on sanity
pins run with the suite; the full CSV report is env-gated:
`BALANCE_REPORT=out.csv ./build/tests/dungeon_tests -tc="*balance report*"`, then
`python3 tools/balance_chart.py out.csv -o out.html` for the chart page. Three single-source
extractions exist FOR the lab — the sustained-DPS cycle (`game/weapon_dps.h`, shared with
`build_score.h`), `Combat::armorMitigation` (inline in `combat.h`), `kClassDefs`
(`game/class_defs.cpp`), and `enemyTierForFloor` (`enemy_def.h`, shared with the spawner) —
re-inlining any of them re-creates the scorer-drift bug the 2026-07-22 loot fixes cleaned up.
Phase 2 (pending): chosen target bands become REQUIREs in `test_balance_lab.cpp` so CI fails
when a content/constant change knocks a floor out of band.
```

- [ ] **Step 2: engine-how-to** — add a "Run a balance report" recipe mirroring the paragraph above (how to run, where the model parameters live — `tests/balance/balance_lab.h` (`DROPS_PER_FLOOR`, `WINDOW_FLOORS`, `TRIALS`, `columnClass`) — and the phase-2 note that target bands go in `test_balance_lab.cpp` with a why-comment per number, `game_constants.h` discipline).

- [ ] **Step 3: Full verification**

```bash
cmake --build build 2>&1 | tail -3          # game + tests compile
./build/tests/dungeon_tests --no-version | tail -3   # whole suite green
cmake --build build-rel 2>&1 | tail -3      # release config still builds (if build-rel exists)
```

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md .claude/skills/engine-how-to/
git commit -m "docs: balance lab — CLAUDE.md paragraph + engine-how-to recipe

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Post-plan notes for the executor

- **Do not** widen scope into actually retuning constants — this plan ends at "report exists, charts render, pins green." Target bands are phase 2, chosen with Aaron off the first report.
- If any always-on pin reveals a real imbalance (no wand in a Magic window, non-monotone gear power), **stop and surface it** — that's the lab working, not the lab broken.
- Runtime budget: the full dump is allowed minutes; the always-on pins must stay under ~10 s total (they use 10-40 trials and sampled floors for exactly that reason).
