#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/weapon.h"

struct LevelGrid; // forward declaration for WorldItemSystem::spawn

// ---- Constants ----

static constexpr u32 MAX_ITEM_DEFS       = 160; // expanded for full floor coverage on all weapon subtypes
static constexpr u32 MAX_AFFIX_DEFS      = 32;
static constexpr u32 MAX_AFFIXES_PER_ITEM = 4;
static constexpr u32 MAX_INVENTORY_ITEMS = 24;
static constexpr u32 MAX_SKILL_DEFS      = 64;
static constexpr u32 MAX_WORLD_ITEMS     = 32;

// ---- Rarity tiers ----

enum struct Rarity : u8 {
    COMMON,      // grey
    MAGIC,       // blue
    RARE,        // yellow
    LEGENDARY,   // orange
    COUNT
};

// ---- Equipment slots ----

enum struct ItemSlot : u8 {
    WEAPON,
    OFFHAND,
    HELMET,
    ARMOR,
    BOOTS,
    RING,
    COUNT
};

// ---- Affix stat types ----

enum struct AffixType : u8 {
    DAMAGE_FLAT,
    HEALTH_FLAT,
    MOVE_SPEED_FLAT,
    DAMAGE_PCT,
    COOLDOWN_REDUCTION,
    HEALTH_PCT,
    LIFE_ON_HIT,
    PROJECTILE_SPEED,
    CONE_ANGLE,
    RANGE_BONUS,
    DAMAGE_TO_FLYING,
    CLIP_SIZE_PCT,      // % extra magazine capacity
    RELOAD_SPEED_PCT,   // % faster reload
    ENERGY_FLAT,        // flat bonus to max energy
    COUNT
};

// ---- Player class ----

enum struct PlayerClass : u8 {
    WARRIOR,
    RANGER,
    SORCERER,
    ROGUE,
    PALADIN,
    COMBAT_ENGINEER,
    MARKSMAN,
    TINKERER,
    CLASS_COUNT
};

// ---- Skill IDs (legendary powers + class skills) ----

enum struct SkillId : u8 {
    NONE = 0,
    // Legacy legendary skills
    FROZEN_ORB,
    CHAIN_LIGHTNING,
    METEOR_STRIKE,
    BLOOD_NOVA,
    PHASE_DASH,

    // Warrior
    CLEAVE,
    WAR_CRY,
    THUNDERCLAP,  // ground stomp AoE with stun + slow
    WHIRLWIND,
    EARTHQUAKE,

    // Ranger
    MULTI_SHOT,         // legacy
    RAIN_OF_ARROWS,     // legacy
    POISON_ARROW,       // legacy
    SHADOW_SHOT,        // legacy
    VOLLEY,             // 20 arrows rain in target area
    PIERCING_SHOT,      // penetrating arrow + bleed DoT
    BARRAGE,            // 10-arrow shotgun blast
    MARK_PREY,          // 2× damage debuff, chain clear on kill

    // Sorcerer
    FIREBALL,
    // (reuses FROZEN_ORB, CHAIN_LIGHTNING, METEOR_STRIKE)

    // Rogue
    KNIFE_BURST,        // legacy
    POISON_CLOUD,
    SHADOW_STRIKE,      // legacy
    FAN_OF_KNIVES,      // 8-knife 360° burst + stealth
    SHADOW_STEP,        // teleport behind + backstab from stealth
    SHADOW_DANCE,       // 2s stealth + 2× damage + speed, kills extend

    // Paladin
    HOLY_SMITE,
    CONSECRATION,       // legacy — kept for save compat
    DIVINE_SHIELD,      // legacy — kept for save compat
    HOLY_BOMBARDMENT,   // replaces Consecration
    HOLY_NOVA,          // Paladin-specific AoE (no HP cost)
    DIVINE_JUDGMENT,    // replaces Divine Shield

    // Combat Engineer
    SHOCK_BOLT,
    DEPLOY_TURRET,
    TESLA_COIL,
    MECH_OVERDRIVE,

    // Marksman
    AIMED_SHOT,
    EXPLOSIVE_ROUND,
    RAPID_FIRE,             // legacy — kept for save compat
    OVERCHARGED_MAGAZINE,   // replaces Rapid Fire
    HEADSHOT,

    // Tinkerer
    COMBAT_DRONE,       // legacy
    SWARM_DRONES,       // legacy
    STUN_GRENADE,       // legacy
    SWARM_DEPLOY,       // mass drone spawn (3 spiders + 3 bats)
    OVERCLOCK,          // buff all drones +100% dmg +50% speed
    DETONATE_SWARM,     // explode all drones for AoE
    SWARM_QUEEN,        // summon auto-spawning queen
    // (reuses DEPLOY_TURRET)

    // Legendary weapon effects
    THROWAWAY,      // throw weapon as projectile on reload
    VOID_ZONE,      // on-hit: dark zone dealing flat + 60% missing HP
    SHADOW_RICOCHET, // on-hit: 2 shadow bolts seek nearby enemies (can re-proc)

    // Ring passives (always-on while equipped)
    LIFE_STEAL,     // heal 5% of damage dealt
    THORNS,         // reflect 20% damage to attacker
    BERSERKER,      // +1% damage per 1% missing HP
    SECOND_WIND,    // at <20% HP: heal 30% + 2s invuln (60s cooldown)
    SOUL_HARVEST,   // on kill: +5% speed +3% damage for 10s (5 stacks)
    GRAVITY_PULL,   // pull enemies within 5m toward player
    PHASE_STRIKE,   // 10% on hit: teleport behind target
    VOID_KILL,      // on kill: 15% chance void zone on corpse
    ARC_FIRE,       // melee proc: 20% chance spawn fire in swing arc (1.5s)

    COUNT
};

// ---- Class definition (static template per class) ----

struct ClassDef {
    const char* name;
    const char* description;
    f32 baseHealth;
    f32 baseMoveSpeed;
    f32 baseEnergy;
    const char* startingWeaponName; // matched against ItemDef.name
    SkillId skills[4];             // skill slots 1-4
    u8 skillUnlockFloor[4];        // floor at which each skill becomes available
    u8 skillUpgradeFloor[4];       // floor at which each skill gets its upgrade
    WeaponType preferredWeapon;    // +20% damage with this weapon type
};

// Global class definition table (defined in engine.cpp)
extern const ClassDef kClassDefs[static_cast<u32>(PlayerClass::CLASS_COUNT)];

// ---- Affix instance (a single rolled modifier on an item) ----

struct Affix {
    AffixType type  = AffixType::DAMAGE_FLAT;
    f32       value = 0.0f;
};

// ---- Affix definition (template loaded from JSON) ----

struct AffixDef {
    char      name[32] = {};           // e.g. "of Striking"
    AffixType type     = AffixType::DAMAGE_FLAT;
    f32       minValue = 0.0f;
    f32       maxValue = 0.0f;
    u8        validSlots = 0;          // bitmask of ItemSlot values
};

// ---- Item definition (template loaded from JSON) ----

// Static item template loaded from assets/config/items.json via ItemLoader.
// Each entry defines base stats; actual items are ItemInstance with rolled values.
struct ItemDef {
    char     name[32] = {};
    ItemSlot slot     = ItemSlot::WEAPON;
    Rarity   maxRarity = Rarity::COMMON;

    // Weapon base stats (0 for non-weapons)
    WeaponType weaponType            = WeaponType::MELEE;
    f32        baseDamage            = 0.0f;
    f32        baseRange             = 0.0f;
    f32        baseCooldown          = 0.0f;
    f32        baseConeAngle         = 0.0f;
    f32        baseProjectileSpeed   = 0.0f;
    f32        baseProjectileRadius  = 0.0f;
    f32        baseRecoil            = 0.0f;
    u8         baseClipSize          = 0;     // 0 = no clip (melee/projectile)
    f32        baseReloadTime        = 0.0f;  // seconds to reload magazine

    // Armor base stats
    f32 baseHealth = 0.0f;

    // Legendary skill
    SkillId legendarySkillId = SkillId::NONE;

    // Drop level range
    u8 minLevel = 1;
    u8 maxLevel = 10;

    // Weapon subtype for visual/gameplay identity (NONE for non-weapons)
    WeaponSubtype weaponSubtype = WeaponSubtype::NONE;

    // Visual identity — resolved at init from mesh/material name strings
    u8  meshId     = 0;   // index into Engine::m_meshDefs (0 = cube fallback)
    u8  materialId = 0;   // index into MaterialSystem

    // Loot generation weight (higher = more likely to drop)
    f32 dropWeight = 1.0f;

    // Mesh/material name strings for deferred resolution (not serialized at runtime)
    char meshName[32]     = {};
    char materialName[32] = {};
};

// ---- Item instance (actual rolled item at runtime) ----

// Runtime item instance with rolled rarity, affixes, and level-scaled stats.
// defId indexes into the ItemDef table; 0xFFFF = empty/invalid slot.
struct ItemInstance {
    u16    defId      = 0xFFFF;        // index into ItemDef table (0xFFFF = empty)
    Rarity rarity     = Rarity::COMMON;
    u8     itemLevel  = 0;
    u8     affixCount = 0;
    Affix  affixes[MAX_AFFIXES_PER_ITEM] = {};

    // Rolled base stats (after level scaling)
    f32 damage      = 0.0f;
    f32 bonusHealth = 0.0f;

    // Unique instance ID for networking
    u32 uid = 0;
};

inline bool isItemEmpty(const ItemInstance& item) {
    return item.defId == 0xFFFF;
}

// Special defId values for globe drops (not real items — auto-pickup, instant effect)
static constexpr u16 GLOBE_HEALTH_ID = 0xFFFE;
static constexpr u16 GLOBE_ENERGY_ID = 0xFFFD;

inline bool isGlobe(const ItemInstance& item) {
    return item.defId == GLOBE_HEALTH_ID || item.defId == GLOBE_ENERGY_ID;
}

// ---- Rarity color lookup ----

inline Vec3 rarityColor(Rarity r) {
    switch (r) {
        case Rarity::COMMON:    return {0.9f, 0.9f, 0.9f};   // white
        case Rarity::MAGIC:     return {0.2f, 0.9f, 0.3f};   // green
        case Rarity::RARE:      return {0.3f, 0.5f, 1.0f};   // blue
        case Rarity::LEGENDARY: return {1.0f, 0.82f, 0.2f};  // gold
        default:                return {1.0f, 1.0f, 1.0f};
    }
}

// ---- Skill definition (template loaded from JSON) ----

struct SkillDef {
    char    name[32] = {};
    SkillId id       = SkillId::NONE;
    f32     cooldown = 1.0f;
    f32     energyCost = 0.0f;
    f32     damage   = 0.0f;
    f32     radius   = 0.0f;
    f32     duration = 0.0f;
    f32     projectileSpeed = 0.0f;
    u8      projectileCount = 0;

    // Frozen Orb specifics
    f32 shardDamage   = 0.0f;
    u8  shardCount    = 0;
    f32 shardInterval = 0.0f;
    f32 shardSpeed    = 0.0f;
    f32 shardRadius   = 0.0f;
    f32 angleStepDeg  = 0.0f;

    // Chain Lightning specifics
    u8  bounces      = 0;
    f32 bounceRange  = 0.0f;
    f32 damageFalloff = 0.0f;

    // Blood Nova specifics
    f32 healthCostPct = 0.0f;

    // Meteor Strike specifics
    f32 delay = 0.0f;

    // Phase Dash specifics
    f32 distance        = 0.0f;
    f32 corridorWidth   = 0.0f;
    f32 invulnDuration  = 0.0f;

    // Holy Nova specifics (dual-hit AoE)
    f32 secondaryDamage = 0.0f;
    f32 allyHealPct     = 0.0f;  // fraction of ally max HP healed per hit
};

// ---- Skill state per player ----

struct SkillState {
    SkillId activeSkill    = SkillId::NONE;
    f32     cooldownTimer  = 0.0f;
    f32     energy         = 100.0f;
    f32     maxEnergy      = 100.0f;
};

// ---- NPC equipment (simplified — no backpack, just 6 equipped slots) ----

struct NpcEquipment {
    ItemInstance equipped[static_cast<u32>(ItemSlot::COUNT)] = {};
    // Cached stat bonuses (same layout as PlayerInventory)
    f32 bonusDamageFlat         = 0.0f;
    f32 bonusDamagePct          = 0.0f;
    f32 bonusHealthFlat         = 0.0f;
    f32 bonusHealthPct          = 0.0f;
    f32 bonusMoveSpeed          = 0.0f;
    f32 bonusCooldownReduction  = 0.0f;
    f32 bonusLifeOnHit          = 0.0f;
    f32 bonusProjectileSpeedPct = 0.0f;
    f32 bonusConeAngle          = 0.0f;
    f32 bonusRange              = 0.0f;
    f32 bonusDamageToFlying     = 0.0f;
    bool active = false;
    u8   floorsSurvived = 0;  // how many floors this NPC has survived (for upgrades)
};

// ---- Player inventory ----

// Player equipment + backpack. Stat bonuses are cached and recalculated on equip/unequip.
// getEffectiveWeapon() merges equipped weapon's ItemDef + affixes into a WeaponDef.
struct PlayerInventory {
    ItemInstance equipped[static_cast<u32>(ItemSlot::COUNT)] = {};
    ItemInstance backpack[MAX_INVENTORY_ITEMS] = {};
    u8           backpackCount = 0;

    // Computed stat bonuses (recalculated on equip/unequip)
    f32 bonusDamageFlat         = 0.0f;
    f32 bonusDamagePct          = 0.0f;
    f32 bonusHealthFlat         = 0.0f;
    f32 bonusHealthPct          = 0.0f;
    f32 bonusMoveSpeed          = 0.0f;
    f32 bonusCooldownReduction  = 0.0f;  // 0.0 to 0.5 cap
    f32 bonusLifeOnHit          = 0.0f;
    f32 bonusProjectileSpeedPct = 0.0f;
    f32 bonusConeAngle          = 0.0f;
    f32 bonusRange              = 0.0f;
    f32 bonusDamageToFlying     = 0.0f;
    f32 bonusClipSizePct        = 0.0f;
    f32 bonusReloadSpeedPct     = 0.0f;
    f32 bonusEnergyFlat         = 0.0f;
};

// ---- World item (dropped loot on the ground) ----

// Dropped loot in the world. Bobs and spins, has exclusive ownership timer
// before becoming free-for-all pickup. Lifetime-limited (60s default).
struct WorldItem {
    ItemInstance item;
    Vec3         position      = {0, 0, 0};
    f32          bobTimer      = 0.0f;
    f32          lifetime      = 60.0f;
    bool         active        = false;
    u8           ownerSlot     = 0xFF;   // 0xFF = free for all, else exclusive to one player
    f32          exclusiveTimer = 3.0f;
};

struct WorldItemPool {
    WorldItem items[MAX_WORLD_ITEMS] = {};
    u32       activeCount = 0;
    u32       nextUid     = 1;           // server assigns unique IDs
};

// ---- Quickbar (8 assignable hotbar slots, keys 1-8) ----

static constexpr u32 QUICKBAR_SLOTS = 4;

struct QuickbarSlot {
    enum Type : u8 { EMPTY, BACKPACK_REF, EQUIPPED_REF };
    Type type = EMPTY;
    u8   sourceIndex = 0;  // backpack index or ItemSlot index
    u32  itemUid = 0;      // validates reference hasn't gone stale
};

struct QuickbarState {
    QuickbarSlot slots[QUICKBAR_SLOTS] = {};
    u8 activeSlot = 0;  // 0-7, selected via keys 1-8
};

// ---- Inventory drag-and-drop state ----

enum struct DragSource : u8 { NONE, BACKPACK, EQUIPMENT, QUICKBAR };

// Transient UI state tracking an active drag operation
struct InventoryDragState {
    DragSource source = DragSource::NONE;
    u8  sourceIndex = 0;       // backpack/equipment/quickbar slot index
    u32 itemUid = 0;           // UID of dragged item (stale detection)
    u16 itemDefId = 0xFFFF;    // cached defId for rendering drag icon
    s32 startX = 0, startY = 0; // mouse pos at drag start (HUD coords)
    bool dragging = false;     // true once mouse moves > 3px from start
};

// Tracks last click for double-click detection
struct DoubleClickState {
    f32 timer = 0.0f;          // seconds since last click
    u8  lastSlot = 0xFF;       // slot index of last click
    bool wasBackpack = false;  // true if last click was backpack (vs equipment)
};

inline bool isDragActive(const InventoryDragState& ds) {
    return ds.source != DragSource::NONE && ds.dragging;
}

// Skill ID string lookup (used by item and boss loaders)
#include <string>
SkillId skillIdFromString(const std::string& s);

// ---- System namespaces ----

namespace ItemLoader {
    // Load item, affix, and skill definitions from JSON
    bool loadItemDefs (const char* path, ItemDef*   defs, u32& count);
    bool loadAffixDefs(const char* path, AffixDef*  defs, u32& count);
    bool loadSkillDefs(const char* path, SkillDef*  defs, u32& count);
    // Resolve mesh/material name strings to runtime IDs. Call after mesh + material systems init.
    void resolveVisuals(ItemDef* defs, u32 count);
}

namespace ItemGen {
    void init(u32 seed);
    ItemInstance rollItem(u8 enemyLevel, const ItemDef* defs, u32 defCount,
                          const AffixDef* affixDefs, u32 affixDefCount);
    Rarity rollRarity(u8 enemyLevel);
    void   rollAffixes(ItemInstance& item, u8 itemLevel, ItemSlot slot,
                       const AffixDef* affixDefs, u32 affixDefCount,
                       WeaponType weaponType = WeaponType::MELEE);
}

namespace Inventory {
    // Optional callback fired after recalculateStats — lets the engine apply
    // derived bonuses (e.g. energy_flat → SkillState.maxEnergy) without coupling.
    using StatsChangedCallback = void(*)(PlayerInventory& inv);
    void setStatsChangedCallback(StatsChangedCallback cb);

    void init(PlayerInventory& inv);
    void recalculateStats(PlayerInventory& inv);
    // Recalculate cached stat bonuses for NPC equipment (same logic, different struct)
    void recalculateNpcStats(NpcEquipment& equip);
    bool addToBackpack(PlayerInventory& inv, const ItemInstance& item);
    void equip(PlayerInventory& inv, u8 backpackIndex, const ItemDef* itemDefs);
    bool unequip(PlayerInventory& inv, ItemSlot slot);
    ItemInstance dropFromBackpack(PlayerInventory& inv, u8 backpackIndex);
    // Remove item from an equipment slot and return it (clears slot, recalculates stats)
    ItemInstance dropFromEquipment(PlayerInventory& inv, ItemSlot slot);
    WeaponDef    getEffectiveWeapon(const PlayerInventory& inv,
                                    const ItemDef* itemDefs, const WeaponDef& baseWeapon);
    // Build a WeaponDef from a specific item instance + inventory bonuses
    WeaponDef    getWeaponFromItem(const PlayerInventory& inv,
                                   const ItemDef* itemDefs, const ItemInstance& item);
    f32          getEffectiveMaxHealth(const PlayerInventory& inv, f32 baseMaxHealth);
}

namespace WorldItemSystem {
    void init(WorldItemPool& pool);
    void update(WorldItemPool& pool, f32 dt);
    bool spawn(WorldItemPool& pool, const ItemInstance& item, Vec3 position,
               const LevelGrid* grid = nullptr);
    // Returns true if pickup succeeded, fills outItem
    bool tryPickup(WorldItemPool& pool, Vec3 playerPos, u8 playerSlot,
                   ItemInstance& outItem);
}

namespace Quickbar {
    void init(QuickbarState& qb, const PlayerInventory& inv);
    // Assign item from backpack to first free slot (slot 0 is reserved for weapon)
    void assignItem(QuickbarState& qb, const PlayerInventory& inv, u8 backpackIdx);
    // Remove item from a slot (slot 0 protected)
    void removeItem(QuickbarState& qb, u8 slotIdx);
    // Keep slot 0 in sync with equipped weapon
    void syncWeaponSlot(QuickbarState& qb, const PlayerInventory& inv);
    // Returns pointer to the ItemInstance for a slot, or nullptr if empty/stale
    const ItemInstance* resolveSlot(const QuickbarState& qb, const PlayerInventory& inv, u8 slot);
    // Assign an item from a drag source to a specific quickbar slot (deduplicates)
    void assignToSlot(QuickbarState& qb, const PlayerInventory& inv,
                      u8 targetSlot, DragSource source, u8 sourceIndex);
    // Swap two quickbar slots
    void swapSlots(QuickbarState& qb, u8 a, u8 b);
}
