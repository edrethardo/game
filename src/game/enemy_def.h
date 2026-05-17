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
    u8   role            = 0;     // EnemyRole bitmask
    u8   aiPreference    = 0;     // AIState enum value (initial state)
    u8   onHitEffect     = 0;     // 0=none, 1=poison, 2=slow, 3=burn, 4=freeze
    f32  onHitDuration   = 0.0f;
    f32  onHitDps        = 0.0f;
    f32  dropWeight      = 1.0f;

    // Resolved IDs (filled after mesh/material systems init)
    u8   meshId          = 0;
    u8   materialId      = 0;
    EnemyType enemyType  = EnemyType::SKELETON;
};

struct EnemyDefTable {
    EnemyDef defs[MAX_ENEMY_DEFS];
    u32 count = 0;
};

// Utility: collect defs for a specific tier into a pointer array.
// Returns count of matching defs.
inline u32 collectTierDefs(const EnemyDefTable& table, u8 tier,
                           const EnemyDef** out, u32 outMax) {
    u32 n = 0;
    for (u32 i = 0; i < table.count && n < outMax; i++) {
        if (table.defs[i].tier == tier) out[n++] = &table.defs[i];
    }
    return n;
}
