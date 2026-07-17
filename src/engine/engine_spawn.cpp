// Engine floor-population helpers — see engine.h for class definition.
// All methods extracted from Engine::startGame to separate spawn concerns.
// Each method populates one category of floor content (enemies, boss, chests,
// decorations, NPCs) and must be called in the order startGame specifies.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
#include "audio/audio.h"
#include "platform/window.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "renderer/gl_context.h"
#include "renderer/renderer.h"
#include "renderer/debug_draw.h"
#include "renderer/hud.h"
#include "renderer/minimap.h"
#include "renderer/font.h"
#include "renderer/item_icons.h"
#include "renderer/material.h"
#include "renderer/obj_loader.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "world/level_loader.h"
#include "world/collision.h"
#include "world/combat_query.h"
#include "game/player.h"
#include "game/combat.h"
#include "game/enemy_ai.h"
#include "game/squad.h"
#include "game/limb_system.h"
#include "game/projectile.h"
#include "game/item.h"
#include "game/shrine.h"
#include "game/champion.h"  // champion affix roll + pack tunables
#include "game/floor_event.h"  // floor-event pick + loot-goblin tunables
#include "game/skill.h"
#include "game/inventory_ui.h"
#include "game/game_constants.h"
#include "net/net.h"
#include "net/server.h"
#include "net/client.h"
#include "net/snapshot.h"
#include "net/packet.h"
#include "core/log.h"
#include "core/math.h"
#include "core/frame_allocator.h"
#include "core/allocation_tracker.h"
#include "core/profiler.h"
#include "game/boss_def.h"
#include "game/enemy_def.h"
#include "game/boss_loader.h"

#include <glad/glad.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Enemy template tables — promoted from static locals in startGame so they
// are shared across the translation unit without changing their content or
// linkage semantics (file-scope static const is behavior-identical to
// block-scope static const for these read-only tables).
// ---------------------------------------------------------------------------

namespace {

struct EnemyTemplate {
    f32 health, moveSpeed, detRange, atkRange, atkCool, damage;
    Vec3 halfExtents;
    bool flying;
    u8 meshIdx;       // 0=skeleton, 1=bat, 2=spider, 3=human
    EnemyType etype;
    const char* matName;
    u8 onHitEffect;   // 0=none, 1=poison, 2=slow, 3=burn, 4=freeze
    f32 onHitDuration;
    f32 onHitDps;     // for poison/burn
};

// Tier 1 (floors 1-10): Dungeon — standard + zombie (Diablo 1) + imp (Barony)
static const EnemyTemplate kTier1[] = {
    {GameConst::SKELETON_HEALTH, GameConst::SKELETON_SPEED, GameConst::SKELETON_DET_RANGE,
     GameConst::SKELETON_ATK_RANGE, GameConst::SKELETON_ATK_COOL, GameConst::SKELETON_DAMAGE,
     {0.4f,0.9f,0.4f}, false, 0, EnemyType::SKELETON, "skeleton_skin", 0, 0, 0},
    {GameConst::BAT_HEALTH, GameConst::BAT_SPEED, GameConst::BAT_DET_RANGE,
     GameConst::BAT_ATK_RANGE, GameConst::BAT_ATK_COOL, GameConst::BAT_DAMAGE,
     {0.5f,0.4f,0.4f}, true,  1, EnemyType::BAT,      "bat_skin",      0, 0, 0},
    {GameConst::SPIDER_HEALTH, GameConst::SPIDER_SPEED, GameConst::SPIDER_DET_RANGE,
     GameConst::SPIDER_ATK_RANGE, GameConst::SPIDER_ATK_COOL, GameConst::SPIDER_DAMAGE,
     {0.65f,0.39f,0.65f}, false, 2, EnemyType::SPIDER,   "spider_skin",   0, 0, 0},
    // Zombie (Diablo 1) — slow, tanky, human mesh
    {70,  1.8f, 18, 3.0f, 1.2f, 13, {0.4f,0.9f,0.4f}, false, 3, EnemyType::SKELETON, "zombie_skin",  0, 0, 0},
    // Imp (Barony) — small fast flying ranged nuisance, fires weak projectiles
    {20,  7.0f, 24, 9.0f, 0.8f,  3, {0.3f,0.3f,0.3f}, true,  1, EnemyType::BAT,      "imp_skin",     0, 0, 0},
    // Gargoyle — stone ambush, starts dormant, high burst damage
    {50, 4.5f, 15, 3.0f, 0.8f, 18, {0.45f,0.9f,0.45f}, false, 4, EnemyType::SKELETON, "gargoyle_skin", 0, 0, 0},
};
// Tier 2 (floors 11-20): Catacombs — poison + ghoul (D2) + bone mage (Barony)
static const EnemyTemplate kTier2[] = {
    {60, 3.0f, 22, 3.5f, 1.0f, 12, {0.4f,0.9f,0.4f}, false, 0, EnemyType::SKELETON, "catacomb_skeleton", 1, 3.0f, 4.0f},
    {35, 6.5f, 22, 3.5f, 0.8f,  8, {0.5f,0.4f,0.4f}, true,  1, EnemyType::BAT,      "catacomb_bat",      1, 2.0f, 3.0f},
    {48, 4.2f, 20, 3.0f, 0.8f, 11, {0.65f,0.39f,0.65f}, false, 2, EnemyType::SPIDER,   "catacomb_spider",   1, 3.0f, 5.0f},
    // Ghoul (D2) — fast melee, high damage, lower HP
    {40, 4.5f, 22, 3.0f, 0.6f, 16, {0.4f,0.85f,0.4f}, false, 3, EnemyType::SKELETON, "ghoul_skin",       1, 2.0f, 3.0f},
    // Bone Mage (Barony) — ranged skeleton caster
    {35, 2.5f, 24, 11.f, 1.2f, 14, {0.4f,0.9f,0.4f},  false, 0, EnemyType::SKELETON, "bone_mage_skin",   1, 3.0f, 4.0f},
    // Necromancer — ranged caster, resurrects dead enemies
    {30, 2.0f, 20, 11.0f, 1.5f, 10, {0.4f,1.0f,0.4f}, false, 5, EnemyType::SKELETON, "necromancer_skin", 1, 2.0f, 3.0f},
};
// Tier 3 (floors 21-30): Caverns — slow + broodmother (Barony) + stalker (HGL)
static const EnemyTemplate kTier3[] = {
    {65, 3.2f, 24, 3.5f, 0.9f, 13, {0.4f,0.9f,0.4f}, false, 0, EnemyType::SKELETON, "cavern_skeleton", 2, 2.0f, 0},
    {38, 7.0f, 24, 3.5f, 0.7f,  9, {0.5f,0.4f,0.4f}, true,  1, EnemyType::BAT,      "cavern_bat",      2, 1.5f, 0},
    {52, 4.8f, 22, 3.0f, 0.7f, 12, {0.65f,0.39f,0.65f}, false, 2, EnemyType::SPIDER,   "cavern_spider",   2, 2.5f, 0},
    // Broodmother (Barony) — large slow spider, extra tanky
    {90, 2.5f, 20, 3.5f, 1.0f, 14, {0.91f,0.52f,0.91f}, false, 2, EnemyType::SPIDER,   "broodmother_skin", 2, 3.0f, 0},
    // Stalker (HGL) — fast, stealthy humanoid
    {45, 5.0f, 26, 3.0f, 0.5f, 11, {0.35f,0.85f,0.35f}, false, 3, EnemyType::SKELETON, "stalker_skin", 2, 2.0f, 0},
    // Sniper Imp — flying ranged, long range, slow fire, fast small projectiles
    {25, 4.0f, 30, 16.f, 2.0f, 18, {0.3f,0.3f,0.3f}, true, 1, EnemyType::BAT, "sniper_imp_skin", 2, 2.0f, 0},
    // Cavern Shaman — healer, heals injured allies
    {35, 2.5f, 18, 10.0f, 1.2f, 8, {0.45f,0.9f,0.45f}, false, 6, EnemyType::SKELETON, "cavern_shaman_skin", 2, 2.0f, 0},
    // Cavern Herald — aura buff, +10% speed/attack for nearby enemies
    {45, 3.0f, 18, 3.5f, 0.9f, 12, {0.4f,1.0f,0.4f}, false, 7, EnemyType::SKELETON, "cavern_herald_skin", 2, 2.5f, 0},
};
// Tier 4 (floors 31-40): Hellforge — burn + hellhound (D2) + demon (HGL)
static const EnemyTemplate kTier4[] = {
    {70, 3.5f, 24, 3.5f, 0.8f, 15, {0.4f,0.9f,0.4f}, false, 0, EnemyType::SKELETON, "hellforge_skeleton", 3, 2.5f, 6.0f},
    {40, 7.5f, 24, 3.5f, 0.6f, 10, {0.5f,0.4f,0.4f}, true,  1, EnemyType::BAT,      "hellforge_bat",      3, 2.0f, 5.0f},
    {58, 5.0f, 22, 3.0f, 0.6f, 14, {0.65f,0.39f,0.65f}, false, 2, EnemyType::SPIDER,   "hellforge_spider",   3, 2.5f, 7.0f},
    // Hellhound (D2) — fast charging beast, spider rig
    {50, 6.0f, 24, 3.5f, 0.5f, 16, {0.65f,0.455f,0.65f}, false, 2, EnemyType::SPIDER,   "hellhound_skin",    3, 2.0f, 8.0f},
    // Demon (HGL) — ranged fire caster, humanoid
    {55, 3.0f, 26, 13.f, 1.0f, 18, {0.45f,1.0f,0.45f}, false, 3, EnemyType::SKELETON, "demon_skin",        3, 3.0f, 6.0f},
    // Infernal Herald — burn aura, area denial
    {55, 3.0f, 20, 3.5f, 0.9f, 14, {0.4f,1.0f,0.4f}, false, 7, EnemyType::SKELETON, "infernal_herald_skin", 3, 2.5f, 6.0f},
};
// Tier 5 (floors 41-50): Void — freeze + shade (Barony) + void demon (HGL)
static const EnemyTemplate kTier5[] = {
    {80, 3.8f, 26, 3.5f, 0.7f, 16, {0.4f,0.9f,0.4f}, false, 0, EnemyType::SKELETON, "void_skeleton", 4, 1.5f, 0},
    {45, 8.0f, 26, 3.5f, 0.5f, 11, {0.5f,0.4f,0.4f}, true,  1, EnemyType::BAT,      "void_bat",      4, 1.0f, 0},
    {65, 5.5f, 24, 3.0f, 0.5f, 15, {0.65f,0.39f,0.65f}, false, 2, EnemyType::SPIDER,   "void_spider",   4, 1.5f, 0},
    // Shade (Barony) — fast phasing humanoid, semi-transparent
    {40, 5.5f, 28, 3.0f, 0.4f, 14, {0.35f,0.9f,0.35f}, false, 3, EnemyType::SKELETON, "shade_skin",      4, 2.0f, 0},
    // Void Demon (HGL) — heavy tanky skeleton, high damage
    {100, 2.5f, 24, 4.0f, 0.8f, 20, {0.5f,1.0f,0.5f}, false, 0, EnemyType::SKELETON, "void_demon_skin", 4, 2.0f, 0},
    // Void Necromancer — resurrects dead, freeze
    {40, 2.2f, 22, 12.0f, 1.2f, 12, {0.4f,1.0f,0.4f}, false, 5, EnemyType::SKELETON, "void_necromancer_skin", 4, 1.5f, 0},
    // Void Shaman — heals allies, freeze
    {45, 2.5f, 20, 11.0f, 1.0f, 10, {0.45f,0.9f,0.45f}, false, 6, EnemyType::SKELETON, "void_shaman_skin", 4, 1.5f, 0},
    // Void Herald — freeze aura
    {65, 2.8f, 22, 3.5f, 0.8f, 16, {0.4f,1.0f,0.4f}, false, 7, EnemyType::SKELETON, "void_herald_skin", 4, 2.0f, 0},
};

// Boss template table — promoted from a static local in startGame.
struct BossTemplate {
    u8 floor;
    const char* name;
    const char* speech;
    f32 baseHp, baseDmg, speed, atkRange, atkCooldown;
    Vec3 halfExtents;
    bool isMajor;       // major = bigger arena + iron maidens
    const char* meshName;   // reuse existing mesh
    const char* matName;    // tint material
    const char* weaponName; // weapon mesh (nullptr = none)
};

static constexpr u32 BOSS_COUNT = 10;
static const BossTemplate kBosses[BOSS_COUNT] = {
    // Mini-bosses (floors 5, 15, 25, 35, 45) — should shred NPCs in 1-2 hits
    //                                          HP   DMG  SPD  RNG  COOL  halfExtents
    {  5, "The Butcher",   "FRESH MEAT!",         800, 80, 3.0f, 3.5f, 0.4f, {0.8f,1.25f,0.8f}, false, "butcher",  "butcher_skin",      "cleaver"},
    { 15, "Lich Lord",     "Your soul is MINE!",  500, 30, 2.8f, 12.f, 0.8f, {0.5f,1.0f, 0.5f}, false, "skeleton", "boss_lich",         "staff"},
    { 25, "Spider Queen",  "*HISSSS*",            700, 30, 5.0f, 3.0f, 0.4f, {1.04f,0.65f, 1.04f}, false, "spider",   "boss_spider_queen", nullptr},
    { 35, "Demon Knight",  "Kneel before me!",    800, 25, 3.5f, 3.5f, 0.5f, {0.7f,1.2f, 0.7f}, false, "butcher",  "boss_demon_knight", "sword"},
    { 45, "Arch Mage",     "Feel the arcane!",    600, 20, 3.0f, 14.f, 0.4f, {0.5f,1.0f, 0.5f}, false, "skeleton", "boss_arch_mage",    "staff"},
    // Major bosses (floors 10, 20, 30, 40, 50) — devastating, need full player focus
    { 10, "Andariel",      "Die, insect!",       1000, 65, 4.0f, 3.5f, 0.4f, {0.7f,1.1f, 0.7f}, true,  "andariel", "boss_andariel",     nullptr},
    { 20, "Mephisto",      "You cannot stop me.",1200, 30, 2.5f, 14.f, 0.5f, {0.6f,1.1f, 0.6f}, true,  "skeleton", "boss_mephisto",     "staff"},
    { 30, "Baal",          "I am undefeated!",   1800, 30, 3.0f, 4.0f, 0.4f, {0.9f,1.3f, 0.9f}, true,  "butcher",  "boss_baal",         nullptr},
    { 40, "DiaBRO",        "NOT EVEN DEATH...",  1600, 30, 3.5f, 4.0f, 0.35f,{0.8f,1.3f, 0.8f}, true,  "butcher",  "boss_diabro",       "sword"},
    { 50, "Grim Reaper",   "YOUR TIME HAS COME.",2500, 30, 4.0f, 4.0f, 0.3f, {0.7f,1.4f, 0.7f}, true,  "skeleton", "boss_reaper",       "axe"},
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// tryMakeChampion — promote a just-spawned enemy into a champion pack leader.
//
// Champions exist because the enemy pool is tier-gated to 6-9 types per 10-floor band: the player
// fights the same handful of monsters for ten floors. An affixed leader turns that small pool into
// a fight the player has to read.
//
// Minions are cloned from the leader's stats BEFORE the champion buffs are applied — that is what
// lets one helper serve both the JSON and the fallback spawn path (EnemyDef and EnemyTemplate are
// different types; duplicating this per branch is how those two paths would rot apart).
// ---------------------------------------------------------------------------
bool Engine::tryMakeChampion(Entity& leader, u16 leaderIdx, const DungeonRoom& room, u32 effFloor)
{
    if (effFloor < Champion::MIN_FLOOR) return false;
    if (m_championPacksThisFloor >= Champion::MAX_PACKS_PER_FLOOR) return false;
    // Never promote something that isn't a plain hostile: bosses have their own encounter design,
    // ambushers are placed in doorways, and mimics masquerade as chests.
    if (leader.isBoss || (leader.flags & ENT_FRIENDLY)) return false;
    if (leader.enemyType == EnemyType::MIMIC || leader.enemyType == EnemyType::PROP) return false;
    if (leader.enemyRole & EnemyRole::AMBUSH) return false;

    if (static_cast<f32>(std::rand()) / static_cast<f32>(RAND_MAX) >= Champion::SPAWN_CHANCE)
        return false;

    // Entity budget: MAX_ENTITIES is 128 and is NOT being raised, so a pack must never crowd out the
    // floor's normal spawns. Bail before touching the leader if the whole pack won't fit.
    const u32 free = (m_entities.activeCount < MAX_ENTITIES)
                   ? (MAX_ENTITIES - m_entities.activeCount) : 0;
    if (free < Champion::ENTITY_HEADROOM) return false;

    u8 minions = static_cast<u8>(Champion::MIN_MINIONS +
        std::rand() % (Champion::MAX_MINIONS - Champion::MIN_MINIONS + 1));
    if (minions > free - 1) minions = static_cast<u8>(free - 1);   // leader already occupies a slot

    // Snapshot the base stats to clone minions from, BEFORE the leader is buffed.
    const f32       baseHealth   = leader.maxHealth;
    const f32       baseDamage   = leader.damage;
    const f32       baseSpeed    = leader.baseMoveSpeed;
    const Vec3      baseHalf     = leader.halfExtents;
    const bool      flying       = (leader.flags & ENT_FLYING) != 0;
    const u8        meshId       = leader.meshId;
    const u8        materialId   = leader.materialId;
    const EnemyType enemyType    = leader.enemyType;
    const u8        enemyRole    = leader.enemyRole;
    const u8        weaponMeshId = leader.weaponMeshId;
    const f32       detection    = leader.detectionRange;
    const f32       atkRange     = leader.attackRange;
    const f32       atkCooldown  = leader.baseAttackCooldown;
    const u16       level        = leader.level;

    // --- Promote the leader ---
    u32 rng = static_cast<u32>(std::rand());
    leader.champAffixes = Champion::rollAffixes(effFloor, minions > 0, rng);
    leader.champNameIdx = static_cast<u8>(std::rand() % Champion::NAME_COUNT);
    leader.flags       |= ENT_CHAMPION;

    leader.maxHealth  = baseHealth * Champion::HEALTH_MULT;
    leader.health     = leader.maxHealth;
    leader.damage     = baseDamage * Champion::DAMAGE_MULT;
    // halfExtents drives the model scale AND the hitbox AND is already replicated — so this one
    // assignment gives the guest the size tell for free.
    leader.halfExtents = baseHalf * Champion::SCALE_MULT;

    if (leader.champAffixes & ChampAffix::EXTRA_FAST) {
        // BOTH must move: the AURA herald role restores moveSpeed from baseMoveSpeed, so buffing
        // only moveSpeed would have the aura system silently stomp the affix back to normal.
        leader.moveSpeed     = baseSpeed * Champion::EXTRA_FAST_MULT;
        leader.baseMoveSpeed = leader.moveSpeed;
    }
    Collision::ensureNotInWall(leader.position, leader.halfExtents, m_level.grid);

    // --- Escort ---
    u8 spawned = 0;
    for (u8 i = 0; i < minions; i++) {
        f32 ang = (6.2831853f * static_cast<f32>(i)) / static_cast<f32>(minions);
        Vec3 pos = leader.position + Vec3{ cosf(ang) * 1.8f, 0.0f, sinf(ang) * 1.8f };

        EntityHandle mh = EntitySystem::spawn(m_entities, pos, baseHalf, flying,
                                              baseHealth * Champion::MINION_HEALTH_MULT,
                                              baseSpeed, detection, atkRange, atkCooldown,
                                              baseDamage * Champion::MINION_DAMAGE_MULT);
        Entity* m = handleGet(m_entities, mh);
        if (!m) break;   // pool exhausted — the leader simply escorts fewer

        m->meshId       = meshId;
        m->materialId   = materialId;
        m->enemyType    = enemyType;
        m->enemyRole    = enemyRole;
        m->weaponMeshId = weaponMeshId;
        m->level        = level;
        m->maxHealth    = m->health;
        m->baseMoveSpeed      = m->moveSpeed;
        m->baseAttackCooldown = m->attackCooldown;
        // Minions carry NO affixes (leader-only, Diablo 2 style) — but they ARE the pack, so the
        // link is what lets HEALTH_LINK split the leader's damage onto them.
        m->champAffixes   = 0;
        m->champLeaderIdx = leaderIdx;
        Collision::ensureNotInWall(m->position, m->halfExtents, m_level.grid);
        spawned++;
    }

    // If the pool starved us of every minion, HEALTH_LINK would advertise a power the champion does
    // not have (it splits damage onto minions that don't exist). Strip it rather than lie.
    if (spawned == 0) leader.champAffixes &= static_cast<u8>(~ChampAffix::HEALTH_LINK);

    m_championPacksThisFloor++;
    char cname[48];
    Champion::formatName(cname, sizeof(cname), leader.champNameIdx, leader.champAffixes);
    LOG_INFO("Champion: %s (mask 0x%02X, %u minions) in room at %.1f,%.1f",
             cname, leader.champAffixes, spawned,
             static_cast<f64>(room.x), static_cast<f64>(room.z));
    return true;
}

// ---------------------------------------------------------------------------
// spawnFloorEnemies — fill all rooms (except spawn + its neighbors) with
// tier-appropriate hostile entities. Reads tier to pick kTier* fallback table
// and collectTierDefs for the JSON path. Writes into m_entities and m_level.grid.
// ---------------------------------------------------------------------------
void Engine::spawnFloorEnemies(DungeonResult& dungeon, u8 tier)
{
    // Fresh floor, fresh pack budget.
    m_championPacksThisFloor = 0;

    // CAVERN floors are one huge open chamber: the player can SEE an enemy from 30-40 m while
    // its authored detectionRange (12-22 m) was tuned for corridor floors where walls hid
    // anything unaware. Watching monsters stand oblivious across the cave reads as "the enemies
    // are passive", so open-layout floors get a wider aggro bubble to match their sightlines.
    const f32 detectMult =
        (m_level.layoutStyle == LevelGen::LayoutStyle::CAVERN) ? 1.5f : 1.0f;

    // Collect JSON defs for this tier
    const EnemyDef* tierDefs[MAX_ENEMY_DEFS];
    u32 tierDefCount = collectTierDefs(m_enemyDefs, tier, tierDefs, MAX_ENEMY_DEFS);

    // Fallback: use kTier arrays if no JSON defs loaded
    const EnemyTemplate* fallbackTier = kTier1;
    u32 fallbackCount = sizeof(kTier1) / sizeof(kTier1[0]);
    if      (tier == 5) { fallbackTier = kTier5; fallbackCount = sizeof(kTier5) / sizeof(kTier5[0]); }
    else if (tier == 4) { fallbackTier = kTier4; fallbackCount = sizeof(kTier4) / sizeof(kTier4[0]); }
    else if (tier == 3) { fallbackTier = kTier3; fallbackCount = sizeof(kTier3) / sizeof(kTier3[0]); }
    else if (tier == 2) { fallbackTier = kTier2; fallbackCount = sizeof(kTier2) / sizeof(kTier2[0]); }

    bool useJsonDefs = (tierDefCount > 0);

    // Mesh lookup for fallback path only
    u8 meshLookup[] = {m_meshIdSkeleton, m_meshIdBat, m_meshIdSpider, m_meshIdHuman,
                       findMeshByName("gargoyle"), findMeshByName("necromancer"),
                       findMeshByName("shaman"), findMeshByName("herald")};

    // Skip the spawn room + its corridor-connected neighbours ("the room after"),
    // so the player never spawns next to monsters. adjacentRooms is now true corridor
    // connectivity (see LevelGen::generate), so this reliably covers the next room.
    // Rooms 2 hops out get a reduced count.
    const DungeonRoom& spawnRm = dungeon.rooms[dungeon.spawnRoomIdx];
    for (u32 r = 0; r < dungeon.roomCount; r++) {
        if (r == dungeon.spawnRoomIdx) continue;
        // Skip the room directly adjacent to spawn (no enemies there)
        bool adjacentToSpawn = false;
        for (u8 a = 0; a < spawnRm.adjacentCount; a++) {
            if (spawnRm.adjacentRooms[a] == r) { adjacentToSpawn = true; break; }
        }
        if (adjacentToSpawn) continue;

        // Check if this room is 2 hops from spawn (adjacent to spawn's neighbor)
        bool twoHopsFromSpawn = false;
        for (u8 a = 0; a < spawnRm.adjacentCount; a++) {
            u32 nb = spawnRm.adjacentRooms[a];
            if (nb >= dungeon.roomCount) continue;
            const DungeonRoom& nbRoom = dungeon.rooms[nb];
            for (u8 b = 0; b < nbRoom.adjacentCount; b++) {
                if (nbRoom.adjacentRooms[b] == r) { twoHopsFromSpawn = true; break; }
            }
            if (twoHopsFromSpawn) break;
        }

        const DungeonRoom& room = dungeon.rooms[r];

        u32 area = room.w * room.d;
        u32 enemyCount;
        if (m_level.currentFloor <= 10) {
            enemyCount = 1 + (area / 25);
            if (enemyCount > 3) enemyCount = 3;
        } else {
            enemyCount = 1 + (area / 15);
            if (enemyCount > 5) enemyCount = 5;
        }

        // Halve enemy count in rooms 2 hops from spawn
        if (twoHopsFromSpawn && enemyCount > 1) enemyCount = (enemyCount + 1) / 2;

#ifdef __SWITCH__
        // Switch: cap at 3/room for all floors to keep entity count under ~48
        if (enemyCount > 3) enemyCount = 3;
#endif

        for (u32 e = 0; e < enemyCount; e++) {

            if (useJsonDefs) {
                // --- JSON path: spawn from EnemyDef ---
                u32 typeIdx = static_cast<u32>(std::rand()) % tierDefCount;
                const EnemyDef& def = *tierDefs[typeIdx];

                f32 ex = (room.x + 1 + static_cast<u32>(std::rand()) % (room.w > 2 ? room.w - 2 : 1) + 0.5f) * m_level.grid.cellSize;
                f32 ez = (room.z + 1 + static_cast<u32>(std::rand()) % (room.d > 2 ? room.d - 2 : 1) + 0.5f) * m_level.grid.cellSize;
                f32 spawnY = def.flying ? (room.floorHeight + 1.5f) : (room.floorHeight + def.halfExtents.y);

                Vec3 spawnPos = {ex, spawnY, ez};
                u32 spGx, spGz;
                if (LevelGridSystem::worldToGrid(m_level.grid, spawnPos, spGx, spGz) &&
                    LevelGridSystem::isSolid(m_level.grid, spGx, spGz)) {
                    ex = (room.x + room.w * 0.5f) * m_level.grid.cellSize;
                    ez = (room.z + room.d * 0.5f) * m_level.grid.cellSize;
                    spawnPos = {ex, spawnY, ez};
                }

                EntityHandle h = EntitySystem::spawn(m_entities,
                    spawnPos, def.halfExtents, def.flying,
                    def.health, def.moveSpeed, def.detectionRange * detectMult,
                    def.attackRange, def.attackCooldown, def.damage);
                Entity* ent = handleGet(m_entities, h);
                if (ent) {
                    ent->meshId     = def.meshId;
                    ent->materialId = def.materialId;
                    ent->enemyType  = def.enemyType;
                    ent->enemyRole  = def.role;
                    // Authored combat opener (strafe/flank/surround/...) — consumed by the
                    // IDLE-aggro and damage-wake transitions. This field spent months parsed
                    // and discarded (the spawn block below only ever read role & AMBUSH), which
                    // flattened every roster into charge-only — worst on tier 3, where 7 of 8
                    // defs author a non-chase opener.
                    ent->aiPreference = def.aiPreference;
                    // WHICH monster this is, not just which rig it wears. Replicated, so a guest can
                    // name it too. Derived from the pointer's offset into the table — tierDefs holds
                    // pointers INTO m_enemyDefs.defs, so this is its real index.
                    const ptrdiff_t defSlot = &def - m_enemyDefs.defs;
                    ent->enemyDefIdx = (defSlot >= 0 && defSlot < static_cast<ptrdiff_t>(m_enemyDefs.count))
                                     ? static_cast<u8>(defSlot) : 0xFF;

                    ent->baseMoveSpeed      = ent->moveSpeed;
                    ent->baseAttackCooldown = ent->attackCooldown;

                    // AMBUSH-role enemies start DORMANT at a doorway. (The authored
                    // aiPreference is the COMBAT OPENER, applied at the aggro transition in
                    // enemy_ai_states.cpp — not the initial state; a "chase" opener set here
                    // would beeline across the whole floor from spawn.)
                    if (def.role & EnemyRole::AMBUSH) {
                        ent->aiState = AIState::DORMANT;
                        // Burrowers (Burrowing Widow) wait genuinely UNDERGROUND: hidden,
                        // unhittable, non-blocking, until proximity makes them erupt.
                        if (def.burrower) ent->flags |= ENT_BURROWED;
                        // Reposition ambush enemies to doorways — a statue flanking an
                        // archway. They stay DORMANT there: the old AIState::AMBUSH hold
                        // rotated its yaw to track the player (a statue that watches you
                        // is visibly alive) and woke on plain proximity; the weeping-angel
                        // DORMANT rule (invulnerable stone, wakes only unobserved) is the
                        // gargoyle's whole gimmick now, doorway or not.
                        Vec3 doorPos[4];
                        u8 doorCount = LevelGridQuery::findDoorwayCells(
                            m_level.grid, room.x, room.z, room.w, room.d, doorPos, 4);
                        if (doorCount > 0) {
                            u8 pick = static_cast<u8>(std::rand() % doorCount);
                            f32 doorY = def.flying
                                ? (room.floorHeight + 1.5f)
                                : (room.floorHeight + def.halfExtents.y);
                            ent->position = {doorPos[pick].x, doorY, doorPos[pick].z};
                        }
                    }
                    if (def.role & EnemyRole::SUMMONER) ent->tacticalTimer = 8.0f;
                    if (def.role & EnemyRole::HEALER)   ent->tacticalTimer = 5.0f;

                    // Floor scaling
                    u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
                    ent->level = static_cast<u16>(effectiveFloor);
                    // HP compounds with the effective floor (Nightmare/Hell ramp
                    // exponentially); damage stays on the linear floor curve plus a flat
                    // per-difficulty bump — compounding damage would one-shot the player.
                    f32 hpMult  = GameConst::floorHealthMult(effectiveFloor);
                    f32 dmgMult = GameConst::floorDamageMult(effectiveFloor)
                                  * GameConst::difficultyDamageBump(m_difficulty);
                    ent->health    *= hpMult;
                    ent->maxHealth  = ent->health;
                    ent->damage    *= dmgMult;

                    ent->onHitEffect   = def.onHitEffect;
                    ent->onHitDuration = def.onHitDuration;
                    ent->onHitDps      = def.onHitDps * dmgMult;

                    // Weapon assignment for skeleton-rig enemies
                    if (ent->enemyType == EnemyType::SKELETON) {
                        if (def.role & EnemyRole::SHIELD_BEARER) {
                            // Shield bearers hold a shield instead of a weapon
                            ent->weaponMeshId = findMeshByName("shield");
                        } else if (ent->attackRange > 5.0f) {
                            ent->weaponMeshId = m_meshIdStaff;
                        } else {
                            u8 weapMeshes[] = {m_meshIdSword, m_meshIdDagger, m_meshIdAxe};
                            ent->weaponMeshId = weapMeshes[static_cast<u32>(std::rand()) % 3];
                        }
                    }
                    // Validate spawn position — nudge out of walls if AABB overlaps
                    Collision::ensureNotInWall(ent->position, ent->halfExtents, m_level.grid);

                    // Champion promotion (both spawn paths call the same helper — see the fallback
                    // branch below — so the two can't drift apart).
                    tryMakeChampion(*ent, h.index, room, effectiveFloor);
                }

            } else {
                // --- Fallback path: spawn from kTier inline arrays ---
                u32 typeIdx = static_cast<u32>(std::rand()) % fallbackCount;
                const EnemyTemplate& tmpl = fallbackTier[typeIdx];

                f32 ex = (room.x + 1 + static_cast<u32>(std::rand()) % (room.w > 2 ? room.w - 2 : 1) + 0.5f) * m_level.grid.cellSize;
                f32 ez = (room.z + 1 + static_cast<u32>(std::rand()) % (room.d > 2 ? room.d - 2 : 1) + 0.5f) * m_level.grid.cellSize;
                f32 spawnY = tmpl.flying ? (room.floorHeight + 1.5f) : (room.floorHeight + tmpl.halfExtents.y);

                Vec3 spawnPos = {ex, spawnY, ez};
                u32 spGx, spGz;
                if (LevelGridSystem::worldToGrid(m_level.grid, spawnPos, spGx, spGz) &&
                    LevelGridSystem::isSolid(m_level.grid, spGx, spGz)) {
                    ex = (room.x + room.w * 0.5f) * m_level.grid.cellSize;
                    ez = (room.z + room.d * 0.5f) * m_level.grid.cellSize;
                    spawnPos = {ex, spawnY, ez};
                }

                EntityHandle h = EntitySystem::spawn(m_entities,
                    spawnPos, tmpl.halfExtents, tmpl.flying,
                    tmpl.health, tmpl.moveSpeed, tmpl.detRange * detectMult,
                    tmpl.atkRange, tmpl.atkCool, tmpl.damage);
                Entity* ent = handleGet(m_entities, h);
                if (ent) {
                    ent->meshId = meshLookup[tmpl.meshIdx];
                    ent->materialId = MaterialSystem::getIdByName(tmpl.matName);
                    ent->baseMoveSpeed      = ent->moveSpeed;
                    ent->baseAttackCooldown = ent->attackCooldown;

                    if (std::strstr(tmpl.matName, "gargoyle")) {
                        ent->enemyRole = EnemyRole::AMBUSH;
                        // Stays DORMANT even at a doorway — same reasoning as the JSON
                        // path above: the stone-statue disguise (invulnerable, wakes only
                        // unobserved) replaces the old proximity-triggered AMBUSH hold.
                        ent->aiState = AIState::DORMANT;
                        Vec3 doorPos[4];
                        u8 doorCount = LevelGridQuery::findDoorwayCells(
                            m_level.grid, room.x, room.z, room.w, room.d, doorPos, 4);
                        if (doorCount > 0) {
                            u8 pick = static_cast<u8>(std::rand() % doorCount);
                            ent->position = {doorPos[pick].x,
                                tmpl.flying ? (room.floorHeight + 1.5f) : (room.floorHeight + tmpl.halfExtents.y),
                                doorPos[pick].z};
                        }
                    } else if (std::strstr(tmpl.matName, "necromancer")) {
                        ent->enemyRole = EnemyRole::SUMMONER;
                        ent->tacticalTimer = 8.0f;
                    } else if (std::strstr(tmpl.matName, "shaman")) {
                        ent->enemyRole = EnemyRole::HEALER;
                        ent->tacticalTimer = 5.0f;
                    } else if (std::strstr(tmpl.matName, "herald")) {
                        ent->enemyRole = EnemyRole::AURA;
                    }
                    ent->enemyType = tmpl.etype;

                    u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
                    ent->level = static_cast<u16>(effectiveFloor);
                    // HP compounds; damage linear + per-difficulty bump (see other spawn path).
                    f32 hpMult  = GameConst::floorHealthMult(effectiveFloor);
                    f32 dmgMult = GameConst::floorDamageMult(effectiveFloor)
                                  * GameConst::difficultyDamageBump(m_difficulty);
                    ent->health    *= hpMult;
                    ent->maxHealth  = ent->health;
                    ent->damage    *= dmgMult;
                    ent->onHitEffect   = tmpl.onHitEffect;
                    ent->onHitDuration = tmpl.onHitDuration;
                    ent->onHitDps      = tmpl.onHitDps * dmgMult;

                    if (ent->enemyType == EnemyType::SKELETON) {
                        if (ent->attackRange > 5.0f) {
                            ent->weaponMeshId = m_meshIdStaff;
                        } else {
                            u8 weapMeshes[] = {m_meshIdSword, m_meshIdDagger, m_meshIdAxe};
                            ent->weaponMeshId = weapMeshes[static_cast<u32>(std::rand()) % 3];
                        }
                    }
                    // Validate spawn position — nudge out of walls if AABB overlaps
                    Collision::ensureNotInWall(ent->position, ent->halfExtents, m_level.grid);

                    // Champion promotion — same helper as the JSON path above, deliberately.
                    tryMakeChampion(*ent, h.index, room, effectiveFloor);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// initBossEntity — apply a BossDef's visuals/role/phase/on-hit fields to an already-spawned entity.
// Factored out of spawnFloorBoss so the milestone bosses, the Engine superboss, and the Engine's
// recompiled wave-adds all build from one code path. HP is set at EntitySystem::spawn time and the
// leash is owned by the caller's arena, so neither is touched here. `def` MUST be an element of
// m_bossDefs.defs — bossDefIdx is recovered from its address.
// ---------------------------------------------------------------------------
void Engine::initBossEntity(Entity& e, const BossDef& def, u32 effFloor)
{
    // boss DoT scales with floor/difficulty like its hit damage (matches the old inline path)
    f32 dmgMult = GameConst::floorDamageMult(effFloor) * GameConst::difficultyDamageBump(m_difficulty);

    e.isBoss     = true;
    e.meshId     = findMeshByName(def.meshName);
    e.materialId = MaterialSystem::getIdByName(def.matName);
    e.enemyType  = EnemyType::BOSS;
    e.nameTag    = def.name;
    e.level      = static_cast<u16>(effFloor);
    e.bossLimbConfig = def.limbConfig;
    if (def.weaponName[0] != '\0') e.weaponMeshId = findMeshByName(def.weaponName);
    e.speechText  = def.speech;
    e.speechTimer = 6.0f;

    // Recover the def's index in the table from its address (valid because def is a table element).
    e.bossDefIdx    = static_cast<u8>(&def - m_bossDefs.defs);
    e.enemyRole     = def.roles;
    e.minionShield  = def.minionShield;
    e.bossPhase     = def.secondPhase ? BossPhase::ARMED : BossPhase::NONE;
    e.onHitEffect   = def.onHitEffect;
    e.onHitDuration = def.onHitDuration;
    e.onHitDps      = def.onHitDps * dmgMult;
    e.baseMoveSpeed      = e.moveSpeed;
    e.baseAttackCooldown = e.attackCooldown;

    // Ranged bosses use projectile attacks — weapon mesh rides the projectile
    if (def.atkRange > 5.0f) {
        e.npcWeaponType = WeaponType::PROJECTILE;
        e.npcProjectileSpeed = 18.0f;
        e.npcProjectileRadius = 0.15f;
    }
}

// ---------------------------------------------------------------------------
// spawnFloorBoss — place the milestone boss (if one exists for this floor).
// Mutates dungeon.rooms[bossRoomIdx] to expand the arena, writes m_level.grid
// cells to blood materials, and rebuilds the level mesh (which the exit-portal
// placement after this call depends on). Returns the boss room index used for
// exit-portal placement, or 0xFFFFFFFF if no boss was spawned.
// ---------------------------------------------------------------------------
u32 Engine::spawnFloorBoss(DungeonResult& dungeon)
{
    u32 bossRoomForExit = 0xFFFFFFFF; // set below, consumed by exit-portal placement in startGame

    // Find boss for this floor — prefer JSON-loaded BossDef, fall back to hardcoded
    u8 bossIdx = findBossDefIdx(m_bossDefs, static_cast<u8>(m_level.currentFloor));
    const BossDef* bd = (bossIdx != 0xFF) ? &m_bossDefs.defs[bossIdx] : nullptr;

    // Fallback: if no JSON boss def, check the hardcoded table
    const BossTemplate* bt = nullptr;
    if (!bd) {
        for (u32 b = 0; b < BOSS_COUNT; b++) {
            if (kBosses[b].floor == m_level.currentFloor) { bt = &kBosses[b]; break; }
        }
    }

    if ((bd || bt) && dungeon.roomCount > 2) {
        // Boss room: pick the exit room or a neighbor of it, but NEVER the
        // spawn room or a room adjacent to spawn.
        const DungeonRoom& spawnRm2 = dungeon.rooms[dungeon.spawnRoomIdx];

        auto isNearSpawn = [&](u32 idx) -> bool {
            if (idx == dungeon.spawnRoomIdx) return true;
            for (u8 a = 0; a < spawnRm2.adjacentCount; a++)
                if (spawnRm2.adjacentRooms[a] == idx) return true;
            return false;
        };

        // First choice: exit room (if not near spawn)
        // Second choice: a neighbor of exit that's not near spawn
        // Last resort: any room that's not near spawn
        u32 bossRoomIdx = 0xFFFFFFFF;
        if (!isNearSpawn(dungeon.exitRoomIdx)) {
            bossRoomIdx = dungeon.exitRoomIdx;
        } else {
            const DungeonRoom& exitRoom = dungeon.rooms[dungeon.exitRoomIdx];
            for (u8 a = 0; a < exitRoom.adjacentCount; a++) {
                u32 nb = exitRoom.adjacentRooms[a];
                if (nb < dungeon.roomCount && !isNearSpawn(nb)) {
                    bossRoomIdx = nb;
                    break;
                }
            }
        }
        // Last resort: pick any room far from spawn
        if (bossRoomIdx == 0xFFFFFFFF) {
            for (u32 i = dungeon.roomCount; i > 0; i--) {
                if (!isNearSpawn(i - 1)) { bossRoomIdx = i - 1; break; }
            }
        }
        if (bossRoomIdx == 0xFFFFFFFF) bossRoomIdx = dungeon.exitRoomIdx; // absolute fallback
        bossRoomForExit = bossRoomIdx;

        DungeonRoom& bossRoom = dungeon.rooms[bossRoomIdx];

        // Arena size: major bosses get a larger arena
        bool isMajor = bd ? bd->isMajor : bt->isMajor;
        u32 arenaScale = isMajor ? 4 : 3;
        u32 expandW = bossRoom.w * arenaScale;
        u32 expandD = bossRoom.d * arenaScale;
        s32 startX = static_cast<s32>(bossRoom.x) - static_cast<s32>(bossRoom.w * (arenaScale / 2));
        s32 startZ = static_cast<s32>(bossRoom.z) - static_cast<s32>(bossRoom.d * (arenaScale / 2));
        u8 bloodFloor = MaterialSystem::getIdByName("blood_floor");
        u8 bloodWall  = MaterialSystem::getIdByName("blood_wall");
        u8 bloodCeil  = MaterialSystem::getIdByName("blood_ceiling");
        u8 floorQH = static_cast<u8>(bossRoom.floorHeight / 0.25f);

        for (u32 ez = 0; ez < expandD; ez++) {
            for (u32 ex = 0; ex < expandW; ex++) {
                s32 gx = startX + static_cast<s32>(ex);
                s32 gz = startZ + static_cast<s32>(ez);
                if (gx < 1 || gz < 1 || static_cast<u32>(gx) >= m_level.grid.width - 1 ||
                    static_cast<u32>(gz) >= m_level.grid.depth - 1) continue;
                GridCell& cell = LevelGridSystem::getCell(m_level.grid, static_cast<u32>(gx), static_cast<u32>(gz));
                cell.flags = CELL_FLOOR | CELL_CEILING;
                cell.floorHeight = floorQH;
                cell.ceilingHeight = 16;
                cell.floorMaterialId = bloodFloor;
                cell.wallMaterialId = bloodWall;
                cell.ceilMaterialId = bloodCeil;
            }
        }

        // Expand the DungeonRoom record to match the new arena geometry — the
        // exit-portal placement in startGame reads bossRoom.{x,z,w,d} after this call.
        bossRoom.x = (startX > 0) ? static_cast<u32>(startX) : 1;
        bossRoom.z = (startZ > 0) ? static_cast<u32>(startZ) : 1;
        bossRoom.w = expandW;
        bossRoom.d = expandD;

        // Rebuild mesh so the arena geometry is visible immediately.
        // The exit portal and flow-field build that follows depend on this grid state.
        // Same per-floor mesh seed as startGame so the rebuilt arena keeps a matching look.
        m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid,
                                 m_level.levelSeed + m_level.currentFloor * 7919u,
                                 m_level.sections, MAX_LEVEL_SECTIONS);

        // Iron maidens in corners for major bosses
        if (isMajor && m_meshIdIronMaiden > 0) {
            Vec3 corners[] = {
                {(bossRoom.x + 2) * m_level.grid.cellSize, bossRoom.floorHeight, (bossRoom.z + 2) * m_level.grid.cellSize},
                {(bossRoom.x + bossRoom.w - 2) * m_level.grid.cellSize, bossRoom.floorHeight, (bossRoom.z + 2) * m_level.grid.cellSize},
                {(bossRoom.x + 2) * m_level.grid.cellSize, bossRoom.floorHeight, (bossRoom.z + bossRoom.d - 2) * m_level.grid.cellSize},
                {(bossRoom.x + bossRoom.w - 2) * m_level.grid.cellSize, bossRoom.floorHeight, (bossRoom.z + bossRoom.d - 2) * m_level.grid.cellSize},
            };
            for (u32 c = 0; c < 4; c++) {
                EntityHandle ih = EntitySystem::spawn(m_entities,
                    corners[c] + Vec3{0, 0.45f, 0}, {0.2f, 0.45f, 0.15f}, false,
                    9999.0f, 0.0f, 0.0f, 0.0f, 999.0f, 0.0f);
                Entity* prop = handleGet(m_entities, ih);
                if (prop) {
                    prop->meshId = m_meshIdIronMaiden;
                    prop->materialId = bloodWall;
                    prop->enemyType = EnemyType::PROP;
                }
            }
        }

        // Spawn the boss in the center of the arena
        f32 bx = (bossRoom.x + bossRoom.w * 0.5f) * m_level.grid.cellSize;
        f32 bz = (bossRoom.z + bossRoom.d * 0.5f) * m_level.grid.cellSize;
        f32 by = bossRoom.floorHeight;
        // Bosses previously scaled off the RAW floor with no difficulty term, so a Hell boss
        // had the same HP/damage as a Normal boss (only ability keying changed via level).
        // Use the effective floor so bosses ramp with difficulty like every other enemy:
        // HP compounds, damage is linear + the per-tier bump. (bossEffFloor is reused below
        // for boss->level / ability keying.)
        u32 bossEffFloor = m_level.currentFloor + m_difficulty * 50;
        f32 bossHpMult   = GameConst::floorHealthMult(bossEffFloor);
        f32 bossDmgMult  = GameConst::floorDamageMult(bossEffFloor)
                           * GameConst::difficultyDamageBump(m_difficulty);

        // Extract stats from whichever source is active (JSON bd or hardcoded bt)
        Vec3 bossHalf    = bd ? bd->halfExtents  : bt->halfExtents;
        f32  bossHp      = bd ? bd->baseHp       : bt->baseHp;
        f32  bossSpd     = bd ? bd->speed         : bt->speed;
        f32  bossDetect  = bd ? bd->detectionRange : 40.0f;
        f32  bossAtkRng  = bd ? bd->atkRange      : bt->atkRange;
        f32  bossAtkCool = bd ? bd->atkCooldown   : bt->atkCooldown;
        f32  bossDmg     = bd ? bd->baseDmg       : bt->baseDmg;

        EntityHandle bh = EntitySystem::spawn(m_entities,
            Vec3{bx, by + bossHalf.y, bz}, bossHalf, false,
            bossHp * bossHpMult, bossSpd, bossDetect,
            bossAtkRng, bossAtkCool, bossDmg * bossDmgMult);
        Entity* boss = handleGet(m_entities, bh);
        if (boss) {
            // Boss-room leash: confine the boss to its arena and only wake it when
            // the player enters. Radius = half the arena diagonal (+1m margin) so he
            // covers the whole room and engages at the doorway. homePosition (set at
            // spawn) is the arena centre. Without this, bosses chase the player out
            // into the corridors and the boss-room encounter never happens.
            {
                f32 arenaW = bossRoom.w * m_level.grid.cellSize;
                f32 arenaD = bossRoom.d * m_level.grid.cellSize;
                boss->leashRadius = 0.5f * sqrtf(arenaW * arenaW + arenaD * arenaD) + 1.0f;
            }
            // CHARGER bosses (the Butcher) pursue relentlessly: drop the arena
            // leash so they chase the player OUT of the room and through the floor
            // instead of being clamped to the arena centre. Other bosses stay
            // confined so their set-piece encounter happens in the room.
            if (bd && (bd->roles & EnemyRole::CHARGER)) boss->leashRadius = 0.0f;
            boss->isBoss = true; // canonical milestone-boss marker (gates the floor exit)
            if (bd) {
                // JSON-loaded boss path — all the def-driven fields live in initBossEntity now so
                // spawnSourceBoss and the Engine's wave-summons build identical boss entities.
                initBossEntity(*boss, *bd, bossEffFloor);
            } else {
                // Hardcoded fallback path (kBosses)
                boss->meshId = findMeshByName(bt->meshName);
                boss->materialId = MaterialSystem::getIdByName(bt->matName);
                boss->enemyType = EnemyType::BOSS;
                boss->nameTag = bt->name;
                boss->level = static_cast<u16>(bossEffFloor);  // bossEffFloor computed above
                // Hardcoded limb config per floor
                if (bt->floor == 10) boss->bossLimbConfig = 1; // Andariel: spider legs
                if (bt->floor == 20) boss->bossLimbConfig = 2; // Mephisto: tentacles
                if (bt->floor == 40) boss->bossLimbConfig = 3; // DiaBRO: back spikes
                if (bt->floor == 50) boss->bossLimbConfig = 4; // Reaper: blade arms
                if (bt->weaponName) {
                    boss->weaponMeshId = findMeshByName(bt->weaponName);
                }
                boss->speechText = bt->speech;
                boss->speechTimer = 6.0f;
                boss->baseMoveSpeed      = boss->moveSpeed;
                boss->baseAttackCooldown = boss->attackCooldown;

                if (bt->atkRange > 5.0f) {
                    boss->npcWeaponType = WeaponType::PROJECTILE;
                    boss->npcProjectileSpeed = 18.0f;
                    boss->npcProjectileRadius = 0.15f;
                }
            }
        }
        if (boss) Collision::ensureNotInWall(boss->position, boss->halfExtents, m_level.grid);
        const char* bossName = bd ? bd->name : bt->name;
        LOG_INFO("Spawned boss '%s' on floor %u (%.0f HP, %.0f DMG, arena %ux%u, limbConfig=%u, src=%s)",
                 bossName, m_level.currentFloor, bossHp * bossHpMult,
                 bossDmg * bossDmgMult, expandW, expandD,
                 boss ? boss->bossLimbConfig : 0,
                 bd ? "json" : "hardcoded");
    }

    return bossRoomForExit;
}

// ---------------------------------------------------------------------------
// spawnSourceBoss — spawn The Dungeon Engine superboss at the centre of The Source.
// Off the numbered ladder (floor-99 BossDef), so it can't go through spawnFloorBoss (which keys
// on currentFloor). Scales like every boss off the effective floor (50 + difficulty*50), so the
// Engine is tougher the deeper into the curse you reached it. Marked isEngine for the bespoke
// "immune while any summoned wave-boss lives" rule and its dedicated AI dispatch.
// Returns the Engine's entity pool index (0xFFFF on failure).
// ---------------------------------------------------------------------------
u16 Engine::spawnSourceBoss(Vec3 center)
{
    u8 idx = findBossDefIdx(m_bossDefs, 99);
    if (idx == 0xFF) { LOG_WARN("spawnSourceBoss: no floor-99 Engine boss def loaded"); return 0xFFFF; }
    const BossDef& def = m_bossDefs.defs[idx];

    u32 effFloor = 50u + static_cast<u32>(m_difficulty) * 50u;       // currentFloor is 50 in The Source
    f32 hpMult   = GameConst::floorHealthMult(effFloor);
    f32 dmgMult  = GameConst::floorDamageMult(effFloor) * GameConst::difficultyDamageBump(m_difficulty);

    EntityHandle h = EntitySystem::spawn(m_entities,
        Vec3{center.x, def.halfExtents.y, center.z}, def.halfExtents, false,
        def.baseHp * hpMult, def.speed, def.detectionRange,
        def.atkRange, def.atkCooldown, def.baseDmg * dmgMult);
    Entity* e = handleGet(m_entities, h);
    if (!e) return 0xFFFF;

    initBossEntity(*e, def, effFloor);          // visuals/role/level/bossDefIdx (+ isBoss)
    e->isEngine    = true;                       // bespoke immunity + AI dispatch marker
    e->bossPhase   = BossPhase::ENG_OPEN;        // start the shielded-wave ladder (override NONE)
    e->homePosition = Vec3{center.x, 0.0f, center.z};
    e->leashRadius = 0.0f;                        // immobile turret — never leashes/chases
    e->aiState     = AIState::CHASE;             // active immediately (no LOS warm-up needed)
    Collision::ensureNotInWall(e->position, e->halfExtents, m_level.grid);

    LOG_INFO("Spawned The Dungeon Engine at (%.1f,%.1f) — effFloor %u, %.0f HP",
             center.x, center.z, effFloor, def.baseHp * hpMult);
    return h.index;
}

// ---------------------------------------------------------------------------
// spawnFloorChests — place chests (and mimic impostors) in dungeon rooms.
// ---------------------------------------------------------------------------
void Engine::spawnFloorChests(const DungeonResult& dungeon)
{
    u8 chestMeshId = m_meshIdChest;

    // Spawn-proximity test (mirrors the boss placement): the spawn room and its
    // corridor-connected neighbours must stay monster-free, so mimics are barred there.
    const DungeonRoom& spawnRm = dungeon.rooms[dungeon.spawnRoomIdx];
    auto isNearSpawn = [&](u32 idx) -> bool {
        if (idx == dungeon.spawnRoomIdx) return true;
        for (u8 a = 0; a < spawnRm.adjacentCount; a++)
            if (spawnRm.adjacentRooms[a] == idx) return true;
        return false;
    };

    for (u32 r = 1; r < dungeon.roomCount; r++) {
        if ((std::rand() % 2) != 0) continue; // 50% of rooms get a chest
        const DungeonRoom& room = dungeon.rooms[r];

        // Place chests against inner wall face, 1 cell in from boundary
        // so they don't block corridor entrances
        if (room.w < 4 || room.d < 4) continue; // skip tiny rooms

        f32 cs = m_level.grid.cellSize;
        f32 cy = room.floorHeight;
        f32 cx, cz;
        u32 chestWall = std::rand() % 4;
        if (chestWall < 2) {
            cx = (room.x + 2 + std::rand() % (room.w > 4 ? room.w - 4 : 1)) * cs;
            cz = (chestWall == 0) ? (room.z + 1) * cs + 0.35f
                                   : (room.z + room.d - 2) * cs + cs - 0.35f;
        } else {
            cz = (room.z + 2 + std::rand() % (room.d > 4 ? room.d - 4 : 1)) * cs;
            cx = (chestWall == 2) ? (room.x + 1) * cs + 0.35f
                                   : (room.x + room.w - 2) * cs + cs - 0.35f;
        }

        bool isMimic = (std::rand() % 5) == 0;
        if (isNearSpawn(r)) isMimic = false; // no monsters in the spawn room / the room after

        if (isMimic) {
            // Mimics are hostile ambush enemies — scale them with floor/difficulty exactly
            // like every other enemy (HP compounds, damage linear + per-tier bump) so a
            // late-Hell mimic isn't a trivial 60 HP. effectiveFloor also drives loot/DoT credit.
            u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
            f32 mimicHp  = GameConst::MIMIC_HEALTH * GameConst::floorHealthMult(effectiveFloor);
            f32 mimicDmg = GameConst::MIMIC_DAMAGE * GameConst::floorDamageMult(effectiveFloor)
                           * GameConst::difficultyDamageBump(m_difficulty);
            EntityHandle h = EntitySystem::spawn(m_entities,
                Vec3{cx, cy + 0.4f, cz}, {0.45f, 0.4f, 0.45f}, false,
                mimicHp, 4.0f, GameConst::MIMIC_TRIGGER_DIST,
                2.0f, 0.6f, mimicDmg);
            Entity* ent = handleGet(m_entities, h);
            if (ent) {
                ent->meshId = chestMeshId;
                ent->enemyType = EnemyType::MIMIC;
                ent->level = static_cast<u16>(effectiveFloor); // loot/DoT-credit scale with depth
                ent->aiState = AIState::DORMANT;
                Collision::ensureNotInWall(ent->position, ent->halfExtents, m_level.grid);
            }
        } else {
            // Real chest — a closed world-item SENTINEL (shrine pattern: replication and the
            // server-validated E path come free), not bare loot on the floor. It renders and
            // prompts EXACTLY like the dormant mimic above, which is what gives the mimic
            // something to hide among. No item is rolled here: itemLevel carries the loot
            // level and Engine::openChest rolls at open time on the authoritative sim.
            // Level scales with effectiveFloor like the mimic's drop — if real chests paid
            // early-floor trash while mimics paid depth-scaled loot, the LOOT would become
            // the tell.
            u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
            u32 lootLvl = effectiveFloor + r / 3;   // deeper rooms slightly better, as before
            ItemInstance chest;
            chest.defId     = CHEST_ID;
            chest.itemLevel = static_cast<u8>(lootLvl > 255 ? 255 : lootLvl);
            chest.uid       = m_worldItems.nextUid++;   // direct-construction uid (globes/shrines pattern)
            WorldItemSystem::spawn(m_worldItems, chest, Vec3{cx, cy + 0.3f, cz}, &m_level.grid);
        }
    }
}

// ---------------------------------------------------------------------------
// spawnFloorDecorations — scatter themed prop entities (webs, braziers) that
// fit the current depth tier. Reads m_level.currentFloor to select the tier.
// ---------------------------------------------------------------------------
void Engine::spawnFloorDecorations(const DungeonResult& dungeon)
{
    // Resolve decoration mesh IDs once
    u8 mWeb        = findMeshByName("web");         // flat slab for ceilings
    u8 mWebWall    = findMeshByName("web_wall");   // upright panel for walls
    u8 mShackles = findMeshByName("shackles");
    u8 mBarrel   = findMeshByName("barrel");
    u8 mCage     = findMeshByName("cage");
    u8 mBones    = findMeshByName("bones");
    u8 mBrazier  = findMeshByName("brazier");
    u8 matWood   = MaterialSystem::getIdByName("prop_wood");
    u8 matIron   = MaterialSystem::getIdByName("prop_iron");
    u8 matBone   = MaterialSystem::getIdByName("prop_bone");
    u8 matWeb    = MaterialSystem::getIdByName("prop_web");
    u8 matBrazier = MaterialSystem::getIdByName("prop_brazier");

    // Each tier defines which props can appear and their materials.
    // wallOnly = true places props against room edges so they don't block movement.
    struct PropDef { u8 meshId; u8 matId; Vec3 halfExt; f32 yOff; bool wallOnly; };

    // Build tier-specific prop lists — only webs and braziers.
    // Bones and barrels removed (boring, block movement/items).
    PropDef dungeonProps[] = {
        {mWeb, matWeb, {0.50f, 0.50f, 0.02f}, 0.0f, true},
    };
    PropDef catacombProps[] = {
        {mWeb,     matWeb,     {0.50f, 0.50f, 0.02f}, 0.0f, true},
        {mBrazier, matBrazier, {0.12f, 0.30f, 0.12f}, 0.30f, true},
    };
    PropDef cavernProps[] = {
        {mWeb, matWeb, {0.50f, 0.50f, 0.02f}, 0.0f, true},
    };
    PropDef hellforgeProps[] = {
        {mWeb,     matWeb,     {0.50f, 0.50f, 0.02f}, 0.0f, true},
        {mBrazier, matBrazier, {0.12f, 0.30f, 0.12f}, 0.30f, true},
    };
    PropDef voidProps[] = {
        {mWeb,     matWeb,     {0.50f, 0.50f, 0.02f}, 0.0f, true},
        {mBrazier, matBrazier, {0.12f, 0.30f, 0.12f}, 0.30f, true},
    };

    const PropDef* props = dungeonProps;
    u32 propCount = 1;
    if (m_level.currentFloor >= 41)      { props = voidProps;      propCount = 2; }
    else if (m_level.currentFloor >= 31) { props = hellforgeProps;  propCount = 2; }
    else if (m_level.currentFloor >= 21) { props = cavernProps;     propCount = 1; }
    else if (m_level.currentFloor >= 11) { props = catacombProps;   propCount = 2; }

    u32 decoCount = 0;
    for (u32 r = 1; r < dungeon.roomCount; r++) {
        const DungeonRoom& room = dungeon.rooms[r];
        // 0-3 decorations per room, biased by room area
        u32 area = room.w * room.d;
        u32 numDecos = (area > 20) ? 3 : (area > 10) ? 2 : 1;
        if ((std::rand() % 3) == 0) numDecos = 0; // 33% chance for empty room

        for (u32 d = 0; d < numDecos; d++) {
            // Pick a random prop from the tier list
            const PropDef& prop = props[std::rand() % propCount];

            f32 px, pz;
            u32 wall = std::rand() % 4; // 0=north, 1=south, 2=west, 3=east
            // Webs: 30% ceiling, 70% wall. Decide before placement.
            bool ceilingWeb = false;
            f32 yOff = prop.yOff;
            Vec3 spawnHalf = prop.halfExt;
            if (prop.matId == matWeb && (std::rand() % 100) < 30) {
                ceilingWeb = true;
            }

            if (ceilingWeb) {
                // Ceiling web — random position, flush against ceiling surface.
                // yOff is relative to floorHeight, but ceiling is always at 3.0m absolute.
                f32 cs = m_level.grid.cellSize;
                px = (room.x + 1 + std::rand() % (room.w > 2 ? room.w - 2 : 1)) * cs;
                pz = (room.z + 1 + std::rand() % (room.d > 2 ? room.d - 2 : 1)) * cs;
                yOff = 3.0f - room.floorHeight;
                spawnHalf = {0.50f, 0.02f, 0.50f};
            } else if (prop.wallOnly) {
                // Place directly on the wall surface
                if (room.w < 4 || room.d < 4) continue;
                f32 cs = m_level.grid.cellSize;
                f32 margin = prop.halfExt.x + 0.1f;
                if (wall < 2) {
                    f32 minX = (room.x * cs) + margin;
                    f32 maxX = ((room.x + room.w) * cs) - margin;
                    if (minX >= maxX) { px = (minX + maxX) * 0.5f; }
                    else { px = minX + (std::rand() % 1000) * 0.001f * (maxX - minX); }
                    // Flush on wall face (z at cell boundary)
                    if (wall == 0)
                        pz = room.z * cs;
                    else
                        pz = (room.z + room.d) * cs;
                } else {
                    f32 minZ = (room.z * cs) + margin;
                    f32 maxZ = ((room.z + room.d) * cs) - margin;
                    if (minZ >= maxZ) { pz = (minZ + maxZ) * 0.5f; }
                    else { pz = minZ + (std::rand() % 1000) * 0.001f * (maxZ - minZ); }
                    // Flush on wall face (x at cell boundary)
                    if (wall == 2)
                        px = room.x * cs;
                    else
                        px = (room.x + room.w) * cs;
                }
                // Skip doorways — verify solid wall behind
                u32 checkX, checkZ;
                if (wall == 0)      { checkX = static_cast<u32>(px / cs); checkZ = room.z - 1; }
                else if (wall == 1) { checkX = static_cast<u32>(px / cs); checkZ = room.z + room.d; }
                else if (wall == 2) { checkX = room.x - 1;               checkZ = static_cast<u32>(pz / cs); }
                else                { checkX = room.x + room.w;          checkZ = static_cast<u32>(pz / cs); }
                if (!LevelGridSystem::isInBounds(m_level.grid, checkX, checkZ) ||
                    !LevelGridSystem::isSolid(m_level.grid, checkX, checkZ)) continue;
                // Wall webs: random height 1.6-2.1m
                if (prop.matId == matWeb) yOff = 1.6f + (std::rand() % 50) * 0.01f;
            } else {
                // Small/flat props (bones) can go anywhere in the room
                px = (room.x + 1 + std::rand() % (room.w > 2 ? room.w - 2 : 1)) * m_level.grid.cellSize;
                pz = (room.z + 1 + std::rand() % (room.d > 2 ? room.d - 2 : 1)) * m_level.grid.cellSize;
            }
            f32 py = room.floorHeight;

            // Skip if too close to an existing web (min 2m spacing)
            if (prop.matId == matWeb) {
                Vec3 candidate = {px, py + yOff, pz};
                bool tooClose = false;
                for (u32 ci = 0; ci < m_entities.activeCount; ci++) {
                    const Entity& other = m_entities.entities[m_entities.activeList[ci]];
                    if (other.enemyType != EnemyType::PROP) continue;
                    // Check if other is a web by checking known web material IDs
                    bool otherIsWeb = false;
                    for (u8 w = 0; w < 4; w++) {
                        u8 wids[] = {matWeb,
                            MaterialSystem::getIdByName("prop_web_b"),
                            MaterialSystem::getIdByName("prop_web_c"),
                            MaterialSystem::getIdByName("prop_web_d")};
                        if (other.materialId == wids[w]) { otherIsWeb = true; break; }
                    }
                    if (otherIsWeb && lengthSq(other.position - candidate) < 4.0f) {
                        tooClose = true;
                        break;
                    }
                }
                if (tooClose) continue;
            }

            EntityHandle dh = EntitySystem::spawn(m_entities,
                Vec3{px, py + yOff, pz}, spawnHalf, false,
                9999.0f, 0.0f, 0.0f, 0.0f, 999.0f, 0.0f);
            Entity* deco = handleGet(m_entities, dh);
            if (deco) {
                deco->meshId = prop.meshId;
                deco->materialId = prop.matId;
                deco->enemyType = EnemyType::PROP;
                // Randomize web texture for variety
                if (prop.matId == matWeb) {
                    static const char* webMats[] = {"prop_web", "prop_web_b", "prop_web_c", "prop_web_d"};
                    deco->materialId = MaterialSystem::getIdByName(webMats[std::rand() % 4]);
                }
                if (ceilingWeb) {
                    // Ceiling webs use fallback cube path scaled flat by halfExtents
                    // (meshId 0 = cube, renderer scales by halfExtents directly)
                    deco->meshId = 0;
                    deco->yaw = (std::rand() % 628) * 0.01f;
                } else if (prop.matId == matWeb) {
                    // Wall webs use the upright mesh, yaw faces the wall normal
                    deco->meshId = mWebWall;
                    // N/S walls face along Z → yaw 0, E/W walls face along X → yaw 90°
                    static const f32 wallYaw[] = {0.0f, 0.0f, 1.5708f, 1.5708f};
                    deco->yaw = wallYaw[wall];
                } else if (prop.wallOnly) {
                    static const f32 wallYaw[] = {0.0f, 0.0f, 1.5708f, 1.5708f};
                    deco->yaw = wallYaw[wall];
                } else {
                    deco->yaw = (std::rand() % 628) * 0.01f;
                }
                decoCount++;
            }
        }
    }
    LOG_INFO("Spawned %u themed decorations for tier %u-%u",
             decoCount, (m_level.currentFloor / 10) * 10 + 1, ((m_level.currentFloor / 10) + 1) * 10);
}

// ---------------------------------------------------------------------------
// spawnFloorNpcs — place the 3 friendly NPCs (cleric, archer, rogue) in the
// spawn room. Equipment is handled inside spawnFriendlyNpc (which stays in
// engine_startgame.cpp).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// spawnFloorEvents — roll this floor's one-off event (or none) and spawn it.
//
// Called AFTER spawnFloorBoss + buildClearanceField: the boss call mutates the boss room's geometry
// and rebuilds the level mesh, so anything placed before it can be swallowed by the arena
// expansion — and the goblin needs the clearance field to path away from you.
//
// Host/SP only. The goblin is a normal entity, so it reaches clients through the snapshot like any
// other; nothing here is re-rolled client-side.
// ---------------------------------------------------------------------------
void Engine::spawnFloorEvents(DungeonResult& dungeon)
{
    if (m_netRole == NetRole::CLIENT) return;

    const u32 effFloor = m_level.currentFloor + m_difficulty * 50;
    u32 rng = static_cast<u32>(std::rand());
    const FloorEventId id = FloorEvent::pick(m_floorEvents, effFloor, rng);
    if (id == FloorEventId::NONE) return;

    switch (id) {
        case FloorEventId::LOOT_GOBLIN: spawnLootGoblin(dungeon); break;
        default: break;   // an id with no spawner would be a silent no-op event; the loader rejects those
    }
}

// The loot goblin. Spawns far from the player so the chase has somewhere to happen.
void Engine::spawnLootGoblin(const DungeonResult& dungeon)
{
    if (dungeon.roomCount < 2) return;

    // Farthest room from the spawn room, so you have to commit to the chase rather than trip over
    // it — but NEVER the exit room. The farthest room is very often exactly where the exit door
    // lands, which parked the goblin at the exit on most floors: the player tripped over it while
    // leaving instead of ever choosing to hunt it.
    const u32 doorGX = static_cast<u32>(m_level.floorDoorPos.x / m_level.grid.cellSize);
    const u32 doorGZ = static_cast<u32>(m_level.floorDoorPos.z / m_level.grid.cellSize);
    u32 best = 0xFFFFFFFF; f32 bestDist = -1.0f;
    const DungeonRoom& start = dungeon.rooms[0];
    for (u32 r = 1; r < dungeon.roomCount; r++) {
        const DungeonRoom& room = dungeon.rooms[r];
        const bool holdsExit = doorGX >= room.x && doorGX < room.x + room.w &&
                               doorGZ >= room.z && doorGZ < room.z + room.d;
        if (holdsExit) continue;
        f32 dx = static_cast<f32>(room.x) - static_cast<f32>(start.x);
        f32 dz = static_cast<f32>(room.z) - static_cast<f32>(start.z);
        f32 d2 = dx * dx + dz * dz;
        if (d2 > bestDist) { bestDist = d2; best = r; }
    }
    // Two-room floor whose only other room holds the exit: better a goblin at the exit than none.
    if (best == 0xFFFFFFFF) best = 1;
    const DungeonRoom& room = dungeon.rooms[best];

    // Center y = floor + half height. Its IDLE state deliberately runs no movement/floor-snap,
    // so a wrong height here would just persist.
    Vec3 pos = { (room.x + room.w * 0.5f) * m_level.grid.cellSize,
                 room.floorHeight + 0.5f,
                 (room.z + room.d * 0.5f) * m_level.grid.cellSize };

    const u32 effFloor = m_level.currentFloor + m_difficulty * 50;
    const f32 hp = Goblin::HEALTH * GameConst::floorHealthMult(effFloor);
    const f32 speed = 5.0f * Goblin::SPEED_MULT;   // ~player base speed, scaled

    // damage 0 and attackRange 0: it has no attack at all. The FLEE state never attacks either —
    // belt and braces, because a goblin that fights back is just a fast monster.
    EntityHandle h = EntitySystem::spawn(m_entities, pos, {0.35f, 0.5f, 0.35f}, false,
                                         hp, speed, Goblin::DETECT_RANGE, 0.0f, 1.0f, 0.0f);
    Entity* g = handleGet(m_entities, h);
    if (!g) { LOG_WARN("LootGoblin: entity pool full — event skipped"); return; }

    g->meshId     = m_goblinMeshId;
    g->materialId = MaterialSystem::getIdByName("goblin");
    g->enemyType  = EnemyType::GENERIC;
    g->enemyRole  = EnemyRole::NORMAL;
    g->flags     |= ENT_LOOT_GOBLIN;      // survives death, unlike aiState — the drop handler needs it
    // Starts IDLE: it stands motionless over its hoard, channeling its escape portal — the portal
    // is a pure render-side effect keyed on this replicated state (ENT_LOOT_GOBLIN + IDLE + yaw are
    // all on the wire), drawn in front of the goblin along its facing; see engine_render_effects.cpp.
    // It only bolts once the player ATTACKS it (Combat::applyDamage flips it to FLEE, which is also
    // the instant the portal effect vanishes — the summoning was interrupted). A goblin already
    // scattering the instant the floor loads is a chase the player never chose to start, and
    // usually never even sees.
    g->aiState    = AIState::IDLE;
    g->level      = static_cast<u16>(effFloor);
    g->maxHealth  = g->health;
    g->baseMoveSpeed      = g->moveSpeed;
    g->baseAttackCooldown = g->attackCooldown;
    // The escape clock does NOT start here — it starts the moment the player provokes it (see
    // Combat::applyDamage). Otherwise the goblin could quietly time out and vanish while the player
    // was still two rooms away, and the whole event would happen off-screen. On expiry
    // EntitySystem::tickTimers frees the slot WITHOUT firing the death callback, so an escaped
    // goblin pays out nothing — which is what makes the chase a real choice.
    g->lifeTimer  = 0.0f;
    // tacticalTimer is the bleed cadence. Free to reuse here: it is only otherwise read for the
    // SUMMONER/HEALER roles, and the goblin has neither.
    g->tacticalTimer = Goblin::BLEED_SECONDS;
    Collision::ensureNotInWall(g->position, g->halfExtents, m_level.grid);

    LOG_INFO("LootGoblin: spawned in room %u (%.0f HP, idle until attacked, then %.0fs to escape)",
             best, static_cast<f64>(hp), static_cast<f64>(Goblin::ESCAPE_SECONDS));
}

// ---------------------------------------------------------------------------
// togglePetCompanion — "use" a pet consumable (the goblin's 1% Mini Loot Goblin, or any
// enemy's 1-in-10000 "Mini <Enemy>" — both engine_death.cpp): dismiss the owner's live pet if
// it is the same kind, swap if it is a different kind, otherwise summon one at their side.
// Server/SP only — the entity replicates to guests through the snapshot like any other, and a
// guest's use-click arrives here as CL_USE_PET via Engine::onUsePet. Pets do not survive a
// floor transition (the entity pool is rebuilt); the item has infinite uses, so re-summoning
// is one click.
// ---------------------------------------------------------------------------
void Engine::togglePetCompanion(u8 ownerSlot, u16 petDefId)
{
    if (m_netRole == NetRole::CLIENT) return;   // authoritative path only
    if (ownerSlot >= MAX_PLAYERS || !m_players[ownerSlot].active) return;
    if (petDefId >= m_itemDefCount || !m_itemDefs[petDefId].petSummon) return;

    // Which creature this item summons: an enemies.json def ("Mini <Enemy>"), or the special
    // goblin entity for the original Mini Loot Goblin (petEnemyIdx 0xFF). The pet entity carries
    // the same index in enemyDefIdx, which is how we recognise which pet is already out.
    const u8 wantIdx = m_itemDefs[petDefId].petEnemyIdx;

    // Dismiss pass: a live pet owned by this slot winks out — the same no-payout death the
    // Swarm Queen expiry uses (never Combat::killEntity, which would fire the loot callback).
    // Using the SAME pet's item toggles it away; using a DIFFERENT one swaps (dismiss, then
    // fall through to summon) — one companion per player, never a menagerie.
    bool wasSameKind = false;
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        Entity& e = m_entities.entities[m_entities.activeList[a]];
        if (!(e.flags & ENT_FRIENDLY) || (e.flags & ENT_DEAD)) continue;
        if (e.npcClass != NpcClass::PET || e.ownerNetSlot != ownerSlot) continue;
        e.flags     |= ENT_DEAD;
        e.aiState    = AIState::DEAD;
        e.deathTimer = 0.01f;
        wasSameKind  = (e.enemyDefIdx == wantIdx);
        LOG_INFO("Pet: companion dismissed (owner slot %u)", ownerSlot);
        break;
    }
    if (wasSameKind) return;

    // Summon beside the owner. Entity position is the CENTER, owner position is feet.
    // Mini = half the source creature's extents — the renderer scales the shared mesh to
    // 2×halfExtents, so every miniature costs no new asset.
    const NetPlayer& owner = m_players[ownerSlot];
    const EnemyDef* src = (wantIdx < m_enemyDefs.count) ? &m_enemyDefs.defs[wantIdx] : nullptr;
    const Vec3 halfExt = src ? src->halfExtents * 0.5f
                             : Vec3{0.18f, 0.26f, 0.18f};   // goblin: ~half the real one
    Vec3 pos = owner.position + Vec3{0.9f, halfExt.y, 0.9f};

    // Stats are cosmetic: damage/attackRange 0 (it never fights), detectionRange 0 (the friendly
    // target scan can't acquire anything), HP nominal (applyDamage ignores pets regardless).
    // Grounded even for flying sources — the PET follow AI walks and snaps to the floor.
    EntityHandle h = EntitySystem::spawn(m_entities, pos, halfExt, false,
                                         30.0f, 6.5f, 0.0f, 0.0f, 1.0f, 0.0f);
    Entity* p = handleGet(m_entities, h);
    if (!p) { LOG_WARN("Pet: entity pool full — summon failed"); return; }

    if (src) {
        // Miniature of a real monster: wear its rig and skin, and animate like it (the renderer
        // keys attack/idle motion off enemyType). enemyDefIdx also lets nameplates call it by name.
        p->meshId     = src->meshId;
        p->materialId = src->materialId;
        p->enemyType  = src->enemyType;
    } else {
        p->meshId       = m_goblinMeshId;
        // Gilded, not green: the goblin pet is a LEGENDARY drop, so it wears the gold_trim
        // material (the Swarm Queen's) instead of the plain goblin skin — reads as "special"
        // at a glance and can never be mistaken for a hostile loot goblin in the same room.
        p->materialId   = MaterialSystem::getIdByName("gold_trim");
        if (p->materialId == 0) p->materialId = MaterialSystem::getIdByName("goblin"); // fallback
        p->enemyType    = EnemyType::GENERIC;
    }
    p->enemyDefIdx  = wantIdx;
    p->npcClass     = NpcClass::PET;
    p->flags       |= ENT_FRIENDLY | ENT_UNTARGETABLE;  // never a combat participant on either side
    p->ownerNetSlot = ownerSlot;                         // follow anchor (enemy_ai.cpp)
    p->ownerLocalPlayer = (ownerSlot < m_splitPlayerCount) ? ownerSlot : 0; // split-screen fallback anchor
    p->baseMoveSpeed = p->moveSpeed;
    Collision::ensureNotInWall(p->position, p->halfExtents, m_level.grid);
    LOG_INFO("Pet: %s summoned (owner slot %u)", m_itemDefs[petDefId].name, ownerSlot);
}

// ---------------------------------------------------------------------------
// spawnFloorShrines — scatter walk-up shrines through the floor.
//
// Shrines are WorldItem SENTINELS, not entities or props: that inherits spawning, snapshot
// replication and the server-authoritative pickup path for free, instead of building a parallel
// interactable-object system for one feature.
// ---------------------------------------------------------------------------
void Engine::spawnFloorShrines(const DungeonResult& dungeon)
{
    if (m_netRole == NetRole::CLIENT) return;   // world state is the server's to author

    u8 placed = 0;
    for (u32 r = 1; r < dungeon.roomCount && placed < Shrine::MAX_PER_FLOOR; r++) {
        if (static_cast<f32>(std::rand()) / static_cast<f32>(RAND_MAX) >= Shrine::ROOM_CHANCE)
            continue;

        const DungeonRoom& room = dungeon.rooms[r];
        Vec3 pos = { (room.x + room.w * 0.5f) * m_level.grid.cellSize,
                     room.floorHeight + 0.5f,
                     (room.z + room.d * 0.5f) * m_level.grid.cellSize };

        const u8 kind = static_cast<u8>(1 + (std::rand() % 4));   // POWER / SPEED / VITALITY / SPELL
        ItemInstance s;
        s.defId = Shrine::defIdFor(kind);
        s.uid   = m_worldItems.nextUid++;
        // ownerSlot 0xFF: a shrine is never reserved to one player — in co-op it is first-come.
        if (!WorldItemSystem::spawn(m_worldItems, s, pos, &m_level.grid, 0xFF)) {
            LOG_WARN("Shrine: world-item pool full — shrine skipped");
            break;
        }
        placed++;
        LOG_INFO("Shrine: %s placed in room %u", Shrine::nameOf(kind), r);
    }
}

void Engine::spawnFloorNpcs(const DungeonResult& dungeon)
{
    // Clear NPC equipment pool on floor 1, preserve on descent
    if (m_level.currentFloor <= 1) {
        for (u32 i = 0; i < MAX_NPC_EQUIP; i++) m_npcEquip[i] = NpcEquipment{};
    }

    u8 floor = static_cast<u8>(m_level.currentFloor);
    f32 sy = dungeon.spawnPos.y + 0.9f;

    // NPC 0: Cleric — front left (healer)
    spawnFriendlyNpc({dungeon.spawnPos.x - 2.5f, sy, dungeon.spawnPos.z + 2.0f},
                      NpcClass::CLERIC, floor);
    // NPC 1: Archer — front right (ranged DPS)
    spawnFriendlyNpc({dungeon.spawnPos.x + 2.5f, sy, dungeon.spawnPos.z + 2.0f},
                      NpcClass::ARCHER, floor);
    // NPC 2: Rogue — back center (fast melee)
    spawnFriendlyNpc({dungeon.spawnPos.x, sy, dungeon.spawnPos.z - 2.0f},
                      NpcClass::ROGUE, floor);
    LOG_INFO("Spawned 3 friendly NPCs (cleric, archer, rogue) in spawn room");
}
