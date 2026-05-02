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
    LimbSystem::init(m_meshDefs, m_meshDefCount);

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
        (void)pool; (void)entityIndex;
        if (!s_engine) return;
        // 40% base drop chance
        if ((std::rand() % 100) < 40) {
            u8 enemyLevel = 1; // TODO: derive from entity or dungeon depth
            ItemInstance item = ItemGen::rollItem(enemyLevel, s_engine->m_itemDefs,
                                                   s_engine->m_itemDefCount,
                                                   s_engine->m_affixDefs,
                                                   s_engine->m_affixDefCount);
            if (!isItemEmpty(item)) {
                WorldItemSystem::spawn(s_engine->m_worldItems, item,
                                       position + Vec3{0, 0.5f, 0});
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

void Engine::startGame() {
    // Build level — use BSP procedural generation with random seed
    u32 dungeonSeed = static_cast<u32>(std::rand());
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
            {"skeleton", 50.0f, 2.5f, 15.0f, 2.5f, 1.0f, 10.0f, {0.4f, 0.9f, 0.4f}, false},
            {"bat",      30.0f, 7.0f, 18.0f, 8.0f, 0.8f, 12.0f, {0.5f, 0.4f, 0.4f}, true},
            {"spider",   40.0f, 3.5f, 12.0f, 2.0f, 0.8f, 12.0f, {0.5f, 0.3f, 0.5f}, false},
        };

        for (u32 r = 1; r < dungeon.roomCount; r++) {
            const DungeonRoom& room = dungeon.rooms[r];

            // 1-3 enemies per room, scaled by room area
            u32 area = room.w * room.d;
            u32 enemyCount = 1 + (area / 20);
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

    LOG_INFO("Spawned %u enemies", EntitySystem::activeCount(m_entities));
    ProjectileSystem::init(m_projectiles);

    // Init inventory & world items
    WorldItemSystem::init(m_worldItems);
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        Inventory::init(m_inventories[i]);
        m_skillStates[i] = {};
        // Quickbar init after inventory so weapon slot sync is correct
        Quickbar::init(m_quickbars[i], m_inventories[i]);
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
    m_players[m_localPlayerIndex].health = 100.0f;
    m_players[m_localPlayerIndex].maxHealth = 100.0f;
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
    if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) {
        if (m_gameState == GameState::IN_GAME) {
            m_running = false;
        } else if (m_gameState != GameState::MENU) {
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
        if (m_menuSelection < 2) m_menuSelection++;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_RETURN) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
        switch (m_menuSelection) {
        case 0: // Singleplayer
            m_netRole = NetRole::NONE;
            m_localPlayerIndex = 0;
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

    // Projectiles
    { PROFILE_SCOPE(2, "Projectiles");
    ProjectileSystem::update(m_projectiles, m_grid, m_entities, m_localPlayer, dt);
    }

    // Entity timers
    EntitySystem::tickTimers(m_entities, dt);

    // Update world items
    WorldItemSystem::update(m_worldItems, dt);

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

    // Item pickup (E key)
    if (Input::isKeyPressed(SDL_SCANCODE_E)) {
        ItemInstance picked;
        if (WorldItemSystem::tryPickup(m_worldItems, m_localPlayer.position, 0, picked)) {
            if (Inventory::addToBackpack(m_inventories[0], picked)) {
                LOG_INFO("Picked up item (defId=%u, rarity=%u)", picked.defId, (u32)picked.rarity);
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
    case WeaponType::PROJECTILE:
        Combat::fireProjectile(wpn, eyePos, forward, m_projectiles);
        result.didFire = true;
        break;
    }

    // Amplified kick for viewmodel so the visual response is clearly visible
    m_viewmodelState.recoilKick += wpn.recoilKick * 1.5f;
    // Ranged weapons get rapid vibration while firing
    if (wpn.type != WeaponType::MELEE) m_viewmodelState.fireShakeTimer = 0.15f;
    // Melee fires a swing animation of fixed duration
    if (wpn.type == WeaponType::MELEE) m_viewmodelState.attackAnimT = 0.3f;
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

    // Resolve equipped weapon mesh
    const ItemInstance& equipped = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
    if (isItemEmpty(equipped)) return;

    u8 weaponMeshId = m_itemDefs[equipped.defId].meshId;
    if (weaponMeshId == 0 || weaponMeshId >= m_meshDefCount) return;

    const ItemDef& def = m_itemDefs[equipped.defId];

    // Clear depth so viewmodel renders on top of everything
    glClear(GL_DEPTH_BUFFER_BIT);

    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();
    f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);

    // Tight near plane for viewmodel
    Mat4 proj = Mat4::perspective(70.0f * (3.14159f / 180.0f), aspect, 0.01f, 10.0f);

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
        f32 t = m_viewmodelState.attackAnimT / 0.3f; // normalized 1→0

        if (def.weaponType == WeaponType::MELEE) {
            switch (def.weaponSubtype) {
                case WeaponSubtype::DAGGER:
                case WeaponSubtype::THROWING_KNIFE:
                    // Stab: thrust forward then retract
                    attackZ = -0.25f * sinf(t * 3.14159f);
                    attackPitch = -0.15f * t; // slight downward angle during stab
                    break;
                case WeaponSubtype::AXE:
                    // Heavy overhead chop: big downward arc
                    attackPitch = -0.9f * sinf(t * 3.14159f);
                    break;
                case WeaponSubtype::SWORD:
                default:
                    // Horizontal slash: sweep from right to left
                    attackYaw = -0.8f * sinf(t * 3.14159f);
                    attackPitch = -0.15f * t; // slight lean into swing
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
            holdYaw = 0.4f;
            holdPitch = -0.2f;
            break;
        case WeaponType::HITSCAN:
            offset = {0.40f + bobX, -0.30f + bobY, -0.50f};
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

    MeshSystem::draw(m_meshDefs[weaponMeshId].mesh);
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
                // Wing flap: dramatic X+Z scale pulsing
                f32 flapSpeed = isMoving ? 16.0f : 8.0f;
                f32 flapAmp = isMoving ? 0.35f : 0.25f;
                animScaleX = 1.0f + sinf(e.animTimer * flapSpeed) * flapAmp;
                // Z-axis wing tilt for depth
                f32 animScaleZ = 1.0f + cosf(e.animTimer * flapSpeed) * flapAmp * 0.5f;
                renderHalf.z *= animScaleZ;
                // Hover bob — more pronounced
                animBobY = sinf(e.animTimer * 5.0f) * 0.08f;
                // Lean into dive during flyby
                if (e.aiState == AIState::FLYBY) {
                    animLean = -0.5f; // steep nose-down dive
                }
                // Attack swipe: rapid wing fold + body roll
                if (e.attackAnimT > 0.0f) {
                    f32 t = e.attackAnimT / 0.4f; // 0→1
                    animScaleX = 0.5f + 0.5f * (1.0f - t); // wings fold in then snap out
                    animLean = -0.6f * t; // aggressive forward lunge
                    animBobY += 0.12f * t; // hop upward during swipe
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

        // Resolve tint once so it's available for both body and limb rendering
        Vec4 tint = (e.materialId > 0) ? entMat->tint : (
            (e.flags & ENT_FLYING)
                ? Vec4{0.4f, 0.5f, 1.0f, 1.0f}
                : Vec4{0.8f, 0.5f, 0.3f, 1.0f}
        );
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

                // Skeleton weapon: rendered at right-hand position, rotated with arm swing
                if (e.enemyType == EnemyType::SKELETON && e.weaponMeshId > 0 && e.weaponMeshId < m_meshDefCount) {
                    f32 armAngle = LimbSystem::computeAngle(e, 2, EnemyType::SKELETON); // right upper arm

                    Vec3 weaponPivot = renderPos - Vec3{0, e.halfExtents.y * scaleY, 0}
                                    + Vec3{0, animBobY, 0}
                                    + Vec3{-0.35f, 0.30f, 0.0f}; // right hand position

                    f32 limbScale = 1.0f;
                    if (meshId > 0 && meshId < m_meshDefCount) {
                        const AABB& meshBounds = m_meshDefs[meshId].bounds;
                        f32 meshH = meshBounds.max.y - meshBounds.min.y;
                        f32 targetH = e.halfExtents.y * 2.0f * scaleY;
                        limbScale = (meshH > 0.001f) ? (targetH / meshH) : 1.0f;
                    }

                    // Scale weapon proportionally to limb size
                    const AABB& wb = m_meshDefs[e.weaponMeshId].bounds;
                    f32 wH = wb.max.y - wb.min.y;
                    f32 wScale = (wH > 0.001f) ? (0.5f * limbScale / wH) : 0.3f;

                    Mat4 weaponModel = Mat4::translate(weaponPivot)
                                     * Mat4::rotateY(e.yaw)
                                     * Mat4::rotateX(-armAngle)
                                     * Mat4::scale({wScale, wScale, wScale});

                    AABB wBounds = {weaponPivot - Vec3{0.5f,0.5f,0.5f},
                                    weaponPivot + Vec3{0.5f,0.5f,0.5f}};

                    // Steel tint for skeleton weapons
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

        // Hover bob just above the floor
        static constexpr f32 ITEM_SCALE = 1.4f;
        f32 bobY = sinf(wi.bobTimer * 3.0f) * 0.08f;
        Vec3 pos = {wi.position.x, floorY + ITEM_SCALE * 0.5f + bobY, wi.position.z};
        f32 spin = wi.bobTimer * 2.0f;

        // Use weapon-specific mesh if available, otherwise cube fallback
        const Mesh* itemMesh = &m_cubeMesh;
        Texture itemTex = defaultTex;
        Vec4 tint = {color.x, color.y, color.z, 1.0f};

        if (wi.item.defId < m_itemDefCount) {
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
            model = Mat4::translate(pos) * Mat4::rotateY(spin) * Mat4::scale({ITEM_SCALE, ITEM_SCALE, ITEM_SCALE});
        } else {
            // Cube fallback — smaller so it doesn't look like a block
            model = Mat4::translate(pos) * Mat4::rotateY(spin) * Mat4::scale({0.3f, 0.3f, 0.3f});
        }
        AABB bounds = {pos - Vec3{ITEM_SCALE,ITEM_SCALE,ITEM_SCALE}, pos + Vec3{ITEM_SCALE,ITEM_SCALE,ITEM_SCALE}};
        Renderer::submit(m_unlitShader, itemTex, *itemMesh, model, bounds, tint);

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

    DebugDraw::flush(m_camera.viewProjection);

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

    // Menu title indicator (top area)
    Vec3 titleColor = {0.8f, 0.8f, 0.8f};
    HUD::drawMenuOption(sw, sh, sh * 0.6f, 280, 20, titleColor, false);

    // Draw 3 options as colored bars
    Vec3 colors[] = {
        {0.2f, 0.9f, 0.2f}, // singleplayer - green
        {0.2f, 0.5f, 1.0f}, // host - blue
        {1.0f, 0.7f, 0.2f}, // join - orange
    };

    for (u32 i = 0; i < 3; i++) {
        f32 y = sh * 0.35f + (2 - i) * 55.0f; // bottom-up (HUD Y=0 is bottom)
        Vec3 color = colors[i];
        if (i != m_menuSelection) {
            color = color * 0.4f; // dim unselected
        }
        HUD::drawMenuOption(sw, sh, y, 250, 35, color, i == m_menuSelection);
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
