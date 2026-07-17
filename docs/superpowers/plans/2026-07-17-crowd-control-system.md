# Crowd Control System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a unified gear CC-Resistance stat, player-facing stun, a counter-CC legendary boots, PvP block hardening, a themed CC tool for every under-served class, and PvP stun diminishing returns — all at the engine's "Rocket League" performance bar (60 FPS / 16.6 ms, predictive+reconciled netcode, no rubber-band).

**Architecture:** CC resistance is summed on-demand from equipped affixes (like the Armor/Thorns defensive pack) and stamped into a transient `Player` field — **no cached field, no SAVE_VERSION bump.** All player CC routes through one choke function, `Combat::applyCCToPlayer`, which applies tenacity, immunity, and PvP stun diminishing returns in one place. Player stun is server-authoritative and replicated via the `shadowDanceTimer` pattern (NetPlayer field + seed/writeback mirroring + a SnapPlayer status bit + the existing `reserved0` byte), so a stunned guest predicts its own input-lock and reconciles without rubber-banding. Class CC lands on Arena players through the existing `Combat::pvp*` helper twins.

**Tech Stack:** C++17, doctest (vendored `external/doctest/doctest.h`), the engine's pool-allocation / fixed-timestep / server-authoritative-snapshot conventions.

**Spec:** `docs/superpowers/specs/2026-07-17-crowd-control-system-design.md` (read it first — the decision table is the source of truth for every number).

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/game/crowd_control.h` | **NEW** pure header: CC duration scaling, 60% cap, stun DR ladder. Testable with no engine deps (the `arena.h`/`stash.h` pattern). | Create |
| `tests/game/test_crowd_control.h` *(cpp)* | Unit tests for the pure logic. | Create |
| `src/game/item.h` | `AffixType::CC_RESIST`; `Inventory::ccResist` decl; `SkillId::BREAK_FREE`. | Modify |
| `src/game/item_loader.cpp` | `affixTypeFromString("cc_resist")`. | Modify |
| `src/game/inventory.cpp` | `Inventory::ccResist()` on-demand sum (no cached field). | Modify |
| `assets/config/affixes.json` | The "of Steadfastness" affix entry. | Modify |
| `src/game/player.h` | Transient CC fields: `ccResist`, `stunTimer`, `ccImmuneTimer`, `ccDodgeImmune`, `stunDrTimer`, `stunDrCount` (all never-serialized). | Modify |
| `src/game/combat.h` / `combat.cpp` | `Combat::applyCCToPlayer` choke; `CcType`; PvP block classify + energy; `PvpHit` gains `stunDuration`/`knockback`. | Modify |
| `src/engine/engine_update_skills.cpp` | Stamp `ccResist`/`ccDodgeImmune`; tick CC timers; boots passive. | Modify |
| `src/engine/engine_update_player.cpp` | Stun input-lock; block energy drain; perfect-dodge clears CC. | Modify |
| `src/net/net_player.h` | NetPlayer CC fields (`shadowDanceTimer` template). | Modify |
| `src/net/snapshot.h` / `snapshot.cpp` | SnapPlayer stun bit (`flags` bit5) + `reserved0` → quantized stun timer; serialize/deserialize. | Modify |
| `src/net/net.h` | `PROTOCOL_VERSION` 20 → 21. | Modify |
| `src/engine/engine.cpp` | Mirror CC fields in the 3 seed/writeback sites; `landPvpHit` routes through `applyCCToPlayer`; stun onHitEffect. | Modify |
| `src/engine/engine_net.cpp` | Server-tick CC decay for remote lanes; client adopt of stun. | Modify |
| `src/engine/engine.h` | `ScorchZone` gains `slowPct` (Ranger slow-zone reuse). | Modify |
| `src/game/skill_ranger.cpp` / `skill_marksman.cpp` / `skill_tinkerer.cpp` | Class CC + PvP twins. | Modify |
| `assets/config/items.json` | Steadfast Greaves legendary boots (append-only). | Modify |
| `assets/config/skills.json` | `break_free` SkillDef. | Modify |
| `src/engine/asset_manifest.h` + `tools/build_assets.py` + `tools/gen_mesh.py` | Steadfast Greaves mesh (both, or the asset build fails). | Modify |
| `tests/CMakeLists.txt` | Register the new test file. | Modify |

**Commit trailer for every commit:** `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

## Task 1: Pure CC logic — duration scaling, cap, stun diminishing returns

**Files:**
- Create: `src/game/crowd_control.h`
- Test: `tests/game/test_crowd_control.cpp`
- Modify: `tests/CMakeLists.txt:58` (register the test)

- [ ] **Step 1: Write the failing test**

Create `tests/game/test_crowd_control.cpp`:

```cpp
// test_crowd_control.cpp — pure CC math: tenacity duration scaling, the 60% resist cap,
// and the PvP stun diminishing-returns ladder. No engine deps (arena.h/stash.h pattern).
#include <doctest/doctest.h>
#include "game/crowd_control.h"

TEST_CASE("CC: tenacity scales duration, cap clamps at 0.60") {
    CHECK(CrowdControl::scaleDuration(2.0f, 0.0f)  == doctest::Approx(2.0f));
    CHECK(CrowdControl::scaleDuration(2.0f, 0.30f) == doctest::Approx(1.4f));
    CHECK(CrowdControl::scaleDuration(2.0f, 0.60f) == doctest::Approx(0.8f));
    // Over-cap resist is clamped by capResist, never by scaleDuration itself:
    CHECK(CrowdControl::capResist(0.95f) == doctest::Approx(0.60f));
    CHECK(CrowdControl::capResist(0.25f) == doctest::Approx(0.25f));
}

TEST_CASE("CC: stun DR ladder 1.0 -> 0.5 -> 0.25 -> 0.0, then window reset") {
    CrowdControl::StunDr dr;                       // fresh: count 0, timer 0
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(1.0f));  // 1st
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(0.5f));  // 2nd
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(0.25f)); // 3rd
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(0.0f));  // 4th: immune
    CHECK(dr.count == 4);
    // Window lapses -> count resets, next stun is full again.
    CrowdControl::tickStunDr(dr, 9.0f);            // 9s > 8s window
    CHECK(dr.count == 0);
    CHECK(CrowdControl::advanceStunDr(dr, 8.0f) == doctest::Approx(1.0f));
}
```

Add to `tests/CMakeLists.txt` after line 58 (`game/test_arena.cpp`):

```cmake
    game/test_crowd_control.cpp      # CC math: tenacity scaling, 60% cap, stun DR ladder (header-only)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target dungeon_tests 2>&1 | tail -5`
Expected: FAIL — `crowd_control.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `src/game/crowd_control.h`:

```cpp
#pragma once
// crowd_control.h — pure CC math shared by the choke helper (Combat::applyCCToPlayer) and the
// unit tests. No engine deps (the arena.h / stash.h pattern). Holds the tenacity duration
// scale, the 60% resist cap, and the PvP stun diminishing-returns ladder so PvP can't perma-lock.
#include "core/types.h"

namespace CrowdControl {

constexpr f32 RESIST_CAP   = 0.60f;   // max CC Resistance from all gear combined
constexpr f32 DR_WINDOW    = 8.0f;    // seconds; repeated stuns inside this window diminish
constexpr u8  DR_IMMUNE_AT = 3;       // the 4th stun (count==3 before advance) is fully negated

// Tenacity: a CC's duration after % resistance. resist is assumed already capped (capResist).
inline f32 scaleDuration(f32 duration, f32 resist) { return duration * (1.0f - resist); }

// Clamp a summed gear resistance to the hard cap. Negative guarded to 0.
inline f32 capResist(f32 resist) {
    if (resist < 0.0f)        return 0.0f;
    if (resist > RESIST_CAP)  return RESIST_CAP;
    return resist;
}

// Per-victim stun diminishing-returns state (PvP victims only). Lives on Player/NetPlayer.
struct StunDr {
    f32 timer = 0.0f;   // seconds remaining in the current DR window (0 = window closed)
    u8  count = 0;      // stuns landed in the window so far
};

// Decay the DR window; when it lapses, the count resets so the next stun is full again.
inline void tickStunDr(StunDr& dr, f32 dt) {
    if (dr.timer > 0.0f) {
        dr.timer -= dt;
        if (dr.timer <= 0.0f) { dr.timer = 0.0f; dr.count = 0; }
    }
}

// Returns the duration multiplier for the NEXT stun AND advances the ladder as a side effect:
// count 0->1 returns 1.0, 1->2 returns 0.5, 2->3 returns 0.25, 3->4 returns 0.0 (immune).
// Refreshes the window to `window` seconds on every stun.
inline f32 advanceStunDr(StunDr& dr, f32 window) {
    f32 mult;
    switch (dr.count) {
        case 0:  mult = 1.0f;  break;
        case 1:  mult = 0.5f;  break;
        case 2:  mult = 0.25f; break;
        default: mult = 0.0f;  break;   // 4th+ stun in the window: immune
    }
    if (dr.count < 255) dr.count++;
    dr.timer = window;
    return mult;
}

} // namespace CrowdControl
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="CC:*"`
Expected: PASS (2 test cases).

- [ ] **Step 5: Commit**

```bash
git add src/game/crowd_control.h tests/game/test_crowd_control.cpp tests/CMakeLists.txt
git commit -m "feat(cc): pure CC math — tenacity scale, 60% cap, stun DR ladder"
```

---

## Task 2: CC_RESIST affix — on-demand sum (no save bump)

**Files:**
- Modify: `src/game/item.h:75-77` (append `CC_RESIST` before `COUNT`)
- Modify: `src/game/item_loader.cpp:177` (add the string case)
- Modify: `src/game/inventory.cpp:95` (add `Inventory::ccResist`), and its decl in `item.h`
- Modify: `assets/config/affixes.json` (the affix entry)
- Test: extend `tests/game/test_crowd_control.cpp`

> **Save safety (critical):** `PlayerInventory` is serialized raw (`static_assert(sizeof == 1676)`
> in `engine_persist.cpp:97`). A cached `bonus*` field would force a SAVE_VERSION bump. Do it the
> way `Inventory::armorRating`/`thornsPct` do — sum on demand (`inventory.cpp:84` comment) — so
> there is **no `PlayerInventory` field and no save change.**

- [ ] **Step 1: Write the failing test**

Append to `tests/game/test_crowd_control.cpp`:

```cpp
#include "game/inventory.h"
#include "game/item.h"

static ItemInstance mkResistBoots(f32 resist) {
    ItemInstance it{};
    it.defId = 0;                    // any valid def; ccResist only reads affixes
    it.affixCount = 1;
    it.affixes[0].type  = AffixType::CC_RESIST;
    it.affixes[0].value = resist;
    return it;
}

TEST_CASE("Inventory::ccResist sums equipped CC_RESIST and clamps to 0.60") {
    PlayerInventory inv{};
    CHECK(Inventory::ccResist(inv) == doctest::Approx(0.0f));           // nothing equipped
    inv.equipped[(u32)ItemSlot::BOOTS] = mkResistBoots(0.30f);
    CHECK(Inventory::ccResist(inv) == doctest::Approx(0.30f));
    inv.equipped[(u32)ItemSlot::HELMET] = mkResistBoots(0.10f);         // stacks
    CHECK(Inventory::ccResist(inv) == doctest::Approx(0.40f));
    inv.equipped[(u32)ItemSlot::ARMOR] = mkResistBoots(0.50f);          // would be 0.90
    CHECK(Inventory::ccResist(inv) == doctest::Approx(0.60f));          // clamped
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target dungeon_tests 2>&1 | tail -5`
Expected: FAIL — `'CC_RESIST' is not a member of 'AffixType'` and `'ccResist' is not a member of 'Inventory'`.

- [ ] **Step 3: Implement — enum, loader, sum**

In `src/game/item.h`, append **before `COUNT`** in `enum struct AffixType` (after `SPELL_DAMAGE_PCT`, ~line 76):

```cpp
    CC_RESIST,          // % reduction of incoming stun/slow/freeze DURATION (tenacity). Summed on
                        // demand (no cached field, no save bump) and capped at CrowdControl::RESIST_CAP.
```

In `src/game/item_loader.cpp`, after line 177 (`spell_damage_pct`):

```cpp
    if (s == "cc_resist"          || s == "CC_RESIST")          return AffixType::CC_RESIST;
```

In `src/game/inventory.cpp`, after `thornsPct` (~line 95):

```cpp
// CC Resistance — on-demand sum (no cached field → no save-format change), clamped to the hard
// cap. Stamped into the transient Player.ccResist each frame in tickPassiveEquipment and consumed
// by Combat::applyCCToPlayer. The legendary boots carry a high CC_RESIST affix, so they need no
// special-casing here.
f32 Inventory::ccResist(const PlayerInventory& inv) {
    return CrowdControl::capResist(sumEquippedAffix(inv, AffixType::CC_RESIST));
}
```

Add `#include "game/crowd_control.h"` to `inventory.cpp` if not already present. Declare in
`item.h` beside the other `Inventory::` on-demand helpers (search for `f32 thornsPct`):

```cpp
    f32 ccResist(const PlayerInventory& inv);
```

- [ ] **Step 4: Add the affix JSON entry**

Append to the `"affixes"` array in `assets/config/affixes.json` (per-slot bands from the spec):

```json
    {
      "name": "of Steadfastness",
      "type": "cc_resist",
      "slots": ["boots", "helmet", "armor"],
      "minValue": 0.05,
      "maxValue": 0.30,
      "minLevel": 1,
      "weight": 1.0
    }
```

> The boots-signature vs small-elsewhere split (boots 0.15–0.30, helmet/armor 0.05–0.12) is a
> value-band nuance; the single entry above is the minimal wiring. If per-slot bands are desired,
> confirm the affix roll path supports slot-conditioned ranges before splitting into entries.

- [ ] **Step 5: Run to verify it passes + full suite**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="Inventory::ccResist*,CC:*"`
Expected: PASS. Then `./build/tests/dungeon_tests 2>&1 | tail -3` — full suite green.

- [ ] **Step 6: Commit**

```bash
git add src/game/item.h src/game/item_loader.cpp src/game/inventory.cpp assets/config/affixes.json tests/game/test_crowd_control.cpp
git commit -m "feat(cc): CC_RESIST affix — on-demand sum, 60% cap, no save bump"
```

---

## Task 3: Player stun state + the applyCCToPlayer choke + input-lock

**Files:**
- Modify: `src/game/player.h:124` (transient CC fields — never serialized)
- Modify: `src/game/combat.h` / `combat.cpp` (the choke helper + `CcType`)
- Modify: `src/engine/engine_update_skills.cpp:177` (stamp ccResist; tick CC timers)
- Modify: `src/engine/engine_update_player.cpp` (stun input-lock)
- Modify: existing player-CC sites to route through the choke
- Test: extend `tests/game/test_crowd_control.cpp` (choke unit test)

- [ ] **Step 1: Add transient Player fields**

In `src/game/player.h`, after `blockTimer` (line 124), in the transient/never-serialized block:

```cpp
    // --- Crowd control (all TRANSIENT, never serialized — like armorRating above) ---
    f32  ccResist       = 0.0f;  // stamped each frame from Inventory::ccResist (0..0.60)
    f32  stunTimer      = 0.0f;  // >0 = action-locked (no move/attack/cast/dodge; camera free). PvP-only source.
    f32  ccImmuneTimer  = 0.0f;  // >0 = immune to ALL new CC (Break Free / post-cleanse window)
    bool ccDodgeImmune  = false; // Steadfast Greaves equipped: i-frame dodge negates+clears CC
    CrowdControl::StunDr stunDr; // PvP stun diminishing-returns state
```

Add `#include "game/crowd_control.h"` to `player.h`.

- [ ] **Step 2: Write the failing choke test**

Append to `tests/game/test_crowd_control.cpp`:

```cpp
#include "game/player.h"

TEST_CASE("applyCCToPlayer: resist scales, immunity blocks, stun DR in PvP") {
    Player p;
    p.ccResist = 0.50f;
    // Slow scales by resist, never diminishes:
    Combat::applyCCToPlayer(p, Combat::CcType::SLOW, 2.0f, /*isPvp=*/true);
    CHECK(p.slowTimer == doctest::Approx(1.0f));
    // Immunity blocks everything:
    p.ccImmuneTimer = 1.0f; p.stunTimer = 0.0f;
    Combat::applyCCToPlayer(p, Combat::CcType::STUN, 2.0f, true);
    CHECK(p.stunTimer == doctest::Approx(0.0f));
    // PvP stun: resist (0.5) then DR first-hit (1.0) -> 1.0s:
    p.ccImmuneTimer = 0.0f;
    Combat::applyCCToPlayer(p, Combat::CcType::STUN, 2.0f, true);
    CHECK(p.stunTimer == doctest::Approx(1.0f));
    // PvE stun (isPvp=false) skips DR entirely — but stun has no PvE source; assert resist only:
    Player e; e.ccResist = 0.0f;
    Combat::applyCCToPlayer(e, Combat::CcType::FREEZE, 1.0f, false);
    CHECK(e.freezeTimer == doctest::Approx(1.0f));
}
```

- [ ] **Step 3: Implement the choke helper**

In `src/game/combat.h`, in the `Combat` namespace near the other player helpers:

```cpp
    enum struct CcType : u8 { STUN, SLOW, FREEZE };
    // The SINGLE entry point for applying crowd control to a player. Applies tenacity (ccResist),
    // an immunity/dodge-invuln early-out, and PvP stun diminishing returns. Every CC source
    // (enemy hits, projectile onHit, arena PvP) MUST route through this — a direct timer write
    // bypasses resist/DR and re-creates the perma-lock the DR ladder exists to prevent.
    void applyCCToPlayer(Player& p, CcType type, f32 duration, bool isPvp);
```

In `src/game/combat.cpp`:

```cpp
void Combat::applyCCToPlayer(Player& p, CcType type, f32 duration, bool isPvp) {
    if (p.ccImmuneTimer > 0.0f) return;                 // Break Free / post-cleanse immunity
    // Boots 2a: negate during a dodge roll's i-frames — ONLY with the Steadfast Greaves flag
    // (a base dodge must NOT shrug off CC, or CC is universally weak and the boots lose identity).
    if (p.ccDodgeImmune && p.rollTimer > 0.0f) return;
    duration = CrowdControl::scaleDuration(duration, p.ccResist);
    switch (type) {
        case CcType::STUN:
            if (isPvp) duration *= CrowdControl::advanceStunDr(p.stunDr, CrowdControl::DR_WINDOW);
            p.stunTimer   = fmaxf(p.stunTimer,   duration);
            break;
        case CcType::SLOW:   p.slowTimer   = fmaxf(p.slowTimer,   duration); break;
        case CcType::FREEZE: p.freezeTimer = fmaxf(p.freezeTimer, duration); break;
    }
}
```

Add `#include "game/crowd_control.h"` to `combat.cpp` if needed.

- [ ] **Step 4: Run the choke test**

Run: `cmake --build build --target dungeon_tests && ./build/tests/dungeon_tests -tc="applyCCToPlayer*"`
Expected: PASS.

- [ ] **Step 5: Stamp ccResist; tick CC timers**

In `src/engine/engine_update_skills.cpp`, in `tickPassiveEquipment` next to `armorRating` (line 177):

```cpp
        m_localPlayer.ccResist = Inventory::ccResist(inv);
```

(The `ccDodgeImmune` stamp needs `SkillId::BREAK_FREE`, which Task 5 adds — it is wired there, so
`ccDodgeImmune` stays its default `false` until then, which the choke handles harmlessly.)

Add stun/immunity decay where `slowTimer`/`freezeTimer` already decay for the local player
(search `m_localPlayer.freezeTimer -= dt` / the player timer-decay block in
`engine_update_player.cpp`), plus DR window tick:

```cpp
    if (m_localPlayer.stunTimer > 0.0f)     m_localPlayer.stunTimer     -= dt;
    if (m_localPlayer.ccImmuneTimer > 0.0f) m_localPlayer.ccImmuneTimer -= dt;
    CrowdControl::tickStunDr(m_localPlayer.stunDr, dt);
```

- [ ] **Step 6: Stun input-lock (action-lock, camera-free)**

In the player-input path (`PlayerController` update / `engine_update_player.cpp` where inputs are
read into movement/attack/skill), gate on stun. **Suppress move/attack/cast/dodge; leave look
untouched.** Representative guard at the top of the input-application block:

```cpp
    const bool stunned = m_localPlayer.stunTimer > 0.0f;
    // stunned: zero movement intent, block fire/skill/dodge edges. Camera (yaw/pitch) still applies.
    if (stunned) { moveInput = {0,0,0}; fireHeld = false; /* skip skill + dodge activation below */ }
```

Ensure the dodge-activation branch is skipped when `stunned` **unless** `ccDodgeImmune` (the boots
let you dodge out — Task 5 wires the clear-on-roll).

- [ ] **Step 7: Route existing player-CC sites through the choke**

Replace direct player-timer writes so resist/immunity apply everywhere:

- `src/game/enemy_ai_states.cpp:556` (`targetPlayer->slowTimer = ...`) →
  `Combat::applyCCToPlayer(*targetPlayer, Combat::CcType::SLOW, e.onHitDuration, false);`
- `src/game/enemy_ai_states.cpp:558` (freeze) →
  `Combat::applyCCToPlayer(*targetPlayer, Combat::CcType::FREEZE, e.onHitDuration, false);`
- `src/game/projectile.cpp:138` (slow) and `:142` (freeze) → the same, `isPvp=false`.
  (Leave `poisonTimer`/`burnTimer` writes as direct — DoTs are not CC.)

> `landPvpHit` (engine.cpp) is routed in Task 6 (it needs the `isPvp=true` + block gating).

- [ ] **Step 8: Run full suite + build the game**

Run: `cmake --build build && ./build/tests/dungeon_tests 2>&1 | tail -3`
Expected: game builds clean; suite green.

- [ ] **Step 9: Commit**

```bash
git add src/game/player.h src/game/combat.h src/game/combat.cpp src/engine/engine_update_skills.cpp src/engine/engine_update_player.cpp src/game/enemy_ai_states.cpp src/game/projectile.cpp tests/game/test_crowd_control.cpp
git commit -m "feat(cc): player stun state + applyCCToPlayer choke + input-lock, route PvE CC through it"
```

---

## Task 4: Netcode — replicate player stun (predictive, reconciled, delta-cheap)

**Files:**
- Modify: `src/net/net_player.h:166` (NetPlayer CC fields — `shadowDanceTimer` template)
- Modify: `src/net/snapshot.h:16` (SnapPlayer stun bit + `reserved0` → stun timer byte)
- Modify: `src/net/snapshot.cpp` (serialize + deserialize the stun byte/bit)
- Modify: `src/net/net.h` (`PROTOCOL_VERSION` 20 → 21)
- Modify: `src/engine/engine.cpp` (mirror in 3 seed/writeback sites)
- Modify: `src/engine/engine_net.cpp` (server-tick decay for remote lanes; client adopt)

> **The RL-feel requirement:** the client adopts `stunTimer` from the snapshot and locks its own
> input the same frame, and the stun rides the existing rollback-replay reconciliation, so there
> is no rubber-band. DR counters stay server-side (not in SnapPlayer) so they never disagree with
> the client. Copy `shadowDanceTimer` (done right); do NOT copy `overdriveTimer` (has no NetPlayer
> field — broken in co-op).

- [ ] **Step 1: NetPlayer fields**

In `src/net/net_player.h`, next to `shadowDanceTimer` (line 166):

```cpp
    f32  stunTimer      = 0.0f;  // action-lock (mirrors Player::stunTimer). PvP-only source.
    f32  ccImmuneTimer  = 0.0f;  // immune to new CC (Break Free / post-cleanse)
    CrowdControl::StunDr stunDr; // PvP stun diminishing-returns state (server-side only)
```

Add `#include "game/crowd_control.h"`.

- [ ] **Step 2: Mirror in the 3 seed/writeback sites (engine.cpp)**

Add the three fields (`stunTimer`, `ccImmuneTimer`, `stunDr`) to **all three** mirror functions —
the host/couch `syncNetPlayerToLocalPlayer` (~1317 seed) / `syncLocalPlayerToNetPlayer` (~1351
writeback), and `seedRemoteView` (~1347) / `writeBackRemoteView` (~1398). Beside each existing
`slowTimer`/`freezeTimer` line:

```cpp
    np.stunTimer     = m_localPlayer.stunTimer;     np.ccImmuneTimer = m_localPlayer.ccImmuneTimer;  // seed
    m_localPlayer.stunTimer = np.stunTimer;         m_localPlayer.ccImmuneTimer = np.ccImmuneTimer;  // writeback
    v.stunTimer = np.stunTimer;  v.ccImmuneTimer = np.ccImmuneTimer;                                 // seedRemoteView
    np.stunTimer = v.stunTimer;  np.ccImmuneTimer = v.ccImmuneTimer;  np.stunDr = v.stunDr;          // writeBackRemoteView
```

> `seedRemoteView` starts from `Player{}` — an unmirrored field is zeroed every frame. Mirror
> `stunDr` on the writeback side (server-authoritative) so DR persists across the atomic apply.

- [ ] **Step 3: SnapPlayer stun bit + timer byte**

In `src/net/snapshot.h` (`SnapPlayer`, line 16):
- Repurpose `flags` bit5 as `stunned` (comment says bits5-7 unused): update the `flags` comment to
  `bit5=stunned`.
- Repurpose `reserved0` (line 68, "always 0") as `stunTimerQ` — quantized 0–10 s in 0.04 s steps
  (same quantization as `invulnTimer`). Rename the field and its comment:

```cpp
    u8   stunTimerQ;    // 1: quantized stun remaining 0-10s in 0.04s steps (was reserved0)
```

- [ ] **Step 4: Serialize/deserialize the stun state**

In `src/net/snapshot.cpp`, wherever `SnapPlayer.statusFlags`/`invulnTimer` are packed and
`reserved0` is written (search `reserved0`): set `flags |= (stunned ? 0x20 : 0)` on pack, quantize
`stunTimerQ = clamp(stunTimer / 0.04)`, and on unpack read them back. On the **server pack** side
source `stunned`/`stunTimer` from the authoritative NetPlayer; on the **client unpack** side write
into the client's local `NetPlayer.stunTimer` so `Client::reconcile`/the local `Player` adopt it.

- [ ] **Step 5: Client adopt + predictive lock (engine_net.cpp)**

Where the client applies a decoded `SnapPlayer` to its local slot (`Client::reconcile` /
`clientNetPost`), set `m_localPlayer.stunTimer = fmaxf(m_localPlayer.stunTimer, snap.stunTimerQ *
0.04f)` so the client's own input-lock (Task 3 Step 6) engages immediately and self-heals mid-join.
For remote slots, set the interpolated `NetPlayer.stunTimer` for any stun VFX.

- [ ] **Step 6: Server-tick decay for remote lanes (engine_net.cpp)**

Next to the `shadowDanceTimer` server decay (line 855), for remote lanes only (the
`pi >= m_splitPlayerCount` guard region):

```cpp
            if (np.stunTimer > 0.0f)     np.stunTimer     -= dt;
            if (np.ccImmuneTimer > 0.0f) np.ccImmuneTimer -= dt;
            CrowdControl::tickStunDr(np.stunDr, dt);
```

- [ ] **Step 7: Bump the protocol**

In `src/net/net.h`, `PROTOCOL_VERSION` 20 → 21; extend the version comment: `v21: SnapPlayer
flags bit5 = stunned + stunTimerQ (was reserved0) — player-facing CC.`

- [ ] **Step 8: Build both + run suite**

Run: `cmake --build build && ./build/tests/dungeon_tests 2>&1 | tail -3`
Expected: clean; suite green. (Snapshot wire asserts, if any, still hold — `reserved0` was already
a byte, so `sizeof(SnapPlayer)` is unchanged.)

- [ ] **Step 9: Commit**

```bash
git add src/net/net_player.h src/net/snapshot.h src/net/snapshot.cpp src/net/net.h src/engine/engine.cpp src/engine/engine_net.cpp
git commit -m "feat(cc): replicate player stun (predictive+reconciled), PROTOCOL 21"
```

---

## Task 5: Steadfast Greaves — legendary boots (resist + perfect-dodge + F cleanse)

**Files:**
- Modify: `src/game/item.h:173` (`SkillId::BREAK_FREE` — append before the legendary block)
- Modify: `assets/config/skills.json` (the `break_free` SkillDef)
- Modify: `src/game/skill_system.cpp` or the boot-skill dispatch (`engine_net.cpp:333`) — BREAK_FREE `tryActivate`
- Modify: `src/engine/engine_update_player.cpp` (perfect-dodge clears CC)
- Modify: `assets/config/items.json` (append the boots — APPEND-ONLY)
- Modify: `tools/gen_mesh.py` + `src/engine/asset_manifest.h` + `tools/build_assets.py` (mesh, BOTH)

- [ ] **Step 1: SkillId + SkillDef**

In `src/game/item.h`, append `BREAK_FREE` before the "Legendary weapon effects" block (after
`DEATHS_DANCE`, ~line 173) — **append, never insert** (SkillId is serialized by ordinal):

```cpp
    BREAK_FREE,         // legendary boots active (F): cleanse all CC + brief CC-immunity
```

Append to `assets/config/skills.json`:

```json
    {
      "id": "break_free",
      "name": "Break Free",
      "description": "Shatter all crowd control on you and become immune to control for 1.5s.",
      "cooldown": 20.0,
      "energyCost": 0.0,
      "duration": 1.5
    }
```

- [ ] **Step 2: BREAK_FREE activation (cleanse + immunity)**

In the skill activation switch (the `tryActivate`/boot-skill dispatch that fires
`bootSkill` — `engine_net.cpp:336` resolves `legendarySkillId` to `m_bootSkillStates`), add a
`BREAK_FREE` case that cleanses and grants immunity **on the acting player** (local and remote):

```cpp
    case SkillId::BREAK_FREE:
        player.stunTimer = player.slowTimer = player.freezeTimer = 0.0f;
        player.ccImmuneTimer = def->duration > 0.0f ? def->duration : 1.5f;
        break;
```

Because it rides `BOOT_SKILL` (already F-bound, `INPUT_EX_BOOT_SKILL`, per-slot cooldown state,
`bootSkillLastActivationTick` replication), F and the cooldown are free. Ensure the remote-cast
path (`processRemoteActivation`) applies it too, so a co-op guest's Break Free works.

- [ ] **Step 3: Stamp ccDodgeImmune + perfect-dodge clears CC (boots passive 2a)**

First, in `src/engine/engine_update_skills.cpp` `tickPassiveEquipment` (beside the `ccResist`
stamp from Task 3 Step 5), set the boots marker — true iff the equipped boots' legendary is
BREAK_FREE (the single marker for the anti-CC boots):

```cpp
        {
            const ItemInstance& boots = inv.equipped[(u32)ItemSlot::BOOTS];
            m_localPlayer.ccDodgeImmune = !isItemEmpty(boots) &&
                m_itemDefs[boots.defId].legendarySkillId == SkillId::BREAK_FREE;
        }
```

Then in `engine_update_player.cpp`, at the frame a dodge roll BEGINS (search where `rollTimer` is
set to its start value / the dodge activation), gate on the flag and clear CC:

```cpp
        if (m_localPlayer.ccDodgeImmune) {           // Steadfast Greaves: rolling sheds CC
            m_localPlayer.stunTimer = m_localPlayer.slowTimer = m_localPlayer.freezeTimer = 0.0f;
        }
```

(The i-frame *prevention* half is already in the choke via `ccDodgeImmune && rollTimer>0`.)

- [ ] **Step 4: Generate the mesh (never hand-author)**

Add a `steadfast_greaves` boots mesh generator to `tools/gen_mesh.py` (copy an existing boots
mesh generator), register the name in **both** `src/engine/asset_manifest.h` (the mesh table) and
`tools/build_assets.py` (either alone fails the asset build), then run:

```bash
python3 tools/build_assets.py
```

- [ ] **Step 5: Append the item def**

Append to the `"items"` array in `assets/config/items.json` (**append-only** — `defId` is the
saved array index):

```json
    {
      "name": "Steadfast Greaves",
      "slot": "boots",
      "mesh": "steadfast_greaves",
      "material": "armor_heavy",
      "minLevel": 20, "maxLevel": 50,
      "maxRarity": "legendary",
      "dropWeight": 0.4,
      "baseHealth": 20,
      "legendarySkill": "break_free",
      "affixes": [ { "type": "cc_resist", "value": 0.45 } ]
    }
```

> Confirm the item schema's forced-affix mechanism (how a legendary guarantees an affix) — match
> whatever existing legendary armor uses to carry a built-in stat. The 0.45 CC-resist + gear pushes
> toward the 0.60 cap without reaching it alone.

- [ ] **Step 6: Build, verify tooltip text resolves, commit**

Run: `cmake --build build` then in-game (or a targeted check) confirm the boots' F tooltip reads
the `skills.json` description (it resolves through `HUD::resolveSkillDescription` — a def-having
skill gets its text from JSON; do NOT add a C++ fallback case).

```bash
git add src/game/item.h assets/config/skills.json assets/config/items.json src/engine/*.cpp src/engine/asset_manifest.h tools/gen_mesh.py tools/build_assets.py
git commit -m "feat(cc): Steadfast Greaves — CC-resist boots, perfect-dodge clears CC, F Break Free"
```

---

## Task 6: PvP block hardening (energy drain + perfect-only CC negation, no cooldown)

**Files:**
- Modify: `src/engine/engine_update_player.cpp` (block energy drain in PvP)
- Modify: `src/engine/engine.cpp:1563` (`landPvpHit` — route CC through the choke + block gating)
- Test: `tests/game/test_block_outcome.cpp` (extend — no cooldown regression)

> **Principle (from the user):** a perfect block is a timing feat and is ALWAYS rewarded — no
> cooldown. `classifyBlock` (`combat.h:67`) stays unchanged. The only throttle is energy drain on
> a held block. Only a PERFECT block negates CC; a held block eats damage but the CC lands.

- [ ] **Step 1: Energy drain while holding block (PvP only)**

In the player update, when `m_localPlayer.blocking` and the match is PvP (`Combat::pvpActive()`),
drain energy per second; drop the block at 0:

```cpp
    if (m_localPlayer.blocking && Combat::pvpActive()) {
        m_localPlayer.energy -= BLOCK_DRAIN_PER_SEC * dt;   // e.g. 20/s
        if (m_localPlayer.energy <= 0.0f) { m_localPlayer.energy = 0.0f; m_localPlayer.blocking = false; }
    }
```

(Confirm the player energy field name in `player.h`; add the `BLOCK_DRAIN_PER_SEC` constant near
the block code.)

- [ ] **Step 2: Extend PvpHit for stun; route landPvpHit CC through the choke with block gating**

First, in `src/game/combat.h` `struct PvpHit` (line 105), add the stun field (Task 7 later adds
`knockback` beside it) and document `onHitEffect == 5` = stun:

```cpp
    f32  stunDuration = 0.0f;  // set by stun sources (onHitEffect==5); resisted + DR'd on the victim
```

Then in `src/engine/engine.cpp`, `landPvpHit` (line 1581-1584 currently writes CC timers
directly). Replace with the choke, and gate CC negation on a **perfect** block:

```cpp
    const Combat::BlockOutcome blk = Combat::classifyBlock(v.blocking, v.blockTimer);
    // Damage: perfect negates, held halves (existing applyDamageToPlayer already does this for the
    // damage number). CC: ONLY a perfect block negates it — a held block lets the CC land.
    if (blk != Combat::BlockOutcome::PERFECT) {
        if (hit.onHitEffect == 2) Combat::applyCCToPlayer(v, Combat::CcType::SLOW,   hit.onHitDuration, true);
        if (hit.onHitEffect == 4) Combat::applyCCToPlayer(v, Combat::CcType::FREEZE, hit.onHitDuration, true);
        if (hit.onHitEffect == 5) Combat::applyCCToPlayer(v, Combat::CcType::STUN,   hit.stunDuration,  true);
    }
    // (poison=1 / burn=3 remain direct DoT writes — not CC.)
```

- [ ] **Step 3: Extend the block test**

In `tests/game/test_block_outcome.cpp`, add a case pinning that a perfect block is available on
every well-timed block (no cooldown gate in `classifyBlock`):

```cpp
TEST_CASE("Block: perfect is always available (no cooldown) — repeated perfects classify PERFECT") {
    using Combat::classifyBlock; using Combat::BlockOutcome;
    CHECK(classifyBlock(true, 0.10f) == BlockOutcome::PERFECT);
    CHECK(classifyBlock(true, 0.10f) == BlockOutcome::PERFECT);   // again, immediately — no lockout
    CHECK(classifyBlock(true, 0.30f) == BlockOutcome::BLOCKED);
    CHECK(classifyBlock(false, 0.0f) == BlockOutcome::NONE);
}
```

- [ ] **Step 4: Build + suite + commit**

Run: `cmake --build build && ./build/tests/dungeon_tests 2>&1 | tail -3`

```bash
git add src/engine/engine_update_player.cpp src/engine/engine.cpp tests/game/test_block_outcome.cpp
git commit -m "feat(cc): PvP block hardening — energy drain, perfect-only CC negation, no cooldown"
```

---

## Task 7: Class CC — the four fairness additions (with PvP twins)

**Files:**
- Modify: `src/game/combat.h` (`PvpHit` gains `stunDuration` + `knockback`; a `pvpStun`/`pvpKnockback` helper if needed)
- Modify: `src/engine/engine.h:691` (`ScorchZone` gains `slowPct`)
- Modify: `src/engine/engine_update.cpp` (`tickSharedFX` scorch loop applies slow)
- Modify: `src/game/skill_ranger.cpp:182` (Barrage slow-zone)
- Modify: `src/game/skill_marksman.cpp:63` (Explosive Round knockback + stagger)
- Modify: `src/game/skill_tinkerer.cpp:124` (Detonate Swarm EMP stun)
- Modify: the Deflect absorb path (`combat.cpp:379`) (stun the attacker; PvP twin)

> Every new CC gets a `Combat::pvp*` twin beside its entity query, attacker slot from
> `s_castingPlayer` (skills) / the deflect victim's slot (Wanderer). Registry-gated → free no-op in
> PvE. Missing the twin = the CC silently does nothing in the Arena.

- [ ] **Step 1: Extend PvpHit for knockback (stunDuration already added in Task 6)**

In `src/game/combat.h`, `struct PvpHit` (line 105): add `f32 knockback = 0.0f;` beside the
`stunDuration` field added in Task 6. In `landPvpHit` the stun branch is already wired (Task 6
Step 2); add a knockback apply (impulse along `hit.origin → victim`) when `hit.knockback > 0` —
reuse the existing player-knockback/velocity path.

- [ ] **Step 2: Ranger — Barrage slow zone (reuse ScorchZone)**

Add `f32 slowPct = 0.0f;` to `ScorchZone` (`engine.h:691`). In `tickSharedFX`'s scorch loop
(`engine_update.cpp`), when `slowPct > 0` apply slow to entities in radius (`e.freezeTimer` is the
enemy slow field) and, if `Combat::pvpActive()`, slow players in radius via
`Combat::applyCCToPlayer(..., SLOW, ...)`. In `skill_ranger.cpp:182` `fireBarrage`, after the
arrows spawn, register a zone at the aim point:

```cpp
    if (s_scorchCallbackSlow) s_scorchCallbackSlow(aimPoint, 4.0f, 3.0f, 0.40f); // r=4m, 3s, 40% slow
```

(Add a slow-zone registration callback mirroring `s_scorchCallback`, or extend the existing
callback signature with a `slowPct` arg — prefer extending, DRY.)

- [ ] **Step 3: Marksman — Explosive Round knockback + stagger**

In `skill_marksman.cpp:63` `fireExplosiveRound`, on the explosion's entity hits apply a knockback
+ a 0.2 s stun (`e->stunTimer = fmaxf(e->stunTimer, 0.2f)` for enemies), and add the PvP twin —
a `pvpRadius` at the blast that carries `knockback` + `stunDuration = 0.2f` in the `PvpHit`.

- [ ] **Step 4: Tinkerer — Detonate Swarm EMP stun**

In `skill_tinkerer.cpp:124` `fireDetonateSwarm`, on each drone's 3 m detonation AoE apply
`stunTimer = fmaxf(.., 0.6f)` to enemies in radius, and the PvP twin `pvpRadius` with
`stunDuration = 0.6f`.

- [ ] **Step 5: Wanderer — Deflect stuns the attacker (PvP twin)**

The melee-attacker stun on a perfect deflect exists for entities (`SkillId::DEFLECT` comment). In
the deflect absorb path (`combat.cpp:379`), when the deflect succeeds and the attacker is an
**entity**, apply `attacker->stunTimer = fmaxf(.., 0.75f)`. Add the PvP twin: when the deflected
hit came from a rival **player** (arena), stun that player slot via
`Combat::applyCCToPlayer(pvpTargets()[slot].view, STUN, 0.75f, true)` routed through
`Engine::pvpApplyHit` so it lands atomically on the authoritative view.

- [ ] **Step 6: Build + suite + commit**

Run: `cmake --build build && ./build/tests/dungeon_tests 2>&1 | tail -3`

```bash
git add src/game/combat.h src/engine/engine.h src/engine/engine_update.cpp src/game/skill_ranger.cpp src/game/skill_marksman.cpp src/game/skill_tinkerer.cpp src/game/combat.cpp
git commit -m "feat(cc): class CC — Ranger slow-zone, Marksman knockback, Tinkerer EMP, Wanderer deflect-stun (PvP twins)"
```

---

## Task 8: Verification — Arena integration, performance gate, docs sync

**Files:**
- Modify: `CLAUDE.md`, `.claude/skills/engine-reference/SKILL.md`, `.claude/skills/engine-how-to/SKILL.md`
- Modify: memory `project_pending_playtest.md`

- [ ] **Step 1: Full suite + both build configs**

```bash
cmake --build build && ./build/tests/dungeon_tests 2>&1 | tail -3
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel 2>&1 | grep -iE "error|warning" | head
```
Expected: suite green; Release clean, zero new warnings.

- [ ] **Step 2: PvE regression grep-audit**

Confirm CC resist/DR/block changes never touch PvE incorrectly: `applyCCToPlayer(..., false)` is
used for enemy/projectile CC (no DR); block energy drain + perfect-only-CC gating are behind
`Combat::pvpActive()`. Confirm no direct player `stunTimer`/`slowTimer`/`freezeTimer` writes remain
that bypass the choke (`grep -rn "Timer *= " src/game/enemy_ai_states.cpp src/game/projectile.cpp`).

- [ ] **Step 3: Runtime — Arena soak (the RL performance gate)**

```bash
./build/src/DungeonEngine --arena --host   # + a joined client, or the bot rig
```
With CC live, confirm from `DungeonEngine.log` / F9 net-graph under
`--net-loss 15 --net-latency 100 --bot-walk`: **0 hard snaps, deltas engaged, steady snap-Hz, a
stunned guest with zero rubber-band, frame time within 16.6 ms, draw calls 300–500.** Any
regression fails the feature (per the spec's performance gate).

Manual checks: CC-resist boots visibly shorten a slow on the guest; a perfect dodge in the
Steadfast Greaves clears a stun; F cleanses + grants immunity on cooldown; two CC classes focusing
one player hit DR immunity (not perma-lock); held block eats a stun while a perfect block dodges it.

- [ ] **Step 4: Docs sync**

- `CLAUDE.md`: add a Crowd Control paragraph (the choke helper, gear-only resist, PvP-only stun,
  DR, PROTOCOL 21) beside the Arena paragraph.
- `engine-reference`: CC constants (`RESIST_CAP` 0.60, `DR_WINDOW` 8 s), the `applyCCToPlayer`
  choke, the SnapPlayer stun bit + `stunTimerQ`, PROTOCOL 21.
- `engine-how-to`: a pitfall — "a new player-CC source MUST call `Combat::applyCCToPlayer`, not a
  direct timer write, or it bypasses resistance and DR"; and "CC resist is on-demand (no cached
  field) — never add a `PlayerInventory bonus*` field without a SAVE_VERSION bump."
- Memory `project_pending_playtest.md`: add the CC group (what needs human playtest — duel feel,
  DR pacing, block energy tuning, boots feel).

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md .claude/skills/engine-reference/SKILL.md .claude/skills/engine-how-to/SKILL.md
git commit -m "docs(cc): sync CLAUDE.md + engine skills for the crowd-control system"
```

---

## Verification checklist (whole feature)

1. **Unit** — `test_crowd_control.cpp` (scale/cap/DR/choke/ccResist) + the block no-cooldown case; full suite green on Debug and Release.
2. **Save safety** — no SAVE_VERSION bump; `sizeof(PlayerInventory)` unchanged (no cached field); `sizeof(SnapPlayer)` unchanged (`reserved0` repurposed).
3. **PvE untouched** — enemy slow/freeze now resisted but no DR, no player stun, block unchanged out of PvP.
4. **PvP** — every new class CC lands on players (pvp twins present); DR caps chains; perfect block/dodge always available; block energy-gated.
5. **Netcode (RL bar)** — stun predicted+reconciled, no rubber-band; delta sends ~1 byte/event; DR server-only; PROTOCOL 21 clean reject vs old clients.
6. **Perf** — 16.6 ms budget held, 300–500 draw calls, no particle spike, `PROFILE_SCOPE` within noise.
```
