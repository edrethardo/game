#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/weapon.h"
#include "renderer/frustum.h"

static constexpr u32 MAX_ENTITIES = 128;

enum EntityFlags : u8 {
    ENT_ACTIVE       = 1 << 0,
    ENT_FLYING       = 1 << 1,
    ENT_DEAD         = 1 << 2,
    ENT_FRIENDLY     = 1 << 3,  // allied NPC, not targeted by player weapons
    ENT_UNTARGETABLE = 1 << 4,  // enemies ignore this entity (swarm drones, effects)
};

enum struct AIState : u8 {
    IDLE,
    CHASE,
    ATTACK,
    FLYBY,    // bat swoops past player to attack from behind
    DORMANT,  // mimic: looks like a chest until player approaches
    FLANK,    // circling to player's side/rear via A* path
    RETREAT,  // falling back to cover (low HP or post-attack feint)
    AMBUSH,   // holding doorway position, waiting for player
    STRAFE,   // ranged: sidestepping while firing
    SURROUND, // melee: spreading to surround target
    DEAD,
};

// Enemy type determines limb configuration and animation behavior
enum struct EnemyType : u8 {
    GENERIC = 0,  // no limbs, single mesh
    SKELETON,     // 2 legs, 2 arms, weapon carrying
    BAT,          // 2 wings, 2 claws
    SPIDER,       // 8 legs, 2 mandibles
    MIMIC,        // disguised as chest, attacks when approached
    HELLHOUND,    // quadruped canine demon — 4 legs, galloping animation
    SENTINEL,     // armored shield-bearer — 2 legs, shield arm, blocking stance
    SUCCUBUS,     // harpy-style flyer — 2 bat wings, 2 dangling talons, no walking legs
    PIT_FIEND,    // winged demon — 2 bat wings, 2 legs, wing-flap + walk animation
    HELLFORGE_SMITH, // hunched blacksmith — 2 legs, hammer arm, hammer-swing idle
    BOSS,         // large boss enemy (uses skeleton rig, oversized)
    PROP,         // static decoration — no AI, no collision response, no animation
    COUNT
};

// Special archetype role — bitmask so bosses can combine multiple roles.
// Regular enemies typically have one role; bosses can stack 2-3.
namespace EnemyRole {
    constexpr u8 NORMAL        = 0x00;
    constexpr u8 AMBUSH        = 0x01;  // gargoyle — starts dormant, wakes when player is close
    constexpr u8 SUMMONER      = 0x02;  // necromancer — resurrects dead enemies
    constexpr u8 HEALER        = 0x04;  // shaman — heals injured allies
    constexpr u8 AURA          = 0x08;  // herald — passive damage aura around self
    constexpr u8 RANGED_CASTER = 0x10;  // bone mage — prefers strafe, fires projectiles
    constexpr u8 CHARGER       = 0x20;  // ghoul — sprint-charge then retreat
    constexpr u8 BOMBER        = 0x40;  // plague bat — dive-bomb or explode on death
    constexpr u8 SHIELD_BEARER = 0x80;  // sentinel — frontal damage reduction, forces flanking
}

// Squad role assigned by room coordinator to spread tactics across a group
enum struct SquadRole : u8 {
    ROLE_NONE = 0,
    ROLE_RUSH,    // charge head-on
    ROLE_FLANK,   // circle to side/rear
    ROLE_HOLD,    // ranged: hold doorway/distance
    ROLE_HARASS,  // flying: orbit and dive
};

// NPC class determines base stats, AI behavior, and starting equipment
enum struct NpcClass : u8 {
    NONE = 0,   // not an NPC (hostile enemy)
    CLERIC,     // melee healer, mace, heavy armor
    ARCHER,     // ranged, bow, light armor, less HP
    MAGE,       // ranged, staff projectiles, robes
    ROGUE,      // ranged, throwing knives, fast, leather
    PALADIN,    // melee tank, mace, heavy plate
    COUNT
};

// Max friendly NPCs that can carry equipment simultaneously
static constexpr u32 MAX_NPC_EQUIP = 8;

struct Entity {
    // Identity
    u16 generation = 0;
    u8  flags      = 0;

    // Transform
    Vec3 position  = {0,0,0};   // centre of AABB
    Vec3 velocity  = {0,0,0};
    f32  yaw       = 0.0f;

    // Collision
    Vec3 halfExtents = {0.4f, 0.5f, 0.4f};

    // Combat
    f32  health         = 50.0f;
    f32  maxHealth      = 50.0f;
    f32  attackRange    = 2.0f;
    f32  attackCooldown = 1.0f;
    f32  attackTimer    = 0.0f;
    f32  damage         = 10.0f;
    u16  level          = 1;   // effective floor (currentFloor + difficulty*50); scales stats/loot
    f32  moveSpeed      = 3.0f;
    f32  detectionRange = 15.0f;

    // AI
    AIState aiState    = AIState::IDLE;
    u16     aiCheckIdx = 0;  // staggered LOS frame counter
    Vec3    flybyTarget = {0,0,0};  // waypoint for FLYBY state
    f32     flybyTimer  = 0.0f;     // time left in flyby maneuver
    f32     stuckTimer  = 0.0f;     // stuck detection accumulator (friendly NPCs)

    // Spawn position — set once on EntitySystem::spawn, used to walk enemies home on player death
    Vec3 homePosition = {0,0,0};

    // Pathfinding (A* waypoint cache — filled by tactical planner, consumed by FLANK/RETREAT/SURROUND)
    Vec3 pathWaypoints[6] = {};
    u8   pathLen = 0;  // number of valid waypoints
    u8   pathIdx = 0;  // next waypoint to move toward

    // Squad coordination
    SquadRole squadRole = SquadRole::ROLE_NONE;
    u16 squadId = 0xFFFF;  // room-based squad identifier (0xFFFF = unassigned)

    // Tactical state timers (multi-purpose to avoid proliferating fields)
    f32  tacticalTimer = 0.0f;  // re-flank interval, retreat hold duration, ambush patience
    f32  sprintTimer   = 0.0f;  // anti-kite sprint burst cooldown
    f32  kiteTimer     = 0.0f;  // how long target has maintained distance (triggers sprint)
    bool hasRetreated  = false; // prevents immediate re-retreat after re-engage
    u8 enemyRole = EnemyRole::NORMAL; // archetype bitmask (summoner, healer, aura, ambush, etc.)
    u8  resurrectCount = 0;  // necromancer: how many dead enemies have been raised (no cap; stat only)

    // Identity — stable name for game logic (boss reactions, quests, etc.)
    const char* nameTag = nullptr;  // e.g. "butcher", "lich_lord" (nullptr = anonymous)

    // Rendering
    u8  meshId     = 0;  // index into Engine::m_meshDefs
    u8  materialId = 0;  // index into MaterialSystem
    EnemyType enemyType = EnemyType::GENERIC;
    u8 weaponMeshId = 0;  // skeleton weapon mesh index (0 = none)
    u8 bossLimbConfig = 0; // 0=default, 1-4=boss-specific extra limbs
    u8 bossDefIdx = 0xFF;  // index into Engine::m_bossDefs (0xFF = not a boss)
    u16 spawnerIdx = 0xFFFF; // entity pool index of boss that spawned this minion
    bool minionShield = false; // boss takes 75% reduced damage while alive minions exist

    // NPC equipment and class (friendly NPCs only)
    NpcClass npcClass   = NpcClass::NONE;
    u8 npcEquipIdx      = 0xFF;  // index into NpcEquipment pool (0xFF = none)
    WeaponType npcWeaponType = WeaponType::MELEE;  // attack method for friendly AI
    f32 npcProjectileSpeed  = 0.0f;  // projectile speed (PROJECTILE type only)
    f32 npcProjectileRadius = 0.0f;

    // NPC speech bubble
    const char* speechText = nullptr;  // current speech (nullptr = no bubble)
    f32 speechTimer = 0.0f;            // countdown, bubble fades and clears at 0

    // NPC combat targeting (index into entity pool, 0xFFFF = no target)
    u16 targetEntityIdx = 0xFFFF;
    Vec3 lastSeenPos    = {0,0,0};  // last position where target had LOS (for move-to when blocked)
    bool hasTargetLOS   = false;    // whether current target is visible this frame

    // Animation
    f32  animTimer    = 0.0f;  // continuous timer for procedural animation
    f32  attackAnimT  = 0.0f;  // brief attack animation countdown

    // Status effects (timer > 0 = active, ticked down each frame)
    f32  poisonTimer    = 0.0f;
    f32  poisonDps      = 0.0f;  // damage per second while poisoned
    f32  burnTimer      = 0.0f;
    f32  burnDps        = 0.0f;
    f32  freezeTimer    = 0.0f;  // halves movement speed
    f32  stunTimer      = 0.0f;  // fully immobilized, no AI, no attacks
    f32  overclockTimer = 0.0f;  // Tinkerer overclock buff: 2× dmg, 1.5× speed
    f32  queenLifeTimer = 0.0f;  // Swarm Queen despawn countdown
    f32  queenSpawnTimer = 0.0f; // Swarm Queen auto-spawn interval
    f32  markPreyTimer  = 0.0f;  // Ranger Mark Prey: takes 2× damage while active
    f32  markPreyDmgMult = 1.0f; // resets to 1.0 when markPreyTimer expires

    // On-hit status effect this entity applies to targets (0=none)
    // 1=poison, 2=slow, 3=burn, 4=freeze
    u8   onHitEffect    = 0;
    f32  onHitDuration  = 0.0f;
    f32  onHitDps       = 0.0f;  // for poison/burn

    // Aura buff state — set on spawn from template, modified at runtime by AURA heralds
    f32  baseMoveSpeed      = 0.0f;   // original moveSpeed from spawn template
    f32  baseAttackCooldown = 0.0f;   // original attackCooldown from spawn template
    bool hasAuraBuff        = false;  // true while within range of an AURA herald this frame

    // Feedback
    f32  flashTimer      = 0.0f;
    f32  knockbackTimer  = 0.0f;  // >0 while a knockback impulse is decaying
    f32  deathTimer      = 0.0f;
};

struct EntityHandle {
    u16 index      = 0xFFFF;
    u16 generation = 0;
};

struct EntityPool {
    Entity entities[MAX_ENTITIES];
    u16    freeList[MAX_ENTITIES];
    u32    freeCount = 0;

    // Active entity indices for fast iteration
    u32    activeList[MAX_ENTITIES];
    u32    activeCount = 0;
};

inline bool handleValid(const EntityPool& pool, EntityHandle h) {
    return h.index < MAX_ENTITIES &&
           (pool.entities[h.index].flags & ENT_ACTIVE) &&
           pool.entities[h.index].generation == h.generation;
}

inline Entity* handleGet(EntityPool& pool, EntityHandle h) {
    if (!handleValid(pool, h)) return nullptr;
    return &pool.entities[h.index];
}

inline AABB entityAABB(const Entity& e) {
    return { e.position - e.halfExtents, e.position + e.halfExtents };
}

namespace EntitySystem {
    void init(EntityPool& pool);

    EntityHandle spawn(EntityPool& pool, Vec3 position, Vec3 halfExtents,
                       bool flying, f32 health, f32 moveSpeed,
                       f32 detectionRange, f32 attackRange,
                       f32 attackCooldown, f32 damage);

    void despawn(EntityPool& pool, EntityHandle handle);

    // Tick timers (flash, death). Called from engine update.
    void tickTimers(EntityPool& pool, f32 dt);

    // Count currently active (including dying)
    u32 activeCount(const EntityPool& pool);
}
