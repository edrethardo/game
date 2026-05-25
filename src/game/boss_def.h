// Boss definition structs and personality enum.
// Bosses are elite enemies with role combos, AI personalities, skill access,
// and enrage curves. Loaded from assets/config/bosses.json at init.
// See CLAUDE.md "Architecture" and the enemy/boss rework design spec.

#pragma once

#include "core/types.h"
#include "core/math.h"

static constexpr u32 MAX_BOSS_DEFS = 16;

// AI personality — overrides FSM state selection for bosses.
// Regular enemies don't use this; their behavior comes from EnemyRole alone.
enum struct BossPersonality : u8 {
    BERSERKER,    // never retreats, shortest-path chase, extra enrage scaling
    KITER,        // maintains preferred range, retreats aggressively, hides behind summons
    TELEPORTER,   // blinks to random walkable cell near player every 4-6s
    DUELIST,      // circle-strafes, counter-charges when player attacks
    COUNT
};

// Optional ranged projectile attack for bosses (e.g. Butcher's cleaver throw)
struct BossProjectileDef {
    bool enabled       = false;
    bool usesWeaponMesh = false; // render projectile using boss's weapon mesh
    f32  speed         = 18.0f;
    f32  radius        = 0.15f;
    f32  cooldown      = 4.0f;   // seconds between throws
    u8   onHitEffect   = 0;      // 0=none, 1=poison, 2=slow, 3=burn, 4=freeze
    f32  onHitDuration = 0.0f;
};

// Full boss definition — loaded from bosses.json.
// One per milestone floor (5,10,15,...50). Indexed by BossDef array position.
struct BossDef {
    u8  floor           = 0;
    bool isMajor        = false;   // major = 4x arena + iron maidens

    // Identity
    char name[48]       = {};
    char speech[64]     = {};

    // Base stats (scaled by floor mult at spawn time)
    f32 baseHp          = 500.0f;
    f32 baseDmg         = 30.0f;
    f32 speed           = 3.0f;
    f32 atkRange        = 3.5f;
    f32 atkCooldown     = 1.0f;
    f32 detectionRange  = 40.0f;
    Vec3 halfExtents    = {0.5f, 1.0f, 0.5f};

    // Visuals (resolved to IDs after init)
    char meshName[32]   = {};
    char matName[32]    = {};
    char weaponName[32] = {};  // empty = no weapon mesh

    // Behavior
    u8  roles           = 0x00;  // EnemyRole bitmask
    BossPersonality personality = BossPersonality::BERSERKER;
    u8  skillId         = 0;     // SkillId to cast (0 = none)
    f32 enrageFactor    = 0.3f;  // 0.0 = no enrage, 0.5 = very aggressive
    bool minionShield   = false; // 75% damage reduction while minions alive
    bool secondPhase    = false; // "false death": survives first kill, entombs + summons guardians

    // On-hit status effect for melee attacks
    u8  onHitEffect     = 0;     // 0=none, 1=poison, 2=slow, 3=burn, 4=freeze
    f32 onHitDuration   = 0.0f;
    f32 onHitDps        = 0.0f;

    // Ranged projectile (optional)
    BossProjectileDef projectile;

    // Loot
    u8  lootGuarantee   = 0;     // minimum Rarity enum value (1=magic, 2=rare, 3=legendary)
    u8  bonusDrops      = 0;     // extra items dropped on death

    // Limb configuration (0=default, 1=spider legs, 2=tentacles, 3=back spikes, 4=blade arms)
    u8  limbConfig      = 0;

    // Resolved IDs (filled after mesh/material systems init)
    u8  meshId          = 0;
    u8  materialId      = 0;
    u8  weaponMeshId    = 0;
};

// Loaded boss def array — stored on Engine, populated by loadBossDefs()
struct BossDefTable {
    BossDef defs[MAX_BOSS_DEFS];
    u32 count = 0;
};

// Lookup boss def by floor number. Returns nullptr if no boss on that floor.
inline const BossDef* findBossDefByFloor(const BossDefTable& table, u8 floor) {
    for (u32 i = 0; i < table.count; i++) {
        if (table.defs[i].floor == floor) return &table.defs[i];
    }
    return nullptr;
}

// Lookup boss def index by floor number. Returns 0xFF if not found.
inline u8 findBossDefIdx(const BossDefTable& table, u8 floor) {
    for (u32 i = 0; i < table.count; i++) {
        if (table.defs[i].floor == floor) return static_cast<u8>(i);
    }
    return 0xFF;
}
