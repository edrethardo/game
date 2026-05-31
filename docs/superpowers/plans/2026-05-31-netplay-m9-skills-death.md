# Netplay M9: Predicted Skills + Death/Respawn Plan

> superpowers:subagent-driven-development. Checkbox `- [ ]`.

**Status check:** The existing M0 baseline already locally activates skills for the local player on input — `updateSkills` runs `SkillSystem::tryActivate` for the local slot regardless of NetRole. So cooldown + energy deduction already happens at zero latency on CLIENT. The snapshot then mirrors the server's state (which agrees because the server processed the same input from the wire). So *the local prediction part is already done*.

What's left for M9:
1. Add a `PendingSkillRing` so M10 can ack reliable `SV_SKILL_RESULT` events (server rejection due to insufficient energy / cooldown not ready, mostly defensive).
2. Predict respawn transition — client locally clears `damageFlashTimer`, restores HP to spawn value, ungates input. Server confirms via snapshot.
3. Defer the "smooth-revert on rejected skill" to M13.

---

## Task 1: PendingSkillRing scaffold + tests

**Files:**
- Create: `src/net/pending_skill_ring.h`, `src/net/pending_skill_ring.cpp`
- Create: `tests/net/test_pending_skill_ring.cpp`
- Modify: both CMakeLists

Same pattern as M6/M7/M8 — small bounded ring for M10 to consume.

- [ ] **Step 1**: Create `tests/net/test_pending_skill_ring.cpp` with 4 TEST_CASEs (empty, record, ack, expire) — copy the structure from M8 test file. The struct stores `(u32 clientTick, u8 skillSlot)` — skillSlot is the enum that identifies which class/boot/helm skill was activated.

- [ ] **Step 2**: Create `src/net/pending_skill_ring.h`:

```cpp
#pragma once
#include "core/types.h"

static constexpr u32 PENDING_SKILL_RING_CAPACITY = 16;

struct PendingSkill {
    u32 clientTick = 0;
    u8  skillSlot  = 0;   // matches NetInput.skillSlot (0-3 = class skill slot; higher = boot/helm if extended)
};

struct PendingSkillRing {
    PendingSkill entries[PENDING_SKILL_RING_CAPACITY] = {};
    u32          count = 0;
};

namespace PendingSkillRingOps {
    void reset(PendingSkillRing& r);
    void record(PendingSkillRing& r, u32 clientTick, u8 skillSlot);
    bool ack(PendingSkillRing& r, u32 clientTick, u8 skillSlot);
    void expireOlderThan(PendingSkillRing& r, u32 cutoffClientTick);
}
```

- [ ] **Step 3**: Create `src/net/pending_skill_ring.cpp` — same shape as M8's pending_pickup_ring.cpp, adapted to the (clientTick, skillSlot) key.

- [ ] **Step 4**: Wire into both CMakeLists. Build. 49/49 pass (45 + 4 new).

- [ ] **Step 5**: Commit: `feat(net): PendingSkillRing scaffold + tests (TDD) — M9.1`.

---

## Task 2: Wire predicted skill recording

**Files:**
- Modify: `src/engine/engine.h` — add `PendingSkillRing m_pendingSkills;`
- Modify: `src/engine/engine_update_skills.cpp` — in each successful `tryActivate(...)` branch where `extFlags & INPUT_EX_SKILL` (or boot/helm) is the trigger, record an entry on CLIENT only.

- [ ] **Step 1**: Add to engine.h:
```cpp
    PendingSkillRing m_pendingSkills;
```

- [ ] **Step 2**: In engine_update_skills.cpp, find the three `if (SkillSystem::tryActivate(...))` success branches (class skill at ~170, boot at ~218, helm at ~236). In each success branch, on CLIENT only, add:

```cpp
    if (m_netRole == NetRole::CLIENT) {
        PendingSkillRingOps::record(m_pendingSkills, m_clientTick, /*skillSlot=*/ slot);
    }
```

For boot/helm where there's no `slot` variable, use a sentinel like 0xFE / 0xFF — adapt to what makes sense semantically.

- [ ] **Step 3**: Reset in engine_startgame.cpp's CLIENT connect block. Add `PendingSkillRingOps::reset(m_pendingSkills);`.

- [ ] **Step 4**: Build, 49/49 still pass. Commit: `feat(net): record predicted skill activations into PendingSkillRing — M9.2`.

---

## Task 3: Predicted respawn transition

**Files:**
- Modify: wherever the respawn input is handled on CLIENT

- [ ] **Step 1**: Search for `CL_RESPAWN` or `sendRespawnRequest`:
```bash
grep -n 'CL_RESPAWN\|respawnRequest\|requestRespawn' src/ -r 2>&1 | head -10
```

- [ ] **Step 2**: At the CL_RESPAWN send site on CLIENT, predict the transition. Find what the host does at respawn (look in engine_death.cpp or wherever respawn applies — searches for `respawn` should turn it up). Replicate the local state changes on the CLIENT immediately after sendRespawn:

```cpp
    // Predict respawn locally — server confirms via snapshot. If server rejects (rare),
    // the snapshot still shows us dead and reconcile will roll back.
    m_localPlayer.health = m_localPlayer.maxHealth;
    m_localPlayer.damageFlashTimer = 0.0f;
    m_localPlayer.invulnTimer = 1.0f;  // brief respawn invuln, matches host
    m_localPlayer.position = m_localPlayer.spawnPosition;
    // Reset any flags / status timers / death-cam state etc. — match host's path.
```

If the project already has a helper `applyRespawnState(Player&)` or similar, call it. If not, write out the field assignments.

- [ ] **Step 3**: Build, 49/49 tests pass. Commit: `feat(net): predict respawn transition on CLIENT — M9.3`.

---

## Task 4: Verify

- [ ] Clean tree, build, 49/49 tests. M9 = 3 commits.

## Definition of Done
- [ ] 49/49 tests pass
- [ ] `PendingSkillRing m_pendingSkills` in engine.h
- [ ] Skill activations recorded in PendingSkillRing on CLIENT
- [ ] Local-player respawn state applied immediately on CL_RESPAWN send
