// test_engine_shield.cpp — The Dungeon Engine secret superboss damage-immunity predicate.
//
// The Engine is invulnerable while any boss it "recompiled" (a wave-add) is alive; clearing the
// wave reopens it. The crux (and the easy bug — R3 in the design doc) is the discriminator: the
// wave-adds are themselves real bosses (isBoss/bossDefIdx set), so the shield MUST key on
// spawnerIdx, not isBoss — otherwise the adds would be immune too. This pins that contract.
//
// Combat::engineShieldActive is header-inline, so this links against no production .cpp.

#include "doctest/doctest.h"
#include "game/combat.h"

TEST_CASE("Engine shield is active only while a summoned wave-add is alive") {
    EntityPool pool{};
    const u16 engineIdx = 0;

    // The Engine itself (no adds yet) — must be damageable in the OPEN window.
    pool.entities[engineIdx].isEngine = true;
    pool.entities[engineIdx].flags = 0;
    pool.activeList[0] = engineIdx;
    pool.activeCount = 1;
    CHECK(Combat::engineShieldActive(pool, engineIdx) == false);

    // Recompile a wave-add: a real boss (isBoss true) tied to the Engine via spawnerIdx.
    const u16 addIdx = 1;
    pool.entities[addIdx].isEngine   = false;       // an add is NOT the Engine — it takes full damage
    pool.entities[addIdx].isBoss     = true;        // ...but it IS a boss; the predicate must ignore that
    pool.entities[addIdx].bossDefIdx = 3;           // ...and carries a bossDefIdx; ignore that too
    pool.entities[addIdx].spawnerIdx = engineIdx;
    pool.entities[addIdx].flags = 0;
    pool.activeList[1] = addIdx;
    pool.activeCount = 2;

    // Add alive → the Engine is shielded.
    CHECK(Combat::engineShieldActive(pool, engineIdx) == true);
    // ...but the add itself is never shielded (nothing was spawned BY the add).
    CHECK(Combat::engineShieldActive(pool, addIdx) == false);

    // Kill the add → the Engine becomes damageable again.
    pool.entities[addIdx].flags |= ENT_DEAD;
    CHECK(Combat::engineShieldActive(pool, engineIdx) == false);
}

TEST_CASE("Engine shield counts only its OWN adds (other bosses' minions don't extend it)") {
    EntityPool pool{};
    const u16 engineIdx = 0;
    pool.entities[engineIdx].isEngine = true;
    pool.activeList[0] = engineIdx;

    // A summoned Malachar (the wave-add) at idx 1, and ITS OWN guardian at idx 2 whose spawnerIdx
    // points at Malachar (idx 1), not the Engine. The guardian must NOT keep the Engine shielded.
    pool.entities[1].spawnerIdx = engineIdx;  // Malachar-add → Engine
    pool.entities[2].spawnerIdx = 1;          // guardian → Malachar (not the Engine)
    pool.activeList[1] = 1;
    pool.activeList[2] = 2;
    pool.activeCount = 3;

    CHECK(Combat::engineShieldActive(pool, engineIdx) == true);   // Malachar-add alive

    // Malachar dies but his guardian lingers — the Engine reopens (the guardian isn't its add).
    pool.entities[1].flags |= ENT_DEAD;
    CHECK(Combat::engineShieldActive(pool, engineIdx) == false);
}
