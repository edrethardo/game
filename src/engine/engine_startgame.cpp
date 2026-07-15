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
#include "net/pending_hit_ring.h"
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
extern u16  s_sourceShards;   // secret superboss key — session-only set of collected shards
extern bool s_engineSlain;    // secret superboss — Engine defeated this session (victory variant)

// ---------------------------------------------------------------------------
// NPC equipment and spawning helpers
// ---------------------------------------------------------------------------

void Engine::rollNpcEquipment(NpcEquipment& equip, NpcClass npcClass, u8 floor) {
    // Clear all slots. The named local sidesteps a GCC 13 gimplifier ICE on the
    // direct `equip.equipped[s] = ItemInstance{};` form — see the explanatory comment
    // in handleDropRequest (engine_update.cpp).
    ItemInstance emptySlot;
    for (u32 s = 0; s < static_cast<u32>(ItemSlot::COUNT); s++) {
        equip.equipped[s] = emptySlot;
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

// Fully initialize one local lane as a brand-new character: wipe inventory/quickbar/skills, grant
// the class starting loadout, and set class base HP / move / energy. m_playerClasses[lane] must
// already be set (the menu's class-select does this). Shared by startGame's NEW_GAME path and by
// the couch menu, which prepares each fresh lane before a `lanesPrepared` start so New and Continue
// heroes can coexist without the NEW_GAME wipe erasing a loaded character.
void Engine::equipFreshLane(u8 lane) {
    if (lane >= MAX_LOCAL_PLAYERS) return;
    // Fresh New Game character — it owns its target slot outright, so its saves
    // overwrite at the real floor (the no-downgrade guard must not protect the
    // overwritten character's old progress). See m_laneLoadedFromSave.
    m_laneLoadedFromSave[lane] = false;
    Inventory::init(m_inventories[lane]);
    m_skillStates[lane] = SkillState{};
    Quickbar::init(m_quickbars[lane], m_inventories[lane]);
    equipStartingLoadout(lane);

    const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClasses[lane])];
    m_skillStates[lane].maxEnergy        = cls.baseEnergy + m_inventories[lane].bonusEnergyFlat;
    m_skillStates[lane].energy           = m_skillStates[lane].maxEnergy;
    m_localPlayers[lane].baseMaxHealth   = cls.baseHealth;   // maxHealth is derived from this + gear
    m_localPlayers[lane].maxHealth       = cls.baseHealth;
    m_localPlayers[lane].health          = cls.baseHealth;
    m_localPlayers[lane].moveSpeed       = cls.baseMoveSpeed;
    m_localPlayers[lane].damageReduction = (m_playerClasses[lane] == PlayerClass::WARRIOR) ? 0.3f : 0.0f;
}

void Engine::startGame(GameStart mode, bool lanesPrepared) {
    // Reset first-kill guaranteed drop for this floor
    s_firstKillDropGiven = false;
    // Death-overlay cursor edge flag: clear so a stale value from a prior session can't
    // suppress freeing the cursor on the next networked death (engine_update.cpp loop).
    m_deathCursorFree = false;

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
        // Secret superboss key is session-only and resets on a true run-start (NOT on DESCEND, so
        // shards persist across the floor-50 → next-tier loop). See ~/.claude/plans.
        s_sourceShards = 0;
        s_engineSlain  = false;
        m_level.inSourceChamber    = false;
        m_level.sourcePortalActive = false;
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

    // Fold the floor number into the mesh seed so each floor's baked tile-shade + scatter-prop
    // pattern is distinct (levelSeed alone is constant across a run's floors). Deterministic:
    // host + clients share both levelSeed and currentFloor, so baked floors match everywhere.
    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid,
                             m_level.levelSeed + m_level.currentFloor * 7919u,
                             m_level.sections, MAX_LEVEL_SECTIONS);
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

    // Build the clearance field now the grid geometry is final (post boss-arena
    // expansion). Used by enemy pathfinding to keep paths off wall corners.
    LevelGridSystem::buildClearanceField(m_level.grid);

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

    // Clear the world-item pool BEFORE anything is placed into it. This used to sit ~30 lines below,
    // AFTER spawnFloorShrines — so every shrine was created, logged ("Shrine of Power placed in room
    // 3"), and then wiped by this reset before the first frame. Shrines could not appear in a real
    // game at all; the log line was true at the moment it printed and false a moment later.
    // Anything that spawns a WorldItem must therefore stay below this call.
    WorldItemSystem::init(m_worldItems);

    // Floor event (0 or 1 per floor — currently the loot goblin). Deliberately AFTER spawnFloorBoss
    // and buildClearanceField: the boss call mutates the boss room's geometry and rebuilds the level
    // mesh, so anything placed earlier could be swallowed by the arena expansion, and the goblin
    // needs the clearance/flow fields to path away from the player.
    spawnFloorEvents(dungeon);
    spawnFloorShrines(dungeon);

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

    // Init inventory. (The world-item pool is reset ABOVE, before the floor's shrines and event
    // items are placed into it — resetting it here destroyed them.)
    // Inventory is reset + starter gear granted ONLY on a brand-new run. CONTINUE
    // keeps what loadGame() restored; DESCEND keeps the current run's gear. The mode
    // makes the intent explicit instead of guessing from floor/difficulty/empty-slot.
    // Clear cross-floor SkillSystem state (meteors, overcharge, holy bombardment/nova timers)
    // so a skill warming up on the old floor doesn't trigger on the new one.
    SkillSystem::resetGameplayState();
    if (mode == GameStart::NEW_GAME && !lanesPrepared) {
        // Clear every slot (including inactive ones) so no stale gear leaks in, then fully equip
        // each active local lane (loadout + class base stats + per-lane energy via equipFreshLane).
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            Inventory::init(m_inventories[i]);
            m_skillStates[i] = SkillState{};
            Quickbar::init(m_quickbars[i], m_inventories[i]);
        }
        for (u8 pi = 0; pi < m_splitPlayerCount; pi++) equipFreshLane(pi);
    }
    // When lanesPrepared (couch co-op start), the menu already populated every lane — Continue'd
    // heroes keep their loaded gear/HP and fresh lanes were set up via equipFreshLane — so the
    // wipe+grant above is skipped to avoid erasing a loaded Player 2 (mixed New/Continue couch).

    // Clear per-local-player death flags on any floor/game (re)entry so nobody starts a
    // floor frozen behind the respawn overlay (defensive; the descend path also clears).
    for (u8 p = 0; p < MAX_LOCAL_PLAYERS; p++) m_playerDead[p] = false;

    // Init players.
    // On a CLIENT DESCEND the server hasn't re-sent our HP yet (full authoritative
    // client HP replication lands in a later wave), and NetPlayer{} defaults to
    // health=maxHealth=100 — which the first IN_GAME frame's syncNetPlayerToLocalPlayer
    // would copy into m_localPlayer, clobbering carried HP + per-floor growth. Preserve
    // the local slot's HP/maxHealth across the reset so the descend keeps it.
    f32 carriedHealth = m_players[activeNetSlot()].health;       // local player's net slot
    f32 carriedMaxHealth  = m_players[activeNetSlot()].maxHealth;
    f32 carriedBaseHealth = m_players[activeNetSlot()].baseMaxHealth;   // the growth component
    // Net host only: the wipe below clears every slot's `active`, and just below we
    // re-activate ONLY the local (host) slot — which would silently drop every connected
    // client on a floor DESCEND (onPlayerJoin runs on connect, not on descend). Remember
    // which remote slots were occupied (and their carried HP/class) so we can re-spawn
    // them on the new floor after the reset. See the restore block after local setup.
    bool        netSlotWasActive[MAX_PLAYERS] = {};
    f32         netSlotHealth[MAX_PLAYERS]    = {};
    f32         netSlotMaxHealth[MAX_PLAYERS] = {};
    PlayerClass netSlotClass[MAX_PLAYERS]     = {};
    if (m_netRole == NetRole::SERVER) {
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            netSlotWasActive[i] = m_players[i].active;
            netSlotHealth[i]    = m_players[i].health;
            netSlotMaxHealth[i] = m_players[i].maxHealth;
            netSlotClass[i]     = m_players[i].playerClass;
        }
    }
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        m_players[i] = NetPlayer{};
    }
    if (m_netRole == NetRole::CLIENT && mode == GameStart::DESCEND) {
        m_players[activeNetSlot()].health    = carriedHealth;
        m_players[activeNetSlot()].maxHealth     = carriedMaxHealth;
        m_players[activeNetSlot()].baseMaxHealth = carriedBaseHealth;   // or the floor growth resets
    }

    // Setup local player at its NET slot (client slot may be >=1; m_localPlayerIndex is the
    // split-screen lane and is 0 on a client). slotIndex MUST be the net slot too — the
    // client's reconcile matches the snapshot player by slotIndex == its net slot.
    m_players[activeNetSlot()].active = true;
    m_players[activeNetSlot()].slotIndex = activeNetSlot();
    m_players[activeNetSlot()].position = spawnPos;
    m_players[activeNetSlot()].spawnPosition = spawnPos;
    // Reset health to the class base only on a brand-new run. CONTINUE keeps the
    // saved HP and DESCEND keeps the current run's HP (the old code hard-reset to
    // 100 on every floor<=1 entry, which clobbered class HP and continued saves).
    // lanesPrepared (couch) also skips this: each lane's NetPlayer HP syncs from the
    // prepared/loaded m_localPlayers[lane] on the first per-player frame, so a Continue'd
    // hero isn't reset to class base.
    if (mode == GameStart::NEW_GAME && !lanesPrepared) {
        for (u8 pi = 0; pi < m_splitPlayerCount; pi++) {
            const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClasses[pi])];
            m_players[pi].health = cls.baseHealth;
            m_players[pi].baseMaxHealth = cls.baseHealth;
            m_players[pi].maxHealth     = cls.baseHealth;
            m_players[pi].moveSpeed = cls.baseMoveSpeed;
        }
    }
    // Also seed the host's own NetPlayer moveSpeed every startGame entry — covers DESCEND
    // and CONTINUE (NEW_GAME branch above already wrote it). The host's m_localPlayer
    // gets baseMoveSpeed at character-create; this keeps the NetPlayer mirror in sync so
    // future remote-side reads (chain-damage gate, etc.) see the right value.
    {
        const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClasses[m_localPlayerIndex])];
        m_players[activeNetSlot()].moveSpeed = cls.baseMoveSpeed;
    }
    m_players[activeNetSlot()].weaponState.currentWeapon = 0;

    // Net host: re-establish the remote players that were connected before this floor's
    // reset, re-spawned on the new floor. Without this, a DESCEND leaves their slots
    // inactive, so the server stops applying their input (frozen ack -> the client
    // fallback-snaps in place and can't move) and omits them from snapshots (client sees
    // Players: 0). Mirrors onPlayerJoin's core fields; inventory/skills/quickbar persist
    // across DESCEND (not wiped), so they're left intact. currentWeapon self-corrects from
    // the client's next input. Server-only: clients rebuild remote players from snapshots.
    if (m_netRole == NetRole::SERVER && mode == GameStart::DESCEND) {
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            if (i == m_localPlayerIndex || !netSlotWasActive[i]) continue;
            NetPlayer& np = m_players[i];
            np.active        = true;
            np.slotIndex     = static_cast<u8>(i);
            np.position      = spawnPos;
            np.spawnPosition = spawnPos;
            // Carry HP across the descend, but a remote that was DEAD on the old floor revives
            // to full on the new one — keeping health 0 with isDead=false would make
            // serverNetPost's death check re-kill it instantly on arrival.
            np.health        = (netSlotHealth[i] > 0.0f) ? netSlotHealth[i] : netSlotMaxHealth[i];
            np.maxHealth     = netSlotMaxHealth[i];
            np.playerClass   = netSlotClass[i];
            // Re-seed moveSpeed from the (possibly different) class each descend — covers a
            // mid-run class change and matches m_localPlayer's per-floor reset path.
            np.moveSpeed     = kClassDefs[static_cast<u32>(netSlotClass[i])].baseMoveSpeed;
            np.weaponState.currentWeapon = 0; // self-corrects from next client input
            np.isDead        = false;
            np.invulnTimer   = 2.0f;          // match floor-entry spawn protection
            // Symmetric with the host's per-floor Wanderer reset above (line ~540): clear
            // transient mark-prey / shadow-dance state so timers don't carry across descend.
            np.shadowDanceTimer = 0.0f;
            np.markTimer        = 0.0f;
            np.markSpeedStacks  = 0;
            for (u32 ms = 0; ms < 20; ms++) np.markSpeedTimers[ms] = 0.0f;
            // Also clear other transient ring-passive state (Soul Harvest, etc.) so a fresh
            // floor starts clean for remotes the same way it does for the host.
            np.soulHarvestTimer  = 0.0f;
            np.soulHarvestStacks = 0;
            np.smokeTimer        = 0.0f;
            np.secondWindCooldown = 0.0f;
        }
    }

    // Set player position — both the active alias and the per-player array.
    // In split-screen the active alias may currently hold P2's data (the update()
    // per-player loop always ends with sp = m_splitPlayerCount - 1, and the alias
    // is never reset after the loop). Without this swap-in, `m_localPlayers[0] =
    // m_localPlayer` below would clobber P0's struct (HP/maxHealth/status timers/
    // dodgeState/etc.) with P2's. Restore the alias to P0 first so the assignment
    // targets the right per-player slot.
    if (m_splitPlayerCount > 1) swapInPlayer(0);
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

    // R17 cooldown coherence: m_serverTick just reset to 0, but the tick-based skill/potion cooldown
    // gate compares against these lastActivationTick values via currentLocalTick() (= m_serverTick for
    // NONE/SERVER). A stale prior-floor tick would make (0 - staleLarge + grace) u32-underflow; zero
    // them so the new floor starts with the LOCAL player's skills/potion "never activated" = ready
    // (the pre-R17 fresh-floor behaviour, made explicit instead of relying on the underflow).
    // Remotes gate on their own advancing clientTick, so they're unaffected by this host-side reset.
    m_potionLastActivationTick = 0;
    for (u32 s = 0; s < 4; s++) m_classSkillStates[s].lastActivationTick = 0;
    m_bootSkillStates[m_localPlayerIndex].lastActivationTick   = 0;
    m_helmetSkillStates[m_localPlayerIndex].lastActivationTick = 0;

    // Phase 1.1 — Wipe the CL_FIRE_WEAPON dedup rings + the client retransmit window.
    // m_serverTick just reset to 0, so without this the very first fire on the new
    // floor (clientTick=0) could collide with a leftover clientTick=0 from the prior
    // floor's history and be silently squashed as a duplicate.
    resetAllFireDedup();

    // M11.2/D7.3v2 — Clear the delta-compression ACK cache AND the snapshot history ring.
    // m_serverTick just reset to 0; a prior floor's history entries would otherwise collide
    // with the new session's small tick numbers and delta against a snapshot of a DIFFERENT
    // WORLD. Every client re-anchors with full snapshots until its first new ack round-trips.
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        m_clientAckedSnap[i] = 0;
    }
    m_snapHistoryHead  = 0;
    m_snapHistoryCount = 0;

    // Phase 3.1 — Drop the lag-comp history. The prior floor's entity poses (in
    // geometrically unrelated rooms) would otherwise produce wildly wrong rewound
    // positions for the first ~16 ticks on the new floor.
    resetEntityHistory();

    // Setup net callbacks
    if (m_netRole == NetRole::SERVER) {
        Net::setOnInput(Engine::onInput);
        Net::setOnPickup(Engine::onPickup); // server-authoritative loot pickup (N5)
        Net::setOnMeteor(Engine::onMeteor); // client-predicted proc meteor → authoritative spawn
        Net::setOnDropItem(Engine::onDropItem); // R11: server-authoritative inventory drop
        Net::setOnRespawn(Engine::onRespawn); // server-authoritative client respawn
        Net::setOnDescendRequest(Engine::onDescendRequest); // remote-initiated floor descent
        Net::setOnFireWeapon(Engine::onFireWeapon); // client-side weapon fire prediction (CL_FIRE_WEAPON)
        Net::setOnInventorySync(Engine::onInventorySync); // join-with-save inventory push
        Net::setOnTimePing(Engine::onTimePing); // clock-sync handshake (M1.4)
        Net::setOnPlayerJoin(Engine::onPlayerJoin);
        Net::setOnPlayerLeft(Engine::onPlayerLeft);
        Server::init(m_players, m_level.levelSeed,
                     static_cast<u8>(m_level.currentFloor), m_difficulty);
        // The input-tick space restarts with the buffers Server::init just cleared, so both
        // drain-side watermarks restart with it — a stale activation watermark from the previous
        // floor would silently swallow every press until the new ticks caught up to it.
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            m_starvedRepeats[i]     = 0;
            m_lastActivationTick[i] = 0;
        }
    } else if (m_netRole == NetRole::CLIENT) {
        Net::setOnSnapshot(Engine::onSnapshot);
        Net::setOnEvent(Engine::onEvent);
        Net::setOnPlayerLeft(Engine::onPlayerLeft);
        // Follow the host's mid-run floor descents (server-authoritative).
        Net::setOnLevelSeed(Engine::onLevelSeed);
        // Clock-sync pong decoder (M1.5) — feeds incoming SV_TIME_PONG into ClockSync.
        Net::setOnTimePong(Engine::onTimePong);
        // M10.2/M10.3 — reliable server-confirms for predicted hits and incoming damage.
        Net::setOnDamageDone(Engine::onDamageDone);
        Net::setOnDamageToMe(Engine::onDamageToMe);
        // D1.1/D1.2/D1.3 — reliable event packets for kills, pickup results, loot spawns.
        Net::setOnKill(Engine::onKill);
        Net::setOnPickupResult(Engine::onPickupResult);
        Net::setOnLootSpawn(Engine::onLootSpawn);
        Net::setOnEnergyGain(Engine::onEnergyGain); // server-granted manasteal / mana-on-kill
        // Client::init resets the (shared) snapshot ring + per-lane send windows; pass lane 0's net
        // slot for the s_localPlayerIndex back-compat accessor. Online couch co-op reconciles each
        // lane by the explicit slot passed to Client::reconcile, so this single call covers both.
        Client::init(m_clientNetSlot[0]); // must match snapshot slotIndex / ack key
        // Bootstrap the clock-sync subsystem so reconnects start with a clean estimate.
        // Ping state also resets so clientNetPre sends the 3 handshake pings on the
        // first ticks of the new session.
        ClockSyncOps::reset(m_clockSync);
        m_pingsSent = 0;
        m_lastPingSentSec = 0.0;
        // M3.2/M4 — clear each lane's prediction ring, last-reconciled ack, and smooth-correction
        // offset so stale tick-keyed entries from a prior session don't produce false divergence.
        for (u8 lane = 0; lane < MAX_LOCAL_PLAYERS; lane++) {
            PredictionRingOps::reset(m_predictionRing[lane]);
            m_lastReconciledTick[lane] = 0;
            m_renderOffset[lane].offset = {0, 0, 0};
        }
        // M6 — Clear the pending-hits ring so stale predictions from a prior session
        // don't get acked (or mismatched) against the new connection's server events.
        PendingHitRingOps::reset(m_pendingHits);
        // M7 — Clear the pending-damage ring so visual-feedback predictions from a
        // prior session don't carry over into the new connection.
        PendingDamageRingOps::reset(m_pendingDamage);
        // Companion of the unpredicted-damage detector (clientNetPost): a stale low value from a
        // prior session is harmless (min() only ever suppresses fires), but start clean anyway.
        for (u32 i = 0; i < MAX_LOCAL_PLAYERS; i++) m_lastAdoptedHp[i] = 0.0f;
        // M8 — Clear the pending-pickups ring so world-item disappearance predictions
        // from a prior session don't suppress items in the new connection's world.
        PendingPickupRingOps::reset(m_pendingPickups);
        // M9 — Clear the pending-skills ring so skill activation predictions from a prior
        // session don't get matched (or mismatched) against new connection's server events.
        PendingSkillRingOps::reset(m_pendingSkills);
        // Fresh network join only (mode != DESCEND AND no save loaded): a brand-new
        // joiner has no save to restore from, so locally mirror the deterministic
        // starting loadout the server grants this slot in onPlayerJoin. Both ends thus
        // agree on the joiner's gear for a new run.
        //
        // Skip cases:
        //   - DESCEND: re-entering startGame on floor change, must KEEP accumulated state.
        //   - m_clientLoadedFromSave: joiner picked "Continue" with a saved character;
        //     loadGame already populated m_inventories/m_quickbars/skills locally and
        //     updateLobby will push them to the host via CL_INVENTORY_SYNC. Wiping here
        //     would discard the saved gear before we even finish joining.
        if (mode != GameStart::DESCEND && !m_clientLoadedFromSave) {
            Inventory::init(m_inventories[m_localPlayerIndex]);
            m_skillStates[m_localPlayerIndex] = SkillState{};
            Quickbar::init(m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex]);
            m_playerClasses[m_localPlayerIndex] = m_playerClass; // ensure loadout uses chosen class
            equipStartingLoadout(m_localPlayerIndex);
            const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
            m_skillStates[m_localPlayerIndex].maxEnergy = cls.baseEnergy + m_inventories[m_localPlayerIndex].bonusEnergyFlat;
            m_skillStates[m_localPlayerIndex].energy    = m_skillStates[m_localPlayerIndex].maxEnergy;
        }
    }

    // Brief invulnerability on floor entry for all players
    m_localPlayer.invulnTimer = 2.0f;
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (m_players[pi].active) {
            m_players[pi].invulnTimer = 2.0f;
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

