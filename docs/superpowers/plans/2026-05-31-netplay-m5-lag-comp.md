# Netplay M5: Lag Compensation Wiring Plan

> superpowers:subagent-driven-development. Checkbox `- [ ]`.

**Goal:** Lock the existing lag-comp ring (entity pose history at `s_entHistory[MAX_ENTITIES][LAG_COMP_HISTORY_TICKS]`, wired into hitscan + melee already at engine_combat.cpp:1080-1209) under unit tests, and extend the rewind to also cover projectile spawn position so a projectile is born where the firing client saw their character — not where the server thinks they are "now". AOE skill resolution (`METEOR_STRIKE` and friends) gets lag-comp in M9 when predicted skills land. Visualizer is deferred to M14.

**Status of pre-existing scaffolding:** ✓ History ring + push (M0 baseline). ✓ `beginLagComp` / `endLagComp` implemented (M0 baseline). ✓ Melee + hitscan wrapped (M0 baseline). ✗ Projectile spawn not yet wrapped. ✗ No unit tests covering the ring.

---

## Task 1: Unit tests for entity history ring (lock contract)

**Files:**
- Create: `tests/engine/test_lag_comp_ring.cpp`
- Modify: `tests/CMakeLists.txt`

The ring is private to engine_combat.cpp (statics). Testing requires either exposing them, or testing through the public `pushEntityHistory` / `beginLagComp` API. The cleanest path: add a tiny public method `Engine::lagCompHistoryDepth(u8 entIdx) const` that returns how many filled slots exist for an entity, and another `Engine::lagCompHistoryTickAt(u8 entIdx, u32 offset) const` that returns the tickStamp at a ring position. Tests verify push behavior via these getters.

Actually — given the heavy entanglement (Engine has many members), unit-testing through a real Engine instance pulls the entire game into the test binary. Pragmatic alternative: extract the ring math into a free standing `lag_comp_ring.h/.cpp` that wraps the `EntPoseSnap[N][CAPACITY]` array + push + lookup, and have engine_combat.cpp call into it. Test the free-standing module.

For M5, take the latter approach.

- [ ] **Step 1**: Create `src/engine/lag_comp_ring.h`:

```cpp
#pragma once
#include "core/types.h"
#include "core/math.h"

struct LagCompPose {
    Vec3 position    = {0,0,0};
    f32  yaw         = 0.0f;
    Vec3 halfExtents = {0,0,0};
    u32  tickStamp   = 0;   // 0 = empty
};

static constexpr u32 LAG_COMP_HISTORY_TICKS_MAX = 16;

// Single-entity ring of recent poses. Owners (engine_combat.cpp) maintain one per
// entity slot. tickStamp == 0 means the slot is empty.
struct LagCompRing {
    LagCompPose poses[LAG_COMP_HISTORY_TICKS_MAX] = {};
    u8          head = 0;
};

namespace LagCompRingOps {
    void reset(LagCompRing& r);
    void push(LagCompRing& r, const LagCompPose& pose);
    // Returns pointer to the entry whose tickStamp matches (or null). For approximate
    // rewind, callers can scan with their own logic — keep this exact-match for now.
    const LagCompPose* findByTickStamp(const LagCompRing& r, u32 tickStamp);
    // Returns the entry `ticksAgo` positions before head (1 = newest, etc.), or null
    // if not filled.
    const LagCompPose* atTicksAgo(const LagCompRing& r, u32 ticksAgo);
}
```

- [ ] **Step 2**: Create `src/engine/lag_comp_ring.cpp`:

```cpp
#include "engine/lag_comp_ring.h"

void LagCompRingOps::reset(LagCompRing& r) {
    for (u32 i = 0; i < LAG_COMP_HISTORY_TICKS_MAX; i++) r.poses[i] = {};
    r.head = 0;
}

void LagCompRingOps::push(LagCompRing& r, const LagCompPose& pose) {
    r.poses[r.head] = pose;
    r.head = (r.head + 1) % LAG_COMP_HISTORY_TICKS_MAX;
}

const LagCompPose* LagCompRingOps::findByTickStamp(const LagCompRing& r, u32 tickStamp) {
    if (tickStamp == 0) return nullptr;
    for (u32 i = 0; i < LAG_COMP_HISTORY_TICKS_MAX; i++) {
        if (r.poses[i].tickStamp == tickStamp) return &r.poses[i];
    }
    return nullptr;
}

const LagCompPose* LagCompRingOps::atTicksAgo(const LagCompRing& r, u32 ticksAgo) {
    if (ticksAgo == 0 || ticksAgo > LAG_COMP_HISTORY_TICKS_MAX) return nullptr;
    u32 idx = (r.head + LAG_COMP_HISTORY_TICKS_MAX - ticksAgo) % LAG_COMP_HISTORY_TICKS_MAX;
    if (r.poses[idx].tickStamp == 0) return nullptr;
    return &r.poses[idx];
}
```

- [ ] **Step 3**: Create `tests/engine/test_lag_comp_ring.cpp`:

```cpp
#include <doctest/doctest.h>
#include "engine/lag_comp_ring.h"

TEST_CASE("LagCompRing: empty ring returns null") {
    LagCompRing r;
    LagCompRingOps::reset(r);
    CHECK(LagCompRingOps::findByTickStamp(r, 100) == nullptr);
    CHECK(LagCompRingOps::atTicksAgo(r, 1) == nullptr);
}

TEST_CASE("LagCompRing: push and lookup by tickStamp") {
    LagCompRing r;
    LagCompRingOps::reset(r);
    LagCompPose p; p.position = {1.0f, 2.0f, 3.0f}; p.tickStamp = 42;
    LagCompRingOps::push(r, p);
    const LagCompPose* found = LagCompRingOps::findByTickStamp(r, 42);
    REQUIRE(found != nullptr);
    CHECK(found->position.x == doctest::Approx(1.0f));
}

TEST_CASE("LagCompRing: atTicksAgo returns newest when ticksAgo=1") {
    LagCompRing r;
    LagCompRingOps::reset(r);
    LagCompPose a; a.tickStamp = 100; a.position = {10.0f,0,0};
    LagCompPose b; b.tickStamp = 101; b.position = {20.0f,0,0};
    LagCompRingOps::push(r, a);
    LagCompRingOps::push(r, b);
    const LagCompPose* newest = LagCompRingOps::atTicksAgo(r, 1);
    REQUIRE(newest != nullptr);
    CHECK(newest->tickStamp == 101);
    CHECK(newest->position.x == doctest::Approx(20.0f));
    const LagCompPose* prior = LagCompRingOps::atTicksAgo(r, 2);
    REQUIRE(prior != nullptr);
    CHECK(prior->tickStamp == 100);
}

TEST_CASE("LagCompRing: oldest evicted past capacity") {
    LagCompRing r;
    LagCompRingOps::reset(r);
    for (u32 t = 1; t <= LAG_COMP_HISTORY_TICKS_MAX + 5; t++) {
        LagCompPose p; p.tickStamp = t;
        LagCompRingOps::push(r, p);
    }
    // tick 1..5 evicted. tick 6 should be the oldest in the ring.
    CHECK(LagCompRingOps::findByTickStamp(r, 1) == nullptr);
    CHECK(LagCompRingOps::findByTickStamp(r, 5) == nullptr);
    REQUIRE(LagCompRingOps::findByTickStamp(r, 6) != nullptr);
    REQUIRE(LagCompRingOps::findByTickStamp(r, LAG_COMP_HISTORY_TICKS_MAX + 5) != nullptr);
}
```

- [ ] **Step 4**: Wire in CMake. `src/CMakeLists.txt`: add `engine/lag_comp_ring.cpp`. `tests/CMakeLists.txt`: add both `engine/lag_comp_ring.cpp` and `engine/test_lag_comp_ring.cpp`. Make sure `mkdir -p tests/engine` first.

- [ ] **Step 5**: Build, run. 31/31 pass (27 + 4 new). Commit: `feat(engine): lag-comp ring extracted + tested — M5.1`.

---

## Task 2: Apply lag-comp to projectile spawn

**Files:**
- Modify: `src/engine/engine_combat.cpp` — extend the existing `lagCompTicks > 0` window in the remote-net-player fire path to also rewind around projectile spawn (so `np.eyePos()` reads rewound position).

- [ ] **Step 1**: Find the current `if (wpn.type == WeaponType::MELEE || wpn.type == WeaponType::HITSCAN)` lag-comp gate in handleWeaponFire's remote-net-player branch (around line 1086 from grep). Widen to all weapon types:

```cpp
    u32 lagCompTicks = 0;
    // Lag-comp now covers all three weapon types so projectile spawn position is
    // rewound to the client's perceived np.position at fire time (M5). Without this
    // a strafing remote's projectile would be born ~RTT/2 worth of motion behind
    // their visible firing pose.
    lagCompTicks = computeLagCompTicks(np.slotIndex);
    if (lagCompTicks > 0) beginLagComp(lagCompTicks);
```

Remove the conditional restriction. The existing endLagComp call at the end of the function already runs unconditionally as long as lagCompTicks > 0.

- [ ] **Step 2**: Verify projectile spawn uses `np.eyePos()` (which derives from np.position) — that's what beginLagComp rewinds. Looking at line 1176: `projIdx = Combat::fireProjectile(wpn, eyePos, forward, m_projectiles, ...)`. The local `eyePos` was computed earlier in the function from `np.eyePos()` *before* the lag-comp window opened. We need to make sure eyePos is computed AFTER beginLagComp.

Read the function around 1080-1180 to confirm. If eyePos is set before the lag-comp window, move it inside.

- [ ] **Step 3**: Build, run tests (31/31 still). Manual smoke deferred. Commit: `feat(net): apply lag-comp to projectile spawn position — M5.2`.

---

## Task 3: Verify

- [ ] Clean tree, build, tests, commit count = 2 for M5.

## Definition of Done
- [ ] 31/31 tests pass
- [ ] `grep -c 'WeaponType::PROJECTILE' src/engine/engine_combat.cpp` returns non-zero (projectile path exists)
- [ ] `src/engine/lag_comp_ring.{h,cpp}` exist
- [ ] AOE / METEOR_STRIKE lag-comp deferred to M9 (predicted skill activations) — recorded in M9 plan
- [ ] Visualizer deferred to M14 — recorded
