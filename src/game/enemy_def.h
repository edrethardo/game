// Enemy definition struct — loaded from assets/config/enemies.json.
// Replaces the inline kTier* arrays in engine_startgame.cpp.
// See CLAUDE.md for JSON schema and the enemy rework design spec.

#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"

static constexpr u32 MAX_ENEMY_DEFS = 64;

struct EnemyDef {
    char name[48]     = {};
    u8   tier         = 1;       // 1-5, maps to floor ranges
    char meshName[32] = {};
    char matName[32]  = {};

    // Combat stats (base, scaled by floor mult at spawn)
    f32  health          = 50.0f;
    f32  moveSpeed       = 3.0f;
    f32  detectionRange  = 15.0f;
    f32  attackRange     = 2.5f;
    f32  attackCooldown  = 1.0f;
    f32  damage          = 10.0f;

    bool flying          = false;
    Vec3 halfExtents     = {0.4f, 0.9f, 0.4f};

    // Behavior (from JSON)
    u16  role            = 0;     // EnemyRole bitmask (u16 — the byte filled up)
    u8   aiPreference    = 0;     // AIState enum value (initial state)
    bool burrower        = false; // dormant AMBUSH spawns UNDERGROUND (ENT_BURROWED): hidden,
                                  // unhittable, non-blocking; erupts on plain proximity
    u8   onHitEffect     = 0;     // 0=none, 1=poison, 2=slow, 3=burn, 4=freeze
    f32  onHitDuration   = 0.0f;
    f32  onHitDps        = 0.0f;
    f32  dropWeight      = 1.0f;

    // NAMED, one-of-a-kind monster: never drawn by the random tier roster. An act boss lives in
    // enemies.json like anything else (it needs a mesh, a skin, roles and an onHit like any enemy),
    // but it is placed BY NAME at a fixed spot — a zone's `boss` field — and a quest may name it as
    // a SLAY target. Without this flag it also sits in its tier's spawn pool, which produced two
    // Griswalds in one TristRAM and put Act 2's final boss in an Act 1 field as ordinary trash.
    // Filtered in collectTierDefs, the single choke every random roster goes through.
    bool unique          = false;

    // WHICH BESTIARY this enemy belongs to. 0 = the dungeon (floors 1-50), 1 = overworld Act 1,
    // 2 = Act 2's Underground. The acts are tier 5 like the deepest dungeon floors — post-Inferno
    // heroes would find tier-1 wildlife to be scenery — so WITHOUT this tag the two pools are the
    // same pool: a Zombie Process turns up on Hell floor 45, and TristRAM fields Void Heralds and
    // Act 2's tube-dwellers instead of the act's own roster. Filtered in collectTierDefs beside
    // `unique`, so every random roster in the game honours it.
    u8   act             = 0;

    // Breeder: this enemy periodically spawns a fresh copy of another enemy (Broodmother →
    // Dungeon Spider). Name from JSON ("spawnEnemy"); resolved to an index after all defs load.
    // 0xFFFF ⇒ not a breeder. The breed cadence/cap lives in the engine breeder pass (tickBreeders).
    char spawnEnemyName[32] = {};

    // Resolved IDs (filled after mesh/material systems init)
    u8   meshId          = 0;
    u8   materialId      = 0;
    u16  spawnEnemyIdx   = 0xFFFF; // resolved from spawnEnemyName (0xFFFF = not a breeder)
    EnemyType enemyType  = EnemyType::SKELETON;
};

struct EnemyDefTable {
    EnemyDef defs[MAX_ENEMY_DEFS];
    u32 count = 0;
};

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

// Utility: collect defs for a specific tier into a pointer array.
// Returns count of matching defs.
// `act` is REQUIRED rather than defaulted to 0: a default is exactly how the dungeon would keep
// silently drawing overworld monsters the day someone adds a call site and forgets the argument,
// which is the bug this parameter exists to prevent.
inline u32 collectTierDefs(const EnemyDefTable& table, u8 tier,
                           const EnemyDef** out, u32 outMax, u8 act) {
    u32 n = 0;
    for (u32 i = 0; i < table.count && n < outMax; i++) {
        const EnemyDef& d = table.defs[i];
        // `unique` defs are placed by name, never rolled — see EnemyDef::unique.
        if (d.tier == tier && d.act == act && !d.unique) out[n++] = &d;
    }
    return n;
}
