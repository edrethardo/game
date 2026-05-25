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

// Equip the class starting weapon for one local player. Centralizes the loadout
// logic that used to be copy-pasted across the menu start paths; called from
// startGame() on a NEW_GAME only. Floor-1 shield drops are separate world-item
// logic (the player picks those up), so this grants the weapon only.
void Engine::equipStartingLoadout(u8 playerIdx) {
    const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClasses[playerIdx])];
    for (u32 wi = 0; wi < m_itemDefCount; wi++) {
        if (std::strcmp(m_itemDefs[wi].name, cls.startingWeaponName) != 0) continue;
        ItemInstance wpn;
        wpn.defId = static_cast<u16>(wi);
        wpn.rarity = Rarity::COMMON;
        wpn.itemLevel = 1;
        wpn.damage = m_itemDefs[wi].baseDamage;
        wpn.uid = m_worldItems.nextUid++;
        m_inventories[playerIdx].equipped[static_cast<u32>(ItemSlot::WEAPON)] = wpn;
        Inventory::recalculateStats(m_inventories[playerIdx]);
        Quickbar::syncWeaponSlot(m_quickbars[playerIdx], m_inventories[playerIdx]);
        break;
    }
}

void Engine::startGame(GameStart mode) {
    // Reset first-kill guaranteed drop for this floor
    s_firstKillDropGiven = false;

    // Reset per-floor stats for transition screen
    m_transition.floorKillCount = 0;
    m_transition.floorTime = 0.0f;

    // Every floor opens with a brief calm window so the player isn't dropped into
    // an already-raging fight (enemies don't auto-aggro, friendly NPCs wait).
    m_spawnCalmTimer = GameConst::SPAWN_CALM_SECONDS;

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

    // Build level. The dungeon layout comes from a dedicated PER-RUN seed
    // (m_level.levelSeed), NOT the global std::rand() that gameplay (loot/procs/spawns)
    // advances at different rates on host vs client. NEW_GAME mints a fresh run seed from
    // entropy (so runs vary); DESCEND keeps it; CONTINUE already has it (restored from a
    // save, or set from the server's JOIN_ACCEPT on a network client). This makes the
    // dungeon reproducible per run AND identical across host/client.
    if (mode == GameStart::NEW_GAME) {
        m_level.levelSeed = static_cast<u32>(std::rand()); // global RNG is srand()-seeded at init
        m_level.savedSeed = m_level.levelSeed;             // persist the run seed
    }
    // Floor + difficulty fold in so each floor and each difficulty-loop tier differs.
    u32 dungeonSeed = m_level.levelSeed
                    + m_level.currentFloor * 7919u
                    + static_cast<u32>(m_difficulty) * 104729u;
    // Early floors (1-9) use a smaller grid for simpler, more linear layouts
    // so the exit is easier to find.  Later floors get the full 48x48 grid.
    u32 gridSize = 48;
    if (m_level.currentFloor <= 3)       gridSize = 24;  // very small, almost linear
    else if (m_level.currentFloor <= 6)  gridSize = 32;  // small, few branches
    else if (m_level.currentFloor <= 9)  gridSize = 40;  // medium, some exploration

    // Generate the level once — spawn/exit room selection always succeeds
    // by falling back to the best available rooms.
    LevelGridSystem::init(m_level.grid, gridSize, gridSize, 1.0f);
    m_level.dungeon = LevelGen::generate(m_level.grid, dungeonSeed, gridSize, gridSize);
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

    // Determine tier for enemy spawning (1-5 based on current floor)
    u8 currentTier = 1;
    if      (m_level.currentFloor >= 41) currentTier = 5;
    else if (m_level.currentFloor >= 31) currentTier = 4;
    else if (m_level.currentFloor >= 21) currentTier = 3;
    else if (m_level.currentFloor >= 11) currentTier = 2;

    // Spawn enemies procedurally — themed variants + unique monsters per tier
    spawnFloorEnemies(dungeon, currentTier);

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
    spawnFloorChests(dungeon);

    // Spawn the milestone boss if one exists for this floor.
    // spawnFloorBoss mutates dungeon.rooms[bossRoom] (arena expansion) and rebuilds
    // the level mesh — the exit-portal placement below depends on the post-boss grid.
    u32 bossRoomForExit = spawnFloorBoss(dungeon);
    // Record whether this floor actually has a boss — gates the exit lock so the
    // portal is only sealed on boss floors (not regular floors like 21).
    m_level.floorHasBoss = (bossRoomForExit != 0xFFFFFFFF);

    LOG_INFO("Spawned %u enemies", EntitySystem::activeCount(m_entities));

    // ---------------------------------------------------------------------------
    // Themed decorations — scatter props that fit the current depth tier.
    // ---------------------------------------------------------------------------
    spawnFloorDecorations(dungeon);

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

    // Spawn friendly NPCs in the spawn room (cleric, archer, rogue)
    spawnFloorNpcs(dungeon);

    // Reset Wanderer transient combat state on every floor entry so parry windows,
    // marks, and ultimates don't carry over across levels in an active state.
    if (m_playerClass == PlayerClass::WANDERER) {
        m_localPlayer.dodgeState      = {};
        m_localPlayer.deflectTimer    = 0.0f;
        m_localPlayer.deflectAbsorbed = 0.0f;
        m_localPlayer.deflectHitCount = 0;
        m_localPlayer.deflectSpeedTimer = 0.0f;
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
    // Inventory is reset + starter gear granted ONLY on a brand-new run. CONTINUE
    // keeps what loadGame() restored; DESCEND keeps the current run's gear. The mode
    // makes the intent explicit instead of guessing from floor/difficulty/empty-slot.
    if (mode == GameStart::NEW_GAME) {
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            Inventory::init(m_inventories[i]);
            m_skillStates[i] = SkillState{};
            Quickbar::init(m_quickbars[i], m_inventories[i]);
        }
        // Grant the class starting weapon to each active local player.
        for (u8 pi = 0; pi < m_splitPlayerCount; pi++) equipStartingLoadout(pi);
        // The stats-changed callback recomputes maxEnergy from the *active* alias
        // (m_localPlayerIndex / m_playerClass), but no swapInPlayer has run yet in this
        // loop, so it wrote P0's energy for every iteration. Set each local player's
        // energy ceiling explicitly from its own class + energy affixes.
        for (u8 pi = 0; pi < m_splitPlayerCount; pi++) {
            const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClasses[pi])];
            m_skillStates[pi].maxEnergy = cls.baseEnergy + m_inventories[pi].bonusEnergyFlat;
            m_skillStates[pi].energy    = m_skillStates[pi].maxEnergy;
        }
    }

    // Clear per-local-player death flags on any floor/game (re)entry so nobody starts a
    // floor frozen behind the respawn overlay (defensive; the descend path also clears).
    for (u8 p = 0; p < MAX_LOCAL_PLAYERS; p++) m_playerDead[p] = false;

    // Init players
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        m_players[i] = NetPlayer{};
    }

    // Setup local player
    m_players[m_localPlayerIndex].active = true;
    m_players[m_localPlayerIndex].slotIndex = m_localPlayerIndex;
    m_players[m_localPlayerIndex].position = spawnPos;
    m_players[m_localPlayerIndex].spawnPosition = spawnPos;
    // Reset health to the class base only on a brand-new run. CONTINUE keeps the
    // saved HP and DESCEND keeps the current run's HP (the old code hard-reset to
    // 100 on every floor<=1 entry, which clobbered class HP and continued saves).
    if (mode == GameStart::NEW_GAME) {
        for (u8 pi = 0; pi < m_splitPlayerCount; pi++) {
            const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClasses[pi])];
            m_players[pi].health = cls.baseHealth;
            m_players[pi].maxHealth = cls.baseHealth;
        }
    }
    m_players[m_localPlayerIndex].weaponState.currentWeapon = 0;

    // Set player position — both the active alias and the per-player array
    m_localPlayer.position = spawnPos;
    m_localPlayer.yaw = 0.0f;
    m_localPlayer.pitch = 0.0f;
    m_localPlayer.eyeHeight = 1.7f; // reset view-bob offset so the camera snap is level
    m_localPlayers[0] = m_localPlayer; // sync to array so swapInPlayer doesn't overwrite
    // Snap the camera onto the new spawn (prev == current) BEFORE saving it, so the
    // first IN_GAME frame doesn't interpolate from the old floor's camera position.
    snapCameraToPlayer();
    m_cameras[0] = m_camera;

    m_serverTick = 0;
    m_hitMarkerTimer = 0.0f;

    // Setup net callbacks
    if (m_netRole == NetRole::SERVER) {
        Net::setOnInput(Engine::onInput);
        Net::setOnPlayerJoin(Engine::onPlayerJoin);
        Net::setOnPlayerLeft(Engine::onPlayerLeft);
        Server::init(m_players, m_level.levelSeed,
                     static_cast<u8>(m_level.currentFloor), m_difficulty);
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

