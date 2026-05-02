// Top-level engine: owns all pools, defs, and networking state.
// Drives the fixed-timestep loop in run() (60 Hz update, render once per frame).
// update() dispatches by GameState (MENU / LOBBY_* / IN_GAME) and, in-game,
// by NetRole (NONE -> singleplayerUpdate, SERVER -> serverUpdate, CLIENT -> clientUpdate).
// init() loads shaders/meshes/materials/JSON defs, registers Combat death callback
// (rolls loot drop), and sets up Net callbacks. startGame() generates the dungeon
// and spawns enemies. See CLAUDE.md for the full subsystem map and lifecycles.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "engine/engine.h"
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
#include "game/limb_system.h"
#include "game/projectile.h"
#include "game/item.h"
#include "game/skill.h"
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

#include <glad/glad.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static FrameAllocator s_frameAllocator;

// Global engine pointer for static callbacks
static Engine* s_engine = nullptr;

// ---------------------------------------------------------------------------
// Spawn helpers for test enemies
// ---------------------------------------------------------------------------
static void spawnTestEnemies(EntityPool& pool, const LevelGrid& grid) {
    struct SpawnInfo { f32 x, z; bool flying; };
    SpawnInfo spawns[] = {
        {15.5f, 4.5f, false}, {17.5f, 6.5f, false}, {19.0f, 4.0f, false},
        {24.5f, 4.5f, false}, {26.5f, 5.5f, false}, {25.5f, 4.5f, true},
        {4.5f, 14.5f, false}, {6.0f, 17.0f, false}, {5.0f, 15.5f, true},
        {16.5f, 15.5f, false}, {19.5f, 16.5f, false}, {17.5f, 15.0f, true},
        {20.0f, 17.0f, true},
    };

    for (auto& s : spawns) {
        u32 gx = static_cast<u32>(s.x);
        u32 gz = static_cast<u32>(s.z);
        f32 floorH = 0.0f;
        if (LevelGridSystem::isInBounds(grid, gx, gz) &&
            !LevelGridSystem::isSolid(grid, gx, gz)) {
            floorH = LevelGridSystem::getFloorHeight(grid, gx, gz);
        }

        Vec3 halfExtents = s.flying ? Vec3{0.3f, 0.3f, 0.3f} : Vec3{0.4f, 0.5f, 0.4f};
        f32 spawnY = s.flying ? (floorH + 1.5f) : (floorH + halfExtents.y);

        EntitySystem::spawn(pool,
            Vec3{s.x, spawnY, s.z}, halfExtents, s.flying,
            s.flying ? 30.0f : 50.0f, s.flying ? 4.0f : 2.5f,
            15.0f, s.flying ? 8.0f : 2.5f,
            s.flying ? 1.5f : 1.0f, s.flying ? 8.0f : 10.0f);
    }
}

// ---------------------------------------------------------------------------
// Net callbacks (static — forwarded to engine)
// ---------------------------------------------------------------------------
void Engine::onSnapshot(const u8* data, u32 size) {
    Client::receiveSnapshot(data, size);
}

void Engine::onInput(u8 playerSlot, const u8* data, u32 size) {
    Server::receiveInput(playerSlot, data, size);
}

void Engine::onEvent(const u8* data, u32 size) {
    (void)data; (void)size;
    // TODO: handle damage events for cosmetic feedback
}

void Engine::onPlayerJoin(u8 playerSlot) {
    if (!s_engine) return;
    if (playerSlot < MAX_PLAYERS) {
        NetPlayer& np = s_engine->m_players[playerSlot];
        np.active = true;
        np.slotIndex = playerSlot;
        np.health = 100.0f;
        np.maxHealth = 100.0f;
        np.position = s_engine->m_players[0].spawnPosition; // spawn at host's spawn
        np.spawnPosition = np.position;
        np.weaponState.currentWeapon = 0;
        np.weaponState.cooldownTimer = 0.0f;
        LOG_INFO("Engine: player %u joined, spawned at (%.1f, %.1f, %.1f)",
                 playerSlot, np.position.x, np.position.y, np.position.z);
    }
}

void Engine::onPlayerLeft(u8 playerSlot) {
    if (!s_engine) return;
    if (playerSlot < MAX_PLAYERS) {
        s_engine->m_players[playerSlot].active = false;
        LOG_INFO("Engine: player %u left", playerSlot);
    }
}

// ---------------------------------------------------------------------------
// Engine lifecycle
// ---------------------------------------------------------------------------
void Engine::init() {
    s_engine = this;

    Log::init();
    LOG_INFO("Engine initializing...");

    if (!Window::init("DungeonEngine", 1280, 720)) {
        LOG_ERROR("Failed to initialize window");
        return;
    }

    if (!GLContext::init(Window::getHandle())) {
        LOG_ERROR("Failed to initialize GL context");
        return;
    }

    Clock::init();
    Input::init();
    Input::setRelativeMouseMode(false); // Start with mouse visible for menu

    s_frameAllocator.init(1024 * 1024);
    AllocationTracker::init();

    Renderer::init();
    DebugDraw::init();
    HUD::init();
    FontSystem::init();
    ItemIconSystem::init();
    // NOTE: LimbSystem::init is called later, after OBJ meshes are loaded

    // Shaders
    m_basicShader = ShaderSystem::load("assets/shaders/basic.vert",
                                       "assets/shaders/basic.frag");
    m_unlitShader = ShaderSystem::load("assets/shaders/unlit.vert",
                                       "assets/shaders/unlit.frag");

    // Materials (loads textures from assets/materials.json)
    MaterialSystem::init("assets/materials.json");

    // Meshes
    m_cubeMesh = MeshSystem::createCube();

    // Build procedural hand mesh for viewmodel (palm + 4 fingers)
    {
        // Helper to accumulate box faces into flat vertex/index arrays
        struct BoxBuilder {
            Vertex verts[200];
            u32 indices[360];
            u32 vc = 0, ic = 0;

            void addBox(Vec3 min, Vec3 max) {
                u32 base = vc;
                Vec3 corners[8] = {
                    {min.x, min.y, min.z}, {max.x, min.y, min.z},
                    {max.x, max.y, min.z}, {min.x, max.y, min.z},
                    {min.x, min.y, max.z}, {max.x, min.y, max.z},
                    {max.x, max.y, max.z}, {min.x, max.y, max.z},
                };
                Vec3 normals[6] = {
                    {0,0,-1}, {0,0,1}, {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}
                };
                // Each face: 4 corners in CCW winding, 2 triangles
                u32 faceIndices[6][4] = {
                    {0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {4,5,1,0}, {3,2,6,7}
                };
                for (u32 f = 0; f < 6; f++) {
                    for (u32 v = 0; v < 4; v++) {
                        verts[vc++] = {corners[faceIndices[f][v]], normals[f], {0,0}};
                    }
                    // Two triangles per quad: (0,1,2) and (0,2,3)
                    u32 b = base + f * 4;
                    indices[ic++] = b; indices[ic++] = b+1; indices[ic++] = b+2;
                    indices[ic++] = b; indices[ic++] = b+2; indices[ic++] = b+3;
                }
            }
        };

        BoxBuilder bb;
        // Palm block (centered, slightly flattened)
        bb.addBox({-0.07f, -0.04f, -0.10f}, {0.07f, 0.04f, 0.10f});
        // 4 fingers extending forward (+Z), spaced along X
        for (int i = 0; i < 4; i++) {
            f32 fx = -0.06f + i * 0.035f;
            bb.addBox({fx, -0.02f, 0.10f}, {fx + 0.025f, 0.02f, 0.22f});
        }
        m_handMesh = MeshSystem::create(bb.verts, bb.vc, bb.indices, bb.ic);
    }

    // Register cube as mesh 0 (fallback)
    std::strncpy(m_meshDefs[0].name, "cube", sizeof(m_meshDefs[0].name) - 1);
    m_meshDefs[0].mesh = m_cubeMesh;
    m_meshDefs[0].bounds = {{-0.5f,-0.5f,-0.5f},{0.5f,0.5f,0.5f}};
    m_meshDefCount = 1;

    // Load OBJ meshes if they exist
    {
        struct MeshEntry { const char* name; const char* path; };
        static constexpr MeshEntry kMeshes[] = {
            {"skeleton",       "assets/meshes/skeleton.obj"},
            {"spider",         "assets/meshes/spider.obj"},
            {"bat",            "assets/meshes/bat.obj"},
            {"pillar",         "assets/meshes/pillar.obj"},
            {"chest",          "assets/meshes/chest.obj"},
            {"sword",          "assets/meshes/sword.obj"},
            {"dagger",         "assets/meshes/dagger.obj"},
            {"axe",            "assets/meshes/axe.obj"},
            {"pistol",         "assets/meshes/pistol.obj"},
            {"smg",            "assets/meshes/smg.obj"},
            {"carbine",        "assets/meshes/carbine.obj"},
            {"revolver",       "assets/meshes/revolver.obj"},
            {"bow",            "assets/meshes/bow.obj"},
            {"crossbow",       "assets/meshes/crossbow.obj"},
            {"throwing_knife", "assets/meshes/throwing_knife.obj"},
            {"molotov",        "assets/meshes/molotov.obj"},
            {"helmet",         "assets/meshes/helmet.obj"},
            {"armor",          "assets/meshes/armor.obj"},
            {"boots",          "assets/meshes/boots.obj"},
            {"ring",           "assets/meshes/ring.obj"},
            {"shield",         "assets/meshes/shield.obj"},
            {"human",          "assets/meshes/human.obj"},
            {"wand",           "assets/meshes/wand.obj"},
        };
        for (auto& entry : kMeshes) {
            if (m_meshDefCount >= MAX_MESH_DEFS) break;
            AABB bounds;
            Mesh mesh = ObjLoader::load(entry.path, &bounds);
            if (mesh.vao != 0) {
                MeshDef& def = m_meshDefs[m_meshDefCount];
                std::strncpy(def.name, entry.name, sizeof(def.name) - 1);
                def.mesh = mesh;
                def.bounds = bounds;
                m_meshDefCount++;
            }
        }
    }

    // Build limb meshes AFTER OBJ meshes are loaded (needs valid meshDefCount)
    LimbSystem::init(m_meshDefs, m_meshDefCount);

    // Weapons
    initWeaponTable(m_weaponDefs, m_weaponDefCount);

    // Item/loot system
    ItemLoader::loadItemDefs("assets/config/items.json", m_itemDefs, m_itemDefCount);
    ItemLoader::loadAffixDefs("assets/config/affixes.json", m_affixDefs, m_affixDefCount);
    ItemLoader::loadSkillDefs("assets/config/skills.json", m_skillDefs, m_skillDefCount);
    SkillSystem::init();
    ItemGen::init(42);

    // Resolve item visual references (material names -> IDs)
    ItemLoader::resolveVisuals(m_itemDefs, m_itemDefCount);

    // Resolve mesh names to mesh registry IDs
    for (u32 i = 0; i < m_itemDefCount; i++) {
        if (m_itemDefs[i].meshName[0] != '\0') {
            for (u32 m = 0; m < m_meshDefCount; m++) {
                if (std::strcmp(m_itemDefs[i].meshName, m_meshDefs[m].name) == 0) {
                    m_itemDefs[i].meshId = static_cast<u8>(m);
                    break;
                }
            }
        }
    }

    Combat::setDeathCallback([](EntityPool& pool, u16 entityIndex, Vec3 position) {
        if (!s_engine) return;
        // Friendly NPC death speech — set before loot drop so it's visible
        if (pool.entities[entityIndex].flags & ENT_FRIENDLY) {
            pool.entities[entityIndex].speechText = "Avenge... me...";
            pool.entities[entityIndex].speechTimer = 4.0f;
        }
        // 40% base drop chance (hostile enemies only drop loot)
        if (!(pool.entities[entityIndex].flags & ENT_FRIENDLY) &&
            (std::rand() % 100) < 40) {
            // Derive level from the entity so loot scales with floor depth
            u8 enemyLevel = pool.entities[entityIndex].level;
            if (enemyLevel < 1) enemyLevel = 1;
            ItemInstance item = ItemGen::rollItem(enemyLevel, s_engine->m_itemDefs,
                                                   s_engine->m_itemDefCount,
                                                   s_engine->m_affixDefs,
                                                   s_engine->m_affixDefCount);
            if (!isItemEmpty(item)) {
                WorldItemSystem::spawn(s_engine->m_worldItems, item,
                                       position + Vec3{0, 0.5f, 0});
            }

            // 30% chance to drop a health globe (instant heal on pickup)
            if ((std::rand() % 100) < 30) {
                ItemInstance globe;
                globe.defId = GLOBE_HEALTH_ID;
                globe.uid   = s_engine->m_worldItems.nextUid++;
                WorldItemSystem::spawn(s_engine->m_worldItems, globe,
                                       position + Vec3{0.3f, 0.5f, 0.0f});
            }
            // 20% chance to drop an energy globe (instant energy restore on pickup)
            if ((std::rand() % 100) < 20) {
                ItemInstance globe;
                globe.defId = GLOBE_ENERGY_ID;
                globe.uid   = s_engine->m_worldItems.nextUid++;
                WorldItemSystem::spawn(s_engine->m_worldItems, globe,
                                       position + Vec3{-0.3f, 0.5f, 0.0f});
            }
        }
        (void)position;
    });

    // Splash effect callback — spawns fire VFX at impact point
    ProjectileSystem::setSplashCallback([](Vec3 position, f32 radius) {
        if (!s_engine) return;
        for (u32 i = 0; i < Engine::MAX_FIRE_FX; i++) {
            if (!s_engine->m_fireFX[i].active) {
                s_engine->m_fireFX[i] = {position, radius, 1.0f, true};
                break;
            }
        }
    });

    // Init networking
    Net::init();

    // Start in menu
    m_gameState = GameState::MENU;
    m_menuSelection = 0;
    m_running = true;
    m_accumulator = 0.0;
    m_statsTimer = 0.0;
    m_updateCount = 0;
    m_frameCount = 0;

    LOG_INFO("Engine initialized — Phase 4 multiplayer ready");
}

void Engine::saveGame() {
    FILE* f = std::fopen("save.dat", "wb");
    if (!f) { LOG_WARN("Failed to save game"); return; }

    // Header: floor + seed
    std::fwrite(&m_savedFloor, sizeof(u32), 1, f);
    std::fwrite(&m_savedSeed, sizeof(u32), 1, f);

    // Player health
    f32 hp = m_localPlayer.health;
    f32 maxHp = m_localPlayer.maxHealth;
    std::fwrite(&hp, sizeof(f32), 1, f);
    std::fwrite(&maxHp, sizeof(f32), 1, f);

    // Inventory (equipment + backpack)
    std::fwrite(&m_inventories[0], sizeof(PlayerInventory), 1, f);

    // Quickbar
    std::fwrite(&m_quickbars[0], sizeof(QuickbarState), 1, f);

    // Skill state
    std::fwrite(&m_skillStates[0], sizeof(SkillState), 1, f);

    std::fclose(f);
    LOG_INFO("Game saved at floor %u", m_savedFloor);
}

bool Engine::loadGame() {
    FILE* f = std::fopen("save.dat", "rb");
    if (!f) return false;

    bool ok = true;
    ok = ok && std::fread(&m_savedFloor, sizeof(u32), 1, f) == 1;
    ok = ok && std::fread(&m_savedSeed, sizeof(u32), 1, f) == 1;

    // Player health
    f32 hp = 100.0f, maxHp = 100.0f;
    ok = ok && std::fread(&hp, sizeof(f32), 1, f) == 1;
    ok = ok && std::fread(&maxHp, sizeof(f32), 1, f) == 1;

    // Inventory
    PlayerInventory loadedInv = {};
    ok = ok && std::fread(&loadedInv, sizeof(PlayerInventory), 1, f) == 1;

    // Quickbar
    QuickbarState loadedQb = {};
    ok = ok && std::fread(&loadedQb, sizeof(QuickbarState), 1, f) == 1;

    // Skill state
    SkillState loadedSkill = {};
    ok = ok && std::fread(&loadedSkill, sizeof(SkillState), 1, f) == 1;

    std::fclose(f);

    if (ok) {
        m_localPlayer.health = hp;
        m_localPlayer.maxHealth = maxHp;
        m_inventories[0] = loadedInv;
        m_quickbars[0] = loadedQb;
        m_skillStates[0] = loadedSkill;
        LOG_INFO("Game loaded: floor %u, HP %.0f/%.0f", m_savedFloor, hp, maxHp);
    }
    return ok;
}

void Engine::startGame() {
    // Build level — use BSP procedural generation with random seed
    // Mix floor number into seed so each floor has unique layout
    u32 dungeonSeed = static_cast<u32>(std::rand()) + m_currentFloor * 7919;
    LevelGridSystem::init(m_grid, 48, 48, 1.0f);
    DungeonResult dungeon = LevelGen::generate(m_grid, dungeonSeed, 48, 48);
    Vec3 spawnPos = dungeon.spawnPos;

    m_sectionCount = LevelMeshSystem::buildAll(m_grid, m_sections, MAX_LEVEL_SECTIONS);
    Minimap::init(m_grid.width, m_grid.depth);

    // Init entities
    EntitySystem::init(m_entities);

    // Spawn enemies procedurally in each room (skip room 0 = spawn room)
    {
        // Enemy type templates
        struct EnemyTemplate {
            const char* type;
            f32 health, moveSpeed, detRange, atkRange, atkCool, damage;
            Vec3 halfExtents;
            bool flying;
        };
        static constexpr EnemyTemplate kEnemies[] = {
            {"skeleton", 40.0f, 2.5f, 12.0f, 2.5f, 1.2f,  8.0f, {0.4f, 0.9f, 0.4f}, false},
            {"bat",      25.0f, 4.5f, 12.0f, 2.5f, 1.0f,  6.0f, {0.5f, 0.4f, 0.4f}, true},
            {"spider",   35.0f, 3.0f, 10.0f, 2.0f, 1.0f,  8.0f, {0.5f, 0.3f, 0.5f}, false},
        };

        for (u32 r = 1; r < dungeon.roomCount; r++) {
            const DungeonRoom& room = dungeon.rooms[r];

            // Fewer enemies on floor 1, more on deeper floors
            u32 area = room.w * room.d;
            u32 enemyCount = (m_currentFloor == 1) ? 1 : (1 + (area / 20));
            if (enemyCount > 3) enemyCount = 3;

            for (u32 e = 0; e < enemyCount; e++) {
                // Pick random enemy type
                u32 typeIdx = static_cast<u32>(std::rand()) % 3;
                const EnemyTemplate& tmpl = kEnemies[typeIdx];

                // Random position within room bounds
                f32 ex = (room.x + 1 + static_cast<u32>(std::rand()) % (room.w > 2 ? room.w - 2 : 1)) * m_grid.cellSize;
                f32 ez = (room.z + 1 + static_cast<u32>(std::rand()) % (room.d > 2 ? room.d - 2 : 1)) * m_grid.cellSize;
                f32 spawnY = tmpl.flying ? (room.floorHeight + 1.5f) : (room.floorHeight + tmpl.halfExtents.y);

                // Find mesh and material by enemy type name
                u8 meshId = 0, matId = 0;
                for (u32 m = 0; m < m_meshDefCount; m++) {
                    if (std::strcmp(m_meshDefs[m].name, tmpl.type) == 0) {
                        meshId = static_cast<u8>(m);
                        break;
                    }
                }
                char skinName[64];
                std::snprintf(skinName, sizeof(skinName), "%s_skin", tmpl.type);
                matId = MaterialSystem::getIdByName(skinName);

                EntityHandle h = EntitySystem::spawn(m_entities,
                    Vec3{ex, spawnY, ez}, tmpl.halfExtents, tmpl.flying,
                    tmpl.health, tmpl.moveSpeed, tmpl.detRange,
                    tmpl.atkRange, tmpl.atkCool, tmpl.damage);
                Entity* ent = handleGet(m_entities, h);
                if (ent) {
                    ent->meshId = meshId;
                    ent->materialId = matId;

                    // Set enemy type for limb animation (matches kEnemies[] order)
                    static const EnemyType kEnemyTypes[] = {EnemyType::SKELETON, EnemyType::BAT, EnemyType::SPIDER};
                    ent->enemyType = kEnemyTypes[typeIdx];

                    // Scale enemy stats by floor level (+25% per floor beyond first)
                    ent->level = static_cast<u8>(m_currentFloor);
                    f32 floorMult = 1.0f + (m_currentFloor - 1) * 0.25f;
                    ent->health    *= floorMult;
                    ent->maxHealth  = ent->health;
                    ent->damage    *= floorMult;

                    // Skeletons carry random melee weapons (sword/dagger/axe mesh looked up by name)
                    if (ent->enemyType == EnemyType::SKELETON) {
                        static const char* skelWeapons[] = {"sword", "dagger", "axe"};
                        u32 weapIdx = static_cast<u32>(std::rand()) % 3;
                        for (u32 m = 1; m < m_meshDefCount; m++) {
                            if (std::strcmp(m_meshDefs[m].name, skelWeapons[weapIdx]) == 0) {
                                ent->weaponMeshId = static_cast<u8>(m);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // Spawn chests and mimics (1 per room, 20% chance mimic)
    {
        u8 chestMeshId = 0;
        for (u32 m = 0; m < m_meshDefCount; m++) {
            if (std::strcmp(m_meshDefs[m].name, "chest") == 0) {
                chestMeshId = static_cast<u8>(m);
                break;
            }
        }

        for (u32 r = 1; r < dungeon.roomCount; r++) {
            if ((std::rand() % 2) != 0) continue; // 50% of rooms get a chest
            const DungeonRoom& room = dungeon.rooms[r];

            // Center of room (use float division for accurate centering)
            f32 cx = (room.x + room.w * 0.5f) * m_grid.cellSize;
            f32 cz = (room.z + room.d * 0.5f) * m_grid.cellSize;
            f32 cy = room.floorHeight;

            bool isMimic = (std::rand() % 5) == 0; // 20% of chests are mimics

            if (isMimic) {
                // Mimic: enemy disguised as chest
                EntityHandle h = EntitySystem::spawn(m_entities,
                    Vec3{cx, cy + 0.25f, cz}, {0.3f, 0.25f, 0.3f}, false,
                    60.0f, 4.0f, 3.0f, 2.0f, 0.6f, 20.0f);
                Entity* ent = handleGet(m_entities, h);
                if (ent) {
                    ent->meshId = chestMeshId;
                    ent->enemyType = EnemyType::MIMIC;
                    ent->aiState = AIState::DORMANT;
                }
            } else {
                // Real chest: spawn a world item with good loot at this position
                ItemInstance item = ItemGen::rollItem(
                    static_cast<u8>(2 + r / 3), // higher level deeper in dungeon
                    m_itemDefs, m_itemDefCount, m_affixDefs, m_affixDefCount);
                if (!isItemEmpty(item)) {
                    WorldItemSystem::spawn(m_worldItems, item, Vec3{cx, cy + 0.3f, cz});
                }
            }
        }
    }

    LOG_INFO("Spawned %u enemies", EntitySystem::activeCount(m_entities));

    // Spawn a floor exit portal in the last room (farthest from spawn room 0)
    m_floorDoorActive = false;
    if (dungeon.roomCount > 1) {
        const DungeonRoom& lastRoom = dungeon.rooms[dungeon.roomCount - 1];
        f32 doorX = (lastRoom.x + lastRoom.w * 0.5f) * m_grid.cellSize;
        f32 doorZ = (lastRoom.z + lastRoom.d * 0.5f) * m_grid.cellSize;
        f32 doorY = lastRoom.floorHeight;
        m_floorDoorPos    = {doorX, doorY, doorZ};
        m_floorDoorActive = true;
        LOG_INFO("Floor %u exit portal at (%.1f, %.1f, %.1f)", m_currentFloor, doorX, doorY, doorZ);

        // Spawn 2 friendly NPCs near the floor exit (guards at the stairs)
        static const char* exitGreetings[] = {"This way!", "Hurry!"};
        for (u32 n = 0; n < 2; n++) {
            f32 offsetX = (n == 0) ? -1.5f : 1.5f;
            Vec3 npcPos = {m_floorDoorPos.x + offsetX,
                           m_floorDoorPos.y + 0.9f,
                           m_floorDoorPos.z - 1.0f};

            EntityHandle npcHandle = EntitySystem::spawn(m_entities, npcPos,
                {0.4f, 0.9f, 0.4f}, false,
                50.0f, 3.0f, 15.0f, 2.5f, 0.8f, 15.0f);

            Entity* npc = handleGet(m_entities, npcHandle);
            if (npc) {
                npc->flags |= ENT_FRIENDLY;
                npc->enemyType = EnemyType::SKELETON;
                npc->aiState = AIState::IDLE;
                npc->speechText = exitGreetings[n];
                npc->speechTimer = 5.0f;

                for (u32 m = 0; m < m_meshDefCount; m++) {
                    if (std::strcmp(m_meshDefs[m].name, "human") == 0) {
                        npc->meshId = static_cast<u8>(m);
                        break;
                    }
                }
                npc->materialId = MaterialSystem::getIdByName("human_skin");

                static const char* exitWeapons[] = {"sword", "axe"};
                for (u32 m = 1; m < m_meshDefCount; m++) {
                    if (std::strcmp(m_meshDefs[m].name, exitWeapons[n % 2]) == 0) {
                        npc->weaponMeshId = static_cast<u8>(m);
                        break;
                    }
                }
            }
        }
        LOG_INFO("Spawned 2 exit NPCs near floor door");
    }

    // Spawn 2 friendly NPC allies in the spawn room, offset to either side of the player
    {
        static const char* npcGreetings[] = {"Stay close!", "Let's go!"};
        for (u32 n = 0; n < 2; n++) {
            // Offset NPCs left/right of the spawn position so they don't overlap
            f32 offsetX = (n == 0) ? -1.5f : 1.5f;
            f32 npcHalfY = 0.9f; // skeleton half-height
            Vec3 npcPos = {dungeon.spawnPos.x + offsetX,
                           dungeon.spawnPos.y + npcHalfY, // center above floor
                           dungeon.spawnPos.z + 1.0f};

            EntityHandle npcHandle = EntitySystem::spawn(m_entities, npcPos,
                {0.4f, 0.9f, 0.4f}, false,  // skeleton-sized half-extents
                40.0f, 3.0f, 15.0f, 2.5f, 0.8f, 15.0f);

            Entity* npc = handleGet(m_entities, npcHandle);
            if (npc) {
                npc->flags |= ENT_FRIENDLY;
                npc->enemyType = EnemyType::SKELETON; // uses skeleton limb rig (arms + legs)
                npc->aiState   = AIState::IDLE;
                npc->speechText  = npcGreetings[n];
                npc->speechTimer = 4.0f; // greeting fades after 4 seconds

                // Use the human mesh (broader, solid face) instead of skeleton
                for (u32 m = 0; m < m_meshDefCount; m++) {
                    if (std::strcmp(m_meshDefs[m].name, "human") == 0) {
                        npc->meshId = static_cast<u8>(m);
                        break;
                    }
                }
                npc->materialId = MaterialSystem::getIdByName("human_skin");

                // Give NPC a weapon (alternating sword/axe)
                static const char* npcWeapons[] = {"sword", "axe"};
                const char* wpnName = npcWeapons[n % 2];
                for (u32 m = 1; m < m_meshDefCount; m++) {
                    if (std::strcmp(m_meshDefs[m].name, wpnName) == 0) {
                        npc->weaponMeshId = static_cast<u8>(m);
                        break;
                    }
                }
            }
        }
        LOG_INFO("Spawned 2 friendly NPCs in spawn room");
    }

    ProjectileSystem::init(m_projectiles);

    // Init inventory & world items
    WorldItemSystem::init(m_worldItems);
    // Only reset inventory on floor 1 — preserve gear when descending
    if (m_currentFloor <= 1) {
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            Inventory::init(m_inventories[i]);
            m_skillStates[i] = {};
            Quickbar::init(m_quickbars[i], m_inventories[i]);
        }
    }

    // Init players
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        m_players[i] = {};
    }

    // Setup local player
    m_players[m_localPlayerIndex].active = true;
    m_players[m_localPlayerIndex].slotIndex = m_localPlayerIndex;
    m_players[m_localPlayerIndex].position = spawnPos;
    m_players[m_localPlayerIndex].spawnPosition = spawnPos;
    // Only reset health on floor 1 — keep current HP when descending
    if (m_currentFloor <= 1) {
        m_players[m_localPlayerIndex].health = 100.0f;
        m_players[m_localPlayerIndex].maxHealth = 100.0f;
    }
    m_players[m_localPlayerIndex].weaponState.currentWeapon = 0;

    // Also set legacy player for singleplayer compat
    m_localPlayer.position = spawnPos;
    m_localPlayer.yaw = 0.0f;
    m_localPlayer.pitch = 0.0f;

    m_serverTick = 0;
    m_hitMarkerTimer = 0.0f;

    // Setup net callbacks
    if (m_netRole == NetRole::SERVER) {
        Net::setOnInput(Engine::onInput);
        Net::setOnPlayerJoin(Engine::onPlayerJoin);
        Net::setOnPlayerLeft(Engine::onPlayerLeft);
        Server::init(m_players, m_levelSeed);
    } else if (m_netRole == NetRole::CLIENT) {
        Net::setOnSnapshot(Engine::onSnapshot);
        Net::setOnEvent(Engine::onEvent);
        Net::setOnPlayerLeft(Engine::onPlayerLeft);
        Client::init(m_localPlayerIndex);
    }

    Input::setRelativeMouseMode(true);
    m_gameState = GameState::IN_GAME;
}

void Engine::shutdown() {
    LOG_INFO("Engine shutting down...");

    Net::shutdown();

    // Destroy loaded OBJ meshes (skip index 0 = cube, destroyed below)
    for (u32 i = 1; i < m_meshDefCount; i++) {
        if (m_meshDefs[i].mesh.vao) MeshSystem::destroy(m_meshDefs[i].mesh);
    }

    MeshSystem::destroy(m_cubeMesh);
    MeshSystem::destroy(m_handMesh);
    LevelMeshSystem::destroyAll(m_sections, m_sectionCount);
    LevelGridSystem::shutdown(m_grid);

    MaterialSystem::shutdown();
    ShaderSystem::destroy(m_basicShader);
    ShaderSystem::destroy(m_unlitShader);

    FontSystem::shutdown();
    ItemIconSystem::shutdown();
    HUD::shutdown();
    Minimap::shutdown();
    DebugDraw::shutdown();
    Renderer::shutdown();
    s_frameAllocator.shutdown();
    Input::shutdown();
    GLContext::shutdown();
    Window::shutdown();
    AllocationTracker::shutdown();
    Log::shutdown();

    s_engine = nullptr;
}

void Engine::run() {
    while (m_running) {
        Clock::update();
        f64 frameTime = Clock::getDeltaSeconds();
        f64 maxFrameTime = FIXED_DT * MAX_STEPS_PER_FRAME;
        if (frameTime > maxFrameTime) frameTime = maxFrameTime;

        s_frameAllocator.reset();
        AllocationTracker::resetFrameCount();

        Window::pollEvents();
        if (Window::shouldClose()) { m_running = false; break; }

        glViewport(0, 0, Window::getWidth(), Window::getHeight());

        // Poll network every frame
        if (m_netRole != NetRole::NONE) {
            Net::poll();
        }

        m_accumulator += frameTime;
        while (m_accumulator >= FIXED_DT) {
            Input::update();
            update(static_cast<f32>(FIXED_DT));
            m_accumulator -= FIXED_DT;
            m_updateCount++;
        }

        render(static_cast<f32>(m_accumulator / FIXED_DT));
        m_frameCount++;

        // Record frame time for profiler
        profilerRecordFrame(frameTime * 1000.0);

        m_statsTimer += frameTime;
        if (m_statsTimer >= 1.0) {
            if (m_gameState == GameState::IN_GAME) logStats();
            m_statsTimer  -= 1.0;
            m_updateCount  = 0;
            m_frameCount   = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Sync helpers between Player and NetPlayer
// ---------------------------------------------------------------------------
void Engine::syncLocalPlayerToNetPlayer() {
    NetPlayer& np = m_players[m_localPlayerIndex];
    np.position = m_localPlayer.position;
    np.velocity = m_localPlayer.velocity;
    np.yaw      = m_localPlayer.yaw;
    np.pitch    = m_localPlayer.pitch;
    np.onGround = m_localPlayer.onGround;
    np.health   = m_localPlayer.health;
    np.maxHealth = m_localPlayer.maxHealth;
    np.damageFlashTimer = m_localPlayer.damageFlashTimer;
    np.lockIndex = m_localPlayer.lockIndex;
    np.lockGeneration = m_localPlayer.lockGeneration;
    np.lockActive = m_localPlayer.lockActive;
    np.noclip = m_localPlayer.noclip;
}

void Engine::syncNetPlayerToLocalPlayer() {
    const NetPlayer& np = m_players[m_localPlayerIndex];
    m_localPlayer.position = np.position;
    m_localPlayer.velocity = np.velocity;
    m_localPlayer.yaw      = np.yaw;
    m_localPlayer.pitch    = np.pitch;
    m_localPlayer.onGround = np.onGround;
    m_localPlayer.health   = np.health;
    m_localPlayer.maxHealth = np.maxHealth;
    m_localPlayer.damageFlashTimer = np.damageFlashTimer;
    m_localPlayer.lockIndex = np.lockIndex;
    m_localPlayer.lockGeneration = np.lockGeneration;
    m_localPlayer.lockActive = np.lockActive;
    m_localPlayer.noclip = np.noclip;
}

// ---------------------------------------------------------------------------
// Update (60 Hz fixed timestep) — dispatches based on role
// ---------------------------------------------------------------------------
void Engine::update(f32 dt) {
    // Death screen input — handle before the generic ESC check so ESC goes to menu
    if (m_gameState == GameState::GAME_OVER) {
        if (Input::isKeyPressed(SDL_SCANCODE_RETURN)) {
            // Restart from saved floor; fall back to floor 1 if no save exists
            if (loadGame()) {
                m_currentFloor = m_savedFloor;
            } else {
                m_currentFloor = 1;
            }
            m_localPlayer.health = m_localPlayer.maxHealth;
            startGame();
            m_gameState = GameState::IN_GAME;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) {
            m_currentFloor = 1;
            m_gameState = GameState::MENU;
        }
        return;
    }

    if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) {
        if (m_gameState == GameState::MENU) {
            m_running = false; // ESC on menu exits the game
        } else if (m_gameState == GameState::IN_GAME) {
            m_gameState = GameState::MENU; // ESC in game returns to menu
            Input::setRelativeMouseMode(false);
        } else if (m_gameState != GameState::GAME_OVER) {
            Net::disconnect();
            m_netRole = NetRole::NONE;
            m_gameState = GameState::MENU;
            Input::setRelativeMouseMode(false);
        }
        return;
    }

    switch (m_gameState) {
    case GameState::MENU:
        updateMenu(dt);
        break;
    case GameState::LOBBY_HOST:
    case GameState::LOBBY_JOIN:
    case GameState::CONNECTING:
        updateLobby(dt);
        break;
    case GameState::IN_GAME:
        switch (m_netRole) {
        case NetRole::NONE:   singleplayerUpdate(dt); break;
        case NetRole::SERVER: serverUpdate(dt);       break;
        case NetRole::CLIENT: clientUpdate(dt);       break;
        }
        break;
    case GameState::GAME_OVER:
        break; // handled above
    }
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
void Engine::updateMenu(f32 dt) {
    (void)dt;
    if (Input::isKeyPressed(SDL_SCANCODE_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
        if (m_menuSelection > 0) m_menuSelection--;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
        if (m_menuSelection < 3) m_menuSelection++;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_RETURN) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
        switch (m_menuSelection) {
        case 0: // Singleplayer — try loading save, else start fresh from floor 1
            m_netRole = NetRole::NONE;
            m_localPlayerIndex = 0;
            if (loadGame()) {
                m_currentFloor = m_savedFloor;
            } else {
                m_currentFloor = 1;
            }
            startGame();
            break;
        case 1: // Host
            m_netRole = NetRole::SERVER;
            m_localPlayerIndex = 0;
            if (Net::hostServer()) {
                startGame();
                m_gameState = GameState::IN_GAME;
                LOG_INFO("Hosting game...");
            }
            break;
        case 2: // Join
            m_netRole = NetRole::CLIENT;
            if (Net::connectToServer(m_connectAddress)) {
                m_gameState = GameState::CONNECTING;
                LOG_INFO("Connecting to %s...", m_connectAddress);
            }
            break;
        case 3: // Exit
            m_running = false;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Lobby / Connecting
// ---------------------------------------------------------------------------
void Engine::updateLobby(f32 dt) {
    (void)dt;
    if (m_gameState == GameState::CONNECTING) {
        // Wait for join accept — check if we got assigned a player index
        u8 idx = Net::getLocalPlayerIndex();
        if (idx != 0 || Net::getConnectedCount() > 0) {
            // We're connected and got a slot
            m_localPlayerIndex = idx;
            startGame();
        }
    }
}

// ---------------------------------------------------------------------------
// Singleplayer update (unchanged from Phase 3)
// ---------------------------------------------------------------------------
void Engine::singleplayerUpdate(f32 dt) {
    // Check for player death — transition to GAME_OVER immediately
    if (m_localPlayer.health <= 0.0f) {
        m_gameState = GameState::GAME_OVER;
        return;
    }

    PROFILE_SCOPE(0, "Update");

    // Toggle debug overlay
    if (Input::isKeyPressed(SDL_SCANCODE_F1)) {
        DebugDraw::setEnabled(!DebugDraw::isEnabled());
    }

    // Toggle noclip
    if (Input::isKeyPressed(SDL_SCANCODE_F2)) {
        m_localPlayer.noclip = !m_localPlayer.noclip;
        LOG_INFO("Noclip: %s", m_localPlayer.noclip ? "ON" : "OFF");
    }

    // Toggle profiler overlay
    if (Input::isKeyPressed(SDL_SCANCODE_F3)) {
        Profiler& prof = getProfiler();
        prof.enabled = !prof.enabled;
        LOG_INFO("Profiler: %s", prof.enabled ? "ON" : "OFF");
    }

    // Stress spawner: F4 = 10 enemies, F5 = 50 enemies
    if (Input::isKeyPressed(SDL_SCANCODE_F4)) {
        u32 spawned = 0;
        for (u32 s = 0; s < 10 && m_entities.freeCount > 0; s++) {
            f32 angle = (s / 10.0f) * 6.28f;
            Vec3 pos = m_localPlayer.position + Vec3{cosf(angle) * 5.0f, 0.5f, sinf(angle) * 5.0f};
            bool flying = (s % 3 == 0);
            Vec3 half = flying ? Vec3{0.3f, 0.3f, 0.3f} : Vec3{0.4f, 0.5f, 0.4f};
            EntitySystem::spawn(m_entities, pos, half, flying,
                flying ? 30.0f : 50.0f, flying ? 4.0f : 2.5f,
                15.0f, flying ? 8.0f : 2.5f, flying ? 1.5f : 1.0f, flying ? 8.0f : 10.0f);
            spawned++;
        }
        LOG_INFO("Spawned %u enemies (total: %u)", spawned, EntitySystem::activeCount(m_entities));
    }

    if (Input::isKeyPressed(SDL_SCANCODE_F5)) {
        u32 spawned = 0;
        for (u32 s = 0; s < 50 && m_entities.freeCount > 0; s++) {
            f32 angle = (s / 50.0f) * 6.28f;
            f32 radius = 4.0f + (s % 5) * 2.0f;
            Vec3 pos = m_localPlayer.position + Vec3{cosf(angle) * radius, 0.5f, sinf(angle) * radius};
            bool flying = (s % 4 == 0);
            Vec3 half = flying ? Vec3{0.3f, 0.3f, 0.3f} : Vec3{0.4f, 0.5f, 0.4f};
            EntitySystem::spawn(m_entities, pos, half, flying,
                flying ? 30.0f : 50.0f, flying ? 4.0f : 2.5f,
                15.0f, flying ? 8.0f : 2.5f, flying ? 1.5f : 1.0f, flying ? 8.0f : 10.0f);
            spawned++;
        }
        LOG_INFO("Spawned %u enemies (total: %u)", spawned, EntitySystem::activeCount(m_entities));
    }

    // Switch constraint mode (F6)
    if (Input::isKeyPressed(SDL_SCANCODE_F6)) {
        m_switchMode = !m_switchMode;
        if (m_switchMode) {
            m_camera.farPlane = SWITCH_FAR_PLANE;
            LOG_INFO("[SWITCH] Mode ON — far=%.0f, res=%ux%u", SWITCH_FAR_PLANE, SWITCH_RES_W, SWITCH_RES_H);
        } else {
            m_camera.farPlane = 200.0f;
            LOG_INFO("[SWITCH] Mode OFF");
        }
    }

    // Quickbar slot switching (keys 1-8)
    WeaponState& ws = m_players[0].weaponState;
    for (u32 i = 0; i < QUICKBAR_SLOTS; i++) {
        if (Input::isKeyPressed(SDL_SCANCODE_1 + i)) {
            m_quickbars[0].activeSlot = static_cast<u8>(i);
        }
    }

    // Healing potion (Q key, 15 second cooldown — heals 40% of max HP)
    if (m_potionCooldown > 0.0f) m_potionCooldown -= dt;
    if (Input::isKeyPressed(SDL_SCANCODE_Q) && m_potionCooldown <= 0.0f) {
        f32 healAmount = m_localPlayer.maxHealth * 0.4f;
        m_localPlayer.health += healAmount;
        if (m_localPlayer.health > m_localPlayer.maxHealth)
            m_localPlayer.health = m_localPlayer.maxHealth;
        m_potionCooldown = 15.0f;
        LOG_INFO("Used healing potion: +%.0f HP (cooldown 15s)", healAmount);
    }

    // Player movement
    PlayerController::update(m_localPlayer, dt);
    if (!m_localPlayer.noclip) {
        Collision::moveAndSlide(m_localPlayer, m_grid, dt);
    }

    // Sync to NetPlayer for consistent rendering
    syncLocalPlayerToNetPlayer();

    // Target lock
    updateTargetLock(dt);

    // Weapon fire
    handleWeaponFire(dt);

    // Update viewmodel animation timers
    {
        // Use XZ speed to drive walk bob — ignore vertical velocity
        f32 playerSpeed = length(Vec3{m_localPlayer.velocity.x, 0, m_localPlayer.velocity.z});
        if (playerSpeed > 0.5f) {
            m_viewmodelState.bobTimer += playerSpeed * dt;
        } else {
            // Smoothly decay bob amplitude when stopped
            m_viewmodelState.bobTimer *= 0.95f;
        }
        // Exponential recoil decay each tick
        m_viewmodelState.recoilKick *= 0.92f; // slower decay = smoother
        if (m_viewmodelState.recoilKick < 0.001f) m_viewmodelState.recoilKick = 0.0f;
        // Count down melee swing animation
        if (m_viewmodelState.attackAnimT > 0.0f) m_viewmodelState.attackAnimT -= dt;
        // Count down ranged fire shake
        if (m_viewmodelState.fireShakeTimer > 0.0f) m_viewmodelState.fireShakeTimer -= dt;
    }

    // Enemy AI
    { PROFILE_SCOPE(1, "AI");
    EnemyAI::update(m_entities, m_grid, m_localPlayer, m_projectiles, dt);
    }

    // Decay speech timers for all entities (handles death speech and any
    // speech set outside the AI loop, e.g. from the death callback)
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        u32 idx = m_entities.activeList[a];
        Entity& e = m_entities.entities[idx];
        if (e.speechTimer > 0.0f) {
            e.speechTimer -= dt;
            if (e.speechTimer <= 0.0f) {
                e.speechText  = nullptr;
                e.speechTimer = 0.0f;
            }
        }
    }

    // Projectiles
    { PROFILE_SCOPE(2, "Projectiles");
    ProjectileSystem::update(m_projectiles, m_grid, m_entities, m_localPlayer, dt);
    }

    // Entity timers
    EntitySystem::tickTimers(m_entities, dt);

    // Update world items
    WorldItemSystem::update(m_worldItems, dt);

    // Decay fire AoE effects
    for (u32 i = 0; i < MAX_FIRE_FX; i++) {
        if (m_fireFX[i].active) {
            m_fireFX[i].timer -= dt;
            if (m_fireFX[i].timer <= 0.0f) m_fireFX[i].active = false;
        }
    }

    // Update skill state (energy regen, cooldowns)
    SkillSystem::update(m_skillStates[0], dt);

    // Update orb projectiles (spawn ice shards for Frozen Orb)
    SkillSystem::updateOrbProjectiles(m_projectiles, m_skillDefs, m_skillDefCount, dt);

    // Update pending meteors
    SkillSystem::updateMeteors(m_entities, dt);

    // Skill activation (right mouse button)
    if (Input::isMouseButtonPressed(SDL_BUTTON_RIGHT) && !m_inventoryOpen) {
        Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
        SkillSystem::tryActivate(m_skillStates[0], m_skillDefs, m_skillDefCount,
                                  eyePos, m_localPlayer.forward, m_localPlayer.yaw,
                                  m_projectiles, m_entities, m_grid, m_localPlayer);
    }

    // Update active skill from equipped legendary weapon
    {
        const ItemInstance& wpn = m_inventories[0].equipped[static_cast<u32>(ItemSlot::WEAPON)];
        if (!isItemEmpty(wpn) && wpn.rarity == Rarity::LEGENDARY) {
            SkillId skillId = m_itemDefs[wpn.defId].legendarySkillId;
            m_skillStates[0].activeSkill = skillId;
        } else {
            m_skillStates[0].activeSkill = SkillId::NONE;
        }
    }

    // Auto-pickup health/energy globes (no key press needed, walk-over activation)
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        WorldItem& wi = m_worldItems.items[i];
        if (!wi.active) continue;
        if (!isGlobe(wi.item)) continue;

        Vec3 delta = m_localPlayer.position - wi.position;
        f32 dist = length(delta);
        if (dist < 2.5f) {
            if (wi.item.defId == GLOBE_HEALTH_ID) {
                // Restore 20 HP, capped at max
                m_localPlayer.health += 20.0f;
                if (m_localPlayer.health > m_localPlayer.maxHealth)
                    m_localPlayer.health = m_localPlayer.maxHealth;
            } else if (wi.item.defId == GLOBE_ENERGY_ID) {
                // Restore 25 energy, capped at max
                SkillState& ss = m_skillStates[m_localPlayerIndex];
                ss.energy += 25.0f;
                if (ss.energy > ss.maxEnergy)
                    ss.energy = ss.maxEnergy;
            }
            wi.active = false;
            if (m_worldItems.activeCount > 0) m_worldItems.activeCount--;
        }
    }

    // Item pickup (E key) — globes are consumed above and never reach here
    if (Input::isKeyPressed(SDL_SCANCODE_E)) {
        ItemInstance picked;
        if (WorldItemSystem::tryPickup(m_worldItems, m_localPlayer.position, 0, picked)) {
            // Safety: skip any globe that slipped through auto-pickup
            if (!isGlobe(picked)) {
                if (Inventory::addToBackpack(m_inventories[0], picked)) {
                    LOG_INFO("Picked up item (defId=%u, rarity=%u)", picked.defId, (u32)picked.rarity);
                }
            }
        }
    }

    // Floor door — descend to next floor when player walks near and presses E
    if (m_floorDoorActive) {
        Vec3 toDoor = m_floorDoorPos - m_localPlayer.position;
        if (lengthSq(toDoor) < 4.0f) {
            if (Input::isKeyPressed(SDL_SCANCODE_E)) {
                m_currentFloor++;
                // Save progress before descending so death respawn returns here
                m_savedFloor = m_currentFloor;
                m_savedSeed = static_cast<u32>(std::rand());
                saveGame();
                LOG_INFO("Descending to floor %u", m_currentFloor);
                startGame(); // regenerate dungeon with new floor seed
                return;      // don't process remainder of this frame
            }
        }
    }

    // Toggle inventory (Tab key)
    if (Input::isKeyPressed(SDL_SCANCODE_TAB)) {
        m_inventoryOpen = !m_inventoryOpen;
        Input::setRelativeMouseMode(!m_inventoryOpen);
    }

    // Inventory mouse interaction
    if (m_inventoryOpen) {
        s32 mx, my;
        Input::getMousePosition(mx, my);
        // Convert from top-left origin (SDL) to bottom-left origin (HUD)
        my = static_cast<s32>(Window::getHeight()) - my;

        if (Input::isMouseButtonPressed(SDL_BUTTON_LEFT)) {
            // Detect shift modifier for quickbar assignment vs. equip
            bool shiftHeld = Input::isKeyDown(SDL_SCANCODE_LSHIFT) || Input::isKeyDown(SDL_SCANCODE_RSHIFT);
            f32 bpX      = Window::getWidth()  * 0.55f;
            f32 bpStartY = Window::getHeight() * 0.5f + 60.0f;
            f32 cellSize = 26.0f;
            f32 cellGap  = 3.0f;

            for (u32 i = 0; i < MAX_INVENTORY_ITEMS; i++) {
                u32 col = i % 6;
                u32 row = i / 6;
                f32 x = bpX + static_cast<f32>(col) * (cellSize + cellGap);
                f32 y = bpStartY - static_cast<f32>(row) * (cellSize + cellGap);

                if (mx >= static_cast<s32>(x) && mx <= static_cast<s32>(x + cellSize) &&
                    my >= static_cast<s32>(y) && my <= static_cast<s32>(y + cellSize)) {
                    if (!isItemEmpty(m_inventories[0].backpack[i])) {
                        if (shiftHeld) {
                            // Shift+Click: assign item to quickbar without equipping
                            Quickbar::assignItem(m_quickbars[0], m_inventories[0], static_cast<u8>(i));
                            LOG_INFO("Assigned item to quickbar from slot %u", i);
                        } else {
                            // Normal click: equip item and sync quickbar weapon slot
                            Inventory::equip(m_inventories[0], static_cast<u8>(i), m_itemDefs);
                            Quickbar::syncWeaponSlot(m_quickbars[0], m_inventories[0]);
                            LOG_INFO("Equipped item from slot %u", i);
                        }
                    }
                    break;
                }
            }
        }
    }

    // Debug: F7 gives random item
    if (Input::isKeyPressed(SDL_SCANCODE_F7)) {
        ItemInstance item = ItemGen::rollItem(1, m_itemDefs, m_itemDefCount,
                                              m_affixDefs, m_affixDefCount);
        if (!isItemEmpty(item)) {
            if (Inventory::addToBackpack(m_inventories[0], item)) {
                LOG_INFO("Debug: gave %s (rarity %u, damage %.1f)",
                         m_itemDefs[item.defId].name, (u32)item.rarity, item.damage);
            }
        }
    }

    // Damage flash decay
    if (m_localPlayer.damageFlashTimer > 0.0f)
        m_localPlayer.damageFlashTimer -= dt;
    if (m_hitMarkerTimer > 0.0f)
        m_hitMarkerTimer -= dt;

    // Camera
    PlayerController::applyToCamera(m_localPlayer, m_camera);
    // Screen shake only from enemy hits, not weapon fire
    if (m_localPlayer.hitShakeTimer > 0.0f) {
        m_localPlayer.hitShakeTimer -= dt;
        f32 shake = m_localPlayer.hitShakeTimer * 0.08f;
        m_camera.pitch += sinf(m_localPlayer.hitShakeTimer * 60.0f) * shake;
    }

    // Player-enemy push collision
    AABB playerBox = {
        m_localPlayer.position + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
        m_localPlayer.position + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
    };
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = m_entities.entities[i];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        AABB entBox = entityAABB(e);
        if (CombatQuery::aabbOverlap(playerBox, entBox)) {
            Vec3 toPlayer = m_localPlayer.position - e.position;
            f32 pushX = (e.halfExtents.x + PLAYER_HALF_WIDTH) - fabsf(toPlayer.x);
            f32 pushZ = (e.halfExtents.z + PLAYER_HALF_WIDTH) - fabsf(toPlayer.z);
            if (pushX > 0.0f && pushZ > 0.0f) {
                if (pushX < pushZ)
                    m_localPlayer.position.x += (toPlayer.x > 0) ? pushX : -pushX;
                else
                    m_localPlayer.position.z += (toPlayer.z > 0) ? pushZ : -pushZ;
            }
        }
    }

    // Update fog-of-war
    Minimap::updateVisited(m_grid, m_localPlayer.position);

    syncLocalPlayerToNetPlayer();
}

// ---------------------------------------------------------------------------
// Server update (listen server: host plays + serves)
// ---------------------------------------------------------------------------
void Engine::serverUpdate(f32 dt) {
    m_serverTick++;

    // Toggle debug
    if (Input::isKeyPressed(SDL_SCANCODE_F1))
        DebugDraw::setEnabled(!DebugDraw::isEnabled());
    if (Input::isKeyPressed(SDL_SCANCODE_F2)) {
        m_localPlayer.noclip = !m_localPlayer.noclip;
        LOG_INFO("Noclip: %s", m_localPlayer.noclip ? "ON" : "OFF");
    }

    // Weapon switching for local player
    WeaponState& ws = m_players[m_localPlayerIndex].weaponState;
    for (u32 i = 0; i < m_weaponDefCount && i < 3; i++) {
        if (Input::isKeyPressed(SDL_SCANCODE_1 + i)) {
            ws.currentWeapon = static_cast<u8>(i);
            ws.cooldownTimer = 0.0f;
        }
    }

    // Capture local input and push into server's input buffer
    NetInput localInput = PlayerController::captureLocalInput(m_serverTick, ws.currentWeapon);
    Server::getInputBuffer(m_localPlayerIndex).push(localInput);

    // Process inputs for all active players
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        NetPlayer& np = m_players[i];
        if (!np.active) continue;

        const NetInput* input = Server::getInputBuffer(i).getLatest();
        if (input) {
            PlayerController::updateNetPlayerFromInput(np, *input, dt);
            np.lastProcessedInputTick = input->tick;

            // Weapon switching from input
            if (input->weaponId < m_weaponDefCount)
                np.weaponState.currentWeapon = input->weaponId;
        }
    }

    // Collision for all players (using local Player struct for collision func)
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        NetPlayer& np = m_players[i];
        if (!np.active || np.noclip) continue;

        // Use a temporary Player to call Collision::moveAndSlide
        Player tempP;
        tempP.position = np.position;
        tempP.velocity = np.velocity;
        tempP.onGround = np.onGround;
        tempP.noclip = np.noclip;
        Collision::moveAndSlide(tempP, m_grid, dt);
        np.position = tempP.position;
        np.velocity = tempP.velocity;
        np.onGround = tempP.onGround;
    }

    // Weapon fire for all players
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (!m_players[i].active) continue;
        handleWeaponFireForPlayer(m_players[i], dt);
    }

    // Target lock for local player
    syncNetPlayerToLocalPlayer();
    updateTargetLock(dt);
    syncLocalPlayerToNetPlayer();

    // Enemy AI (still targets single local player for now — Phase D upgrades this)
    EnemyAI::update(m_entities, m_grid, m_localPlayer, m_projectiles, dt);

    // Projectiles
    ProjectileSystem::update(m_projectiles, m_grid, m_entities, m_localPlayer, dt);

    // Entity timers
    EntitySystem::tickTimers(m_entities, dt);

    // Damage flash decay for all players
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (m_players[i].active && m_players[i].damageFlashTimer > 0.0f)
            m_players[i].damageFlashTimer -= dt;
    }

    if (m_hitMarkerTimer > 0.0f) m_hitMarkerTimer -= dt;

    // Camera from local player
    syncNetPlayerToLocalPlayer();
    PlayerController::applyToCamera(m_localPlayer, m_camera);
    // Screen shake only from enemy hits, not weapon fire
    if (m_localPlayer.hitShakeTimer > 0.0f) {
        m_localPlayer.hitShakeTimer -= dt;
        f32 shake = m_localPlayer.hitShakeTimer * 0.08f;
        m_camera.pitch += sinf(m_localPlayer.hitShakeTimer * 60.0f) * shake;
    }

    // Player-enemy push for local player
    AABB playerBox = {
        m_localPlayer.position + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
        m_localPlayer.position + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
    };
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = m_entities.entities[i];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        AABB entBox = entityAABB(e);
        if (CombatQuery::aabbOverlap(playerBox, entBox)) {
            Vec3 toPlayer = m_localPlayer.position - e.position;
            f32 pushX = (e.halfExtents.x + PLAYER_HALF_WIDTH) - fabsf(toPlayer.x);
            f32 pushZ = (e.halfExtents.z + PLAYER_HALF_WIDTH) - fabsf(toPlayer.z);
            if (pushX > 0.0f && pushZ > 0.0f) {
                if (pushX < pushZ)
                    m_localPlayer.position.x += (toPlayer.x > 0) ? pushX : -pushX;
                else
                    m_localPlayer.position.z += (toPlayer.z > 0) ? pushZ : -pushZ;
            }
        }
    }
    syncLocalPlayerToNetPlayer();

    // Broadcast snapshot every TICKS_PER_SNAP ticks
    if (m_serverTick % TICKS_PER_SNAP == 0) {
        Server::sendSnapshot(m_serverTick, m_players, m_entities, m_projectiles);
    }
}

// ---------------------------------------------------------------------------
// Client update (prediction + interpolation)
// ---------------------------------------------------------------------------
void Engine::clientUpdate(f32 dt) {
    m_serverTick++;

    // Toggle debug
    if (Input::isKeyPressed(SDL_SCANCODE_F1))
        DebugDraw::setEnabled(!DebugDraw::isEnabled());

    // Weapon switching
    WeaponState& ws = m_players[m_localPlayerIndex].weaponState;
    for (u32 i = 0; i < m_weaponDefCount && i < 3; i++) {
        if (Input::isKeyPressed(SDL_SCANCODE_1 + i)) {
            ws.currentWeapon = static_cast<u8>(i);
            ws.cooldownTimer = 0.0f;
        }
    }

    // Capture and send input to server
    Client::captureAndSendInput(m_serverTick, ws.currentWeapon);

    // Apply local prediction
    const NetInput* input = Client::getLatestInput();
    if (input) {
        NetPlayer& np = m_players[m_localPlayerIndex];
        PlayerController::updateNetPlayerFromInput(np, *input, dt);

        // Local collision
        Player tempP;
        tempP.position = np.position;
        tempP.velocity = np.velocity;
        tempP.onGround = np.onGround;
        tempP.noclip = np.noclip;
        Collision::moveAndSlide(tempP, m_grid, dt);
        np.position = tempP.position;
        np.velocity = tempP.velocity;
        np.onGround = tempP.onGround;

        Client::storePrediction(*input, np);
    }

    // Reconcile with server
    Client::reconcile(m_players[m_localPlayerIndex], m_grid, dt);

    // Interpolate remote state
    Client::interpolateRemotePlayers(m_localPlayerIndex,
        m_renderPlayerPositions, m_renderPlayerYaws, m_renderPlayerPitches,
        m_renderPlayerActive, m_renderPlayerHealth, m_renderPlayerMaxHealth);
    Client::interpolateEntities(m_renderEntities);
    Client::interpolateProjectiles(m_renderProjectiles);

    // Hit marker decay
    if (m_hitMarkerTimer > 0.0f) m_hitMarkerTimer -= dt;

    // Camera from predicted local player
    syncNetPlayerToLocalPlayer();
    PlayerController::applyToCamera(m_localPlayer, m_camera);
    // Screen shake only from enemy hits, not weapon fire
    if (m_localPlayer.hitShakeTimer > 0.0f) {
        m_localPlayer.hitShakeTimer -= dt;
        f32 shake = m_localPlayer.hitShakeTimer * 0.08f;
        m_camera.pitch += sinf(m_localPlayer.hitShakeTimer * 60.0f) * shake;
    }
}

// ---------------------------------------------------------------------------
// Weapon fire (singleplayer — unchanged from Phase 3)
// ---------------------------------------------------------------------------
void Engine::handleWeaponFire(f32 dt) {
    WeaponState& ws = m_players[m_localPlayerIndex].weaponState;
    ws.cooldownTimer -= dt;
    if (ws.cooldownTimer < 0.0f) ws.cooldownTimer = 0.0f;

    if (!Input::isMouseButtonDown(SDL_BUTTON_LEFT)) return;
    if (ws.cooldownTimer > 0.0f) return;

    // Resolve weapon from the actual quickbar item, not the equipped slot
    const ItemInstance* qbItem = Quickbar::resolveSlot(m_quickbars[m_localPlayerIndex],
                                                       m_inventories[m_localPlayerIndex],
                                                       m_quickbars[m_localPlayerIndex].activeSlot);
    WeaponDef wpn;
    if (qbItem && !isItemEmpty(*qbItem) &&
        m_itemDefs[qbItem->defId].slot == ItemSlot::WEAPON) {
        // Build WeaponDef from the specific quickbar item's stats
        wpn = Inventory::getWeaponFromItem(m_inventories[m_localPlayerIndex],
                                            m_itemDefs, *qbItem);
    } else {
        wpn = m_weaponDefs[ws.currentWeapon];
    }
    ws.cooldownTimer = wpn.cooldown;

    Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
    Vec3 forward = m_localPlayer.forward;

    AttackResult result;
    switch (wpn.type) {
    case WeaponType::MELEE:
        result = Combat::fireMelee(wpn, eyePos, forward, m_entities);
        break;
    case WeaponType::HITSCAN:
        result = Combat::fireHitscan(wpn, eyePos, forward, m_grid, m_entities);
        if (result.hitEntity || result.hitWorld) {
            m_lastCombatHit.hit      = true;
            m_lastCombatHit.position = result.hitPosition;
            m_lastCombatHit.normal   = result.hitNormal;
            m_lastCombatHit.distance = result.hitDistance;
            m_lastCombatHit.type     = result.hitEntity ? CombatHit::ENTITY : CombatHit::WORLD;
        }
        break;
    case WeaponType::PROJECTILE: {
        bool isMolotov = qbItem && !isItemEmpty(*qbItem) &&
                         m_itemDefs[qbItem->defId].weaponSubtype == WeaponSubtype::MOLOTOV;
        bool isWand = qbItem && !isItemEmpty(*qbItem) &&
                      m_itemDefs[qbItem->defId].weaponSubtype == WeaponSubtype::WAND;

        if (isMolotov) {
            Combat::fireProjectile(wpn, eyePos, forward, m_projectiles,
                                    9.8f, 3.0f, wpn.damage * 0.6f);
        } else {
            Combat::fireProjectile(wpn, eyePos, forward, m_projectiles);
        }
        // Wand weapons shoot lightning spark projectiles
        if (isWand) {
            // Tag the most recently spawned projectile at eyePos
            for (u32 pi = MAX_PROJECTILES; pi-- > 0;) {
                Projectile& proj = m_projectiles.projectiles[pi];
                if (proj.active && proj.fromPlayer && proj.projFlags == 0) {
                    Vec3 d = proj.position - eyePos;
                    if (lengthSq(d) < 0.1f) {
                        proj.projFlags |= PROJ_SPARK;
                        break;
                    }
                }
            }
        }
        result.didFire = true;
    } break;
    }

    // Viewmodel animation per weapon type
    if (wpn.type == WeaponType::MELEE) {
        m_viewmodelState.attackAnimT = 0.3f;
    } else if (wpn.type == WeaponType::HITSCAN) {
        m_viewmodelState.attackAnimT = 0.2f; // shorter recoil snap
        m_viewmodelState.fireShakeTimer = 0.1f;
    } else {
        m_viewmodelState.fireShakeTimer = 0.12f;
    }
    m_viewmodelState.recoilKick += wpn.recoilKick * 1.5f;
    if (result.hitEntity) m_hitMarkerTimer = 0.2f;
}

// ---------------------------------------------------------------------------
// Weapon fire for any NetPlayer (server-authoritative)
// ---------------------------------------------------------------------------
void Engine::handleWeaponFireForPlayer(NetPlayer& np, f32 dt) {
    WeaponState& ws = np.weaponState;
    ws.cooldownTimer -= dt;
    if (ws.cooldownTimer < 0.0f) ws.cooldownTimer = 0.0f;

    // Check if fire input is set
    const NetInput* input = Server::getInputBuffer(np.slotIndex).getLatest();
    if (!input) return;
    if (!(input->moveFlags & INPUT_FIRE)) return;
    if (ws.cooldownTimer > 0.0f) return;

    WeaponDef wpn = Inventory::getEffectiveWeapon(m_inventories[np.slotIndex],
                                                    m_itemDefs, m_weaponDefs[ws.currentWeapon]);
    ws.cooldownTimer = wpn.cooldown;

    Vec3 eyePos = np.eyePos();
    Vec3 forward = normalize(Vec3{
        -sinf(np.yaw) * cosf(np.pitch),
         sinf(np.pitch),
        -cosf(np.yaw) * cosf(np.pitch)
    });

    switch (wpn.type) {
    case WeaponType::MELEE:
        Combat::fireMelee(wpn, eyePos, forward, m_entities);
        break;
    case WeaponType::HITSCAN:
        Combat::fireHitscan(wpn, eyePos, forward, m_grid, m_entities);
        break;
    case WeaponType::PROJECTILE:
        Combat::fireProjectile(wpn, eyePos, forward, m_projectiles);
        break;
    }

    // If this is the local player, trigger hit marker
    if (np.slotIndex == m_localPlayerIndex) {
        // Check via the result — simplified for now
    }
}

// ---------------------------------------------------------------------------
// Soft target lock (singleplayer — unchanged from Phase 3)
// ---------------------------------------------------------------------------
void Engine::updateTargetLock(f32 dt) {
    Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
    Vec3 forward = m_localPlayer.forward;

    if (Input::isMouseButtonDown(SDL_BUTTON_MIDDLE)) {
        if (!m_localPlayer.lockActive) {
            EntityHandle hits[8];
            f32 distances[8];
            f32 coneCos = cosf(radians(30.0f));
            u32 count = CombatQuery::queryConeSorted(m_entities, eyePos, forward,
                                                      coneCos, 30.0f,
                                                      hits, distances, 8);
            if (count > 0) {
                m_localPlayer.lockIndex      = hits[0].index;
                m_localPlayer.lockGeneration = hits[0].generation;
                m_localPlayer.lockActive     = true;
                m_localPlayer.lockLosTimer   = 0.0f;
            }
        }

        if (m_localPlayer.lockActive) {
            EntityHandle h = {m_localPlayer.lockIndex, m_localPlayer.lockGeneration};
            Entity* target = handleGet(m_entities, h);

            if (!target || (target->flags & ENT_DEAD)) {
                m_localPlayer.lockActive = false;
            } else {
                Vec3 toTarget = target->position - eyePos;
                f32 dist = length(toTarget);

                if (dist > 40.0f) {
                    m_localPlayer.lockActive = false;
                } else if (dist > 0.001f) {
                    Vec3 dirToTarget = toTarget * (1.0f / dist);
                    f32 d = dot(dirToTarget, forward);
                    if (d < cosf(radians(45.0f))) {
                        m_localPlayer.lockActive = false;
                    } else {
                        f32 lockStrength = 0.05f;
                        Vec3 biased = normalize(forward + (dirToTarget - forward) * lockStrength);
                        m_localPlayer.yaw   = atan2f(-biased.x, -biased.z);
                        m_localPlayer.pitch = asinf(biased.y);
                    }
                }
            }
        }
    } else {
        m_localPlayer.lockActive = false;
    }
}

// ---------------------------------------------------------------------------
// Viewmodel — renders first-person hand + equipped weapon over everything
// ---------------------------------------------------------------------------
void Engine::renderViewmodel() {
    if (m_inventoryOpen) return;
    if (m_gameState != GameState::IN_GAME) return;

    // Resolve equipped weapon mesh — show fist if unarmed
    const ItemInstance& equipped = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
    bool hasWeapon = !isItemEmpty(equipped) &&
                     m_itemDefs[equipped.defId].meshId > 0 &&
                     m_itemDefs[equipped.defId].meshId < m_meshDefCount;

    u8 weaponMeshId = hasWeapon ? m_itemDefs[equipped.defId].meshId : 0;
    // Use a dummy ItemDef for unarmed (melee type)
    ItemDef unarmedDef = {};
    unarmedDef.weaponType = WeaponType::MELEE;
    unarmedDef.weaponSubtype = WeaponSubtype::NONE;
    const ItemDef& def = hasWeapon ? m_itemDefs[equipped.defId] : unarmedDef;

    // Clear depth so viewmodel renders on top of everything
    glClear(GL_DEPTH_BUFFER_BIT);

    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();
    f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);

    // Wide FOV for viewmodel so arm/hand are visible in peripheral vision
    Mat4 proj = Mat4::perspective(85.0f * (3.14159f / 180.0f), aspect, 0.01f, 10.0f);

    // Subtle walk bob
    f32 bobX = sinf(m_viewmodelState.bobTimer * 6.0f) * 0.004f;
    f32 bobY = sinf(m_viewmodelState.bobTimer * 12.0f) * 0.003f;

    // Viewmodel-only recoil (doesn't affect camera)
    f32 recoilPitch = -m_viewmodelState.recoilKick * 0.12f;

    // Attack animation — per-subtype melee, generic recoil for ranged
    f32 attackPitch = 0.0f;  // X rotation (pitch forward/back)
    f32 attackYaw   = 0.0f;  // Y rotation (swing left/right)
    f32 attackZ     = 0.0f;  // Z offset (thrust forward/back)

    if (m_viewmodelState.attackAnimT > 0.0f) {
        if (def.weaponType == WeaponType::MELEE) {
            f32 t = m_viewmodelState.attackAnimT / 0.3f; // normalized 1→0
            switch (def.weaponSubtype) {
                case WeaponSubtype::DAGGER:
                case WeaponSubtype::THROWING_KNIFE:
                    attackZ = -0.45f * sinf(t * 3.14159f);
                    attackPitch = -0.3f * sinf(t * 3.14159f);
                    break;
                case WeaponSubtype::AXE:
                    attackPitch = -0.9f * sinf(t * 3.14159f);
                    break;
                case WeaponSubtype::SWORD:
                default:
                    attackYaw = -0.8f * sinf(t * 3.14159f);
                    attackPitch = -0.15f * t;
                    break;
            }
        } else if (def.weaponType == WeaponType::HITSCAN) {
            f32 t = m_viewmodelState.attackAnimT / 0.2f; // faster snap-back
            switch (def.weaponSubtype) {
                case WeaponSubtype::PISTOL:
                    // Quick upward kick, snaps back
                    attackPitch = 0.25f * t;
                    attackZ = 0.04f * t; // slight pushback
                    break;
                case WeaponSubtype::SMG:
                    // Rapid small jitter — high frequency, low amplitude
                    attackPitch = 0.12f * t + sinf(t * 40.0f) * 0.03f;
                    attackYaw = sinf(t * 30.0f) * 0.02f;
                    break;
                case WeaponSubtype::CARBINE:
                    // Heavy shoulder kick — big pitch, slow return
                    attackPitch = 0.4f * t;
                    attackZ = 0.08f * t;
                    break;
                case WeaponSubtype::REVOLVER:
                    // Strong upward flip with yaw torque
                    attackPitch = 0.35f * t;
                    attackYaw = 0.1f * t;
                    attackZ = 0.06f * t;
                    break;
                default:
                    attackPitch = 0.2f * t;
                    break;
            }
        }
    }

    // Per-weapon-type positioning
    Vec3 offset;
    f32 holdYaw = 0.0f;
    f32 holdPitch = 0.0f;
    switch (def.weaponType) {
        case WeaponType::MELEE:
            offset = {0.35f + bobX, -0.35f + bobY, -0.45f + attackZ};
            if (def.weaponSubtype == WeaponSubtype::DAGGER ||
                def.weaponSubtype == WeaponSubtype::THROWING_KNIFE) {
                // Dagger: held forward for stabbing, blade pointing at target
                holdYaw = 0.1f;
                holdPitch = -0.5f; // angled forward like an icepick grip
            } else {
                holdYaw = 0.4f;
                holdPitch = -0.2f;
            }
            break;
        case WeaponType::HITSCAN:
            offset = {0.40f + bobX, -0.30f + bobY, -0.50f + attackZ};
            holdYaw = 0.1f;
            holdPitch = 0.0f;
            break;
        case WeaponType::PROJECTILE:
            offset = {0.30f + bobX, -0.35f + bobY, -0.50f};
            holdYaw = 0.2f;
            holdPitch = -0.1f;
            break;
    }

    // Rapid vibration while firing ranged weapons
    if (m_viewmodelState.fireShakeTimer > 0.0f) {
        f32 intensity = m_viewmodelState.fireShakeTimer / 0.15f;
        f32 phase = m_viewmodelState.fireShakeTimer * 60.0f;
        offset.x += sinf(phase * 7.3f) * 0.003f * intensity;
        offset.y += sinf(phase * 11.1f) * 0.002f * intensity;
    }

    // Scale weapon mesh to fill viewmodel area (~0.8 units)
    const AABB& wb = m_meshDefs[weaponMeshId].bounds;
    f32 meshH = wb.max.y - wb.min.y;
    f32 meshW = wb.max.x - wb.min.x;
    f32 meshD = wb.max.z - wb.min.z;
    f32 maxDim = meshH;
    if (meshW > maxDim) maxDim = meshW;
    if (meshD > maxDim) maxDim = meshD;
    f32 weaponScale = (maxDim > 0.001f) ? (0.8f / maxDim) : 0.8f;

    // Center the mesh at origin before scaling (offset by mesh center)
    Vec3 meshCenter = {
        (wb.min.x + wb.max.x) * 0.5f,
        (wb.min.y + wb.max.y) * 0.5f,
        (wb.min.z + wb.max.z) * 0.5f
    };

    Mat4 weaponModel = Mat4::translate(offset)
                     * Mat4::rotateX(recoilPitch + attackPitch + holdPitch)
                     * Mat4::rotateY(holdYaw + attackYaw)
                     * Mat4::scale({weaponScale, weaponScale, weaponScale})
                     * Mat4::translate({-meshCenter.x, -meshCenter.y, -meshCenter.z});

    Mat4 weaponMVP = proj * weaponModel;

    // Draw weapon mesh with material tint
    glUseProgram(m_unlitShader.program);
    glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, weaponMVP.m);

    const Material* wpnMat = MaterialSystem::get(def.materialId);
    Vec4 wpnTint = wpnMat ? wpnMat->tint : Vec4{0.7f, 0.7f, 0.7f, 1.0f};
    glUniform4f(m_unlitShader.loc_color, wpnTint.x, wpnTint.y, wpnTint.z, wpnTint.w);

    glActiveTexture(GL_TEXTURE0);
    if (wpnMat) {
        glBindTexture(GL_TEXTURE_2D, wpnMat->texture.handle);
    } else {
        const Material* fallback = MaterialSystem::get(0);
        if (fallback) glBindTexture(GL_TEXTURE_2D, fallback->texture.handle);
    }
    glUniform1i(m_unlitShader.loc_texture0, 0);

    if (hasWeapon) {
        MeshSystem::draw(m_meshDefs[weaponMeshId].mesh);
    }

    // Draw hand gripping the weapon (or fist if unarmed)
    // Hand sits at the weapon's base, rotated to wrap around the grip
    {
        const Material* fallback = MaterialSystem::get(0);
        Vec4 skinTint = {0.85f, 0.70f, 0.55f, 1.0f};
        glUniform4f(m_unlitShader.loc_color, skinTint.x, skinTint.y, skinTint.z, skinTint.w);
        if (fallback) glBindTexture(GL_TEXTURE_2D, fallback->texture.handle);

        // Hand at weapon grip — offset down from weapon center
        Mat4 handModel = Mat4::translate(offset)
                       * Mat4::rotateX(recoilPitch + attackPitch + holdPitch)
                       * Mat4::rotateY(holdYaw + attackYaw)
                       * Mat4::translate({0.0f, -0.12f, 0.05f}) // below weapon, slightly back
                       * Mat4::scale({1.2f, 1.2f, 1.2f});       // slightly larger than default
        Mat4 handMVP = proj * handModel;
        glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, handMVP.m);
        MeshSystem::draw(m_handMesh);

        // Forearm extending back from the hand toward the camera
        Mat4 armModel = Mat4::translate(offset)
                      * Mat4::rotateX(recoilPitch + attackPitch + holdPitch)
                      * Mat4::rotateY(holdYaw + attackYaw)
                      * Mat4::translate({0.02f, -0.18f, 0.25f}) // behind and below hand
                      * Mat4::rotateX(0.15f)  // slight angle following arm
                      * Mat4::scale({0.08f, 0.07f, 0.30f});     // elongated arm shape
        Mat4 armMVP = proj * armModel;
        glUniformMatrix4fv(m_unlitShader.loc_mvp, 1, GL_FALSE, armMVP.m);
        MeshSystem::draw(m_cubeMesh);
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void Engine::render(f32 alpha) {
    (void)alpha;

    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();

    if (m_gameState == GameState::MENU) {
        renderMenu();
        GLContext::swapBuffers(Window::getHandle());
        return;
    }

    if (m_gameState == GameState::CONNECTING) {
        // Pulsing dot to indicate connecting
        f32 pulse = (sinf(m_statsTimer * 6.0f) + 1.0f) * 0.5f;
        HUD::drawCrosshair(sw, sh, {pulse, pulse, 0.5f + pulse * 0.5f});
        GLContext::swapBuffers(Window::getHandle());
        return;
    }

    if (m_gameState == GameState::GAME_OVER) {
        // --- Death screen ---
        const char* deathTitle = "YOU DIED";
        f32 titleW = FontSystem::textWidth(deathTitle, 4);
        FontSystem::drawText(sw, sh, (sw - titleW) * 0.5f, sh * 0.6f, deathTitle, {0.8f, 0.1f, 0.1f}, 4);

        char floorStr[48];
        std::snprintf(floorStr, sizeof(floorStr), "Reached Floor %u", m_currentFloor);
        f32 floorW = FontSystem::textWidth(floorStr, 2);
        FontSystem::drawText(sw, sh, (sw - floorW) * 0.5f, sh * 0.45f, floorStr, {0.7f, 0.7f, 0.7f}, 2);

        const char* restartText = "Press ENTER to restart from last save";
        f32 restartW = FontSystem::textWidth(restartText, 1);
        FontSystem::drawText(sw, sh, (sw - restartW) * 0.5f, sh * 0.3f, restartText, {0.5f, 0.5f, 0.6f}, 1);

        const char* menuText = "Press ESC for main menu";
        f32 menuW = FontSystem::textWidth(menuText, 1);
        FontSystem::drawText(sw, sh, (sw - menuW) * 0.5f, sh * 0.22f, menuText, {0.4f, 0.4f, 0.5f}, 1);

        GLContext::swapBuffers(Window::getHandle());
        return;
    }

    if (m_gameState != GameState::IN_GAME) {
        GLContext::swapBuffers(Window::getHandle());
        return;
    }

    PROFILE_SCOPE(3, "Render");

    // Switch mode: reduced viewport
    if (m_switchMode) {
        sw = SWITCH_RES_W;
        sh = SWITCH_RES_H;
        glViewport(0, 0, sw, sh);
    }

    f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);
    CameraSystem::computeMatrices(m_camera, aspect);

    Renderer::beginFrame(m_camera);
    Renderer::setDirectionalLight(
        normalize(Vec3{-0.3f, -1.0f, -0.5f}),
        {1.0f, 0.95f, 0.9f},
        {0.15f, 0.15f, 0.2f}
    );

    // Level geometry
    LevelMeshSystem::submitAll(m_sections, m_sectionCount, m_basicShader);

    // Choose entity/projectile source based on role
    const EntityPool& entPool = (m_netRole == NetRole::CLIENT) ? m_renderEntities : m_entities;
    const ProjectilePool& projPool = (m_netRole == NetRole::CLIENT) ? m_renderProjectiles : m_projectiles;

    // Entities
    const Texture& defaultTex = MaterialSystem::get(0)->texture;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& e = entPool.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;

        f32 scaleY = 1.0f;
        if (e.flags & ENT_DEAD) {
            scaleY = (e.deathTimer > 0.0f) ? e.deathTimer : 0.01f;
        }

        Vec3 renderHalf = e.halfExtents;
        renderHalf.y *= scaleY;
        Vec3 renderPos = e.position;
        if (e.flags & ENT_DEAD) {
            renderPos.y -= e.halfExtents.y * (1.0f - scaleY);
        }

        // Use mesh from registry if available
        u8 meshId = e.meshId;
        const Mesh& entMesh = (meshId < m_meshDefCount) ? m_meshDefs[meshId].mesh : m_cubeMesh;

        // --- Procedural animation ---
        f32 animBobY = 0.0f;
        f32 animLean = 0.0f;   // forward tilt (pitch) in radians
        f32 animScaleX = 1.0f; // wing flap for bats
        bool isMoving = (lengthSq(e.velocity) > 0.1f);
        bool isBat = (e.flags & ENT_FLYING) != 0;

        if (!(e.flags & ENT_DEAD)) {
            if (isBat) {
                // No body bob — bat body stays steady, only wings move
                // Lean into dive during flyby
                if (e.aiState == AIState::FLYBY) {
                    animLean = -0.5f;
                }
                // Attack: body lunges forward
                if (e.attackAnimT > 0.0f) {
                    f32 t = e.attackAnimT / 0.4f;
                    animLean = -0.6f * t;
                    animBobY += 0.12f * t;
                }
            } else if (isMoving) {
                // Ground enemies: walking bob
                animBobY = sinf(e.animTimer * 10.0f) * 0.04f;
            }

            // Attack lunge for non-bat enemies
            if (!isBat && e.attackAnimT > 0.0f) {
                f32 t = e.attackAnimT / 0.3f; // 0→1
                animLean = -0.3f * t; // lean forward
                animBobY += 0.05f * t; // slight hop
            }
        }

        Mat4 model;
        if (meshId > 0 && meshId < m_meshDefCount) {
            const AABB& meshBounds = m_meshDefs[meshId].bounds;
            f32 meshH = meshBounds.max.y - meshBounds.min.y;
            f32 targetH = e.halfExtents.y * 2.0f * scaleY;
            f32 uniformScale = (meshH > 0.001f) ? (targetH / meshH) : 1.0f;
            Vec3 basePos = renderPos - Vec3{0, e.halfExtents.y * scaleY, 0}
                         + Vec3{0, animBobY, 0};
            model = Mat4::translate(basePos)
                  * Mat4::rotateY(e.yaw)
                  * Mat4::rotateX(animLean)
                  * Mat4::scale({uniformScale * animScaleX, uniformScale, uniformScale});
        } else {
            model = Mat4::translate(renderPos + Vec3{0, animBobY, 0})
                  * Mat4::rotateY(e.yaw)
                  * Mat4::rotateX(animLean)
                  * Mat4::scale(renderHalf * 2.0f);
        }
        AABB bounds = {renderPos - renderHalf, renderPos + renderHalf};

        // Use entity's material texture if assigned, otherwise fallback
        const Material* entMat = MaterialSystem::get(e.materialId);
        const Texture& entTex = (e.materialId > 0) ? entMat->texture : defaultTex;

        // Resolve tint — friendly NPCs get a distinct blue-green tint
        Vec4 tint;
        if (e.flags & ENT_FRIENDLY) {
            tint = {0.4f, 0.8f, 0.6f, 1.0f}; // ally blue-green
        } else if (e.enemyType == EnemyType::MIMIC) {
            // Dormant mimics look like normal chests; active ones turn red
            if (e.aiState == AIState::DORMANT) {
                tint = {0.6f, 0.4f, 0.2f, 1.0f}; // chest brown
            } else {
                tint = {0.9f, 0.3f, 0.2f, 1.0f}; // angry red
            }
        } else if (e.materialId > 0) {
            tint = entMat->tint;
        } else if (e.flags & ENT_FLYING) {
            tint = {0.4f, 0.5f, 1.0f, 1.0f};
        } else {
            tint = {0.8f, 0.5f, 0.3f, 1.0f};
        }
        if (e.flashTimer > 0.0f) {
            f32 flash = e.flashTimer / 0.12f;
            Vec4 flashColor = {1.0f, 0.3f * flash, 0.3f * flash, 1.0f};
            Renderer::submit(m_basicShader, entTex, entMesh, model, bounds, flashColor);
        } else {
            Renderer::submit(m_basicShader, entTex, entMesh, model, bounds, tint);
        }

        // Render articulated limbs (LOD: only when close enough to camera)
        if (e.enemyType != EnemyType::GENERIC && !(e.flags & ENT_DEAD)) {
            Vec3 toCamera = m_camera.position - e.position;
            if (lengthSq(toCamera) < LIMB_LOD_DIST_SQ) {
                const LimbConfig& limbCfg = LimbSystem::getConfig(e.enemyType);
                for (u32 li = 0; li < limbCfg.limbCount; li++) {
                    u8 limbMesh = LimbSystem::getLimbMeshId(e.enemyType, li);
                    if (limbMesh == 0 || limbMesh >= m_meshDefCount) continue;

                    const LimbDef& ld = limbCfg.limbs[li];
                    f32 angle = LimbSystem::computeAngle(e, li, e.enemyType);
                    if (ld.mirrored) angle = -angle;
                    angle += ld.restAngle;

                    // Build limb transform: entity feet position → pivot offset → rotation → box
                    Vec3 limbPivot = renderPos - Vec3{0, e.halfExtents.y * scaleY, 0}
                                   + Vec3{0, animBobY, 0}
                                   + ld.pivotOffset;

                    Mat4 limbRot;
                    switch (ld.pivotAxis) {
                        case 0: limbRot = Mat4::rotateX(angle); break;
                        case 1: limbRot = Mat4::rotateY(angle); break;
                        case 2: limbRot = Mat4::rotateZ(angle); break;
                        default: limbRot = Mat4::identity(); break;
                    }

                    // Scale limb proportionally to entity's rendered height
                    f32 limbScale = 1.0f;
                    if (meshId > 0 && meshId < m_meshDefCount) {
                        const AABB& meshBounds = m_meshDefs[meshId].bounds;
                        f32 meshH = meshBounds.max.y - meshBounds.min.y;
                        f32 targetH = e.halfExtents.y * 2.0f * scaleY;
                        limbScale = (meshH > 0.001f) ? (targetH / meshH) : 1.0f;
                    }

                    Mat4 limbModel = Mat4::translate(limbPivot)
                                   * Mat4::rotateY(e.yaw)
                                   * limbRot
                                   * Mat4::scale(ld.meshHalfSize * 2.0f * limbScale);

                    AABB limbBounds = {limbPivot - Vec3{0.5f,0.5f,0.5f},
                                       limbPivot + Vec3{0.5f,0.5f,0.5f}};

                    // Propagate hit flash to limbs to keep visual feedback consistent
                    Renderer::submit(m_basicShader, entTex, m_meshDefs[limbMesh].mesh,
                                     limbModel, limbBounds,
                                     (e.flashTimer > 0.0f)
                                         ? Vec4{1.0f, 0.3f * (e.flashTimer/0.12f), 0.3f * (e.flashTimer/0.12f), 1.0f}
                                         : tint);
                }

                // Skeleton weapon: attached to right arm, hilt in hand, swings with arm
                if (e.enemyType == EnemyType::SKELETON && e.weaponMeshId > 0 && e.weaponMeshId < m_meshDefCount) {
                    f32 armAngle = LimbSystem::computeAngle(e, 2, EnemyType::SKELETON);
                    // Mirror the angle (right side is mirrored in LimbConfig)
                    armAngle = -armAngle;

                    // Entity base (feet position)
                    Vec3 entBase = renderPos - Vec3{0, e.halfExtents.y * scaleY, 0}
                                 + Vec3{0, animBobY, 0};

                    f32 limbScale = 1.0f;
                    if (meshId > 0 && meshId < m_meshDefCount) {
                        const AABB& meshBounds = m_meshDefs[meshId].bounds;
                        f32 mH = meshBounds.max.y - meshBounds.min.y;
                        f32 targetH = e.halfExtents.y * 2.0f * scaleY;
                        limbScale = (mH > 0.001f) ? (targetH / mH) : 1.0f;
                    }

                    // Right arm pivot (shoulder), scaled to entity
                    Vec3 shoulder = {-0.35f * limbScale, 0.70f * limbScale, 0.0f};
                    // Arm length (upper + lower arm combined)
                    f32 armLen = 0.52f * limbScale;
                    // Hand position = shoulder + arm rotated by armAngle around X
                    // Arm hangs down by default, swings with angle
                    f32 handY = shoulder.y - armLen * cosf(armAngle);
                    f32 handZ = -armLen * sinf(armAngle);

                    // Scale weapon to fit in hand
                    const AABB& wb = m_meshDefs[e.weaponMeshId].bounds;
                    f32 wH = wb.max.y - wb.min.y;
                    f32 wScale = (wH > 0.001f) ? (0.45f * limbScale / wH) : 0.3f;

                    // Weapon position: entity base + rotated hand offset
                    // The weapon's hilt (bottom) should be at the hand
                    Vec3 weaponPos = entBase + Vec3{shoulder.x, handY, handZ};

                    Mat4 weaponModel = Mat4::translate(weaponPos)
                                     * Mat4::rotateY(e.yaw)
                                     * Mat4::rotateX(armAngle) // weapon follows arm swing
                                     * Mat4::scale({wScale, wScale, wScale})
                                     * Mat4::translate({0, wH * 0.5f, 0}); // offset so hilt is at hand

                    AABB wBounds = {weaponPos - Vec3{0.5f,0.5f,0.5f},
                                    weaponPos + Vec3{0.5f,0.5f,0.5f}};

                    Renderer::submit(m_basicShader, entTex, m_meshDefs[e.weaponMeshId].mesh,
                                     weaponModel, wBounds,
                                     Vec4{0.7f, 0.7f, 0.8f, 1.0f});
                }
            }
        }
    }

    // Projectiles
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        const Projectile& p = projPool.projectiles[i];
        if (!p.active) continue;

        if (p.projFlags & PROJ_SPARK) {
            // Lightning bolt: jagged line segments along the velocity direction
            Vec3 vel = p.velocity;
            f32 spd = length(vel);
            Vec3 dir = (spd > 0.01f) ? vel * (1.0f / spd) : Vec3{0, 0, -1};
            // Perpendicular axes for jitter
            Vec3 up = {0, 1, 0};
            Vec3 right = normalize(cross(dir, up));
            Vec3 perpUp = cross(right, dir);

            // Draw 4-segment jagged bolt from projectile tail to head
            static constexpr u32 BOLT_SEGS = 4;
            f32 boltLen = 0.8f;
            Vec3 prev = p.position - dir * boltLen * 0.5f;
            // Use position as seed for consistent jitter per bolt
            u32 seed = static_cast<u32>(p.position.x * 100 + p.position.z * 73 + p.lifetime * 200);
            for (u32 s = 1; s <= BOLT_SEGS; s++) {
                f32 t = static_cast<f32>(s) / BOLT_SEGS;
                Vec3 base = p.position - dir * boltLen * 0.5f + dir * (boltLen * t);
                if (s < BOLT_SEGS) {
                    // Jitter sideways for zigzag look
                    seed = seed * 1103515245u + 12345u;
                    f32 jx = (static_cast<f32>((seed >> 8) & 0xFF) / 128.0f - 1.0f) * 0.12f;
                    seed = seed * 1103515245u + 12345u;
                    f32 jy = (static_cast<f32>((seed >> 8) & 0xFF) / 128.0f - 1.0f) * 0.12f;
                    base = base + right * jx + perpUp * jy;
                }
                // Core bolt: bright white-blue
                DebugDraw::line(prev, base, {0.6f, 0.7f, 1.0f});
                // Glow: slightly offset second line
                DebugDraw::line(prev + right * 0.02f, base + right * 0.02f, {0.3f, 0.4f, 1.0f});
                prev = base;
            }
            // Small bright core at the tip
            DebugDraw::line(p.position - dir * 0.05f, p.position + dir * 0.05f, {1.0f, 1.0f, 1.0f});
        } else {
            // Normal projectile: colored cube
            Mat4 model = Mat4::translate(p.position)
                       * Mat4::scale({p.radius * 2.0f, p.radius * 2.0f, p.radius * 2.0f});
            AABB bounds = {
                p.position - Vec3{p.radius, p.radius, p.radius},
                p.position + Vec3{p.radius, p.radius, p.radius}
            };

            Vec4 projColor = p.fromPlayer
                ? Vec4{1.0f, 0.5f, 0.1f, 1.0f}
                : Vec4{0.8f, 0.2f, 1.0f, 1.0f};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, model, bounds, projColor);
        }
    }

    // --- Floor door — prominent glowing portal to next level ---
    if (m_floorDoorActive) {
        Vec3 dp = m_floorDoorPos;
        f32 t = static_cast<f32>(m_statsTimer);
        f32 pulse = 0.5f + 0.5f * sinf(t * 3.0f);
        f32 fastPulse = 0.5f + 0.5f * sinf(t * 8.0f);

        // Tall vertical beam (bright green, visible from far away)
        Vec3 beamCol = {0.1f, 0.9f * pulse, 0.2f};
        for (f32 ox = -0.08f; ox <= 0.08f; ox += 0.04f) {
            DebugDraw::line(dp + Vec3{ox, 0, 0}, dp + Vec3{ox, 4.0f, 0}, beamCol);
            DebugDraw::line(dp + Vec3{0, 0, ox}, dp + Vec3{0, 4.0f, ox}, beamCol);
        }

        // Spinning portal ring at waist height
        f32 ringY = dp.y + 1.0f;
        f32 ringR = 0.6f + fastPulse * 0.1f;
        Vec3 ringCol = {0.3f * pulse, 1.0f * pulse, 0.4f * pulse};
        for (u32 s = 0; s < 12; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / 12.0f) + t * 2.0f;
            f32 a1 = a0 + (6.28318f / 12.0f);
            Vec3 p0 = dp + Vec3{cosf(a0) * ringR, ringY - dp.y, sinf(a0) * ringR};
            Vec3 p1 = dp + Vec3{cosf(a1) * ringR, ringY - dp.y, sinf(a1) * ringR};
            DebugDraw::line(p0, p1, ringCol);
        }

        // Second ring at head height
        f32 ringY2 = dp.y + 2.0f;
        for (u32 s = 0; s < 12; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / 12.0f) - t * 1.5f;
            f32 a1 = a0 + (6.28318f / 12.0f);
            Vec3 p0 = dp + Vec3{cosf(a0) * ringR * 0.7f, ringY2 - dp.y, sinf(a0) * ringR * 0.7f};
            Vec3 p1 = dp + Vec3{cosf(a1) * ringR * 0.7f, ringY2 - dp.y, sinf(a1) * ringR * 0.7f};
            DebugDraw::line(p0, p1, {0.2f, 0.8f * pulse, 0.3f});
        }

        // Ground circle (large, static)
        for (u32 s = 0; s < 16; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / 16.0f);
            f32 a1 = a0 + (6.28318f / 16.0f);
            Vec3 p0 = dp + Vec3{cosf(a0) * 0.8f, 0.02f, sinf(a0) * 0.8f};
            Vec3 p1 = dp + Vec3{cosf(a1) * 0.8f, 0.02f, sinf(a1) * 0.8f};
            DebugDraw::line(p0, p1, {0.15f, 0.5f, 0.2f});
        }

        // Stairway steps descending
        for (u32 step = 0; step < 4; step++) {
            f32 s = static_cast<f32>(step);
            f32 y = dp.y - s * 0.2f;
            f32 z = dp.z + s * 0.3f;
            f32 w = 0.45f;
            Vec3 stepCol = {0.35f, 0.3f, 0.2f};
            DebugDraw::line({dp.x - w, y, z}, {dp.x + w, y, z}, stepCol);
            DebugDraw::line({dp.x - w, y, z + 0.25f}, {dp.x + w, y, z + 0.25f}, stepCol);
            DebugDraw::line({dp.x - w, y, z}, {dp.x - w, y, z + 0.25f}, stepCol);
            DebugDraw::line({dp.x + w, y, z}, {dp.x + w, y, z + 0.25f}, stepCol);
        }
    }

    // --- World items (rendered with weapon-specific meshes when available) ---
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        const WorldItem& wi = m_worldItems.items[i];
        if (!wi.active) continue;

        Vec3 color = rarityColor(wi.item.rarity);

        // Snap item to floor level of its grid cell
        f32 floorY = 0.0f;
        u32 gx, gz;
        if (LevelGridSystem::worldToGrid(m_grid, wi.position, gx, gz) &&
            !LevelGridSystem::isSolid(m_grid, gx, gz)) {
            floorY = LevelGridSystem::getFloorHeight(m_grid, gx, gz);
        }

        // Hover bob just above the floor (globes float lower and use smaller scale)
        static constexpr f32 ITEM_SCALE = 1.4f;
        bool isGlobeItem = isGlobe(wi.item);
        f32 renderScale = isGlobeItem ? 0.4f : ITEM_SCALE; // globes are small orbs
        f32 bobY = sinf(wi.bobTimer * 3.0f) * 0.08f;
        Vec3 pos = {wi.position.x, floorY + renderScale * 0.5f + bobY, wi.position.z};
        f32 spin = wi.bobTimer * 2.0f;

        // Globes render as small colored cubes; regular items use their mesh
        const Mesh* itemMesh = &m_cubeMesh;
        Texture itemTex = defaultTex;
        Vec4 tint = {color.x, color.y, color.z, 1.0f};

        if (isGlobeItem) {
            // Health globe: bright green; energy globe: bright blue
            if (wi.item.defId == GLOBE_HEALTH_ID) {
                tint = {0.2f, 1.0f, 0.3f, 1.0f};
            } else {
                tint = {0.3f, 0.5f, 1.0f, 1.0f};
            }
        } else if (wi.item.defId < m_itemDefCount) {
            // Use weapon-specific mesh and material if available
            const ItemDef& def = m_itemDefs[wi.item.defId];
            if (def.meshId > 0 && def.meshId < m_meshDefCount) {
                itemMesh = &m_meshDefs[def.meshId].mesh;
            }
            if (def.materialId > 0) {
                const Material* mat = MaterialSystem::get(def.materialId);
                if (mat) {
                    itemTex = mat->texture;
                    tint = {color.x * mat->tint.x, color.y * mat->tint.y,
                            color.z * mat->tint.z, 1.0f};
                }
            }
        }

        // Item mesh — use proper mesh, scaled to fit uniformly
        Mat4 model;
        if (itemMesh != &m_cubeMesh) {
            // Fit mesh into ITEM_SCALE box using its actual bounds
            model = Mat4::translate(pos) * Mat4::rotateY(spin) * Mat4::scale({renderScale, renderScale, renderScale});
        } else {
            // Cube fallback — globes use renderScale (0.4), other items use smaller 0.3 cube
            f32 cubeS = isGlobeItem ? renderScale : 0.3f;
            model = Mat4::translate(pos) * Mat4::rotateY(spin) * Mat4::scale({cubeS, cubeS, cubeS});
        }
        AABB bounds = {pos - Vec3{renderScale,renderScale,renderScale},
                       pos + Vec3{renderScale,renderScale,renderScale}};
        Renderer::submit(m_unlitShader, itemTex, *itemMesh, model, bounds, tint);

        if (!isGlobeItem) {
            // Rarity glow — pulsing cross of debug lines radiating from item center
            f32 glowPulse = 0.6f + 0.4f * sinf(wi.bobTimer * 4.0f);
            Vec3 gc = color * glowPulse;
            f32 gr = 0.4f + glowPulse * 0.2f; // glow radius
            DebugDraw::line(pos - Vec3{gr, 0, 0}, pos + Vec3{gr, 0, 0}, gc);
            DebugDraw::line(pos - Vec3{0, gr, 0}, pos + Vec3{0, gr, 0}, gc);
            DebugDraw::line(pos - Vec3{0, 0, gr}, pos + Vec3{0, 0, gr}, gc);
            // Diagonal cross for more glow volume
            f32 gd = gr * 0.7f;
            DebugDraw::line(pos - Vec3{gd, gd, 0}, pos + Vec3{gd, gd, 0}, gc);
            DebugDraw::line(pos - Vec3{gd, 0, gd}, pos + Vec3{gd, 0, gd}, gc);
            DebugDraw::line(pos - Vec3{0, gd, gd}, pos + Vec3{0, gd, gd}, gc);
            // Loot beam from floor upward
            DebugDraw::line({pos.x, floorY, pos.z}, {pos.x, floorY + 4.0f, pos.z}, color);
        } else {
            // Globe glow — simple pulsing cross of colored lines
            f32 glowPulse = 0.5f + 0.5f * sinf(wi.bobTimer * 6.0f);
            Vec3 gc = (wi.item.defId == GLOBE_HEALTH_ID)
                ? Vec3{0.1f, glowPulse, 0.15f}
                : Vec3{0.15f, 0.25f, glowPulse};
            f32 gr = 0.3f;
            DebugDraw::line(pos - Vec3{gr, 0, 0}, pos + Vec3{gr, 0, 0}, gc);
            DebugDraw::line(pos - Vec3{0, gr, 0}, pos + Vec3{0, gr, 0}, gc);
            DebugDraw::line(pos - Vec3{0, 0, gr}, pos + Vec3{0, 0, gr}, gc);
        }
    }

    // Remote players (multiplayer only)
    if (m_netRole != NetRole::NONE) {
        for (u32 i = 0; i < MAX_PLAYERS; i++) {
            if (i == m_localPlayerIndex) continue;

            bool active = false;
            Vec3 pos;
            f32 yaw = 0.0f;

            if (m_netRole == NetRole::CLIENT) {
                active = m_renderPlayerActive[i];
                pos = m_renderPlayerPositions[i];
                yaw = m_renderPlayerYaws[i];
            } else {
                // Server: use authoritative state
                active = m_players[i].active;
                pos = m_players[i].position;
                yaw = m_players[i].yaw;
            }

            if (!active) continue;

            Vec3 half = {0.3f, 0.9f, 0.3f}; // player-sized
            Mat4 model = Mat4::translate(pos + Vec3{0, half.y, 0})
                       * Mat4::rotateY(yaw)
                       * Mat4::scale(half * 2.0f);
            AABB bounds = {pos, pos + Vec3{half.x*2, half.y*2, half.z*2}};

            // Color by player slot
            Vec4 colors[4] = {
                {0.2f, 0.8f, 0.2f, 1.0f}, // green
                {0.2f, 0.5f, 1.0f, 1.0f}, // blue
                {1.0f, 0.8f, 0.2f, 1.0f}, // yellow
                {1.0f, 0.3f, 0.3f, 1.0f}, // red
            };
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, model, bounds, colors[i]);
        }
    }

    { PROFILE_SCOPE(4, "Flush");
    Renderer::flush();
    }

    // --- Debug overlay ---
    DebugDraw::clear();
    if (DebugDraw::isEnabled()) {
        Vec3 feet = m_localPlayer.position;
        AABB playerBox = {
            feet + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
            feet + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
        };
        Vec3 boxColor = m_localPlayer.onGround ? Vec3{0,1,0} : Vec3{1,1,0};
        DebugDraw::box(playerBox, boxColor);

        for (u32 i = 0; i < MAX_ENTITIES; i++) {
            const Entity& e = entPool.entities[i];
            if (!(e.flags & ENT_ACTIVE)) continue;
            Vec3 c = (e.flags & ENT_DEAD) ? Vec3{0.5f,0.5f,0.5f}
                   : (e.flags & ENT_FLYING) ? Vec3{0.3f,0.3f,1.0f}
                   : Vec3{1.0f,0.3f,0.3f};
            DebugDraw::box(entityAABB(e), c);
        }

        if (m_lastCombatHit.hit) {
            Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};
            DebugDraw::line(eyePos, m_lastCombatHit.position, {1,0,0});
            DebugDraw::cross(m_lastCombatHit.position, 0.15f, {1,0.5f,0});
            DebugDraw::ray(m_lastCombatHit.position, m_lastCombatHit.normal, 0.5f, {1,1,0});
        }
    }

    // Target lock indicator
    if (m_localPlayer.lockActive) {
        const EntityPool& lockPool = (m_netRole == NetRole::CLIENT) ? m_renderEntities : m_entities;
        EntityHandle h = {m_localPlayer.lockIndex, m_localPlayer.lockGeneration};
        Entity* target = handleGet(const_cast<EntityPool&>(lockPool), h);
        if (target) {
            AABB lockBox = entityAABB(*target);
            lockBox.min = lockBox.min - Vec3{0.05f, 0.05f, 0.05f};
            lockBox.max = lockBox.max + Vec3{0.05f, 0.05f, 0.05f};
            bool wasEnabled = DebugDraw::isEnabled();
            DebugDraw::setEnabled(true);
            DebugDraw::box(lockBox, {0.0f, 1.0f, 1.0f});
            DebugDraw::setEnabled(wasEnabled);
        }
    }

    // --- Fire AoE effects (molotov splash) ---
    for (u32 i = 0; i < MAX_FIRE_FX; i++) {
        if (!m_fireFX[i].active) continue;
        const FireFX& fx = m_fireFX[i];
        f32 t = 1.0f - fx.timer; // 0→1 over lifetime
        f32 alpha = fx.timer;    // fades out
        f32 r = fx.radius * (0.3f + t * 0.7f); // expands outward

        // Draw radiating lines from center (fire burst pattern)
        static constexpr u32 FIRE_RAYS = 12;
        for (u32 ray = 0; ray < FIRE_RAYS; ray++) {
            f32 angle = static_cast<f32>(ray) * (6.28318f / FIRE_RAYS) + t * 2.0f;
            f32 dx = cosf(angle) * r;
            f32 dz = sinf(angle) * r;
            // Flame color: orange core, red tips
            Vec3 col = {1.0f * alpha, (0.4f + 0.3f * sinf(angle * 3.0f)) * alpha, 0.1f * alpha};
            // Ground-level radiating lines
            DebugDraw::line(fx.pos, fx.pos + Vec3{dx, 0.1f, dz}, col);
            // Upward flame wisps
            f32 h = 0.5f + sinf(angle * 2.0f + t * 8.0f) * 0.3f;
            DebugDraw::line(fx.pos + Vec3{dx * 0.5f, 0, dz * 0.5f},
                            fx.pos + Vec3{dx * 0.3f, h * alpha, dz * 0.3f},
                            {1.0f * alpha, 0.6f * alpha, 0.0f});
        }
    }

    DebugDraw::flush(m_camera.viewProjection);

    // --- Speech bubbles above entities ---
    // Uses the render entity pool (client uses interpolated snapshot, SP uses live pool)
    {
        const EntityPool& speechPool = (m_netRole == NetRole::CLIENT) ? m_renderEntities : m_entities;
        for (u32 a = 0; a < speechPool.activeCount; a++) {
            u32 idx = speechPool.activeList[a];
            const Entity& e = speechPool.entities[idx];
            if (!e.speechText || e.speechTimer <= 0.0f) continue;

            // Project a point above the entity's head into clip space
            Vec3 headPos = e.position + Vec3{0, e.halfExtents.y * 2.0f + 0.3f, 0};

            // Manual column-major Mat4 * Vec4 (no operator overload assumed)
            const f32* vp = m_camera.viewProjection.m;
            f32 cx = vp[0]*headPos.x + vp[4]*headPos.y + vp[8]*headPos.z  + vp[12];
            f32 cy = vp[1]*headPos.x + vp[5]*headPos.y + vp[9]*headPos.z  + vp[13];
            f32 cw = vp[3]*headPos.x + vp[7]*headPos.y + vp[11]*headPos.z + vp[15];

            if (cw <= 0.01f) continue; // behind the camera

            // NDC to pixel screen coords (y is flipped: NDC +1 = screen top)
            f32 ndcX = cx / cw;
            f32 ndcY = cy / cw;
            f32 screenX = (ndcX + 1.0f) * 0.5f * static_cast<f32>(sw);
            f32 screenY = (1.0f - ndcY) * 0.5f * static_cast<f32>(sh);

            // Cull bubbles that are well off-screen
            if (screenX < -100.0f || screenX > static_cast<f32>(sw) + 100.0f) continue;
            if (screenY < -50.0f  || screenY > static_cast<f32>(sh) + 50.0f)  continue;

            // Fade alpha in the last second of the timer
            f32 alpha = (e.speechTimer < 1.0f) ? e.speechTimer : 1.0f;

            // Green for allies, red for hostile entities
            Vec3 textColor = (e.flags & ENT_FRIENDLY)
                ? Vec3{0.4f, 1.0f, 0.5f}   // ally green
                : Vec3{1.0f, 0.4f, 0.4f};  // enemy red

            HUD::drawSpeechBubble(sw, sh, screenX, screenY, e.speechText, textColor, alpha);
        }
    }

    // Floor door interaction prompt — shown when player is within trigger range
    if (m_floorDoorActive && m_gameState == GameState::IN_GAME) {
        Vec3 toDoor = m_floorDoorPos - m_localPlayer.position;
        if (lengthSq(toDoor) < 4.0f) {
            char doorStr[48];
            std::snprintf(doorStr, sizeof(doorStr), "Press E to descend to Floor %u", m_currentFloor + 1);
            f32 textW = FontSystem::textWidth(doorStr, 1);
            FontSystem::drawText(sw, sh,
                (static_cast<f32>(sw) - textW) * 0.5f,
                static_cast<f32>(sh) * 0.4f,
                doorStr, {0.3f, 1.0f, 0.4f}, 1);
        }
    }

    // First-person viewmodel (hand + weapon) — drawn after world, before HUD
    renderViewmodel();

    // --- HUD ---
    if (m_inventoryOpen) {
        // Inventory screen replaces normal HUD elements
        s32 invMX, invMY;
        Input::getMousePosition(invMX, invMY);
        invMY = static_cast<s32>(sh) - invMY; // flip to HUD coords
        HUD::drawInventoryScreen(sw, sh, m_inventories[m_localPlayerIndex],
                                  m_itemDefs, 0, false, invMX, invMY);
    } else {
        Vec3 crossColor = (m_localPlayer.damageFlashTimer > 0.0f)
                        ? Vec3{1.0f, 0.3f, 0.3f}
                        : Vec3{1.0f, 1.0f, 1.0f};
        HUD::drawCrosshair(sw, sh, crossColor);

        if (m_hitMarkerTimer > 0.0f)
            HUD::drawHitMarker(sw, sh, m_hitMarkerTimer / 0.2f);

        HUD::drawHealthBar(sw, sh, m_localPlayer.health, m_localPlayer.maxHealth);

        HUD::drawWeaponIndicator(sw, sh, m_quickbars[m_localPlayerIndex].activeSlot);

        // Energy bar and skill cooldown
        const SkillState& ss = m_skillStates[m_localPlayerIndex];
        if (ss.activeSkill != SkillId::NONE) {
            HUD::drawEnergyBar(sw, sh, ss.energy, ss.maxEnergy);
            if (ss.cooldownTimer > 0.0f) {
                f32 maxCd = 1.0f;
                for (u32 i = 0; i < m_skillDefCount; i++) {
                    if (m_skillDefs[i].id == ss.activeSkill) {
                        maxCd = m_skillDefs[i].cooldown;
                        break;
                    }
                }
                HUD::drawSkillCooldown(sw, sh, ss.cooldownTimer / maxCd);
            } else {
                HUD::drawSkillCooldown(sw, sh, 0.0f);
            }
        }

        // Minimap (top-right corner)
        Minimap::draw(sw, sh, m_grid, m_localPlayer.position, m_localPlayer.yaw);

        // Door marker on minimap (pulsing green "V" symbol at door grid position)
        if (m_floorDoorActive) {
            u32 doorGx, doorGz;
            if (LevelGridSystem::worldToGrid(m_grid, m_floorDoorPos, doorGx, doorGz)) {
                // Convert grid coords to minimap screen position
                // Minimap: top-right, 150x150px, 10px margin
                f32 mapSize = 150.0f;
                f32 margin = 10.0f;
                f32 mapX = static_cast<f32>(sw) - mapSize - margin;
                f32 mapY = static_cast<f32>(sh) - mapSize - margin;
                f32 normX = (static_cast<f32>(doorGx) + 0.5f) / static_cast<f32>(m_grid.width);
                f32 normZ = (static_cast<f32>(doorGz) + 0.5f) / static_cast<f32>(m_grid.depth);
                f32 dotX = mapX + normX * mapSize;
                f32 dotY = mapY + (1.0f - normZ) * mapSize; // Z flipped

                f32 doorPulse = 0.7f + 0.3f * sinf(m_statsTimer * 5.0f);
                Vec3 doorCol = {0.2f * doorPulse, 1.0f * doorPulse, 0.3f * doorPulse};
                FontSystem::drawText(sw, sh, dotX - 3.0f, dotY - 4.0f, "V", doorCol, 1);
            }
        }

        // Floor indicator (top-left)
        {
            char floorStr[32];
            std::snprintf(floorStr, sizeof(floorStr), "Floor %u", m_currentFloor);
            FontSystem::drawText(sw, sh, 20.0f, static_cast<f32>(sh) - 20.0f,
                                 floorStr, {0.7f, 0.7f, 0.7f}, 1);
        }

        // Potion cooldown indicator (below floor text, Q key binding hint)
        if (m_potionCooldown > 0.0f) {
            char potStr[32];
            std::snprintf(potStr, sizeof(potStr), "Potion: %.0fs", m_potionCooldown);
            FontSystem::drawText(sw, sh, 20.0f, static_cast<f32>(sh) - 35.0f,
                                 potStr, {0.8f, 0.3f, 0.3f}, 1);
        } else {
            FontSystem::drawText(sw, sh, 20.0f, static_cast<f32>(sh) - 35.0f,
                                 "Q: Potion", {0.3f, 0.8f, 0.3f}, 1);
        }
    }

    // Quickbar — always visible at bottom of screen
    {
        f32 cdPct = 0.0f;
        WeaponState& ws = m_players[m_localPlayerIndex].weaponState;
        // Get cooldown percentage for active quickbar weapon
        const ItemInstance* activeItem = Quickbar::resolveSlot(
            m_quickbars[m_localPlayerIndex], m_inventories[m_localPlayerIndex],
            m_quickbars[m_localPlayerIndex].activeSlot);
        if (activeItem && !isItemEmpty(*activeItem)) {
            const ItemDef& def = m_itemDefs[activeItem->defId];
            if (def.baseCooldown > 0.0f && ws.cooldownTimer > 0.0f) {
                cdPct = ws.cooldownTimer / def.baseCooldown;
                if (cdPct > 1.0f) cdPct = 1.0f;
            }
        }
        HUD::drawQuickbar(sw, sh, m_quickbars[m_localPlayerIndex],
                           m_inventories[m_localPlayerIndex], m_itemDefs, cdPct);
    }

    // Profiler overlay (F3)
    HUD::drawProfiler(sw, sh);

    // Net stats overlay in multiplayer
    if (m_netRole != NetRole::NONE) {
        u32 ping = 0;
        if (m_netRole == NetRole::CLIENT) {
            NetStats stats = Net::getStats(m_localPlayerIndex);
            ping = static_cast<u32>(stats.rttMs);
        }
        HUD::drawNetStats(sw, sh, Net::getConnectedCount(), ping,
                          m_netRole == NetRole::SERVER ? "HOST" : "CLIENT");
    }

    GLContext::swapBuffers(Window::getHandle());
}

// ---------------------------------------------------------------------------
// Menu rendering (simple text-based using HUD lines)
// ---------------------------------------------------------------------------
void Engine::renderMenu() {
    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();

    // Title text
    {
        const char* title = "DUNGEON ENGINE";
        f32 titleW = FontSystem::textWidth(title, 3);
        f32 titleX = (static_cast<f32>(sw) - titleW) * 0.5f;
        f32 titleY = sh * 0.65f;
        FontSystem::drawText(sw, sh, titleX, titleY, title, {0.9f, 0.85f, 0.7f}, 3);
    }

    // Menu options with text labels
    static const char* labels[] = {"Single Player", "Host Game", "Join Game", "Exit Game"};
    Vec3 colors[] = {
        {0.2f, 0.9f, 0.2f}, // singleplayer - green
        {0.2f, 0.5f, 1.0f}, // host - blue
        {1.0f, 0.7f, 0.2f}, // join - orange
        {0.7f, 0.2f, 0.2f}, // exit - red
    };

    for (u32 i = 0; i < 4; i++) {
        f32 y = sh * 0.25f + (3 - i) * 50.0f;
        Vec3 color = colors[i];
        bool selected = (i == m_menuSelection);
        if (!selected) {
            color = color * 0.4f;
        }
        HUD::drawMenuOption(sw, sh, y, 250, 35, color, selected);

        // Text label centered on the bar
        f32 textW = FontSystem::textWidth(labels[i], 2);
        f32 textX = (static_cast<f32>(sw) - textW) * 0.5f;
        f32 textY = y + 10.0f;
        Vec3 textColor = selected ? Vec3{1.0f, 1.0f, 1.0f} : Vec3{0.6f, 0.6f, 0.6f};
        FontSystem::drawText(sw, sh, textX, textY, labels[i], textColor, 2);
    }

    // Controls hint at bottom
    {
        const char* hint = "Up/Down to select, Enter to confirm";
        f32 hintW = FontSystem::textWidth(hint, 1);
        f32 hintX = (static_cast<f32>(sw) - hintW) * 0.5f;
        FontSystem::drawText(sw, sh, hintX, sh * 0.15f, hint, {0.4f, 0.4f, 0.5f}, 1);
    }
}

void Engine::renderLobby() {
    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();
    HUD::drawCrosshair(sw, sh, {0.3f, 0.3f, 1.0f});
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
void Engine::logStats() {
    f64 avgFrameTime = (m_frameCount > 0) ? (1000.0 / m_frameCount) : 0.0;

    if (m_netRole == NetRole::NONE) {
        LOG_INFO("FPS: %u | Frame: %.2f ms | Draw: %u | Vis: %u | Ent: %u | Proj: %u | HP: %.0f",
                 m_frameCount, avgFrameTime,
                 Renderer::getDrawCallCount(), Renderer::getVisibleCount(),
                 EntitySystem::activeCount(m_entities),
                 m_projectiles.activeCount,
                 m_localPlayer.health);
    } else {
        LOG_INFO("FPS: %u | Frame: %.2f ms | Draw: %u | Ent: %u | Players: %u | Tick: %u | %s",
                 m_frameCount, avgFrameTime,
                 Renderer::getDrawCallCount(),
                 EntitySystem::activeCount(m_entities),
                 Net::getConnectedCount(),
                 m_serverTick,
                 m_netRole == NetRole::SERVER ? "SERVER" : "CLIENT");
    }
}
