// Engine module — see engine.h for class definition.
// Split from engine.cpp for manageability. All methods are Engine:: members.

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

// Shared statics defined in engine.cpp
// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;

// ---------------------------------------------------------------------------
// NPC equipment and spawning helpers
// ---------------------------------------------------------------------------

void Engine::rollNpcEquipment(NpcEquipment& equip, NpcClass npcClass, u8 floor) {
    // Clear all slots
    for (u32 s = 0; s < static_cast<u32>(ItemSlot::COUNT); s++) {
        equip.equipped[s] = ItemInstance{};
    }

    // Roll weak equipment — always level 1, forced COMMON (no affixes),
    // halved base stats so NPCs are clearly weaker than the player
    (void)floor; // NPCs always get level-1 gear regardless of floor
    auto rollSlot = [&](ItemSlot slot, const char* subtypeHint) {
        for (u32 attempt = 0; attempt < 20; attempt++) {
            ItemInstance item = ItemGen::rollItem(1, m_itemDefs, m_itemDefCount,
                                                   m_affixDefs, m_affixDefCount);
            if (isItemEmpty(item)) continue;
            const ItemDef& def = m_itemDefs[item.defId];
            if (def.slot != slot) continue;
            if (subtypeHint && subtypeHint[0]) {
                if (std::strstr(def.name, subtypeHint) == nullptr) continue;
            }
            item.rarity = Rarity::COMMON;
            item.affixCount = 0;
            item.damage *= 0.5f;
            item.bonusHealth *= 0.5f;
            equip.equipped[static_cast<u32>(slot)] = item;
            return;
        }
        for (u32 attempt = 0; attempt < 40; attempt++) {
            ItemInstance item = ItemGen::rollItem(1, m_itemDefs, m_itemDefCount,
                                                   m_affixDefs, m_affixDefCount);
            if (isItemEmpty(item)) continue;
            const ItemDef& def = m_itemDefs[item.defId];
            if (def.slot != slot) continue;
            item.rarity = Rarity::COMMON;
            item.affixCount = 0;
            item.damage *= 0.5f;
            item.bonusHealth *= 0.5f;
            equip.equipped[static_cast<u32>(slot)] = item;
            return;
        }
    };

    // Class-specific weapon
    switch (npcClass) {
        case NpcClass::CLERIC:  rollSlot(ItemSlot::WEAPON, "Mace"); break;
        case NpcClass::ARCHER:  rollSlot(ItemSlot::WEAPON, "Bow");  break;
        case NpcClass::MAGE:    rollSlot(ItemSlot::WEAPON, "Staff"); break;
        case NpcClass::ROGUE:   rollSlot(ItemSlot::WEAPON, "Kniv"); break;
        case NpcClass::PALADIN: rollSlot(ItemSlot::WEAPON, "Mace"); break;
        default: rollSlot(ItemSlot::WEAPON, nullptr); break;
    }

    // Roll armor pieces for all NPCs
    rollSlot(ItemSlot::ARMOR, nullptr);
    rollSlot(ItemSlot::HELMET, nullptr);
    rollSlot(ItemSlot::BOOTS, nullptr);

    // Recalculate cached stat bonuses from equipped items
    Inventory::recalculateNpcStats(equip);
    equip.active = true;
}

void Engine::applyNpcEquipmentStats(Entity& e, const NpcEquipment& equip) {
    // Get effective weapon stats from equipped weapon
    const ItemInstance& wpn = equip.equipped[static_cast<u32>(ItemSlot::WEAPON)];
    if (!isItemEmpty(wpn) && wpn.defId < m_itemDefCount) {
        const ItemDef& def = m_itemDefs[wpn.defId];
        // Base damage from rolled item + flat bonus, scaled down so NPCs
        // are support characters, not DPS machines that shred bosses
        f32 rawDmg = wpn.damage + equip.bonusDamageFlat;
        e.damage = rawDmg * (1.0f + equip.bonusDamagePct / 100.0f) * 0.08f;
        e.npcWeaponType = def.weaponType;

        // Attack range depends on weapon type
        if (def.weaponType == WeaponType::PROJECTILE) {
            e.attackRange = 12.0f;
            e.npcProjectileSpeed = def.baseProjectileSpeed * (1.0f + equip.bonusProjectileSpeedPct / 100.0f);
            if (e.npcProjectileSpeed < 8.0f) e.npcProjectileSpeed = 12.0f;
            e.npcProjectileRadius = def.baseProjectileRadius;
            if (e.npcProjectileRadius < 0.05f) e.npcProjectileRadius = 0.1f;
        } else {
            e.attackRange = def.baseRange;
            if (e.attackRange < 2.0f) e.attackRange = 2.5f;
        }

        // Cooldown with reduction
        e.attackCooldown = def.baseCooldown * (1.0f - equip.bonusCooldownReduction);
        if (e.attackCooldown < 0.1f) e.attackCooldown = 0.1f;

        // Visual: set weapon mesh from item def
        e.weaponMeshId = def.meshId;
    }

    // Apply health bonuses from armor
    f32 baseHp = e.maxHealth;
    f32 totalFlat = equip.bonusHealthFlat;
    e.maxHealth = (baseHp + totalFlat) * (1.0f + equip.bonusHealthPct / 100.0f);
    e.health = e.maxHealth;

    // Apply move speed bonus
    e.moveSpeed += equip.bonusMoveSpeed;
}

EntityHandle Engine::spawnFriendlyNpc(Vec3 pos, NpcClass npcClass, u8 floor) {
    // Determine base stats from class
    f32 baseHp = 40.0f;
    f32 speed  = 3.0f;
    f32 detRange = 15.0f;
    f32 atkRange = 2.5f;
    f32 atkCool = 0.8f;
    f32 baseDmg = 5.0f;
    Vec3 halfExt = {0.35f, 0.9f, 0.35f};  // slightly smaller so NPCs fit through 1-cell corridors
    u8 meshId = m_meshIdHuman;
    const char* matName = "human_skin";
    u8 weaponMesh = 0;
    const char* speech = "Ready!";

    switch (npcClass) {
        case NpcClass::CLERIC:
            baseHp = GameConst::NPC_HEALTH_CLERIC;
            speed = 3.0f;
            atkRange = 2.5f;
            atkCool = 3.0f;    // melee: 1/3 attack speed
            baseDmg = 1.0f;
            meshId = m_meshIdCleric;
            matName = "cleric_skin";
            weaponMesh = m_meshIdMace;
            speech = "The light protects!";
            break;
        case NpcClass::ARCHER:
            baseHp = GameConst::NPC_HEALTH_ARCHER;
            speed = 3.5f;
            halfExt = {0.35f, 0.85f, 0.35f};
            atkRange = 12.0f;
            atkCool = 7.5f;    // ranged: 1/5 attack speed
            baseDmg = 0.5f;
            meshId = m_meshIdArcher;
            matName = "archer_skin";
            weaponMesh = m_meshIdBow;
            speech = "Ready when you are!";
            break;
        case NpcClass::MAGE:
            baseHp = GameConst::NPC_HEALTH_MAGE;
            speed = 2.8f;
            atkRange = 14.0f;
            atkCool = 9.0f;    // ranged: 1/5 attack speed
            baseDmg = 0.8f;
            meshId = m_meshIdMage;
            matName = "mage_skin";
            weaponMesh = m_meshIdStaff;
            speech = "Knowledge is power.";
            break;
        case NpcClass::ROGUE:
            baseHp = GameConst::NPC_HEALTH_ROGUE;
            speed = 4.0f;
            halfExt = {0.35f, 0.85f, 0.35f};
            atkRange = 10.0f;
            atkCool = 5.0f;    // ranged: 1/5 attack speed
            baseDmg = 0.4f;
            meshId = m_meshIdRogue;
            matName = "rogue_skin";
            weaponMesh = m_meshIdThrowingKnife;
            speech = "Stick to the shadows.";
            break;
        case NpcClass::PALADIN:
            baseHp = GameConst::NPC_HEALTH_PALADIN;
            speed = 2.5f;
            halfExt = {0.45f, 0.95f, 0.45f};
            atkRange = 2.5f;
            atkCool = 3.0f;    // melee: 1/3 attack speed
            baseDmg = 1.2f;
            meshId = m_meshIdPaladin;
            matName = "paladin_skin";
            weaponMesh = m_meshIdMace;
            speech = "By the light!";
            break;
        default: break;
    }

    EntityHandle h = EntitySystem::spawn(m_entities, pos,
        halfExt, false, baseHp, speed, detRange, atkRange, atkCool, baseDmg);
    Entity* npc = handleGet(m_entities, h);
    if (!npc) return h;

    npc->flags |= ENT_FRIENDLY;
    npc->enemyType = EnemyType::SKELETON;
    npc->aiState = AIState::IDLE;
    npc->meshId = meshId;
    npc->materialId = MaterialSystem::getIdByName(matName);
    npc->weaponMeshId = weaponMesh;
    npc->npcClass = npcClass;
    npc->speechText = speech;
    npc->speechTimer = 4.0f;

    // Set weapon type defaults (overridden by equipment if available)
    if (npcClass == NpcClass::ARCHER || npcClass == NpcClass::MAGE || npcClass == NpcClass::ROGUE) {
        npc->npcWeaponType = WeaponType::PROJECTILE;
        npc->npcProjectileSpeed = 15.0f;
        npc->npcProjectileRadius = 0.1f;
    }

    // Allocate equipment slot and roll starting gear
    for (u32 ei = 0; ei < MAX_NPC_EQUIP; ei++) {
        if (!m_npcEquip[ei].active) {
            npc->npcEquipIdx = static_cast<u8>(ei);
            rollNpcEquipment(m_npcEquip[ei], npcClass, floor);
            applyNpcEquipmentStats(*npc, m_npcEquip[ei]);
            break;
        }
    }

    // Ensure NPC doesn't spawn inside a wall
    Collision::ensureNotInWall(npc->position, npc->halfExtents, m_level.grid);

    return h;
}

void Engine::upgradeNpcEquipment(u8 newFloor) {
    // Find surviving friendly NPCs and upgrade their equipment.
    // Guard: if entity pool was already reset, skip (prevents stale iteration).
    if (m_entities.activeCount == 0) return;
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        u32 idx = m_entities.activeList[a];
        if (idx >= MAX_ENTITIES) continue; // safety bound check
        Entity& e = m_entities.entities[idx];
        if (!(e.flags & ENT_FRIENDLY)) continue;
        if (e.flags & ENT_DEAD) continue;
        if (e.npcEquipIdx >= MAX_NPC_EQUIP) continue;

        NpcEquipment& equip = m_npcEquip[e.npcEquipIdx];
        equip.floorsSurvived++;

        // Re-roll equipment at the new floor level for better stats
        rollNpcEquipment(equip, e.npcClass, newFloor);
        // Apply a per-floor bonus multiplier on top of the new gear
        f32 survivalBonus = 1.0f + equip.floorsSurvived * GameConst::NPC_EQUIP_UPGRADE_MULT;
        for (u32 s = 0; s < static_cast<u32>(ItemSlot::COUNT); s++) {
            if (!isItemEmpty(equip.equipped[s])) {
                equip.equipped[s].damage *= survivalBonus;
                equip.equipped[s].bonusHealth *= survivalBonus;
            }
        }
        Inventory::recalculateNpcStats(equip);
        applyNpcEquipmentStats(e, equip);

        // Heal surviving NPCs fully
        e.health = e.maxHealth;

        LOG_INFO("NPC (class %u) survived floor — equipment upgraded to floor %u (survival bonus %.0f%%)",
                 static_cast<u32>(e.npcClass), newFloor, (survivalBonus - 1.0f) * 100.0f);
    }
}

void Engine::startGame() {
    // Reset first-kill guaranteed drop for this floor
    s_firstKillDropGiven = false;

    // Reset per-floor stats for transition screen
    m_transition.floorKillCount = 0;
    m_transition.floorTime = 0.0f;

    // Reset tutorials on floor 1 only (first floor of a new game)
    if (m_level.currentFloor <= 1) {
        m_firstPickupTooltipShown = false;
        m_inventoryOpenedOnce     = false;
        m_equipTooltipShown       = false;
        m_itemEquippedOnce        = false;
        m_tutorialPulseTimer      = 0.0f;
        m_controlsTooltipTimer    = 8.0f;
    }

    // Reset NPC equipment pool so old floor's slots don't persist
    for (u32 i = 0; i < MAX_NPC_EQUIP; i++) m_npcEquip[i] = NpcEquipment{};

    // Clear particle pool and shake so leftover FX from previous floor don't persist
    ParticleSystem::clear(m_particles);
    m_camera.shake.intensity = 0.0f;

    // Build level — use BSP procedural generation with random seed.
    // Early floors (1-9) use a smaller grid for simpler, more linear layouts
    // so the exit is easier to find.  Later floors get the full 48x48 grid.
    u32 dungeonSeed = static_cast<u32>(std::rand()) + m_level.currentFloor * 7919;
    u32 gridSize = 48;
    if (m_level.currentFloor <= 3)       gridSize = 24;  // very small, almost linear
    else if (m_level.currentFloor <= 6)  gridSize = 32;  // small, few branches
    else if (m_level.currentFloor <= 9)  gridSize = 40;  // medium, some exploration

    // Non-boss floors require at least 5 rooms between spawn and exit.
    // Boss floors (every 5th) and small grids (floors 1-6) skip the requirement.
    bool isBossFloor = (m_level.currentFloor % 5 == 0);
    u32 minExitDist = (!isBossFloor && gridSize >= 32) ? 5 : 0;

    // Retry generation until we find a valid layout.
    for (u32 attempt = 0; attempt < 20; attempt++) {
        LevelGridSystem::init(m_level.grid, gridSize, gridSize, 1.0f);
        m_level.dungeon = LevelGen::generate(m_level.grid, dungeonSeed + attempt, gridSize, gridSize, minExitDist);
        if (m_level.dungeon.valid) break;
    }
    DungeonResult& dungeon = m_level.dungeon;
    Vec3 spawnPos = dungeon.spawnPos;

    // ---------------------------------------------------------------------------
    // Floor theme — retheme all cells based on the current depth tier.
    //   1-10: Stone Dungeon (default materials, no change)
    //  11-20: Catacombs    (mossy green/brown)
    //  21-30: Spider Caverns (dark purple)
    //  31-40: Hellforge     (red/orange fire)
    //  41-50: Void          (black/dark blue)
    // ---------------------------------------------------------------------------
    {
        u8 themeWall = 0, themeFloor = 0, themeCeil = 0;
        bool applyTheme = false;

        if (m_level.currentFloor >= 41) {
            themeWall  = MaterialSystem::getIdByName("void_wall");
            themeFloor = MaterialSystem::getIdByName("void_floor");
            themeCeil  = MaterialSystem::getIdByName("void_ceiling");
            applyTheme = true;
        } else if (m_level.currentFloor >= 31) {
            themeWall  = MaterialSystem::getIdByName("hellforge_wall");
            themeFloor = MaterialSystem::getIdByName("hellforge_floor");
            themeCeil  = MaterialSystem::getIdByName("hellforge_ceiling");
            applyTheme = true;
        } else if (m_level.currentFloor >= 21) {
            themeWall  = MaterialSystem::getIdByName("cavern_wall");
            themeFloor = MaterialSystem::getIdByName("cavern_floor");
            themeCeil  = MaterialSystem::getIdByName("cavern_ceiling");
            applyTheme = true;
        } else if (m_level.currentFloor >= 11) {
            themeWall  = MaterialSystem::getIdByName("catacomb_wall");
            themeFloor = MaterialSystem::getIdByName("catacomb_floor");
            themeCeil  = MaterialSystem::getIdByName("catacomb_ceiling");
            applyTheme = true;
        }

        if (applyTheme) {
            for (u32 z = 0; z < m_level.grid.depth; z++) {
                for (u32 x = 0; x < m_level.grid.width; x++) {
                    GridCell& cell = LevelGridSystem::getCell(m_level.grid, x, z);
                    if (!(cell.flags & CELL_FLOOR)) continue; // skip solid cells
                    // Replace default materials with theme — only preserve blood (boss arenas, id >= 20)
                    if (cell.wallMaterialId < 20) cell.wallMaterialId = themeWall;
                    if (cell.floorMaterialId < 20) cell.floorMaterialId = themeFloor;
                    if (cell.ceilMaterialId < 20) cell.ceilMaterialId = themeCeil;
                }
            }
            LOG_INFO("Applied floor theme for depth tier %u-%u",
                     (m_level.currentFloor / 10) * 10 + 1, ((m_level.currentFloor / 10) + 1) * 10);
        }
    }

    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid, m_level.sections, MAX_LEVEL_SECTIONS);
    Minimap::init(m_level.grid.width, m_level.grid.depth);

    // Init entities
    EntitySystem::init(m_entities);

    // Spawn enemies procedurally — themed variants + unique monsters per tier
    {
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

        // --- Enemy spawning: JSON-loaded defs (primary) or kTier fallback ---
        u8 currentTier = 1;
        if      (m_level.currentFloor >= 41) currentTier = 5;
        else if (m_level.currentFloor >= 31) currentTier = 4;
        else if (m_level.currentFloor >= 21) currentTier = 3;
        else if (m_level.currentFloor >= 11) currentTier = 2;

        // Collect JSON defs for this tier
        const EnemyDef* tierDefs[MAX_ENEMY_DEFS];
        u32 tierDefCount = collectTierDefs(m_enemyDefs, currentTier, tierDefs, MAX_ENEMY_DEFS);

        // Fallback: use kTier arrays if no JSON defs loaded
        const EnemyTemplate* fallbackTier = kTier1;
        u32 fallbackCount = sizeof(kTier1) / sizeof(kTier1[0]);
        if      (currentTier == 5) { fallbackTier = kTier5; fallbackCount = sizeof(kTier5) / sizeof(kTier5[0]); }
        else if (currentTier == 4) { fallbackTier = kTier4; fallbackCount = sizeof(kTier4) / sizeof(kTier4[0]); }
        else if (currentTier == 3) { fallbackTier = kTier3; fallbackCount = sizeof(kTier3) / sizeof(kTier3[0]); }
        else if (currentTier == 2) { fallbackTier = kTier2; fallbackCount = sizeof(kTier2) / sizeof(kTier2[0]); }

        bool useJsonDefs = (tierDefCount > 0);

        // Mesh lookup for fallback path only
        u8 meshLookup[] = {m_meshIdSkeleton, m_meshIdBat, m_meshIdSpider, m_meshIdHuman,
                           findMeshByName("gargoyle"), findMeshByName("necromancer"),
                           findMeshByName("shaman"), findMeshByName("herald")};

        // Skip spawn room + its adjacent room. Reduce enemies in 2-hop rooms.
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
                        def.health, def.moveSpeed, def.detectionRange,
                        def.attackRange, def.attackCooldown, def.damage);
                    Entity* ent = handleGet(m_entities, h);
                    if (ent) {
                        ent->meshId     = def.meshId;
                        ent->materialId = def.materialId;
                        ent->enemyType  = def.enemyType;
                        ent->enemyRole  = def.role;

                        ent->baseMoveSpeed      = ent->moveSpeed;
                        ent->baseAttackCooldown = ent->attackCooldown;

                        // Set initial AI state from JSON aiPreference
                        if (def.role & EnemyRole::AMBUSH) {
                            ent->aiState = AIState::DORMANT;
                            // Reposition ambush enemies to doorways
                            Vec3 doorPos[4];
                            u8 doorCount = LevelGridQuery::findDoorwayCells(
                                m_level.grid, room.x, room.z, room.w, room.d, doorPos, 4);
                            if (doorCount > 0) {
                                u8 pick = static_cast<u8>(std::rand() % doorCount);
                                f32 doorY = def.flying
                                    ? (room.floorHeight + 1.5f)
                                    : (room.floorHeight + def.halfExtents.y);
                                ent->position = {doorPos[pick].x, doorY, doorPos[pick].z};
                                ent->aiState = AIState::AMBUSH;
                            }
                        }
                        if (def.role & EnemyRole::SUMMONER) ent->tacticalTimer = 8.0f;
                        if (def.role & EnemyRole::HEALER)   ent->tacticalTimer = 5.0f;

                        // Floor scaling
                        u32 effectiveFloor = m_level.currentFloor + m_difficulty * 50;
                        ent->level = static_cast<u8>(effectiveFloor > 255 ? 255 : effectiveFloor);
                        f32 floorMult = 1.0f + (effectiveFloor - 1) * GameConst::FLOOR_STAT_MULT;
                        ent->health    *= floorMult;
                        ent->maxHealth  = ent->health;
                        ent->damage    *= floorMult;

                        ent->onHitEffect   = def.onHitEffect;
                        ent->onHitDuration = def.onHitDuration;
                        ent->onHitDps      = def.onHitDps * floorMult;

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
                        tmpl.health, tmpl.moveSpeed, tmpl.detRange,
                        tmpl.atkRange, tmpl.atkCool, tmpl.damage);
                    Entity* ent = handleGet(m_entities, h);
                    if (ent) {
                        ent->meshId = meshLookup[tmpl.meshIdx];
                        ent->materialId = MaterialSystem::getIdByName(tmpl.matName);
                        ent->baseMoveSpeed      = ent->moveSpeed;
                        ent->baseAttackCooldown = ent->attackCooldown;

                        if (std::strstr(tmpl.matName, "gargoyle")) {
                            ent->enemyRole = EnemyRole::AMBUSH;
                            ent->aiState = AIState::DORMANT;
                            Vec3 doorPos[4];
                            u8 doorCount = LevelGridQuery::findDoorwayCells(
                                m_level.grid, room.x, room.z, room.w, room.d, doorPos, 4);
                            if (doorCount > 0) {
                                u8 pick = static_cast<u8>(std::rand() % doorCount);
                                ent->position = {doorPos[pick].x,
                                    tmpl.flying ? (room.floorHeight + 1.5f) : (room.floorHeight + tmpl.halfExtents.y),
                                    doorPos[pick].z};
                                ent->aiState = AIState::AMBUSH;
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
                        ent->level = static_cast<u8>(effectiveFloor > 255 ? 255 : effectiveFloor);
                        f32 floorMult = 1.0f + (effectiveFloor - 1) * GameConst::FLOOR_STAT_MULT;
                        ent->health    *= floorMult;
                        ent->maxHealth  = ent->health;
                        ent->damage    *= floorMult;
                        ent->onHitEffect   = tmpl.onHitEffect;
                        ent->onHitDuration = tmpl.onHitDuration;
                        ent->onHitDps      = tmpl.onHitDps * floorMult;

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
                    }
                }
            }
        }
    }

    // Assign entities to room-based squads now that all enemies are placed
    SquadSystem::rebuild(m_level.squads, dungeon, m_entities);

    // Place 1-2 point lights per room for atmospheric lighting
    {
        // Light color per tier
        Vec3 lightCol = {1.0f, 0.7f, 0.3f}; // default: warm torch
        if      (m_level.currentFloor >= 41) lightCol = {0.6f, 0.4f, 1.0f};  // Void: pale purple
        else if (m_level.currentFloor >= 31) lightCol = {1.0f, 0.4f, 0.1f};  // Hellforge: lava red
        else if (m_level.currentFloor >= 21) lightCol = {0.5f, 0.5f, 1.0f};  // Caverns: crystal blue
        else if (m_level.currentFloor >= 11) lightCol = {0.4f, 0.8f, 0.3f};  // Catacombs: sickly green

        m_pointLightCount = 0;
        for (u32 r = 0; r < dungeon.roomCount && m_pointLightCount < MAX_POINT_LIGHTS; r++) {
            const DungeonRoom& room = dungeon.rooms[r];
            // Center light — slightly above floor for visibility
            f32 cx = (room.x + room.w * 0.5f) * m_level.grid.cellSize;
            f32 cz = (room.z + room.d * 0.5f) * m_level.grid.cellSize;
            m_pointLights[m_pointLightCount++] = {
                {cx, room.floorHeight + 1.8f, cz}, lightCol
            };
            // Second light near doorway for larger rooms
            if (room.w * room.d >= 25 && m_pointLightCount < MAX_POINT_LIGHTS) {
                f32 dx = (room.x + 1.0f) * m_level.grid.cellSize;
                f32 dz = (room.z + 1.0f) * m_level.grid.cellSize;
                m_pointLights[m_pointLightCount++] = {
                    {dx, room.floorHeight + 1.5f, dz}, lightCol * 0.7f
                };
            }
        }
        LOG_INFO("Placed %u point lights for floor %u", m_pointLightCount, m_level.currentFloor);
    }

    // Spawn chests and mimics (1 per room, 20% chance mimic)
    {
        u8 chestMeshId = m_meshIdChest;

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

            if (isMimic) {
                EntityHandle h = EntitySystem::spawn(m_entities,
                    Vec3{cx, cy + 0.25f, cz}, {0.3f, 0.25f, 0.3f}, false,
                    GameConst::MIMIC_HEALTH, 4.0f, GameConst::MIMIC_TRIGGER_DIST,
                    2.0f, 0.6f, GameConst::MIMIC_DAMAGE);
                Entity* ent = handleGet(m_entities, h);
                if (ent) {
                    ent->meshId = chestMeshId;
                    ent->enemyType = EnemyType::MIMIC;
                    ent->aiState = AIState::DORMANT;
                    Collision::ensureNotInWall(ent->position, ent->halfExtents, m_level.grid);
                }
            } else {
                // Real chest: spawn a world item with good loot at this position
                ItemInstance item = ItemGen::rollItem(
                    static_cast<u8>(2 + r / 3), // higher level deeper in dungeon
                    m_itemDefs, m_itemDefCount, m_affixDefs, m_affixDefCount);
                if (!isItemEmpty(item)) {
                    WorldItemSystem::spawn(m_worldItems, item, Vec3{cx, cy + 0.3f, cz}, &m_level.grid);
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Boss roster — unique encounters on milestone floors.
    // Mini-bosses on 5/15/25/35/45, major bosses on 10/20/30/40/50.
    // ---------------------------------------------------------------------------
    u32 bossRoomForExit = 0xFFFFFFFF; // set by boss spawn, used by exit portal placement
    {
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
            { 40, "Diablo",        "NOT EVEN DEATH...",  1600, 30, 3.5f, 4.0f, 0.35f,{0.8f,1.3f, 0.8f}, true,  "butcher",  "boss_diablo",       "sword"},
            { 50, "Grim Reaper",   "YOUR TIME HAS COME.",2500, 30, 4.0f, 4.0f, 0.3f, {0.7f,1.4f, 0.7f}, true,  "skeleton", "boss_reaper",       "axe"},
        };

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

            bossRoom.x = (startX > 0) ? static_cast<u32>(startX) : 1;
            bossRoom.z = (startZ > 0) ? static_cast<u32>(startZ) : 1;
            bossRoom.w = expandW;
            bossRoom.d = expandD;

            m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid, m_level.sections, MAX_LEVEL_SECTIONS);

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
            f32 floorMult = 1.0f + (m_level.currentFloor - 1) * GameConst::FLOOR_STAT_MULT;

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
                bossHp * floorMult, bossSpd, bossDetect,
                bossAtkRng, bossAtkCool, bossDmg * floorMult);
            Entity* boss = handleGet(m_entities, bh);
            if (boss) {
                if (bd) {
                    // JSON-loaded boss path
                    boss->meshId = findMeshByName(bd->meshName);
                    boss->materialId = MaterialSystem::getIdByName(bd->matName);
                    boss->enemyType = EnemyType::BOSS;
                    boss->nameTag = bd->name;
                    u32 bossEffFloor = m_level.currentFloor + m_difficulty * 50;
                    boss->level = static_cast<u8>(bossEffFloor > 255 ? 255 : bossEffFloor);
                    boss->bossLimbConfig = bd->limbConfig;
                    if (bd->weaponName[0] != '\0') {
                        boss->weaponMeshId = findMeshByName(bd->weaponName);
                    }
                    boss->speechText = bd->speech;
                    boss->speechTimer = 6.0f;

                    // New BossDef-specific fields
                    boss->bossDefIdx = bossIdx;
                    boss->enemyRole = bd->roles;
                    boss->minionShield = bd->minionShield;
                    boss->onHitEffect = bd->onHitEffect;
                    boss->onHitDuration = bd->onHitDuration;
                    boss->onHitDps = bd->onHitDps;
                    boss->baseMoveSpeed      = boss->moveSpeed;
                    boss->baseAttackCooldown = boss->attackCooldown;

                    // Ranged bosses use projectile attacks — weapon mesh rides the projectile
                    if (bd->atkRange > 5.0f) {
                        boss->npcWeaponType = WeaponType::PROJECTILE;
                        boss->npcProjectileSpeed = 18.0f;
                        boss->npcProjectileRadius = 0.15f;
                    }
                } else {
                    // Hardcoded fallback path (kBosses)
                    boss->meshId = findMeshByName(bt->meshName);
                    boss->materialId = MaterialSystem::getIdByName(bt->matName);
                    boss->enemyType = EnemyType::BOSS;
                    boss->nameTag = bt->name;
                    u32 bossEffFloor = m_level.currentFloor + m_difficulty * 50;
                    boss->level = static_cast<u8>(bossEffFloor > 255 ? 255 : bossEffFloor);
                    // Hardcoded limb config per floor
                    if (bt->floor == 10) boss->bossLimbConfig = 1; // Andariel: spider legs
                    if (bt->floor == 20) boss->bossLimbConfig = 2; // Mephisto: tentacles
                    if (bt->floor == 40) boss->bossLimbConfig = 3; // Diablo: back spikes
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
                     bossName, m_level.currentFloor, bossHp * floorMult,
                     bossDmg * floorMult, expandW, expandD,
                     boss ? boss->bossLimbConfig : 0,
                     bd ? "json" : "hardcoded");
        }
    }

    LOG_INFO("Spawned %u enemies", EntitySystem::activeCount(m_entities));

    // ---------------------------------------------------------------------------
    // Themed decorations — scatter props that fit the current depth tier.
    //   1-10 Dungeon:  barrels, shackles, cages, bones
    //  11-20 Catacombs: bones, shackles, braziers
    //  21-30 Caverns:   webs, bones, barrels
    //  31-40 Hellforge:  braziers, cages, shackles, bones
    //  41-50 Void:       bones, cages, braziers
    // Each room (except spawn room 0) gets 0-3 random decorations.
    // ---------------------------------------------------------------------------
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

    // Spawn exit portal in the boss room so the boss guards the descent.
    // Falls back to exitRoomIdx on non-boss floors.
    m_level.floorDoorActive = false;
    if (dungeon.roomCount > 1) {
        const DungeonRoom& lastRoom = dungeon.rooms[bossRoomForExit < dungeon.roomCount ? bossRoomForExit : dungeon.exitRoomIdx];
        f32 doorX = (lastRoom.x + lastRoom.w * 0.5f) * m_level.grid.cellSize;
        f32 doorZ = (lastRoom.z + lastRoom.d * 0.5f) * m_level.grid.cellSize;
        f32 doorY = lastRoom.floorHeight;
        m_level.floorDoorPos    = {doorX, doorY, doorZ};
        m_level.floorDoorActive = true;
        LOG_INFO("Floor %u exit portal at (%.1f, %.1f, %.1f)", m_level.currentFloor, doorX, doorY, doorZ);

        // Build BFS flow field so NPCs can pathfind toward the exit
        LevelGridSystem::buildFlowField(m_level.grid, m_level.floorDoorPos);
    }

    // Spawn 4 friendly NPCs in the spawn room — one of each class
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

    // Reset Wanderer transient combat state on every floor entry so parry windows,
    // marks, and ultimates don't carry over across levels in an active state.
    if (m_playerClass == PlayerClass::WANDERER) {
        m_localPlayer.dodgeState      = {};
        m_localPlayer.deflectTimer    = 0.0f;
        m_localPlayer.deflectAbsorbed = 0.0f;
        m_localPlayer.deflectHitCount = 0;
        m_localPlayer.deflectSpeedTimer = 0.0f;
        m_localPlayer.markedEntityIdx = 0xFFFF;
        m_localPlayer.markTimer       = 0.0f;
        m_localPlayer.deathsDanceTimer = 0.0f;
    }

    ProjectileSystem::init(m_projectiles);

    // Reset all entity AI state to prevent stale data from previous floor
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        u32 idx = m_entities.activeList[a];
        if (idx >= MAX_ENTITIES) continue;
        Entity& ent = m_entities.entities[idx];
        ent.lastSeenPos = ent.position;
        ent.flybyTimer = 0.0f;
        ent.flybyTarget = {0, 0, 0};
        ent.hasTargetLOS = false;
        ent.targetEntityIdx = 0xFFFF;
    }

    // Init inventory & world items
    WorldItemSystem::init(m_worldItems);
    // Only reset inventory on a true new game (floor 1, Normal difficulty).
    // Nightmare/Hell transitions set floor=1 but must preserve gear.
    if (m_level.currentFloor <= 1 && m_difficulty == 0) {
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            Inventory::init(m_inventories[i]);
            m_skillStates[i] = SkillState{};
            Quickbar::init(m_quickbars[i], m_inventories[i]);
        }
    }

    // Init players
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        m_players[i] = NetPlayer{};
    }

    // Setup local player
    m_players[m_localPlayerIndex].active = true;
    m_players[m_localPlayerIndex].slotIndex = m_localPlayerIndex;
    m_players[m_localPlayerIndex].position = spawnPos;
    m_players[m_localPlayerIndex].spawnPosition = spawnPos;
    // Only reset health on floor 1 — keep current HP when descending
    if (m_level.currentFloor <= 1) {
        m_players[m_localPlayerIndex].health = 100.0f;
        m_players[m_localPlayerIndex].maxHealth = 100.0f;
    }
    m_players[m_localPlayerIndex].weaponState.currentWeapon = 0;

    // Set player position — both the active alias and the per-player array
    m_localPlayer.position = spawnPos;
    m_localPlayer.yaw = 0.0f;
    m_localPlayer.pitch = 0.0f;
    m_localPlayers[0] = m_localPlayer; // sync to array so swapInPlayer doesn't overwrite
    m_cameras[0] = m_camera;

    m_serverTick = 0;
    m_hitMarkerTimer = 0.0f;

    // Setup net callbacks
    if (m_netRole == NetRole::SERVER) {
        Net::setOnInput(Engine::onInput);
        Net::setOnPlayerJoin(Engine::onPlayerJoin);
        Net::setOnPlayerLeft(Engine::onPlayerLeft);
        Server::init(m_players, m_level.levelSeed);
    } else if (m_netRole == NetRole::CLIENT) {
        Net::setOnSnapshot(Engine::onSnapshot);
        Net::setOnEvent(Engine::onEvent);
        Net::setOnPlayerLeft(Engine::onPlayerLeft);
        Client::init(m_localPlayerIndex);
    }

    // Brief invulnerability on floor entry for all players
    m_localPlayer.invulnTimer = 2.5f;
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (m_players[pi].active) {
            m_players[pi].invulnTimer = 2.5f;
            m_players[pi].isDead = false;
        }
    }

    Input::setRelativeMouseMode(true);
    m_gameState = GameState::IN_GAME;

    // Play tier-appropriate ambient music — OGG Vorbis for smaller binary size
    const char* musicFile = ASSET_PATH("assets/audio/music_tier1.ogg");
    if      (m_level.currentFloor >= 41) musicFile = ASSET_PATH("assets/audio/music_tier5.ogg");
    else if (m_level.currentFloor >= 31) musicFile = ASSET_PATH("assets/audio/music_tier4.ogg");
    else if (m_level.currentFloor >= 21) musicFile = ASSET_PATH("assets/audio/music_tier3.ogg");
    else if (m_level.currentFloor >= 11) musicFile = ASSET_PATH("assets/audio/music_tier2.ogg");
    AudioSystem::playMusic(musicFile);
}

