# Netplay M7: Predicted Damage Taken Plan

> superpowers:subagent-driven-development. Checkbox `- [ ]`.

**Goal:** On CLIENT, broadphase-detect incoming enemy projectiles that will hit the local player within the next frame; trigger visual+audio feedback (hit-flash, hurt-vignette, rumble) immediately rather than waiting ~RTT for the snapshot HP drop to land. HP itself is NOT touched locally — it follows the next snapshot's authoritative value (avoids flicker if prediction is wrong). M10's `SV_DAMAGE_TO_ME` reliable event will (in M10) acknowledge predictions and reconcile HP smoothly via the M4 RenderOffset.

**Conservative choice:** v1 predicts visuals only. The design doc proposes also subtracting `expectedDamage` from local HP for a snappier UI bar, but expects M10 to reconcile any mispredict. We defer HP-prediction to a future enhancement — visual feedback alone gets most of the "feel" win.

---

## Task 1: PendingDamageRing scaffold + tests

**Files:**
- Create: `src/net/pending_damage_ring.h`, `src/net/pending_damage_ring.cpp`
- Create: `tests/net/test_pending_damage_ring.cpp`
- Modify: both CMakeLists

Same pattern as M6's PendingHitRing — a small bounded ring tracking predicted self-hits for M10 to ack.

- [ ] **Step 1**: Create `tests/net/test_pending_damage_ring.cpp`:

```cpp
#include <doctest/doctest.h>
#include "net/pending_damage_ring.h"

TEST_CASE("PendingDamageRing: empty has zero pending") {
    PendingDamageRing r;
    PendingDamageRingOps::reset(r);
    CHECK(r.count == 0);
}

TEST_CASE("PendingDamageRing: record adds entry") {
    PendingDamageRing r;
    PendingDamageRingOps::reset(r);
    PendingDamageRingOps::record(r, 100, 42);  // clientTick=100, projectileSrcKey=42
    CHECK(r.count == 1);
    CHECK(r.entries[0].clientTick == 100);
    CHECK(r.entries[0].projectileSrcKey == 42);
}

TEST_CASE("PendingDamageRing: ack matches and clears") {
    PendingDamageRing r;
    PendingDamageRingOps::reset(r);
    PendingDamageRingOps::record(r, 100, 42);
    PendingDamageRingOps::record(r, 101, 43);
    bool acked = PendingDamageRingOps::ack(r, 42);
    CHECK(acked == true);
    CHECK(r.count == 1);
    CHECK(r.entries[0].clientTick == 101);
}

TEST_CASE("PendingDamageRing: ack misses return false") {
    PendingDamageRing r;
    PendingDamageRingOps::reset(r);
    PendingDamageRingOps::record(r, 100, 42);
    CHECK(PendingDamageRingOps::ack(r, 999) == false);
    CHECK(r.count == 1);
}

TEST_CASE("PendingDamageRing: expireOlderThan removes stale entries") {
    PendingDamageRing r;
    PendingDamageRingOps::reset(r);
    PendingDamageRingOps::record(r, 50, 1);
    PendingDamageRingOps::record(r, 100, 2);
    PendingDamageRingOps::record(r, 150, 3);
    PendingDamageRingOps::expireOlderThan(r, 100);
    CHECK(r.count == 2);
}
```

- [ ] **Step 2**: Create `src/net/pending_damage_ring.h`:

```cpp
#pragma once
#include "core/types.h"

static constexpr u32 PENDING_DAMAGE_RING_CAPACITY = 32;

struct PendingDamage {
    u32 clientTick       = 0;   // when the predicted hit fired locally
    u32 projectileSrcKey = 0;   // identifier for the inbound projectile (slot index
                                // or clientTickLow — whatever uniquely keys it)
};

struct PendingDamageRing {
    PendingDamage entries[PENDING_DAMAGE_RING_CAPACITY] = {};
    u32           count = 0;
};

namespace PendingDamageRingOps {
    void reset(PendingDamageRing& r);
    void record(PendingDamageRing& r, u32 clientTick, u32 projectileSrcKey);
    bool ack(PendingDamageRing& r, u32 projectileSrcKey);
    void expireOlderThan(PendingDamageRing& r, u32 cutoffClientTick);
}
```

- [ ] **Step 3**: Create `src/net/pending_damage_ring.cpp`:

```cpp
#include "net/pending_damage_ring.h"

void PendingDamageRingOps::reset(PendingDamageRing& r) {
    for (u32 i = 0; i < PENDING_DAMAGE_RING_CAPACITY; i++) r.entries[i] = {};
    r.count = 0;
}

void PendingDamageRingOps::record(PendingDamageRing& r, u32 clientTick, u32 projectileSrcKey) {
    if (r.count >= PENDING_DAMAGE_RING_CAPACITY) {
        // Drop oldest.
        for (u32 i = 1; i < PENDING_DAMAGE_RING_CAPACITY; i++) r.entries[i-1] = r.entries[i];
        r.count = PENDING_DAMAGE_RING_CAPACITY - 1;
    }
    PendingDamage& e = r.entries[r.count];
    e.clientTick = clientTick;
    e.projectileSrcKey = projectileSrcKey;
    r.count++;
}

bool PendingDamageRingOps::ack(PendingDamageRing& r, u32 projectileSrcKey) {
    for (u32 i = 0; i < r.count; i++) {
        if (r.entries[i].projectileSrcKey == projectileSrcKey) {
            for (u32 j = i + 1; j < r.count; j++) r.entries[j-1] = r.entries[j];
            r.entries[r.count - 1] = {};
            r.count--;
            return true;
        }
    }
    return false;
}

void PendingDamageRingOps::expireOlderThan(PendingDamageRing& r, u32 cutoffClientTick) {
    u32 write = 0;
    for (u32 read = 0; read < r.count; read++) {
        if (r.entries[read].clientTick >= cutoffClientTick) {
            if (write != read) r.entries[write] = r.entries[read];
            write++;
        }
    }
    for (u32 i = write; i < r.count; i++) r.entries[i] = {};
    r.count = write;
}
```

- [ ] **Step 4**: Wire into both CMakeLists. Build. 41/41 pass (36 + 5).

- [ ] **Step 5**: Commit: `feat(net): PendingDamageRing scaffold + tests (TDD) — M7.1`.

---

## Task 2: Predict incoming-projectile hits on local player

**Files:**
- Modify: `src/engine/engine.h` — add `PendingDamageRing m_pendingDamage;`
- Modify: `src/engine/engine_update.cpp` (or wherever the per-frame predicted-projectile collision check lives) — extend client-side projectile broadphase to also check vs local player; on predicted hit, trigger visual feedback + record

- [ ] **Step 1**: Add to engine.h:
```cpp
    PendingDamageRing m_pendingDamage;
```

- [ ] **Step 2**: Find where the client iterates `m_renderInterp.projectiles` per frame. Look at the projectile-vs-entity prediction (Phase 2.3 — engine_update.cpp around 543-571 per the M0 audit). Locate that loop. After the entity-collision check (which already exists), add a player-broadphase check.

For each active projectile in m_renderInterp.projectiles:
- Skip if `proj.ownerSlot == mySlot` (it's our own projectile — not damage to us).
- Compute next-frame swept sphere of projectile (current pos → pos + velocity * dt).
- Compute distance from local player position (m_localPlayer.position + Vec3{0, eyeHeight*0.5, 0}) to the segment.
- If within ~0.7 m (player half-radius), predict impact.
- On predicted impact:
  - Set `m_localPlayer.damageFlashTimer = 0.15f;` or similar (look at existing hit-flash code).
  - Set local hurt-vignette / rumble using whatever the existing impact code does.
  - Generate a unique key for the projectile (e.g., `static_cast<u32>(proj.ownerSlot) << 24 | (proj.clientTick & 0xFFFFFF)`).
  - Call `PendingDamageRingOps::record(m_pendingDamage, m_clientTick, key);` to record for future M10 ack.
  - Set a flag/marker on the projectile so we don't re-predict every frame against the same projectile. Simplest: skip if we've already recorded a key for this projectile (loop the ring for any entry with matching projectileSrcKey before recording).

A minimal first-cut:
```cpp
    if (m_netRole == NetRole::CLIENT && !m_localPlayer.isDead) {
        const f32 PLAYER_HIT_RADIUS = 0.7f;
        Vec3 playerCenter = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight * 0.5f, 0};
        for (u32 a = 0; a < m_renderInterp.projectiles.activeCount; a++) {
            u32 idx = m_renderInterp.projectiles.activeList[a];
            const Projectile& proj = m_renderInterp.projectiles.projectiles[idx];
            if (!proj.active) continue;
            if (proj.ownerSlot == activeNetSlot()) continue;  // our own — not damage to us
            // Skip if already predicted this projectile (key collision check).
            u32 key = (static_cast<u32>(proj.ownerSlot) << 24) | (proj.clientTick & 0xFFFFFFu);
            bool alreadyPredicted = false;
            for (u32 i = 0; i < m_pendingDamage.count; i++) {
                if (m_pendingDamage.entries[i].projectileSrcKey == key) { alreadyPredicted = true; break; }
            }
            if (alreadyPredicted) continue;
            Vec3 toPlayer = playerCenter - proj.position;
            Vec3 nextStep = proj.velocity * dt;
            // Simple proximity check rather than full swept sphere — projectiles tick fast.
            f32 distSq = lengthSq(toPlayer);
            f32 approachSpeed = -dot(toPlayer, nextStep) / sqrtf(distSq + 1e-6f);
            if (distSq < (PLAYER_HIT_RADIUS * PLAYER_HIT_RADIUS) && approachSpeed > 0.0f) {
                // Predicted hit.
                m_localPlayer.damageFlashTimer = 0.15f;
                // ... add hurt-vignette / rumble if exposed via existing helpers
                PendingDamageRingOps::record(m_pendingDamage, m_clientTick, key);
            }
        }
    }
```

Adapt to the project's actual `dot` / `lengthSq` helpers. If hurt-vignette + rumble are wrapped in a helper function (look in engine_update_player.cpp or similar), call it instead of writing the fields directly.

- [ ] **Step 3**: Reset `m_pendingDamage` on CLIENT connect (alongside other M1-M6 resets).

- [ ] **Step 4**: Build, 41/41 tests still pass. Commit: `feat(net): predict incoming-projectile damage feedback (visual only) — M7.2`.

---

## Task 3: Verify

- [ ] Clean build, 41/41 tests. M7 = 2 commits.

## Definition of Done
- [ ] 41/41 tests pass
- [ ] `PendingDamageRing m_pendingDamage` in engine.h
- [ ] Predicted-damage record call exists in the projectile-vs-player check
- [ ] HP is NOT touched by M7 (only by snapshot reconcile + M10 acks)
