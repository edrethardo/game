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

#include <glad/glad.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static FrameAllocator s_frameAllocator;

// Global engine pointer for static callbacks
static Engine* s_engine = nullptr;

// ---------------------------------------------------------------------------
// Player class definitions — 8 classes with 4 skills each
// ---------------------------------------------------------------------------
const ClassDef kClassDefs[static_cast<u32>(PlayerClass::CLASS_COUNT)] = {
    // WARRIOR — Melee Tank
    {"Warrior", "Heavy melee fighter with crowd control",
     150.0f, 5.0f, 80.0f, "Iron Sword",
     {SkillId::CLEAVE, SkillId::WAR_CRY, SkillId::WHIRLWIND, SkillId::EARTHQUAKE},
     {1, 10, 20, 30}, {5, 20, 30, 40}},

    // RANGER — Ranged DPS
    {"Ranger", "Agile archer with poison and piercing shots",
     80.0f, 6.5f, 100.0f, "Short Bow",
     {SkillId::MULTI_SHOT, SkillId::RAIN_OF_ARROWS, SkillId::POISON_ARROW, SkillId::SHADOW_SHOT},
     {1, 10, 20, 30}, {5, 20, 30, 40}},

    // SORCERER — Glass Cannon
    {"Sorcerer", "Devastating elemental magic, fragile body",
     70.0f, 5.5f, 150.0f, "Wand of Sparks",
     {SkillId::FIREBALL, SkillId::FROZEN_ORB, SkillId::CHAIN_LIGHTNING, SkillId::METEOR_STRIKE},
     {1, 10, 20, 30}, {5, 20, 30, 40}},

    // ROGUE — Hit-and-Run
    {"Rogue", "Fast assassin with teleports and poison",
     85.0f, 7.0f, 100.0f, "Rusty Dagger",
     {SkillId::KNIFE_BURST, SkillId::PHASE_DASH, SkillId::POISON_CLOUD, SkillId::SHADOW_STRIKE},
     {1, 10, 20, 30}, {5, 20, 30, 40}},

    // PALADIN — Holy Tank/Support
    {"Paladin", "Holy warrior who heals and protects",
     130.0f, 5.0f, 90.0f, "Iron Sword",
     {SkillId::HOLY_SMITE, SkillId::CONSECRATION, SkillId::BLOOD_NOVA, SkillId::DIVINE_SHIELD},
     {1, 10, 20, 30}, {5, 20, 30, 40}},

    // COMBAT ENGINEER — Gadget Specialist
    {"Combat Engineer", "Turrets, tesla coils, and gadgets",
     100.0f, 5.5f, 120.0f, "Pistol",
     {SkillId::SHOCK_BOLT, SkillId::DEPLOY_TURRET, SkillId::TESLA_COIL, SkillId::MECH_OVERDRIVE},
     {1, 10, 20, 30}, {5, 20, 30, 40}},

    // MARKSMAN — Precision Sniper
    {"Marksman", "Precise hitscan specialist with executes",
     75.0f, 6.0f, 100.0f, "Revolver",
     {SkillId::AIMED_SHOT, SkillId::EXPLOSIVE_ROUND, SkillId::RAPID_FIRE, SkillId::HEADSHOT},
     {1, 10, 20, 30}, {5, 20, 30, 40}},

    // TINKERER — Minion Master
    {"Tinkerer", "Drone commander with stun grenades",
     90.0f, 5.5f, 110.0f, "Pistol",
     {SkillId::COMBAT_DRONE, SkillId::SWARM_DRONES, SkillId::STUN_GRENADE, SkillId::DEPLOY_TURRET},
     {1, 10, 20, 30}, {5, 20, 30, 40}},
};

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
// Mesh name lookup helper
// ---------------------------------------------------------------------------
// Linear scan over the mesh registry. Only called during init/startGame, so
// O(n) cost is acceptable — runtime hot paths use the pre-cached m_meshId* IDs.
void Engine::addChatMessage(const char* speaker, const char* msg, Vec3 color) {
    // Shift existing lines up (oldest at top falls off)
    for (u32 i = MAX_CHAT_LINES - 1; i > 0; i--) {
        m_chatLog[i] = m_chatLog[i - 1];
    }
    // Format "Speaker: message" into line 0
    std::snprintf(m_chatLog[0].text, CHAT_LINE_LEN, "%s: %s", speaker, msg);
    m_chatLog[0].color = color;
    m_chatLog[0].timer = 10.0f; // visible for 10 seconds
}

u8 Engine::findMeshByName(const char* name) const {
    for (u32 m = 0; m < m_meshDefCount; m++) {
        if (std::strcmp(m_meshDefs[m].name, name) == 0)
            return static_cast<u8>(m);
    }
    return 0; // fallback to cube mesh (index 0)
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
            {"mace",           "assets/meshes/mace.obj"},
            {"cleric",         "assets/meshes/cleric.obj"},
            {"archer",         "assets/meshes/archer.obj"},
            {"mage",           "assets/meshes/mage.obj"},
            {"rogue",          "assets/meshes/rogue.obj"},
            {"paladin",        "assets/meshes/paladin.obj"},
            {"staff",          "assets/meshes/staff.obj"},
            {"web",            "assets/meshes/web.obj"},
            {"shackles",       "assets/meshes/shackles.obj"},
            {"barrel",         "assets/meshes/barrel.obj"},
            {"cage",           "assets/meshes/cage.obj"},
            {"bones",          "assets/meshes/bones.obj"},
            {"brazier",        "assets/meshes/brazier.obj"},
            {"butcher",        "assets/meshes/butcher.obj"},
            {"cleaver",        "assets/meshes/cleaver.obj"},
            {"iron_maiden",    "assets/meshes/iron_maiden.obj"},
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

    // Cache mesh IDs for fast lookup in startGame — avoids repeated strcmp loops
    m_meshIdSkeleton = findMeshByName("skeleton");
    m_meshIdBat      = findMeshByName("bat");
    m_meshIdSpider   = findMeshByName("spider");
    m_meshIdChest    = findMeshByName("chest");
    m_meshIdHuman    = findMeshByName("human");
    m_meshIdSword    = findMeshByName("sword");
    m_meshIdDagger   = findMeshByName("dagger");
    m_meshIdAxe      = findMeshByName("axe");
    m_meshIdMace     = findMeshByName("mace");
    m_meshIdCleric   = findMeshByName("cleric");
    m_meshIdArcher   = findMeshByName("archer");
    m_meshIdBow      = findMeshByName("bow");
    m_meshIdMage       = findMeshByName("mage");
    m_meshIdRogue      = findMeshByName("rogue");
    m_meshIdPaladin    = findMeshByName("paladin");
    m_meshIdStaff      = findMeshByName("staff");
    m_meshIdThrowingKnife = findMeshByName("throwing_knife");
    m_meshIdButcher    = findMeshByName("butcher");
    m_meshIdCleaver    = findMeshByName("cleaver");
    m_meshIdIronMaiden = findMeshByName("iron_maiden");

    // Weapons
    initWeaponTable(m_weaponDefs, m_weaponDefCount);

    // Item/loot system — log warnings and zero out tables rather than crashing on bad JSON
    if (!ItemLoader::loadItemDefs("assets/config/items.json", m_itemDefs, m_itemDefCount)) {
        LOG_WARN("Failed to load item defs — using empty table");
        m_itemDefCount = 0;
    }
    if (!ItemLoader::loadAffixDefs("assets/config/affixes.json", m_affixDefs, m_affixDefCount)) {
        LOG_WARN("Failed to load affix defs — using empty table");
        m_affixDefCount = 0;
    }
    if (!ItemLoader::loadSkillDefs("assets/config/skills.json", m_skillDefs, m_skillDefCount)) {
        LOG_WARN("Failed to load skill defs — using empty table");
        m_skillDefCount = 0;
    }
    SkillSystem::init();

    // Register skill visual FX callbacks
    SkillSystem::setNovaCallback([](Vec3 position, f32 radius, Vec3 color) {
        if (!s_engine) return;
        for (u32 i = 0; i < MAX_NOVA_FX; i++) {
            if (!s_engine->m_novaFX[i].active) {
                s_engine->m_novaFX[i] = {position, radius, 0.6f, true, color};
                return;
            }
        }
    });
    SkillSystem::setDashCallback([](Vec3 start, Vec3 end) {
        if (!s_engine) return;
        for (u32 i = 0; i < MAX_DASH_FX; i++) {
            if (!s_engine->m_dashFX[i].active) {
                s_engine->m_dashFX[i] = {start, end, 0.5f, true};
                return;
            }
        }
    });
    SkillSystem::setScorchCallback([](Vec3 position, f32 radius, f32 duration, f32 dps) {
        if (!s_engine) return;
        for (u32 i = 0; i < MAX_SCORCH; i++) {
            if (!s_engine->m_scorchZones[i].active) {
                s_engine->m_scorchZones[i] = {position, radius, duration, dps, true};
                return;
            }
        }
        // Also spawn a fire FX for the visual
        for (u32 i = 0; i < MAX_FIRE_FX; i++) {
            if (!s_engine->m_fireFX[i].active) {
                s_engine->m_fireFX[i] = {position, radius, duration, true};
                return;
            }
        }
    });

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
        // Hostile enemies only drop loot; chance scales with floor depth
        // (40% at floor 1, +1% per floor, capped at 70%)
        u8 enemyLevel = pool.entities[entityIndex].level;
        f32 dropChance = GameConst::LOOT_DROP_CHANCE + enemyLevel * 0.01f;
        if (dropChance > 0.70f) dropChance = 0.70f;
        if (!(pool.entities[entityIndex].flags & ENT_FRIENDLY) &&
            (std::rand() % 100) < static_cast<int>(dropChance * 100.0f)) {
            if (enemyLevel < 1) enemyLevel = 1;
            ItemInstance item = ItemGen::rollItem(enemyLevel, s_engine->m_itemDefs,
                                                   s_engine->m_itemDefCount,
                                                   s_engine->m_affixDefs,
                                                   s_engine->m_affixDefCount);
            if (!isItemEmpty(item)) {
                WorldItemSystem::spawn(s_engine->m_worldItems, item,
                                       position + Vec3{0, 0.5f, 0});
            }

            // Chance to drop a health globe (instant heal on pickup)
            if ((std::rand() % 100) < static_cast<int>(GameConst::HEALTH_GLOBE_CHANCE * 100.0f)) {
                ItemInstance globe;
                globe.defId = GLOBE_HEALTH_ID;
                globe.uid   = s_engine->m_worldItems.nextUid++;
                WorldItemSystem::spawn(s_engine->m_worldItems, globe,
                                       position + Vec3{0.3f, 0.5f, 0.0f});
            }
            // Chance to drop an energy globe (instant energy restore on pickup)
            if ((std::rand() % 100) < static_cast<int>(GameConst::ENERGY_GLOBE_CHANCE * 100.0f)) {
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
        // Snap fire effect to floor level so it doesn't render underground
        u32 gx, gz;
        Vec3 fxPos = position;
        if (LevelGridSystem::worldToGrid(s_engine->m_grid, position, gx, gz) &&
            !LevelGridSystem::isSolid(s_engine->m_grid, gx, gz)) {
            fxPos.y = LevelGridSystem::getFloorHeight(s_engine->m_grid, gx, gz) + 0.1f;
        }
        for (u32 i = 0; i < Engine::MAX_FIRE_FX; i++) {
            if (!s_engine->m_fireFX[i].active) {
                s_engine->m_fireFX[i] = {fxPos, radius, 1.0f, true};
                break;
            }
        }
    });

    // Perfect block callback — legendary shield triggers stun bash
    Combat::setPerfectBlockCallback([](Player& player) {
        if (!s_engine) return;
        // Check if offhand is a legendary shield
        const ItemInstance& shield = s_engine->m_inventories[0].equipped[static_cast<u32>(ItemSlot::OFFHAND)];
        bool hasLegendaryShield = !isItemEmpty(shield) && shield.rarity == Rarity::LEGENDARY;

        // Stun all enemies within 3m (1 second)
        if (hasLegendaryShield) {
            for (u32 a = 0; a < s_engine->m_entities.activeCount; a++) {
                u32 idx = s_engine->m_entities.activeList[a];
                Entity& ent = s_engine->m_entities.entities[idx];
                if (ent.flags & ENT_DEAD) continue;
                if (ent.flags & ENT_FRIENDLY) continue;
                if (ent.enemyType == EnemyType::PROP) continue;
                f32 dist = length(ent.position - player.position);
                if (dist < 3.0f) {
                    ent.freezeTimer = 1.0f; // stun via freeze (stops movement)
                }
            }
            // Visual feedback
            for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                if (!s_engine->m_novaFX[ni].active) {
                    s_engine->m_novaFX[ni] = {player.position, 3.0f, 0.4f, true, {0.8f, 0.8f, 1.0f}};
                    break;
                }
            }
        }
    });

    // Drone/turret spawn callback — skill.cpp delegates spawning here so we
    // have direct access to the mesh registry.  type: 0=combat drone, 1=swarm, 2=turret
    SkillSystem::setDroneSpawnCallback([](Vec3 position, u8 type) {
        if (!s_engine) return;
        EntityPool& pool = s_engine->m_entities;

        if (type == 0) {
            // Combat drone — large metal spider, rushes enemies, melee
            // Check if one already exists (heal instead of double-spawn)
            for (u32 a = 0; a < pool.activeCount; a++) {
                u32 idx = pool.activeList[a];
                Entity& ex = pool.entities[idx];
                if ((ex.flags & ENT_FRIENDLY) && !(ex.flags & ENT_DEAD) &&
                    ex.npcClass == NpcClass::NONE && ex.enemyType == EnemyType::SPIDER) {
                    ex.health = ex.maxHealth; // full repair
                    LOG_INFO("Combat Drone REPAIRED (existing drone found at idx %u)", idx);
                    return;
                }
            }
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.4f, 0.2f, 0.4f}, false, 80.0f, 5.0f, 15.0f, 2.5f, 0.6f, 10.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->flags        |= ENT_FRIENDLY;
                e->enemyType     = EnemyType::SPIDER;
                e->meshId        = s_engine->m_meshIdSpider;
                e->materialId    = 49; // prop_iron
                e->npcWeaponType = WeaponType::MELEE;
                e->aiState       = AIState::IDLE;
                LOG_INFO("Combat Drone SPAWNED at (%.1f,%.1f,%.1f) meshId=%u",
                         position.x, position.y, position.z, e->meshId);
            } else {
                LOG_WARN("Combat Drone spawn FAILED (pool full? h.index=%u)", h.index);
            }
        } else if (type == 1) {
            // Swarm drone — tiny flying metal bat body, weak projectile
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.15f, 0.1f, 0.15f}, true, 9999.0f, 5.0f, 12.0f, 8.0f, 1.5f, 3.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->flags        |= ENT_FRIENDLY | ENT_FLYING | ENT_UNTARGETABLE;
                e->enemyType     = EnemyType::GENERIC;
                e->meshId        = s_engine->m_meshIdBat;
                e->materialId    = 49; // prop_iron
                e->npcWeaponType = WeaponType::PROJECTILE;
                e->npcProjectileSpeed  = 22.0f;
                e->npcProjectileRadius = 0.04f;
                e->aiState       = AIState::IDLE;
            }
        } else if (type == 2) {
            // Turret — stationary, hitscan, timed despawn
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.3f, 0.6f, 0.3f}, false, 80.0f, 0.0f, 15.0f, 10.0f, 1.5f, 12.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->flags        |= ENT_FRIENDLY;
                e->enemyType     = EnemyType::GENERIC;
                e->meshId        = s_engine->m_meshIdSpider;
                e->materialId    = 49;
                e->npcWeaponType = WeaponType::PROJECTILE;
                e->npcProjectileSpeed  = 30.0f;
                e->npcProjectileRadius = 0.06f;
                e->deathTimer    = 15.0f;
                e->aiState       = AIState::IDLE;
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

// ---------------------------------------------------------------------------
// NPC equipment and spawning helpers
// ---------------------------------------------------------------------------

void Engine::rollNpcEquipment(NpcEquipment& equip, NpcClass npcClass, u8 floor) {
    // Clear all slots
    for (u32 s = 0; s < static_cast<u32>(ItemSlot::COUNT); s++) {
        equip.equipped[s] = {};
    }

    // Roll a weapon appropriate for the class
    auto rollSlot = [&](ItemSlot slot, const char* subtypeHint) {
        // Try up to 20 times to find a matching item
        for (u32 attempt = 0; attempt < 20; attempt++) {
            ItemInstance item = ItemGen::rollItem(floor, m_itemDefs, m_itemDefCount,
                                                   m_affixDefs, m_affixDefCount);
            if (isItemEmpty(item)) continue;
            const ItemDef& def = m_itemDefs[item.defId];
            if (def.slot != slot) continue;
            // If caller specified a subtype hint, filter for it
            if (subtypeHint && subtypeHint[0]) {
                if (std::strstr(def.name, subtypeHint) == nullptr) continue;
            }
            equip.equipped[static_cast<u32>(slot)] = item;
            return;
        }
        // Fallback: just roll any item of the right slot
        for (u32 attempt = 0; attempt < 40; attempt++) {
            ItemInstance item = ItemGen::rollItem(floor, m_itemDefs, m_itemDefCount,
                                                   m_affixDefs, m_affixDefCount);
            if (isItemEmpty(item)) continue;
            const ItemDef& def = m_itemDefs[item.defId];
            if (def.slot != slot) continue;
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
        e.damage = rawDmg * (1.0f + equip.bonusDamagePct / 100.0f) * 0.15f;
        e.npcWeaponType = def.weaponType;

        // Attack range depends on weapon type
        if (def.weaponType == WeaponType::PROJECTILE) {
            e.attackRange = 12.0f + equip.bonusRange;
            e.npcProjectileSpeed = def.baseProjectileSpeed * (1.0f + equip.bonusProjectileSpeedPct / 100.0f);
            if (e.npcProjectileSpeed < 8.0f) e.npcProjectileSpeed = 12.0f;
            e.npcProjectileRadius = def.baseProjectileRadius;
            if (e.npcProjectileRadius < 0.05f) e.npcProjectileRadius = 0.1f;
        } else {
            e.attackRange = def.baseRange + equip.bonusRange;
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
    f32 baseDmg = 10.0f;
    Vec3 halfExt = {0.4f, 0.9f, 0.4f};
    u8 meshId = m_meshIdHuman;
    const char* matName = "human_skin";
    u8 weaponMesh = 0;
    const char* speech = "Ready!";

    switch (npcClass) {
        case NpcClass::CLERIC:
            baseHp = GameConst::NPC_HEALTH_CLERIC;
            speed = 3.0f;
            atkRange = 2.5f;
            atkCool = 1.0f;
            baseDmg = 2.0f;
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
            atkCool = 1.5f;
            baseDmg = 1.0f;
            meshId = m_meshIdArcher;
            matName = "archer_skin";
            weaponMesh = m_meshIdBow;
            speech = "Ready when you are!";
            break;
        case NpcClass::MAGE:
            baseHp = GameConst::NPC_HEALTH_MAGE;
            speed = 2.8f;
            atkRange = 14.0f;
            atkCool = 1.8f;
            baseDmg = 1.5f;
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
            atkCool = 1.0f;
            baseDmg = 0.8f;
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
            atkCool = 1.0f;
            baseDmg = 2.5f;
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

    return h;
}

void Engine::upgradeNpcEquipment(u8 newFloor) {
    // Find surviving friendly NPCs and upgrade their equipment
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        u32 idx = m_entities.activeList[a];
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
    // Build level — use BSP procedural generation with random seed
    // Mix floor number into seed so each floor has unique layout
    u32 dungeonSeed = static_cast<u32>(std::rand()) + m_currentFloor * 7919;
    LevelGridSystem::init(m_grid, 48, 48, 1.0f);
    DungeonResult dungeon = LevelGen::generate(m_grid, dungeonSeed, 48, 48);
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

        if (m_currentFloor >= 41) {
            themeWall  = MaterialSystem::getIdByName("void_wall");
            themeFloor = MaterialSystem::getIdByName("void_floor");
            themeCeil  = MaterialSystem::getIdByName("void_ceiling");
            applyTheme = true;
        } else if (m_currentFloor >= 31) {
            themeWall  = MaterialSystem::getIdByName("hellforge_wall");
            themeFloor = MaterialSystem::getIdByName("hellforge_floor");
            themeCeil  = MaterialSystem::getIdByName("hellforge_ceiling");
            applyTheme = true;
        } else if (m_currentFloor >= 21) {
            themeWall  = MaterialSystem::getIdByName("cavern_wall");
            themeFloor = MaterialSystem::getIdByName("cavern_floor");
            themeCeil  = MaterialSystem::getIdByName("cavern_ceiling");
            applyTheme = true;
        } else if (m_currentFloor >= 11) {
            themeWall  = MaterialSystem::getIdByName("catacomb_wall");
            themeFloor = MaterialSystem::getIdByName("catacomb_floor");
            themeCeil  = MaterialSystem::getIdByName("catacomb_ceiling");
            applyTheme = true;
        }

        if (applyTheme) {
            for (u32 z = 0; z < m_grid.depth; z++) {
                for (u32 x = 0; x < m_grid.width; x++) {
                    GridCell& cell = LevelGridSystem::getCell(m_grid, x, z);
                    if (!(cell.flags & CELL_FLOOR)) continue; // skip solid cells
                    // Replace default stone/brick with theme, preserve blood (boss arenas)
                    if (cell.wallMaterialId <= 3) cell.wallMaterialId = themeWall;
                    if (cell.floorMaterialId <= 2) cell.floorMaterialId = themeFloor;
                    if (cell.ceilMaterialId <= 2) cell.ceilMaterialId = themeCeil;
                }
            }
            LOG_INFO("Applied floor theme for depth tier %u-%u",
                     (m_currentFloor / 10) * 10 + 1, ((m_currentFloor / 10) + 1) * 10);
        }
    }

    m_sectionCount = LevelMeshSystem::buildAll(m_grid, m_sections, MAX_LEVEL_SECTIONS);
    Minimap::init(m_grid.width, m_grid.depth);

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

        static constexpr u32 ENEMIES_PER_TIER = 5;

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
             {0.5f,0.3f,0.5f}, false, 2, EnemyType::SPIDER,   "spider_skin",   0, 0, 0},
            // Zombie (Diablo 1) — slow, tanky, human mesh
            {70,  1.8f, 10, 2.0f, 1.2f, 13, {0.4f,0.9f,0.4f}, false, 3, EnemyType::SKELETON, "zombie_skin",  0, 0, 0},
            // Imp (Barony) — small fast flying ranged nuisance, fires weak projectiles
            {20,  7.0f, 16, 8.0f, 0.8f,  3, {0.3f,0.3f,0.3f}, true,  1, EnemyType::BAT,      "imp_skin",     0, 0, 0},
        };
        // Tier 2 (floors 11-20): Catacombs — poison + ghoul (D2) + bone mage (Barony)
        static const EnemyTemplate kTier2[] = {
            {60, 3.0f, 14, 2.5f, 1.0f, 12, {0.4f,0.9f,0.4f}, false, 0, EnemyType::SKELETON, "catacomb_skeleton", 1, 3.0f, 4.0f},
            {35, 6.5f, 14, 2.5f, 0.8f,  8, {0.5f,0.4f,0.4f}, true,  1, EnemyType::BAT,      "catacomb_bat",      1, 2.0f, 3.0f},
            {48, 4.2f, 12, 2.0f, 0.8f, 11, {0.5f,0.3f,0.5f}, false, 2, EnemyType::SPIDER,   "catacomb_spider",   1, 3.0f, 5.0f},
            // Ghoul (D2) — fast melee, high damage, lower HP
            {40, 4.5f, 14, 2.0f, 0.6f, 16, {0.4f,0.85f,0.4f}, false, 3, EnemyType::SKELETON, "ghoul_skin",       1, 2.0f, 3.0f},
            // Bone Mage (Barony) — ranged skeleton caster
            {35, 2.5f, 16, 10.f, 1.2f, 14, {0.4f,0.9f,0.4f},  false, 0, EnemyType::SKELETON, "bone_mage_skin",   1, 3.0f, 4.0f},
        };
        // Tier 3 (floors 21-30): Caverns — slow + broodmother (Barony) + stalker (HGL)
        static const EnemyTemplate kTier3[] = {
            {65, 3.2f, 15, 2.5f, 0.9f, 13, {0.4f,0.9f,0.4f}, false, 0, EnemyType::SKELETON, "cavern_skeleton", 2, 2.0f, 0},
            {38, 7.0f, 15, 2.5f, 0.7f,  9, {0.5f,0.4f,0.4f}, true,  1, EnemyType::BAT,      "cavern_bat",      2, 1.5f, 0},
            {52, 4.8f, 13, 2.0f, 0.7f, 12, {0.5f,0.3f,0.5f}, false, 2, EnemyType::SPIDER,   "cavern_spider",   2, 2.5f, 0},
            // Broodmother (Barony) — large slow spider, extra tanky
            {90, 2.5f, 12, 2.5f, 1.0f, 14, {0.7f,0.4f,0.7f}, false, 2, EnemyType::SPIDER,   "broodmother_skin", 2, 3.0f, 0},
            // Stalker (HGL) — fast, stealthy humanoid
            {45, 5.0f, 18, 2.0f, 0.5f, 11, {0.35f,0.85f,0.35f}, false, 3, EnemyType::SKELETON, "stalker_skin", 2, 2.0f, 0},
        };
        // Tier 4 (floors 31-40): Hellforge — burn + hellhound (D2) + demon (HGL)
        static const EnemyTemplate kTier4[] = {
            {70, 3.5f, 16, 2.5f, 0.8f, 15, {0.4f,0.9f,0.4f}, false, 0, EnemyType::SKELETON, "hellforge_skeleton", 3, 2.5f, 6.0f},
            {40, 7.5f, 16, 2.5f, 0.6f, 10, {0.5f,0.4f,0.4f}, true,  1, EnemyType::BAT,      "hellforge_bat",      3, 2.0f, 5.0f},
            {58, 5.0f, 14, 2.0f, 0.6f, 14, {0.5f,0.3f,0.5f}, false, 2, EnemyType::SPIDER,   "hellforge_spider",   3, 2.5f, 7.0f},
            // Hellhound (D2) — fast charging beast, spider rig
            {50, 6.0f, 16, 2.5f, 0.5f, 16, {0.5f,0.35f,0.5f}, false, 2, EnemyType::SPIDER,   "hellhound_skin",    3, 2.0f, 8.0f},
            // Demon (HGL) — ranged fire caster, humanoid
            {55, 3.0f, 18, 12.f, 1.0f, 18, {0.45f,1.0f,0.45f}, false, 3, EnemyType::SKELETON, "demon_skin",        3, 3.0f, 6.0f},
        };
        // Tier 5 (floors 41-50): Void — freeze + shade (Barony) + void demon (HGL)
        static const EnemyTemplate kTier5[] = {
            {80, 3.8f, 18, 2.5f, 0.7f, 16, {0.4f,0.9f,0.4f}, false, 0, EnemyType::SKELETON, "void_skeleton", 4, 1.5f, 0},
            {45, 8.0f, 18, 2.5f, 0.5f, 11, {0.5f,0.4f,0.4f}, true,  1, EnemyType::BAT,      "void_bat",      4, 1.0f, 0},
            {65, 5.5f, 16, 2.0f, 0.5f, 15, {0.5f,0.3f,0.5f}, false, 2, EnemyType::SPIDER,   "void_spider",   4, 1.5f, 0},
            // Shade (Barony) — fast phasing humanoid, semi-transparent
            {40, 5.5f, 20, 2.0f, 0.4f, 14, {0.35f,0.9f,0.35f}, false, 3, EnemyType::SKELETON, "shade_skin",      4, 2.0f, 0},
            // Void Demon (HGL) — heavy tanky skeleton, high damage
            {100, 2.5f, 16, 3.0f, 0.8f, 20, {0.5f,1.0f,0.5f}, false, 0, EnemyType::SKELETON, "void_demon_skin", 4, 2.0f, 0},
        };

        // Select tier based on current floor
        const EnemyTemplate* tier = kTier1;
        if      (m_currentFloor >= 41) tier = kTier5;
        else if (m_currentFloor >= 31) tier = kTier4;
        else if (m_currentFloor >= 21) tier = kTier3;
        else if (m_currentFloor >= 11) tier = kTier2;

        // meshIdx 3 = human mesh (used by zombies, ghouls, stalkers, demons, shades)
        u8 meshLookup[] = {m_meshIdSkeleton, m_meshIdBat, m_meshIdSpider, m_meshIdHuman};

        for (u32 r = 1; r < dungeon.roomCount; r++) {
            const DungeonRoom& room = dungeon.rooms[r];

            u32 area = room.w * room.d;
            u32 enemyCount = (m_currentFloor == 1) ? 1 : (1 + (area / 20));
            if (enemyCount > 3) enemyCount = 3;

            for (u32 e = 0; e < enemyCount; e++) {
                u32 typeIdx = static_cast<u32>(std::rand()) % ENEMIES_PER_TIER;
                const EnemyTemplate& tmpl = tier[typeIdx];

                f32 ex = (room.x + 1 + static_cast<u32>(std::rand()) % (room.w > 2 ? room.w - 2 : 1)) * m_grid.cellSize;
                f32 ez = (room.z + 1 + static_cast<u32>(std::rand()) % (room.d > 2 ? room.d - 2 : 1)) * m_grid.cellSize;
                f32 spawnY = tmpl.flying ? (room.floorHeight + 1.5f) : (room.floorHeight + tmpl.halfExtents.y);

                EntityHandle h = EntitySystem::spawn(m_entities,
                    Vec3{ex, spawnY, ez}, tmpl.halfExtents, tmpl.flying,
                    tmpl.health, tmpl.moveSpeed, tmpl.detRange,
                    tmpl.atkRange, tmpl.atkCool, tmpl.damage);
                Entity* ent = handleGet(m_entities, h);
                if (ent) {
                    ent->meshId = meshLookup[tmpl.meshIdx];
                    ent->materialId = MaterialSystem::getIdByName(tmpl.matName);
                    ent->enemyType = tmpl.etype;
                    ent->level = static_cast<u8>(m_currentFloor);

                    // Floor scaling
                    f32 floorMult = 1.0f + (m_currentFloor - 1) * GameConst::FLOOR_STAT_MULT;
                    ent->health    *= floorMult;
                    ent->maxHealth  = ent->health;
                    ent->damage    *= floorMult;

                    // On-hit status effect
                    ent->onHitEffect   = tmpl.onHitEffect;
                    ent->onHitDuration = tmpl.onHitDuration;
                    ent->onHitDps      = tmpl.onHitDps * floorMult;

                    // Weapon assignment — ranged enemies get staves/bows, melee get blades
                    if (ent->enemyType == EnemyType::SKELETON) {
                        if (ent->attackRange > 5.0f) {
                            // Ranged caster — give a staff or wand
                            ent->weaponMeshId = m_meshIdStaff;
                        } else {
                            u8 weapMeshes[] = {m_meshIdSword, m_meshIdDagger, m_meshIdAxe};
                            ent->weaponMeshId = weapMeshes[static_cast<u32>(std::rand()) % 3];
                        }
                    }
                }
            }
        }
    }

    // Spawn chests and mimics (1 per room, 20% chance mimic)
    {
        u8 chestMeshId = m_meshIdChest;

        for (u32 r = 1; r < dungeon.roomCount; r++) {
            if ((std::rand() % 2) != 0) continue; // 50% of rooms get a chest
            const DungeonRoom& room = dungeon.rooms[r];

            // Center of room (use float division for accurate centering)
            f32 cx = (room.x + room.w * 0.5f) * m_grid.cellSize;
            f32 cz = (room.z + room.d * 0.5f) * m_grid.cellSize;
            f32 cy = room.floorHeight;

            bool isMimic = (std::rand() % 5) == 0; // 20% of chests are mimics

            if (isMimic) {
                // Mimic: enemy disguised as chest; springs to life within MIMIC_TRIGGER_DIST
                EntityHandle h = EntitySystem::spawn(m_entities,
                    Vec3{cx, cy + 0.25f, cz}, {0.3f, 0.25f, 0.3f}, false,
                    GameConst::MIMIC_HEALTH, 4.0f, GameConst::MIMIC_TRIGGER_DIST,
                    2.0f, 0.6f, GameConst::MIMIC_DAMAGE);
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

    // ---------------------------------------------------------------------------
    // Boss roster — unique encounters on milestone floors.
    // Mini-bosses on 5/15/25/35/45, major bosses on 10/20/30/40/50.
    // ---------------------------------------------------------------------------
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
            { 15, "Lich Lord",     "Your soul is MINE!",  500, 50, 2.8f, 12.f, 0.6f, {0.5f,1.0f, 0.5f}, false, "skeleton", "boss_lich",         "staff"},
            { 25, "Spider Queen",  "*HISSSS*",            700, 45, 5.0f, 3.0f, 0.4f, {0.8f,0.5f, 0.8f}, false, "spider",   "boss_spider_queen", nullptr},
            { 35, "Demon Knight",  "Kneel before me!",    800, 60, 3.5f, 3.5f, 0.5f, {0.7f,1.2f, 0.7f}, false, "butcher",  "boss_demon_knight", "sword"},
            { 45, "Arch Mage",     "Feel the arcane!",    600, 65, 3.0f, 14.f, 0.4f, {0.5f,1.0f, 0.5f}, false, "skeleton", "boss_arch_mage",    "staff"},
            // Major bosses (floors 10, 20, 30, 40, 50) — devastating, need full player focus
            { 10, "Andariel",      "Die, insect!",       1000, 65, 4.0f, 3.5f, 0.4f, {0.7f,1.1f, 0.7f}, true,  "human",    "boss_andariel",     nullptr},
            { 20, "Mephisto",      "You cannot stop me.",1200, 75, 2.5f, 14.f, 0.5f, {0.6f,1.1f, 0.6f}, true,  "skeleton", "boss_mephisto",     "staff"},
            { 30, "Baal",          "I am undefeated!",   1800, 70, 3.0f, 4.0f, 0.4f, {0.9f,1.3f, 0.9f}, true,  "butcher",  "boss_baal",         nullptr},
            { 40, "Diablo",        "NOT EVEN DEATH...",  1600, 90, 3.5f, 4.0f, 0.35f,{0.8f,1.3f, 0.8f}, true,  "butcher",  "boss_diablo",       "sword"},
            { 50, "Grim Reaper",   "YOUR TIME HAS COME.",2500,120, 4.0f, 4.0f, 0.3f, {0.7f,1.4f, 0.7f}, true,  "skeleton", "boss_reaper",       "axe"},
        };

        // Find boss for this floor
        const BossTemplate* bt = nullptr;
        for (u32 b = 0; b < BOSS_COUNT; b++) {
            if (kBosses[b].floor == m_currentFloor) { bt = &kBosses[b]; break; }
        }

        if (bt && dungeon.roomCount > 2) {
            DungeonRoom& bossRoom = dungeon.rooms[dungeon.roomCount - 2];

            // Arena size: major bosses get a larger arena
            u32 arenaScale = bt->isMajor ? 4 : 3;
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
                    if (gx < 1 || gz < 1 || static_cast<u32>(gx) >= m_grid.width - 1 ||
                        static_cast<u32>(gz) >= m_grid.depth - 1) continue;
                    GridCell& cell = LevelGridSystem::getCell(m_grid, static_cast<u32>(gx), static_cast<u32>(gz));
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

            m_sectionCount = LevelMeshSystem::buildAll(m_grid, m_sections, MAX_LEVEL_SECTIONS);

            // Iron maidens in corners for major bosses
            if (bt->isMajor && m_meshIdIronMaiden > 0) {
                Vec3 corners[] = {
                    {(bossRoom.x + 2) * m_grid.cellSize, bossRoom.floorHeight, (bossRoom.z + 2) * m_grid.cellSize},
                    {(bossRoom.x + bossRoom.w - 2) * m_grid.cellSize, bossRoom.floorHeight, (bossRoom.z + 2) * m_grid.cellSize},
                    {(bossRoom.x + 2) * m_grid.cellSize, bossRoom.floorHeight, (bossRoom.z + bossRoom.d - 2) * m_grid.cellSize},
                    {(bossRoom.x + bossRoom.w - 2) * m_grid.cellSize, bossRoom.floorHeight, (bossRoom.z + bossRoom.d - 2) * m_grid.cellSize},
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
            f32 bx = (bossRoom.x + bossRoom.w * 0.5f) * m_grid.cellSize;
            f32 bz = (bossRoom.z + bossRoom.d * 0.5f) * m_grid.cellSize;
            f32 by = bossRoom.floorHeight;
            f32 floorMult = 1.0f + (m_currentFloor - 1) * GameConst::FLOOR_STAT_MULT;

            EntityHandle bh = EntitySystem::spawn(m_entities,
                Vec3{bx, by + bt->halfExtents.y, bz}, bt->halfExtents, false,
                bt->baseHp * floorMult, bt->speed, 20.0f,
                bt->atkRange, bt->atkCooldown, bt->baseDmg * floorMult);
            Entity* boss = handleGet(m_entities, bh);
            if (boss) {
                boss->meshId = findMeshByName(bt->meshName);
                boss->materialId = MaterialSystem::getIdByName(bt->matName);
                boss->enemyType = EnemyType::BOSS;
                boss->level = static_cast<u8>(m_currentFloor);
                if (bt->weaponName) {
                    boss->weaponMeshId = findMeshByName(bt->weaponName);
                }
                boss->speechText = bt->speech;
                boss->speechTimer = 6.0f;

                // Ranged bosses (Lich Lord, Arch Mage, Mephisto) use projectile attacks
                // via the boss AI cleaver-throw mechanic — their weapon mesh rides the
                // projectile for visual consistency
                if (bt->atkRange > 5.0f) {
                    boss->npcWeaponType = WeaponType::PROJECTILE;
                    boss->npcProjectileSpeed = 18.0f;
                    boss->npcProjectileRadius = 0.15f;
                }
            }
            LOG_INFO("Spawned boss '%s' on floor %u (%.0f HP, %.0f DMG, arena %ux%u)",
                     bt->name, m_currentFloor, bt->baseHp * floorMult,
                     bt->baseDmg * floorMult, expandW, expandD);
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
        u8 mWeb      = findMeshByName("web");
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

        // Build tier-specific prop lists
        PropDef dungeonProps[] = {
            {mBarrel,   matWood, {0.25f, 0.35f, 0.25f}, 0.35f, true},
            {mShackles, matIron, {0.15f, 0.40f, 0.05f}, 0.40f, true},
            {mCage,     matIron, {0.40f, 0.75f, 0.40f}, 0.75f, true},
            {mBones,    matBone, {0.15f, 0.06f, 0.15f}, 0.06f, false},
        };
        PropDef catacombProps[] = {
            {mBones,    matBone,    {0.15f, 0.06f, 0.15f}, 0.06f, false},
            {mShackles, matIron,    {0.15f, 0.40f, 0.05f}, 0.40f, true},
            {mBrazier,  matBrazier, {0.12f, 0.30f, 0.12f}, 0.30f, true},
        };
        PropDef cavernProps[] = {
            {mWeb,    matWeb,  {0.50f, 0.02f, 0.50f}, 1.80f, false},  // webs near ceiling
            {mBones,  matBone, {0.15f, 0.06f, 0.15f}, 0.06f, false},
            {mBarrel, matWood, {0.25f, 0.35f, 0.25f}, 0.35f, true},
        };
        PropDef hellforgeProps[] = {
            {mBrazier,  matBrazier, {0.12f, 0.30f, 0.12f}, 0.30f, true},
            {mCage,     matIron,    {0.40f, 0.75f, 0.40f}, 0.75f, true},
            {mShackles, matIron,    {0.15f, 0.40f, 0.05f}, 0.40f, true},
            {mBones,    matBone,    {0.15f, 0.06f, 0.15f}, 0.06f, false},
        };
        PropDef voidProps[] = {
            {mBones,   matBone,    {0.15f, 0.06f, 0.15f}, 0.06f, false},
            {mCage,    matIron,    {0.40f, 0.75f, 0.40f}, 0.75f, true},
            {mBrazier, matBrazier, {0.12f, 0.30f, 0.12f}, 0.30f, true},
        };

        // Select the right prop list for the current tier
        const PropDef* props = dungeonProps;
        u32 propCount = 4;
        if (m_currentFloor >= 41)      { props = voidProps;      propCount = 3; }
        else if (m_currentFloor >= 31) { props = hellforgeProps;  propCount = 4; }
        else if (m_currentFloor >= 21) { props = cavernProps;     propCount = 3; }
        else if (m_currentFloor >= 11) { props = catacombProps;   propCount = 3; }

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
                if (prop.wallOnly) {
                    // Place flush against room walls — offset by halfExtents so
                    // the prop's edge touches the wall, not its center.
                    u32 wall = std::rand() % 4;
                    f32 cs = m_grid.cellSize;
                    if (wall < 2) {
                        // North/south wall: fixed z, random x along wall
                        px = (room.x + 1 + std::rand() % (room.w > 2 ? room.w - 2 : 1)) * cs;
                        if (wall == 0)
                            pz = room.z * cs + prop.halfExt.z;                  // north: push inward
                        else
                            pz = (room.z + room.d) * cs - prop.halfExt.z;       // south: push inward
                    } else {
                        // West/east wall: fixed x, random z along wall
                        pz = (room.z + 1 + std::rand() % (room.d > 2 ? room.d - 2 : 1)) * cs;
                        if (wall == 2)
                            px = room.x * cs + prop.halfExt.x;                  // west: push inward
                        else
                            px = (room.x + room.w) * cs - prop.halfExt.x;       // east: push inward
                    }
                } else {
                    // Small/flat props (bones, webs) can go anywhere in the room
                    px = (room.x + 1 + std::rand() % (room.w > 2 ? room.w - 2 : 1)) * m_grid.cellSize;
                    pz = (room.z + 1 + std::rand() % (room.d > 2 ? room.d - 2 : 1)) * m_grid.cellSize;
                }
                f32 py = room.floorHeight;

                EntityHandle dh = EntitySystem::spawn(m_entities,
                    Vec3{px, py + prop.yOff, pz}, prop.halfExt, false,
                    9999.0f, 0.0f, 0.0f, 0.0f, 999.0f, 0.0f);
                Entity* deco = handleGet(m_entities, dh);
                if (deco) {
                    deco->meshId = prop.meshId;
                    deco->materialId = prop.matId;
                    deco->enemyType = EnemyType::PROP;
                    // Random rotation for variety
                    deco->yaw = (std::rand() % 628) * 0.01f;
                    decoCount++;
                }
            }
        }
        LOG_INFO("Spawned %u themed decorations for tier %u-%u",
                 decoCount, (m_currentFloor / 10) * 10 + 1, ((m_currentFloor / 10) + 1) * 10);
    }

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

        // Build BFS flow field so NPCs can pathfind toward the exit
        LevelGridSystem::buildFlowField(m_grid, m_floorDoorPos);
    }

    // Spawn 4 friendly NPCs in the spawn room — one of each class
    {
        // Clear NPC equipment pool on floor 1, preserve on descent
        if (m_currentFloor <= 1) {
            for (u32 i = 0; i < MAX_NPC_EQUIP; i++) m_npcEquip[i] = {};
        }

        u8 floor = static_cast<u8>(m_currentFloor);
        f32 sy = dungeon.spawnPos.y + 0.9f;

        // NPC 0: Cleric — front left
        spawnFriendlyNpc({dungeon.spawnPos.x - 1.5f, sy, dungeon.spawnPos.z + 1.0f},
                          NpcClass::CLERIC, floor);
        // NPC 1: Archer — front right
        spawnFriendlyNpc({dungeon.spawnPos.x + 1.5f, sy, dungeon.spawnPos.z + 1.0f},
                          NpcClass::ARCHER, floor);
        // NPC 2: Mage — back left
        spawnFriendlyNpc({dungeon.spawnPos.x - 1.0f, sy, dungeon.spawnPos.z - 1.0f},
                          NpcClass::MAGE, floor);
        // NPC 3: Rogue — back right
        spawnFriendlyNpc({dungeon.spawnPos.x + 1.0f, sy, dungeon.spawnPos.z - 1.0f},
                          NpcClass::ROGUE, floor);
        // NPC 4: Paladin — center back
        spawnFriendlyNpc({dungeon.spawnPos.x, sy, dungeon.spawnPos.z - 1.5f},
                          NpcClass::PALADIN, floor);
        LOG_INFO("Spawned 5 friendly NPCs (cleric, archer, mage, rogue, paladin) in spawn room");
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

    // Sub-menu for single player: New Game / Continue
    if (m_menuSubState == 1) {
        if (Input::isKeyPressed(SDL_SCANCODE_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
            if (m_menuSubSelection > 0) m_menuSubSelection--;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
            if (m_menuSubSelection < 1) m_menuSubSelection++;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) {
            m_menuSubState = 0; // back to main menu
            return;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_RETURN) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            m_netRole = NetRole::NONE;
            m_localPlayerIndex = 0;
            if (m_menuSubSelection == 0) {
                // New Game — go to class selection
                m_currentFloor = 1;
                m_menuSubState = 2; // class selection screen
                m_menuSubSelection = 0;
            } else {
                // Continue — load save (skip class selection)
                if (loadGame()) {
                    m_currentFloor = m_savedFloor;
                } else {
                    m_currentFloor = 1;
                }
                m_menuSubState = 0;
                startGame();
            }
        }
        return;
    }

    // Class selection screen (subState 2)
    if (m_menuSubState == 2) {
        u8 classCount = static_cast<u8>(PlayerClass::CLASS_COUNT);
        if (Input::isKeyPressed(SDL_SCANCODE_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
            if (m_menuSubSelection > 0) m_menuSubSelection--;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
            if (m_menuSubSelection < classCount - 1) m_menuSubSelection++;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) {
            m_menuSubState = 1; // back to new/continue
            m_menuSubSelection = 0;
            return;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_RETURN) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
            m_playerClass = static_cast<PlayerClass>(m_menuSubSelection);
            m_activeClassSkill = 0;

            // Apply class stats to player
            const ClassDef& cls = kClassDefs[m_menuSubSelection];
            m_localPlayer.maxHealth = cls.baseHealth;
            m_localPlayer.health = cls.baseHealth;
            m_localPlayer.moveSpeed = cls.baseMoveSpeed;
            m_skillStates[0].maxEnergy = cls.baseEnergy;
            m_skillStates[0].energy = cls.baseEnergy;

            // Init class skill cooldown states
            for (u32 s = 0; s < 4; s++) {
                m_classSkillStates[s] = {};
                m_classSkillStates[s].activeSkill = cls.skills[s];
                m_classSkillStates[s].maxEnergy = cls.baseEnergy;
                m_classSkillStates[s].energy = cls.baseEnergy;
            }

            m_menuSubState = 0;
            startGame();

            // Auto-equip starting weapon after startGame inits inventory
            for (u32 wi = 0; wi < m_itemDefCount; wi++) {
                if (std::strcmp(m_itemDefs[wi].name, cls.startingWeaponName) == 0) {
                    ItemInstance wpn;
                    wpn.defId = static_cast<u16>(wi);
                    wpn.rarity = Rarity::COMMON;
                    wpn.itemLevel = 1;
                    wpn.damage = m_itemDefs[wi].baseDamage;
                    wpn.uid = m_worldItems.nextUid++;
                    m_inventories[0].equipped[static_cast<u32>(ItemSlot::WEAPON)] = wpn;
                    Inventory::recalculateStats(m_inventories[0]);
                    Quickbar::syncWeaponSlot(m_quickbars[0], m_inventories[0]);
                    break;
                }
            }
        }
        return;
    }

    if (Input::isKeyPressed(SDL_SCANCODE_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
        if (m_menuSelection > 0) m_menuSelection--;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
        if (m_menuSelection < 3) m_menuSelection++;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_RETURN) || Input::isKeyPressed(SDL_SCANCODE_SPACE)) {
        switch (m_menuSelection) {
        case 0: // Singleplayer — show sub-menu
            m_menuSubState = 1;
            m_menuSubSelection = 0;
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
    // Tick player status effects (poison, burn, freeze)
    if (m_localPlayer.poisonTimer > 0.0f) {
        m_localPlayer.poisonTimer -= dt;
        m_localPlayer.health -= m_localPlayer.poisonDps * dt;
    }
    if (m_localPlayer.burnTimer > 0.0f) {
        m_localPlayer.burnTimer -= dt;
        m_localPlayer.health -= m_localPlayer.burnDps * dt;
    }
    if (m_localPlayer.freezeTimer > 0.0f) {
        m_localPlayer.freezeTimer -= dt;
    }

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

    // Quickbar slot switching (keys 1-8 or mouse wheel)
    WeaponState& ws = m_players[0].weaponState;
    for (u32 i = 0; i < QUICKBAR_SLOTS; i++) {
        if (Input::isKeyPressed(SDL_SCANCODE_1 + i)) {
            m_quickbars[0].activeSlot = static_cast<u8>(i);
        }
    }
    s32 wheel = Input::getMouseWheelDelta();
    if (wheel != 0) {
        s32 slot = static_cast<s32>(m_quickbars[0].activeSlot);
        slot -= wheel; // scroll up = previous slot, down = next
        if (slot < 0) slot = QUICKBAR_SLOTS - 1;
        if (slot >= static_cast<s32>(QUICKBAR_SLOTS)) slot = 0;
        m_quickbars[0].activeSlot = static_cast<u8>(slot);
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

    // Player movement — disable look and movement while inventory is open
    if (!m_inventoryOpen) {
        PlayerController::update(m_localPlayer, dt);
        if (!m_localPlayer.noclip) {
            Collision::moveAndSlide(m_localPlayer, m_grid, dt);
        }
    }

    // Sync to NetPlayer for consistent rendering
    syncLocalPlayerToNetPlayer();

    // Target lock and weapon fire — disabled while inventory is open
    if (!m_inventoryOpen) {
        updateTargetLock(dt);
        handleWeaponFire(dt);
    }

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

    // Decay speech timers + log new speech to chat
    for (u32 a = 0; a < m_entities.activeCount; a++) {
        u32 idx = m_entities.activeList[a];
        Entity& e = m_entities.entities[idx];
        if (e.speechTimer > 0.0f) {
            // Log new speech to the chat log. We detect "first frame" by
            // checking a negative-flag trick: aiCheckIdx bit 15 is set once logged,
            // cleared when speechText changes. Simpler: just compare last-logged
            // pointer. Use the animTimer trick: if speechTimer > 2.0 it's fresh spawn
            // speech, otherwise it's combat/hurt speech that may repeat.
            // Simplest: always log, but cap speechTimer to prevent re-entry.
            if (e.speechText && e.speechTimer > 1.9f) {
                const char* name = "???";
                if (e.flags & ENT_FRIENDLY) {
                    switch (e.npcClass) {
                        case NpcClass::CLERIC:  name = "Cleric";  break;
                        case NpcClass::ARCHER:  name = "Archer";  break;
                        case NpcClass::MAGE:    name = "Mage";    break;
                        case NpcClass::ROGUE:   name = "Rogue";   break;
                        case NpcClass::PALADIN: name = "Paladin"; break;
                        default:                name = "Ally";     break;
                    }
                } else if (e.enemyType == EnemyType::BOSS) {
                    name = "Butcher";
                }
                Vec3 chatCol = (e.flags & ENT_FRIENDLY)
                    ? Vec3{0.4f, 1.0f, 0.5f}
                    : Vec3{1.0f, 0.3f, 0.3f};
                addChatMessage(name, e.speechText, chatCol);
                e.speechTimer = 1.8f; // prevent re-logging on next tick
            }
            e.speechTimer -= dt;
            if (e.speechTimer <= 0.0f) {
                e.speechText  = nullptr;
                e.speechTimer = 0.0f;
            }
        }
    }

    // Decay chat line timers
    for (u32 i = 0; i < MAX_CHAT_LINES; i++) {
        if (m_chatLog[i].timer > 0.0f) m_chatLog[i].timer -= dt;
    }

    // Projectiles
    { PROFILE_SCOPE(2, "Projectiles");
    ProjectileSystem::update(m_projectiles, m_grid, m_entities, m_localPlayer, dt);
    }

    // Entity timers
    EntitySystem::tickTimers(m_entities, dt);

    // Update world items
    WorldItemSystem::update(m_worldItems, dt);

    // Decay visual effects (fire, nova, dash)
    for (u32 i = 0; i < MAX_FIRE_FX; i++) {
        if (m_fireFX[i].active) {
            m_fireFX[i].timer -= dt;
            if (m_fireFX[i].timer <= 0.0f) m_fireFX[i].active = false;
        }
    }
    for (u32 i = 0; i < MAX_NOVA_FX; i++) {
        if (m_novaFX[i].active) {
            m_novaFX[i].timer -= dt;
            if (m_novaFX[i].timer <= 0.0f) m_novaFX[i].active = false;
        }
    }
    for (u32 i = 0; i < MAX_DASH_FX; i++) {
        if (m_dashFX[i].active) {
            m_dashFX[i].timer -= dt;
            if (m_dashFX[i].timer <= 0.0f) m_dashFX[i].active = false;
        }
    }
    // Scorch zones — persistent ground fire dealing AoE DoT each tick
    for (u32 i = 0; i < MAX_SCORCH; i++) {
        if (!m_scorchZones[i].active) continue;
        ScorchZone& sz = m_scorchZones[i];
        sz.timer -= dt;
        if (sz.timer <= 0.0f) { sz.active = false; continue; }
        // Damage all hostile entities standing in the scorch zone
        for (u32 a = 0; a < m_entities.activeCount; a++) {
            u32 idx = m_entities.activeList[a];
            Entity& ent = m_entities.entities[idx];
            if (ent.flags & ENT_DEAD) continue;
            if (ent.flags & ENT_FRIENDLY) continue;
            if (ent.enemyType == EnemyType::PROP) continue;
            f32 dist = length(ent.position - sz.pos);
            if (dist < sz.radius) {
                ent.health -= sz.dps * dt;
                if (ent.health <= 0.0f && !(ent.flags & ENT_DEAD)) {
                    ent.health = 0.0f;
                    ent.flags |= ENT_DEAD;
                    ent.aiState = AIState::DEAD;
                    ent.deathTimer = 1.0f;
                    ent.velocity = {0,0,0};
                }
            }
        }
    }

    // Update skill state (energy regen, cooldowns)
    SkillSystem::update(m_skillStates[0], dt);
    // Tick class skill cooldowns (shared energy synced from main pool)
    for (u32 s = 0; s < 4; s++) {
        if (m_classSkillStates[s].cooldownTimer > 0.0f)
            m_classSkillStates[s].cooldownTimer -= dt;
    }

    // Update orb projectiles (spawn ice shards for Frozen Orb)
    SkillSystem::updateOrbProjectiles(m_projectiles, m_skillDefs, m_skillDefCount, dt);

    // Update pending meteors
    SkillSystem::updateMeteors(m_entities, dt);

    // --- Weapon on-hit proc (legendary weapon passive) ---
    {
        const ItemInstance& wpn = m_inventories[0].equipped[static_cast<u32>(ItemSlot::WEAPON)];
        m_weaponProc = (!isItemEmpty(wpn) && wpn.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[wpn.defId].legendarySkillId : SkillId::NONE;
    }
    // Armor passive aura
    {
        const ItemInstance& armor = m_inventories[0].equipped[static_cast<u32>(ItemSlot::ARMOR)];
        m_armorAura = (!isItemEmpty(armor) && armor.rarity == Rarity::LEGENDARY)
            ? m_itemDefs[armor.defId].legendarySkillId : SkillId::NONE;
    }

    // --- Class skill selection (keys 1-4) ---
    if (!m_inventoryOpen) {
        const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
        for (u8 s = 0; s < 4; s++) {
            if (Input::isKeyPressed(SDL_SCANCODE_1 + s)) {
                // Only select if this skill is unlocked on the current floor
                if (m_currentFloor >= cls.skillUnlockFloor[s]) {
                    m_activeClassSkill = s;
                }
            }
        }
    }

    // --- Class skill activation (right-click) ---
    Vec3 eyePos = m_localPlayer.position + Vec3{0, m_localPlayer.eyeHeight, 0};

    if (Input::isMouseButtonPressed(SDL_BUTTON_RIGHT) && !m_inventoryOpen) {
        const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
        u8 slot = m_activeClassSkill;
        if (m_currentFloor >= cls.skillUnlockFloor[slot]) {
            // Use the class skill state for cooldown tracking, shared energy pool
            m_classSkillStates[slot].activeSkill = cls.skills[slot];
            m_classSkillStates[slot].energy = m_skillStates[0].energy;
            m_classSkillStates[slot].maxEnergy = m_skillStates[0].maxEnergy;

            if (SkillSystem::tryActivate(m_classSkillStates[slot], m_skillDefs, m_skillDefCount,
                                          eyePos, m_localPlayer.forward, m_localPlayer.yaw,
                                          m_projectiles, m_entities, m_grid, m_localPlayer)) {
                m_skillStates[0].energy = m_classSkillStates[slot].energy;

                // (drone/turret mesh assignment handled by DroneSpawnCallback)
            }
        }
    }

    // --- Shield blocking (Ctrl/Shift) ---
    {
        bool wantsBlock = (Input::isKeyDown(SDL_SCANCODE_LCTRL) ||
                           Input::isKeyDown(SDL_SCANCODE_RCTRL) ||
                           Input::isKeyDown(SDL_SCANCODE_LSHIFT) ||
                           Input::isKeyDown(SDL_SCANCODE_RSHIFT)) && !m_inventoryOpen;
        if (wantsBlock && !m_localPlayer.blocking) {
            m_localPlayer.blocking = true;
            m_localPlayer.blockTimer = 0.0f; // start perfect block window
        } else if (!wantsBlock) {
            m_localPlayer.blocking = false;
        }
        if (m_localPlayer.blocking) {
            m_localPlayer.blockTimer += dt;
        }
    }

    // --- Armor passive aura tick ---
    if (m_armorAura != SkillId::NONE) {
        Vec3 playerPos = m_localPlayer.position;
        for (u32 a = 0; a < m_entities.activeCount; a++) {
            u32 idx = m_entities.activeList[a];
            Entity& ent = m_entities.entities[idx];
            if (ent.flags & ENT_DEAD) continue;
            if (ent.flags & ENT_FRIENDLY) continue;
            if (ent.enemyType == EnemyType::PROP) continue;
            f32 dist = length(ent.position - playerPos);

            switch (m_armorAura) {
                case SkillId::METEOR_STRIKE: // Fire aura: 2 dps burn within 3m
                    if (dist < 3.0f) { ent.burnTimer = 0.5f; ent.burnDps = 2.0f; }
                    break;
                case SkillId::FROZEN_ORB: // Frost aura: slow within 4m
                    if (dist < 4.0f) { ent.freezeTimer = 0.5f; }
                    break;
                default: break;
            }
        }
    }

    updatePlayerPickup();

    // updateFloorDoor returns true when the player descends — skip remainder of tick
    if (updateFloorDoor()) return;

    // Toggle inventory (Tab key)
    if (Input::isKeyPressed(SDL_SCANCODE_TAB)) {
        m_inventoryOpen = !m_inventoryOpen;
        Input::setRelativeMouseMode(!m_inventoryOpen);
        // Reset drag/click state when toggling inventory
        m_dragState = {};
        m_dblClickState = {};
    }

    updateInventoryInteraction(dt);

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
    if (m_fullBackpackNotifyTimer > 0.0f) m_fullBackpackNotifyTimer -= dt;

    // Camera
    PlayerController::applyToCamera(m_localPlayer, m_camera);
    // Screen shake only from enemy hits, not weapon fire
    if (m_localPlayer.hitShakeTimer > 0.0f) {
        m_localPlayer.hitShakeTimer -= dt;
        f32 shake = m_localPlayer.hitShakeTimer * 0.08f;
        m_camera.pitch += sinf(m_localPlayer.hitShakeTimer * 60.0f) * shake;
    }

    pushPlayerFromEntities();

    // Update fog-of-war
    Minimap::updateVisited(m_grid, m_localPlayer.position, m_entities);

    syncLocalPlayerToNetPlayer();
}

// ---------------------------------------------------------------------------
// singleplayerUpdate sub-functions
// ---------------------------------------------------------------------------

// Auto-pickup health/energy globes (walk-over) and E-key item pickup.
// Globes are consumed immediately; regular items go to the backpack.
void Engine::updatePlayerPickup() {
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
            if (!isGlobe(picked)) {
                if (Inventory::addToBackpack(m_inventories[0], picked)) {
                    // Auto-equip first weapon; assign subsequent weapons to quickbar
                    if (picked.defId < m_itemDefCount &&
                        m_itemDefs[picked.defId].slot == ItemSlot::WEAPON) {
                        // Find which backpack slot it landed in
                        u8 bpIdx = 0xFF;
                        for (u8 bi = 0; bi < MAX_INVENTORY_ITEMS; bi++) {
                            if (m_inventories[0].backpack[bi].uid == picked.uid) {
                                bpIdx = bi;
                                break;
                            }
                        }
                        if (bpIdx != 0xFF) {
                            const ItemInstance& eqWpn = m_inventories[0].equipped[static_cast<u32>(ItemSlot::WEAPON)];
                            if (isItemEmpty(eqWpn)) {
                                // No weapon equipped — auto-equip and assign to slot 0
                                Inventory::equip(m_inventories[0], bpIdx, m_itemDefs);
                                Quickbar::syncWeaponSlot(m_quickbars[0], m_inventories[0]);
                            } else {
                                // Already have a weapon — assign to quickbar
                                Quickbar::assignItem(m_quickbars[0], m_inventories[0], bpIdx);
                            }
                        }
                    }
                } else {
                    // Backpack full: drop item at player's feet
                    WorldItemSystem::spawn(m_worldItems, picked,
                        m_localPlayer.position + Vec3{0, 0.5f, 0});
                    m_fullBackpackNotifyTimer = 2.0f;
                }
            }
        }
    }
}

// Floor door interaction — descend to next floor when near and E is pressed.
// Returns true if the player descended (caller must return immediately to skip
// the rest of the tick with the now-regenerated level state).
bool Engine::updateFloorDoor() {
    if (m_floorDoorActive) {
        Vec3 toDoor = m_floorDoorPos - m_localPlayer.position;
        if (lengthSq(toDoor) < 4.0f) {
            if (Input::isKeyPressed(SDL_SCANCODE_E)) {
                m_currentFloor++;
                // Upgrade equipment for NPCs that survived this floor
                upgradeNpcEquipment(static_cast<u8>(m_currentFloor));
                // Save progress before descending so death respawn returns here
                m_savedFloor = m_currentFloor;
                m_savedSeed = static_cast<u32>(std::rand());
                saveGame();
                LOG_INFO("Descending to floor %u", m_currentFloor);
                startGame(); // regenerate dungeon with new floor seed
                return true; // skip the remainder of this tick
            }
        }
    }
    return false;
}

// Inventory drag-and-drop state machine — handles click, double-click, and drag
// across backpack, equipment, and quickbar panels.
void Engine::updateInventoryInteraction(f32 dt) {
    if (!m_inventoryOpen) return;

    s32 mx, my;
    Input::getMousePosition(mx, my);
    my = static_cast<s32>(Window::getHeight()) - my; // flip to HUD coords

    u32 sw = Window::getWidth();
    u32 sh = Window::getHeight();

    // Tick double-click timer
    m_dblClickState.timer += dt;

    if (m_dragState.source == DragSource::NONE) {
        // --- No drag active ---

        // Left mouse pressed: detect double-click or begin potential drag
        if (Input::isMouseButtonPressed(SDL_BUTTON_LEFT)) {
            InventoryUI::SlotHit hit = InventoryUI::hitTest(sw, sh, mx, my);

            if (hit.panel == InventoryUI::SlotHit::BACKPACK &&
                hit.index < MAX_INVENTORY_ITEMS &&
                !isItemEmpty(m_inventories[0].backpack[hit.index])) {

                // Double-click detection: same backpack slot within 0.3s
                if (m_dblClickState.wasBackpack &&
                    m_dblClickState.lastSlot == hit.index &&
                    m_dblClickState.timer < 0.3f) {
                    // Double-click: equip directly
                    Inventory::equip(m_inventories[0], hit.index, m_itemDefs);
                    Quickbar::syncWeaponSlot(m_quickbars[0], m_inventories[0]);
                    m_dblClickState = {};
                } else {
                    // Record for potential double-click and begin potential drag
                    m_dblClickState.timer = 0.0f;
                    m_dblClickState.lastSlot = hit.index;
                    m_dblClickState.wasBackpack = true;

                    const ItemInstance& item = m_inventories[0].backpack[hit.index];
                    m_dragState.source = DragSource::BACKPACK;
                    m_dragState.sourceIndex = hit.index;
                    m_dragState.itemUid = item.uid;
                    m_dragState.itemDefId = item.defId;
                    m_dragState.startX = mx;
                    m_dragState.startY = my;
                    m_dragState.dragging = false;
                }
            } else if (hit.panel == InventoryUI::SlotHit::EQUIPMENT &&
                       hit.index < static_cast<u8>(ItemSlot::COUNT) &&
                       !isItemEmpty(m_inventories[0].equipped[hit.index])) {
                // Begin drag from equipment slot
                const ItemInstance& item = m_inventories[0].equipped[hit.index];
                m_dragState.source = DragSource::EQUIPMENT;
                m_dragState.sourceIndex = hit.index;
                m_dragState.itemUid = item.uid;
                m_dragState.itemDefId = item.defId;
                m_dragState.startX = mx;
                m_dragState.startY = my;
                m_dragState.dragging = false;
                m_dblClickState = {};
            } else if (hit.panel == InventoryUI::SlotHit::QUICKBAR &&
                       hit.index < QUICKBAR_SLOTS) {
                const ItemInstance* qbItem = Quickbar::resolveSlot(m_quickbars[0], m_inventories[0], hit.index);
                if (qbItem && !isItemEmpty(*qbItem)) {
                    m_dragState.source = DragSource::QUICKBAR;
                    m_dragState.sourceIndex = hit.index;
                    m_dragState.itemUid = qbItem->uid;
                    m_dragState.itemDefId = qbItem->defId;
                    m_dragState.startX = mx;
                    m_dragState.startY = my;
                    m_dragState.dragging = false;
                }
                m_dblClickState = {};
            } else {
                m_dblClickState = {};
            }
        }

        // Right-click: drop item to world (backpack or equipment)
        if (Input::isMouseButtonPressed(SDL_BUTTON_RIGHT)) {
            InventoryUI::SlotHit hit = InventoryUI::hitTest(sw, sh, mx, my);
            Vec3 dropPos = m_localPlayer.position + m_localPlayer.forward * 1.5f + Vec3{0, 0.5f, 0};
            if (hit.panel == InventoryUI::SlotHit::BACKPACK &&
                hit.index < MAX_INVENTORY_ITEMS &&
                !isItemEmpty(m_inventories[0].backpack[hit.index])) {
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[0], hit.index);
                if (!isItemEmpty(dropped)) {
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
                }
            } else if (hit.panel == InventoryUI::SlotHit::EQUIPMENT &&
                       hit.index < static_cast<u8>(ItemSlot::COUNT) &&
                       !isItemEmpty(m_inventories[0].equipped[hit.index])) {
                ItemInstance dropped = Inventory::dropFromEquipment(m_inventories[0],
                    static_cast<ItemSlot>(hit.index));
                if (!isItemEmpty(dropped)) {
                    WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
                    Quickbar::syncWeaponSlot(m_quickbars[0], m_inventories[0]);
                }
            }
        }

        // Q key: drop all backpack items to world
        if (Input::isKeyPressed(SDL_SCANCODE_Q)) {
            Vec3 dropBase = m_localPlayer.position + m_localPlayer.forward * 1.5f + Vec3{0, 0.5f, 0};
            for (u8 si = 0; si < MAX_INVENTORY_ITEMS; si++) {
                if (isItemEmpty(m_inventories[0].backpack[si])) continue;
                ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[0], si);
                if (!isItemEmpty(dropped)) {
                    // Spread items in a small arc so they don't stack
                    f32 angle = si * 0.4f;
                    Vec3 offset = {sinf(angle) * 0.5f, 0, cosf(angle) * 0.5f};
                    WorldItemSystem::spawn(m_worldItems, dropped, dropBase + offset);
                }
            }
        }

        // Middle mouse: equip from quickbar (item stays in quickbar as EQUIPPED_REF)
        if (Input::isMouseButtonPressed(SDL_BUTTON_MIDDLE)) {
            InventoryUI::SlotHit hit = InventoryUI::hitTest(sw, sh, mx, my);
            if (hit.panel == InventoryUI::SlotHit::QUICKBAR) {
                QuickbarSlot& qs = m_quickbars[0].slots[hit.index];
                if (qs.type == QuickbarSlot::BACKPACK_REF &&
                    qs.sourceIndex < MAX_INVENTORY_ITEMS &&
                    !isItemEmpty(m_inventories[0].backpack[qs.sourceIndex])) {
                    u32 uid = qs.itemUid;
                    ItemSlot itemSlot = m_itemDefs[m_inventories[0].backpack[qs.sourceIndex].defId].slot;
                    Inventory::equip(m_inventories[0], qs.sourceIndex, m_itemDefs);
                    // Update this quickbar slot to point to the equipment slot
                    qs.type = QuickbarSlot::EQUIPPED_REF;
                    qs.sourceIndex = static_cast<u8>(itemSlot);
                    qs.itemUid = uid;
                    Quickbar::syncWeaponSlot(m_quickbars[0], m_inventories[0]);
                }
            }
        }

    } else if (!m_dragState.dragging) {
        // --- Potential drag (mouse pressed but not moved far enough) ---

        if (Input::isMouseButtonDown(SDL_BUTTON_LEFT)) {
            s32 dx = mx - m_dragState.startX;
            s32 dy = my - m_dragState.startY;
            if (dx * dx + dy * dy > 9) { // > 3px dead zone
                m_dragState.dragging = true;
            }
        }
        if (Input::isMouseButtonReleased(SDL_BUTTON_LEFT)) {
            // Single click within dead zone — cancel drag, click was recorded for double-click
            m_dragState = {};
        }

    } else {
        // --- Active drag ---

        if (Input::isMouseButtonReleased(SDL_BUTTON_LEFT)) {
            InventoryUI::SlotHit drop = InventoryUI::hitTest(sw, sh, mx, my);

            if (drop.panel == InventoryUI::SlotHit::QUICKBAR) {
                // Drop on quickbar slot
                if (m_dragState.source == DragSource::QUICKBAR) {
                    Quickbar::swapSlots(m_quickbars[0], m_dragState.sourceIndex, drop.index);
                } else {
                    Quickbar::assignToSlot(m_quickbars[0], m_inventories[0],
                                            drop.index, m_dragState.source, m_dragState.sourceIndex);
                }
            } else if (drop.panel == InventoryUI::SlotHit::EQUIPMENT &&
                       m_dragState.source == DragSource::BACKPACK) {
                // Drop backpack item on equipment slot — equip it
                Inventory::equip(m_inventories[0], m_dragState.sourceIndex, m_itemDefs);
                Quickbar::syncWeaponSlot(m_quickbars[0], m_inventories[0]);
            } else if (drop.panel == InventoryUI::SlotHit::NONE) {
                // Drop outside all panels — drop item to world
                Vec3 dropPos = m_localPlayer.position + Vec3{0, 0.5f, 0};
                if (m_dragState.source == DragSource::BACKPACK) {
                    ItemInstance dropped = Inventory::dropFromBackpack(m_inventories[0], m_dragState.sourceIndex);
                    if (!isItemEmpty(dropped)) {
                        WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
                    }
                } else if (m_dragState.source == DragSource::EQUIPMENT) {
                    ItemInstance dropped = Inventory::dropFromEquipment(m_inventories[0],
                        static_cast<ItemSlot>(m_dragState.sourceIndex));
                    if (!isItemEmpty(dropped)) {
                        WorldItemSystem::spawn(m_worldItems, dropped, dropPos);
                        Quickbar::syncWeaponSlot(m_quickbars[0], m_inventories[0]);
                    }
                } else if (m_dragState.source == DragSource::QUICKBAR) {
                    // Remove from quickbar only (item stays in backpack)
                    Quickbar::removeItem(m_quickbars[0], m_dragState.sourceIndex);
                }
            }
            // Reset drag state
            m_dragState = {};
        }
    }
}

// Pushes the local player out of all active hostile entity AABBs.
// Uses the minimal-penetration axis to avoid tunneling on corners.
// Reverts push if it would land the player inside solid geometry.
void Engine::pushPlayerFromEntities() {
    AABB playerBox = {
        m_localPlayer.position + Vec3{-PLAYER_HALF_WIDTH, 0.0f, -PLAYER_HALF_WIDTH},
        m_localPlayer.position + Vec3{ PLAYER_HALF_WIDTH, PLAYER_HEIGHT, PLAYER_HALF_WIDTH}
    };
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        Entity& e = m_entities.entities[i];
        if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) continue;
        // Friendly NPCs don't push the player — they yield instead
        if (e.flags & ENT_FRIENDLY) continue;
        AABB entBox = entityAABB(e);
        if (CombatQuery::aabbOverlap(playerBox, entBox)) {
            Vec3 toPlayer = m_localPlayer.position - e.position;
            f32 pushX = (e.halfExtents.x + PLAYER_HALF_WIDTH) - fabsf(toPlayer.x);
            f32 pushZ = (e.halfExtents.z + PLAYER_HALF_WIDTH) - fabsf(toPlayer.z);
            if (pushX > 0.0f && pushZ > 0.0f) {
                // Try the push, but reject it if it lands inside a wall
                Vec3 saved = m_localPlayer.position;
                if (pushX < pushZ)
                    m_localPlayer.position.x += (toPlayer.x > 0) ? pushX : -pushX;
                else
                    m_localPlayer.position.z += (toPlayer.z > 0) ? pushZ : -pushZ;
                // Validate — revert if new position is inside solid geometry
                u32 gx, gz;
                if (LevelGridSystem::worldToGrid(m_grid, m_localPlayer.position, gx, gz) &&
                    LevelGridSystem::isSolid(m_grid, gx, gz)) {
                    m_localPlayer.position = saved;
                }
            }
        }
    }
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

    pushPlayerFromEntities();
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

    // Always fire with the equipped weapon (not the active quickbar slot)
    const ItemInstance& eqWpn = m_inventories[m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::WEAPON)];
    WeaponDef wpn;
    if (!isItemEmpty(eqWpn)) {
        wpn = Inventory::getWeaponFromItem(m_inventories[m_localPlayerIndex],
                                            m_itemDefs, eqWpn);
    } else {
        wpn = m_weaponDefs[ws.currentWeapon];
    }
    // Track subtype for projectile flags (molotov/wand detection)
    const ItemInstance* qbItem = &eqWpn;
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

        u16 projIdx;
        if (isMolotov) {
            projIdx = Combat::fireProjectile(wpn, eyePos, forward, m_projectiles,
                                              9.8f, 3.0f, wpn.damage * 0.6f);
        } else {
            u8 flags = isWand ? PROJ_SPARK : 0;
            projIdx = Combat::fireProjectile(wpn, eyePos, forward, m_projectiles, flags);
        }
        // Tag thrown weapons with their mesh (knife, molotov, bow, crossbow)
        if (projIdx != 0xFFFF && qbItem && !isItemEmpty(*qbItem)) {
            u8 wpnMesh = m_itemDefs[qbItem->defId].meshId;
            WeaponSubtype sub = m_itemDefs[qbItem->defId].weaponSubtype;
            bool isThrown = (sub == WeaponSubtype::THROWING_KNIFE ||
                             sub == WeaponSubtype::MOLOTOV ||
                             sub == WeaponSubtype::BOW ||
                             sub == WeaponSubtype::CROSSBOW);
            if (isThrown && wpnMesh > 0) {
                m_projectiles.projectiles[projIdx].meshId = wpnMesh;
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

    // Weapon legendary on-hit proc — % chance to trigger skill at hit position
    if (result.hitEntity && m_weaponProc != SkillId::NONE) {
        u32 procRoll = static_cast<u32>(std::rand()) % 100;
        u32 procChance = 20; // default 20%
        if (m_weaponProc == SkillId::FROZEN_ORB)    procChance = 15;
        if (m_weaponProc == SkillId::CHAIN_LIGHTNING) procChance = 25;
        if (m_weaponProc == SkillId::METEOR_STRIKE)  procChance = 10;
        if (m_weaponProc == SkillId::BLOOD_NOVA)     procChance = 20;

        if (procRoll < procChance) {
            Vec3 procPos = result.hitPosition;
            const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, m_weaponProc);
            if (sd) {
                // Fire the skill effect at the hit position
                switch (m_weaponProc) {
                    case SkillId::FROZEN_ORB: {
                        Vec3 dir = m_localPlayer.forward;
                        u16 orbIdx = ProjectileSystem::spawn(m_projectiles, procPos, dir,
                            sd->projectileSpeed, sd->damage, sd->radius, sd->duration, true);
                        if (orbIdx != 0xFFFF) m_projectiles.projectiles[orbIdx].projFlags = PROJ_ORB;
                    } break;
                    case SkillId::CHAIN_LIGHTNING: {
                        // Spawn spark projectiles in a small fan from hit position
                        for (s32 s = -1; s <= 1; s++) {
                            f32 spread = s * 0.3f;
                            Vec3 dir = normalize(Vec3{m_localPlayer.forward.x + spread,
                                                       0, m_localPlayer.forward.z});
                            ProjectileSystem::spawn(m_projectiles, procPos, dir,
                                18.0f, sd->damage * 0.5f, 0.08f, 1.5f, true, PROJ_SPARK);
                        }
                    } break;
                    case SkillId::METEOR_STRIKE: {
                        // Drop a meteor on the hit position
                        extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
                        for (u32 mi = 0; mi < MAX_PENDING_METEORS; mi++) {
                            if (!s_meteors[mi].active) {
                                s_meteors[mi] = {procPos, sd->damage, sd->radius, sd->delay, true};
                                break;
                            }
                        }
                    } break;
                    case SkillId::BLOOD_NOVA: {
                        // Nova centered on hit target
                        EntityHandle hits[MAX_ENTITIES];
                        f32 dists[MAX_ENTITIES];
                        u32 hitCount = CombatQuery::queryConeSorted(
                            m_entities, procPos, {0,0,-1}, -1.0f, sd->radius,
                            hits, dists, MAX_ENTITIES);
                        for (u32 h = 0; h < hitCount; h++) {
                            Combat::applyDamage(m_entities, hits[h], sd->damage * 0.5f);
                        }
                        // Visual
                        for (u32 ni = 0; ni < MAX_NOVA_FX; ni++) {
                            if (!m_novaFX[ni].active) {
                                m_novaFX[ni] = {procPos, sd->radius, 0.6f, true, {1.0f, 0.15f, 0.1f}};
                                break;
                            }
                        }
                    } break;
                    default: break;
                }
            }
        }
    }
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
    (void)dt;
    // Middle-click outside inventory: equip active quickbar item (keeps ref in quickbar)
    if (Input::isMouseButtonPressed(SDL_BUTTON_MIDDLE)) {
        u8 slot = m_quickbars[0].activeSlot;
        QuickbarSlot& qs = m_quickbars[0].slots[slot];
        if (qs.type == QuickbarSlot::BACKPACK_REF &&
            qs.sourceIndex < MAX_INVENTORY_ITEMS &&
            !isItemEmpty(m_inventories[0].backpack[qs.sourceIndex])) {
            u32 uid = qs.itemUid;
            ItemSlot itemSlot = m_itemDefs[m_inventories[0].backpack[qs.sourceIndex].defId].slot;
            Inventory::equip(m_inventories[0], qs.sourceIndex, m_itemDefs);
            // Convert quickbar ref from backpack to equipment so it stays valid
            qs.type = QuickbarSlot::EQUIPPED_REF;
            qs.sourceIndex = static_cast<u8>(itemSlot);
            qs.itemUid = uid;
            Quickbar::syncWeaponSlot(m_quickbars[0], m_inventories[0]);
        }
    }
    m_localPlayer.lockActive = false;
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

    // Choose entity source based on role (also used by debug overlay below)
    const EntityPool& entPool = (m_netRole == NetRole::CLIENT) ? m_renderEntities : m_entities;

    renderEntities(sw, sh);

    // Clear debug lines before game effects — portal, fire, sparks, loot glow
    // all use DebugDraw::line and must not be wiped before flush
    DebugDraw::clear();

    renderProjectilesAndEffects(sw, sh);
    renderWorldItems(sw, sh);

    { PROFILE_SCOPE(4, "Flush");
    Renderer::flush();
    }

    // --- Debug overlay (F1 toggle — boxes only, lines already accumulated above) ---
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

    renderSpeechBubbles(sw, sh);

    // First-person viewmodel (hand + weapon) — drawn after world, before HUD
    renderViewmodel();

    renderHUD(sw, sh);

    GLContext::swapBuffers(Window::getHandle());
}

// ---------------------------------------------------------------------------
// renderEntities — entity body + limb rendering loop with procedural animation.
// Submits to the shared Renderer batch; caller flushes (Renderer::flush) after
// all world geometry and effects have been submitted.
// ---------------------------------------------------------------------------
void Engine::renderEntities(u32 sw, u32 sh) {
    (void)sw; (void)sh; // sw/sh reserved for future use

    const EntityPool& entPool = (m_netRole == NetRole::CLIENT) ? m_renderEntities : m_entities;
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

        // Resolve tint — friendly NPCs use their material skin tint
        Vec4 tint;
        if (e.flags & ENT_FRIENDLY) {
            tint = (e.materialId > 0) ? entMat->tint : Vec4{0.8f, 0.7f, 0.55f, 1.0f};
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

    // Enemy rim aura — subtle colored lines around the entity's feet so they
    // pop from the background.  Uses DebugDraw (pure color, no texture).
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& e = entPool.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;
        if (e.flags & ENT_DEAD) continue;
        if (e.flags & ENT_FRIENDLY) continue;
        if (e.enemyType == EnemyType::PROP) continue;

        f32 distSq = lengthSq(e.position - m_camera.position);
        if (distSq > 225.0f) continue;

        f32 fade = 1.0f - distSq / 225.0f;
        f32 pulse = 0.7f + 0.3f * sinf(e.animTimer * 3.0f + static_cast<f32>(i));

        Vec3 col;
        f32 r;
        if (e.enemyType == EnemyType::BOSS) {
            col = {0.8f * fade * pulse, 0.15f * fade, 0.05f * fade};
            r = e.halfExtents.x * 1.3f;
        } else {
            col = {0.6f * fade * pulse, 0.25f * fade, 0.08f * fade};
            r = e.halfExtents.x * 1.1f;
        }

        // Ground ring around the entity's feet
        Vec3 base = e.position - Vec3{0, e.halfExtents.y - 0.05f, 0};
        static constexpr u32 RING_SEGS = 8;
        for (u32 s = 0; s < RING_SEGS; s++) {
            f32 a0 = static_cast<f32>(s) * (6.2832f / RING_SEGS);
            f32 a1 = a0 + (6.2832f / RING_SEGS);
            Vec3 p0 = base + Vec3{cosf(a0) * r, 0, sinf(a0) * r};
            Vec3 p1 = base + Vec3{cosf(a1) * r, 0, sinf(a1) * r};
            DebugDraw::line(p0, p1, col);
        }
    }

    // Enemy light source — starburst glow lines radiating from each entity's
    // center to simulate a small point light.  Pure DebugDraw lines (no texture).
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& e = entPool.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;
        if (e.flags & ENT_DEAD) continue;
        if (e.enemyType == EnemyType::PROP) continue;

        Vec3 lightPos = e.position + Vec3{0, e.halfExtents.y * 0.5f, 0};

        f32 distSq = lengthSq(lightPos - m_camera.position);
        if (distSq > 400.0f) continue;
        f32 fade = 1.0f - distSq / 400.0f;
        f32 pulse = 0.7f + 0.3f * sinf(e.animTimer * 4.0f + static_cast<f32>(i));

        Vec3 col;
        f32 lightRadius;
        u32 rayCount;

        if (e.flags & ENT_FRIENDLY) {
            col = {0.1f * fade, 0.45f * fade * pulse, 0.15f * fade};
            lightRadius = 0.4f;
            rayCount = 4;
        } else if (e.enemyType == EnemyType::BOSS) {
            col = {0.9f * fade * pulse, 0.2f * fade, 0.05f * fade};
            lightRadius = 1.0f;
            rayCount = 8;
        } else {
            col = {0.65f * fade * pulse, 0.3f * fade, 0.08f * fade};
            lightRadius = 0.5f;
            rayCount = 6;
        }

        // Starburst: lines radiating outward in all directions from the light center
        for (u32 r = 0; r < rayCount; r++) {
            f32 angle = static_cast<f32>(r) * (6.2832f / static_cast<f32>(rayCount))
                      + e.animTimer * 1.5f; // slow rotation
            f32 dx = cosf(angle) * lightRadius;
            f32 dz = sinf(angle) * lightRadius;
            // Horizontal rays
            DebugDraw::line(lightPos, lightPos + Vec3{dx, 0, dz}, col);
            // Angled rays (upward and downward)
            DebugDraw::line(lightPos, lightPos + Vec3{dx * 0.7f, lightRadius * 0.5f, dz * 0.7f}, col * 0.7f);
            DebugDraw::line(lightPos, lightPos + Vec3{dx * 0.7f, -lightRadius * 0.3f, dz * 0.7f}, col * 0.5f);
        }

        // Vertical accent line (upward glow)
        DebugDraw::line(lightPos, lightPos + Vec3{0, lightRadius * 0.6f, 0}, col * 0.8f);
    }
}

// ---------------------------------------------------------------------------
// renderProjectilesAndEffects — projectile cubes + spark orbs, floor door portal
// debug lines, and fire AoE effects (molotov splash). All DebugDraw::line calls
// here are accumulated before DebugDraw::flush in render().
// ---------------------------------------------------------------------------
void Engine::renderProjectilesAndEffects(u32 sw, u32 sh) {
    (void)sw; (void)sh; // sw/sh reserved for future use

    const ProjectilePool& projPool = (m_netRole == NetRole::CLIENT) ? m_renderProjectiles : m_projectiles;
    const Texture& defaultTex = MaterialSystem::get(0)->texture;

    // Projectiles
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        const Projectile& p = projPool.projectiles[i];
        if (!p.active) continue;

        if (p.projFlags & PROJ_ORB) {
            // Frozen Orb — layered crystalline sphere with frost spiral trail
            f32 t = p.lifetime;
            f32 pulse = 0.7f + 0.3f * sinf(t * 12.0f);

            // Core: bright white-blue inner orb
            f32 coreSize = p.radius * 2.0f * pulse;
            Mat4 coreModel = Mat4::translate(p.position)
                           * Mat4::rotateY(t * 8.0f)
                           * Mat4::rotateX(t * 5.0f)
                           * Mat4::scale({coreSize, coreSize, coreSize});
            AABB coreBounds = {p.position - Vec3{coreSize,coreSize,coreSize},
                               p.position + Vec3{coreSize,coreSize,coreSize}};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, coreModel, coreBounds,
                             {0.8f, 0.9f, 1.0f, 1.0f});

            // Outer shell: larger translucent ice-blue
            f32 shellSize = p.radius * 4.5f * (0.9f + 0.1f * sinf(t * 20.0f));
            Mat4 shellModel = Mat4::translate(p.position)
                            * Mat4::rotateY(-t * 3.0f)
                            * Mat4::rotateZ(t * 4.0f)
                            * Mat4::scale({shellSize, shellSize, shellSize});
            AABB shellBounds = {p.position - Vec3{shellSize,shellSize,shellSize},
                                p.position + Vec3{shellSize,shellSize,shellSize}};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, shellModel, shellBounds,
                             {0.2f, 0.5f, 1.0f, 0.4f});

            // Frost spiral trail — 6 trailing ice motes in a helix
            Vec3 vel = p.velocity;
            f32 spd = length(vel);
            if (spd > 0.01f) {
                Vec3 dir = vel * (1.0f / spd);
                for (u32 m = 0; m < 6; m++) {
                    f32 offset = m * 0.15f;
                    f32 angle = t * 10.0f + m * 1.05f;
                    Vec3 spiral = {sinf(angle) * 0.15f, cosf(angle) * 0.15f, 0};
                    Vec3 motePos = p.position - dir * (offset + 0.1f) + spiral;
                    f32 moteSize = coreSize * (0.3f - m * 0.04f);
                    if (moteSize < 0.02f) moteSize = 0.02f;
                    Mat4 moteModel = Mat4::translate(motePos)
                                   * Mat4::scale({moteSize, moteSize, moteSize});
                    AABB moteBounds = {motePos - Vec3{moteSize,moteSize,moteSize},
                                       motePos + Vec3{moteSize,moteSize,moteSize}};
                    f32 fade = 1.0f - m * 0.15f;
                    Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, moteModel, moteBounds,
                                     {0.4f * fade, 0.7f * fade, 1.0f * fade, 0.7f * fade});
                }
            }

        } else if (p.projFlags & PROJ_ORB_SHARD) {
            // Frozen Orb shard — elongated tumbling ice crystal with sparkle
            f32 t = p.lifetime;
            f32 shardW = p.radius * 1.2f;
            f32 shardH = p.radius * 3.0f;  // elongated
            Mat4 model = Mat4::translate(p.position)
                       * Mat4::rotateY(t * 25.0f)
                       * Mat4::rotateX(t * 15.0f)
                       * Mat4::scale({shardW, shardH, shardW});
            AABB bounds = {p.position - Vec3{shardH,shardH,shardH},
                           p.position + Vec3{shardH,shardH,shardH}};
            f32 sparkle = 0.7f + 0.3f * sinf(t * 40.0f);
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, model, bounds,
                             {0.6f * sparkle, 0.9f * sparkle, 1.0f, 0.9f});

        } else if (p.projFlags & PROJ_SPARK) {
            // Chain Lightning bolt — bright core + jagged electric arcs + trail
            f32 t = p.lifetime;
            f32 pulse = 0.6f + 0.4f * sinf(t * 30.0f);

            // Bright white-blue core
            f32 coreSize = p.radius * 2.5f * pulse;
            Mat4 coreModel = Mat4::translate(p.position)
                           * Mat4::rotateY(t * 20.0f)
                           * Mat4::rotateX(t * 14.0f)
                           * Mat4::scale({coreSize, coreSize, coreSize});
            AABB coreBounds = {p.position - Vec3{coreSize,coreSize,coreSize},
                               p.position + Vec3{coreSize,coreSize,coreSize}};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, coreModel, coreBounds,
                             {0.9f, 0.95f, 1.0f, 1.0f});

            // Outer electric glow
            f32 glowSize = p.radius * 4.0f;
            Mat4 glowModel = Mat4::translate(p.position)
                           * Mat4::rotateZ(t * 12.0f)
                           * Mat4::scale({glowSize, glowSize * 0.6f, glowSize});
            AABB glowBounds = {p.position - Vec3{glowSize,glowSize,glowSize},
                               p.position + Vec3{glowSize,glowSize,glowSize}};
            Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, glowModel, glowBounds,
                             {0.3f, 0.5f, 1.0f, 0.5f});

            // Jagged electric arcs radiating from the bolt (4 arcs via debug lines)
            for (u32 arc = 0; arc < 4; arc++) {
                f32 a = t * 15.0f + arc * 1.57f;
                f32 jitter = sinf(t * 50.0f + arc * 7.0f) * 0.12f;
                Vec3 arcEnd = p.position + Vec3{sinf(a) * (0.2f + jitter),
                                                 cosf(a * 1.3f) * 0.15f,
                                                 cosf(a) * (0.2f + jitter)};
                Vec3 col = {0.5f + pulse * 0.5f, 0.7f + pulse * 0.3f, 1.0f};
                DebugDraw::line(p.position, arcEnd, col);
            }

            // Electric trail behind
            Vec3 vel = p.velocity;
            f32 spd = length(vel);
            if (spd > 0.01f) {
                Vec3 dir = vel * (1.0f / spd);
                for (u32 tr = 0; tr < 3; tr++) {
                    Vec3 trailPos = p.position - dir * (tr * 0.2f + 0.1f);
                    f32 trailSize = coreSize * (0.5f - tr * 0.12f);
                    Mat4 trailModel = Mat4::translate(trailPos)
                                    * Mat4::rotateY(-t * 18.0f + tr)
                                    * Mat4::scale({trailSize, trailSize, trailSize});
                    AABB trailBounds = {trailPos - Vec3{trailSize,trailSize,trailSize},
                                        trailPos + Vec3{trailSize,trailSize,trailSize}};
                    f32 fade = 0.7f - tr * 0.2f;
                    Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, trailModel, trailBounds,
                                     {0.2f * fade, 0.4f * fade, 1.0f * fade, fade});
                }
            }

        } else {
            if (p.meshId > 0 && p.meshId < m_meshDefCount) {
                // Thrown weapon — spinning mesh with motion trail
                Vec3 vel = p.velocity;
                f32 spd = length(vel);
                f32 flyYaw = (spd > 0.01f) ? atan2f(-vel.x, -vel.z) : 0.0f;
                f32 spinAngle = p.lifetime * 15.0f;

                const AABB& mb = m_meshDefs[p.meshId].bounds;
                f32 maxDim = mb.max.y - mb.min.y;
                f32 mw = mb.max.x - mb.min.x;
                f32 md = mb.max.z - mb.min.z;
                if (mw > maxDim) maxDim = mw;
                if (md > maxDim) maxDim = md;
                f32 projScale = (maxDim > 0.001f) ? (0.4f / maxDim) : 0.4f;

                Mat4 model = Mat4::translate(p.position)
                           * Mat4::rotateY(flyYaw)
                           * Mat4::rotateX(spinAngle)
                           * Mat4::scale({projScale, projScale, projScale});
                AABB bounds = {p.position - Vec3{0.3f,0.3f,0.3f},
                               p.position + Vec3{0.3f,0.3f,0.3f}};
                Renderer::submit(m_basicShader, defaultTex, m_meshDefs[p.meshId].mesh, model, bounds,
                                 {0.8f, 0.8f, 0.8f, 1.0f});
            } else {
                // Magic wand / default projectile — glowing energy bolt with trail
                f32 t = p.lifetime;
                f32 pulse = 0.7f + 0.3f * sinf(t * 25.0f);
                bool isPlayer = p.fromPlayer;

                // Core bolt (bright)
                f32 coreSize = p.radius * 1.8f * pulse;
                Mat4 coreModel = Mat4::translate(p.position)
                               * Mat4::rotateY(t * 15.0f)
                               * Mat4::rotateX(t * 10.0f)
                               * Mat4::scale({coreSize, coreSize, coreSize});
                AABB coreBounds = {p.position - Vec3{coreSize,coreSize,coreSize},
                                   p.position + Vec3{coreSize,coreSize,coreSize}};
                Vec4 coreColor = isPlayer
                    ? Vec4{1.0f, 0.8f * pulse, 0.3f, 1.0f}     // warm golden
                    : Vec4{0.9f * pulse, 0.2f, 1.0f, 1.0f};    // purple
                Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, coreModel, coreBounds, coreColor);

                // Outer glow (larger, dimmer, different rotation)
                f32 glowSize = p.radius * 3.5f;
                Mat4 glowModel = Mat4::translate(p.position)
                               * Mat4::rotateZ(t * 8.0f)
                               * Mat4::rotateY(-t * 6.0f)
                               * Mat4::scale({glowSize, glowSize * 0.7f, glowSize});
                AABB glowBounds = {p.position - Vec3{glowSize,glowSize,glowSize},
                                   p.position + Vec3{glowSize,glowSize,glowSize}};
                Vec4 glowColor = isPlayer
                    ? Vec4{1.0f, 0.4f, 0.05f, 0.35f}
                    : Vec4{0.5f, 0.1f, 0.8f, 0.35f};
                Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, glowModel, glowBounds, glowColor);

                // Trailing energy motes (3 fading behind)
                Vec3 vel = p.velocity;
                f32 spd = length(vel);
                if (spd > 0.01f) {
                    Vec3 dir = vel * (1.0f / spd);
                    for (u32 tr = 0; tr < 3; tr++) {
                        f32 offset = (tr + 1) * 0.12f;
                        Vec3 trailPos = p.position - dir * offset;
                        f32 trailSize = coreSize * (0.6f - tr * 0.15f);
                        if (trailSize < 0.01f) trailSize = 0.01f;
                        Mat4 trailModel = Mat4::translate(trailPos)
                                        * Mat4::rotateY(t * 12.0f + tr * 2.0f)
                                        * Mat4::scale({trailSize, trailSize, trailSize});
                        AABB trailBounds = {trailPos - Vec3{trailSize,trailSize,trailSize},
                                            trailPos + Vec3{trailSize,trailSize,trailSize}};
                        f32 fade = 0.6f - tr * 0.18f;
                        Vec4 tc = isPlayer
                            ? Vec4{1.0f * fade, 0.5f * fade, 0.1f * fade, fade}
                            : Vec4{0.6f * fade, 0.1f * fade, 0.9f * fade, fade};
                        Renderer::submit(m_unlitShader, defaultTex, m_cubeMesh, trailModel, trailBounds, tc);
                    }
                }
            }
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

    // --- Blood Nova — expanding blood tendrils + shockwave rings ---
    for (u32 i = 0; i < MAX_NOVA_FX; i++) {
        if (!m_novaFX[i].active) continue;
        const NovaFX& nfx = m_novaFX[i];
        f32 t = 1.0f - nfx.timer / 0.6f; // 0→1 over lifetime
        f32 alpha = nfx.timer / 0.6f;
        f32 r = nfx.maxRadius * t;

        // Outer shockwave ring (expanding)
        static constexpr u32 NOVA_SEGS = 24;
        for (u32 s = 0; s < NOVA_SEGS; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / NOVA_SEGS);
            f32 a1 = a0 + (6.28318f / NOVA_SEGS);
            Vec3 p0 = nfx.pos + Vec3{cosf(a0) * r, 0.1f, sinf(a0) * r};
            Vec3 p1 = nfx.pos + Vec3{cosf(a1) * r, 0.1f, sinf(a1) * r};
            DebugDraw::line(p0, p1, nfx.color * alpha);
        }

        // Inner ring (smaller, brighter)
        f32 r2 = r * 0.6f;
        for (u32 s = 0; s < NOVA_SEGS; s++) {
            f32 a0 = static_cast<f32>(s) * (6.28318f / NOVA_SEGS) + t * 3.0f;
            f32 a1 = a0 + (6.28318f / NOVA_SEGS);
            Vec3 p0 = nfx.pos + Vec3{cosf(a0) * r2, 0.3f, sinf(a0) * r2};
            Vec3 p1 = nfx.pos + Vec3{cosf(a1) * r2, 0.3f, sinf(a1) * r2};
            DebugDraw::line(p0, p1, nfx.color * alpha * 1.2f);
        }

        // Blood tendrils radiating outward with wavy motion
        for (u32 tendril = 0; tendril < 8; tendril++) {
            f32 baseAngle = tendril * (6.28318f / 8.0f);
            Vec3 prev = nfx.pos + Vec3{0, 0.2f, 0};
            for (f32 d = 0.2f; d < r; d += 0.2f) {
                f32 wave = sinf(d * 6.0f + t * 10.0f + tendril) * 0.15f;
                f32 a = baseAngle + wave;
                f32 h = 0.2f + sinf(d * 3.0f) * 0.3f * alpha;
                Vec3 cur = nfx.pos + Vec3{cosf(a) * d, h, sinf(a) * d};
                f32 fade = alpha * (1.0f - d / nfx.maxRadius);
                DebugDraw::line(prev, cur, nfx.color * fade);
                prev = cur;
            }
        }
    }

    // --- Phase Dash — ghostly afterimage trail with energy wisps ---
    for (u32 i = 0; i < MAX_DASH_FX; i++) {
        if (!m_dashFX[i].active) continue;
        const DashFX& dfx = m_dashFX[i];
        f32 alpha = dfx.timer / 0.5f;
        Vec3 dir = dfx.end - dfx.start;
        f32 len = length(dir);
        if (len < 0.1f) continue;
        Vec3 step = dir * (1.0f / len);
        Vec3 perp = {-step.z, 0, step.x};

        // Central energy beam (thick, bright)
        for (f32 h = 0.1f; h < 1.8f; h += 0.4f) {
            Vec3 s0 = dfx.start + Vec3{0, h, 0};
            Vec3 s1 = dfx.end + Vec3{0, h, 0};
            f32 hAlpha = alpha * (1.0f - h / 2.0f);
            DebugDraw::line(s0, s1, {0.2f * hAlpha, 0.5f * hAlpha, 1.0f * hAlpha});
        }

        // Swirling energy wisps along the corridor
        for (f32 d = 0; d < len; d += 0.15f) {
            f32 t2 = d + dfx.timer * 8.0f;
            f32 wave = sinf(t2 * 4.0f) * 0.25f;
            Vec3 p = dfx.start + step * d + Vec3{0, 0.8f, 0};
            Vec3 wispA = p + perp * (0.35f + wave);
            Vec3 wispB = p - perp * (0.35f + wave);
            f32 flicker = 0.5f + 0.5f * sinf(t2 * 12.0f);
            f32 fade = alpha * flicker * (1.0f - d / len);
            DebugDraw::line(p + Vec3{0, wave * 0.5f, 0}, wispA,
                            {0.3f * fade, 0.6f * fade, 1.0f * fade});
            DebugDraw::line(p + Vec3{0, -wave * 0.5f, 0}, wispB,
                            {0.2f * fade, 0.4f * fade, 0.9f * fade});
        }
    }

    // --- Meteor Strike — descending fire pillar + pulsing rune circle ---
    {
        extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
        for (u32 i = 0; i < MAX_PENDING_METEORS; i++) {
            if (!s_meteors[i].active) continue;
            Vec3 mp = s_meteors[i].position;
            f32 mr = s_meteors[i].radius;
            f32 timer = s_meteors[i].timer;
            f32 urgency = 1.0f - timer; // 0→1 as impact approaches
            f32 pulse = 0.5f + 0.5f * sinf(urgency * 30.0f);

            // Outer targeting rune circle (pulsing, accelerating)
            static constexpr u32 RUNE_SEGS = 20;
            for (u32 s = 0; s < RUNE_SEGS; s++) {
                f32 a0 = static_cast<f32>(s) * (6.28318f / RUNE_SEGS) + urgency * 4.0f;
                f32 a1 = a0 + (6.28318f / RUNE_SEGS);
                Vec3 p0 = mp + Vec3{cosf(a0) * mr, 0.05f, sinf(a0) * mr};
                Vec3 p1 = mp + Vec3{cosf(a1) * mr, 0.05f, sinf(a1) * mr};
                DebugDraw::line(p0, p1, {1.0f * pulse, 0.15f * urgency, 0.05f});
            }

            // Inner rune circle (counter-rotating, smaller)
            f32 innerR = mr * 0.5f;
            for (u32 s = 0; s < RUNE_SEGS; s++) {
                f32 a0 = static_cast<f32>(s) * (6.28318f / RUNE_SEGS) - urgency * 6.0f;
                f32 a1 = a0 + (6.28318f / RUNE_SEGS);
                Vec3 p0 = mp + Vec3{cosf(a0) * innerR, 0.08f, sinf(a0) * innerR};
                Vec3 p1 = mp + Vec3{cosf(a1) * innerR, 0.08f, sinf(a1) * innerR};
                DebugDraw::line(p0, p1, {1.0f, 0.5f * pulse, 0.1f});
            }

            // Rune cross-lines (pentagram-like)
            for (u32 s = 0; s < 5; s++) {
                f32 a = s * (6.28318f / 5.0f) + urgency * 2.0f;
                Vec3 p0 = mp + Vec3{cosf(a) * mr, 0.06f, sinf(a) * mr};
                Vec3 p1 = mp + Vec3{cosf(a + 2.513f) * mr, 0.06f, sinf(a + 2.513f) * mr};
                DebugDraw::line(p0, p1, {0.8f * pulse, 0.2f, 0.05f});
            }

            // Descending fire pillar — gets closer to ground as timer counts down
            f32 pillarTop = 6.0f * (1.0f - urgency) + 0.5f;
            f32 pillarBottom = 0.3f;
            Vec3 pillarCol = {1.0f, 0.4f * pulse, 0.08f};
            for (f32 ox = -0.1f; ox <= 0.1f; ox += 0.1f) {
                DebugDraw::line(mp + Vec3{ox, pillarBottom, 0},
                                mp + Vec3{ox, pillarTop, 0}, pillarCol);
                DebugDraw::line(mp + Vec3{0, pillarBottom, ox},
                                mp + Vec3{0, pillarTop, ox}, pillarCol);
            }

            // Ember particles spiraling down the pillar
            for (u32 ember = 0; ember < 6; ember++) {
                f32 eh = pillarTop * (1.0f - ember * 0.15f);
                f32 ea = urgency * 8.0f + ember * 1.05f;
                f32 er = 0.2f + ember * 0.05f;
                Vec3 ep = mp + Vec3{cosf(ea) * er, eh, sinf(ea) * er};
                Vec3 ep2 = mp + Vec3{cosf(ea + 0.5f) * er * 0.8f, eh - 0.2f, sinf(ea + 0.5f) * er * 0.8f};
                DebugDraw::line(ep, ep2, {1.0f, 0.6f * pulse, 0.1f});
            }
        }
    }
}

// ---------------------------------------------------------------------------
// renderWorldItems — dropped items with mesh normalization + rarity glow lines,
// plus remote player models (multiplayer only)
// ---------------------------------------------------------------------------
void Engine::renderWorldItems(u32 sw, u32 sh) {
    (void)sw; (void)sh; // sw/sh reserved for future use (e.g. distance-based culling)

    const Texture& defaultTex = MaterialSystem::get(0)->texture;

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
            // Legendary items override with glowing legendary material
            if (wi.item.rarity == Rarity::LEGENDARY) {
                const char* legMatName = nullptr;
                switch (def.slot) {
                    case ItemSlot::WEAPON:  legMatName = "legendary_weapon"; break;
                    case ItemSlot::OFFHAND: legMatName = "legendary_shield"; break;
                    case ItemSlot::HELMET:  legMatName = "legendary_helm";   break;
                    case ItemSlot::ARMOR:   legMatName = "legendary_armor";  break;
                    case ItemSlot::BOOTS:   legMatName = "legendary_boots";  break;
                    case ItemSlot::RING:    legMatName = "legendary_ring";   break;
                    default: break;
                }
                if (legMatName) {
                    u8 legId = MaterialSystem::getIdByName(legMatName);
                    const Material* legMat = MaterialSystem::get(legId);
                    if (legMat && legId > 0) {
                        itemTex = legMat->texture;
                        tint = legMat->tint; // legendary uses its own golden tint
                    }
                }
            }
        }

        // Item mesh — normalize size so all items render at consistent visual scale
        Mat4 model;
        if (itemMesh != &m_cubeMesh && !isGlobeItem && wi.item.defId < m_itemDefCount) {
            const ItemDef& idef = m_itemDefs[wi.item.defId];
            if (idef.meshId > 0 && idef.meshId < m_meshDefCount) {
                // Scale mesh so its largest axis fills 0.6 units, then multiply by ITEM_SCALE
                const AABB& mb = m_meshDefs[idef.meshId].bounds;
                f32 maxDim = mb.max.y - mb.min.y;
                f32 mw = mb.max.x - mb.min.x;
                f32 md = mb.max.z - mb.min.z;
                if (mw > maxDim) maxDim = mw;
                if (md > maxDim) maxDim = md;
                f32 normScale = (maxDim > 0.001f) ? (0.6f / maxDim) : 1.0f;
                f32 finalScale = normScale * renderScale;
                Vec3 mc = {(mb.min.x + mb.max.x) * 0.5f,
                           (mb.min.y + mb.max.y) * 0.5f,
                           (mb.min.z + mb.max.z) * 0.5f};
                model = Mat4::translate(pos) * Mat4::rotateY(spin)
                      * Mat4::scale({finalScale, finalScale, finalScale})
                      * Mat4::translate({-mc.x, -mc.y, -mc.z});
            } else {
                model = Mat4::translate(pos) * Mat4::rotateY(spin) * Mat4::scale({renderScale, renderScale, renderScale});
            }
        } else {
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
}

// ---------------------------------------------------------------------------
// renderSpeechBubbles — world-to-screen projection for entity speech text,
// plus the floor-door interaction prompt when the player is in range
// ---------------------------------------------------------------------------
void Engine::renderSpeechBubbles(u32 sw, u32 sh) {
    // Uses the render entity pool (client uses interpolated snapshot, SP uses live pool)
    const EntityPool& speechPool = (m_netRole == NetRole::CLIENT) ? m_renderEntities : m_entities;
    for (u32 a = 0; a < speechPool.activeCount; a++) {
        u32 idx = speechPool.activeList[a];
        const Entity& e = speechPool.entities[idx];
        if (!e.speechText || e.speechTimer <= 0.0f) continue;

        // Project a point above the entity's head into clip space.
        // The mesh is rendered with feet at (position.y - halfExtents.y) and the
        // top at (position.y + halfExtents.y), so the bubble sits 0.5m above that.
        f32 topOfHead = e.position.y + e.halfExtents.y;
        Vec3 headPos = {e.position.x, topOfHead + 0.5f, e.position.z};

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

        // drawSpeechBubble places text starting at y going downward, so shift
        // the screen position up by the bubble height so it appears ABOVE the head
        f32 textH = FontSystem::textHeight(1);
        f32 bubbleH = textH + 8.0f + 6.0f; // text + padding + triangle
        screenY -= bubbleH;

        // Fade alpha in the last second of the timer
        f32 alpha = (e.speechTimer < 1.0f) ? e.speechTimer : 1.0f;

        // Green for allies, red for hostile entities
        Vec3 textColor = (e.flags & ENT_FRIENDLY)
            ? Vec3{0.4f, 1.0f, 0.5f}   // ally green
            : Vec3{1.0f, 0.4f, 0.4f};  // enemy red

        HUD::drawSpeechBubble(sw, sh, screenX, screenY, e.speechText, textColor, alpha);
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
}

// ---------------------------------------------------------------------------
// renderHUD — all 2D HUD elements: inventory screen or normal HUD
// (health bar, crosshair, quickbar, minimap, skill bars, net stats, profiler)
// ---------------------------------------------------------------------------
void Engine::renderHUD(u32 sw, u32 sh) {
    if (m_inventoryOpen) {
        // Inventory screen replaces normal HUD elements
        s32 invMX, invMY;
        Input::getMousePosition(invMX, invMY);
        invMY = static_cast<s32>(sh) - invMY; // flip to HUD coords
        HUD::drawInventoryScreen(sw, sh, m_inventories[m_localPlayerIndex],
                                  m_itemDefs, 0, false, invMX, invMY);

        // Draw dragged item icon at cursor position
        if (isDragActive(m_dragState)) {
            s32 dmx, dmy;
            Input::getMousePosition(dmx, dmy);
            dmy = static_cast<s32>(sh) - dmy;
            if (m_dragState.itemDefId < m_itemDefCount) {
                const ItemDef& dragDef = m_itemDefs[m_dragState.itemDefId];
                // Find the rarity of the dragged item
                Rarity dragRarity = Rarity::COMMON;
                if (m_dragState.source == DragSource::BACKPACK &&
                    m_dragState.sourceIndex < MAX_INVENTORY_ITEMS) {
                    dragRarity = m_inventories[0].backpack[m_dragState.sourceIndex].rarity;
                } else if (m_dragState.source == DragSource::EQUIPMENT &&
                           m_dragState.sourceIndex < static_cast<u8>(ItemSlot::COUNT)) {
                    dragRarity = m_inventories[0].equipped[m_dragState.sourceIndex].rarity;
                }
                ItemIconSystem::drawIcon(sw, sh,
                    static_cast<f32>(dmx) - 16.0f,
                    static_cast<f32>(dmy) - 16.0f,
                    32.0f, dragDef, dragRarity);
            }
        }
    } else {
        Vec3 crossColor = (m_localPlayer.damageFlashTimer > 0.0f)
                        ? Vec3{1.0f, 0.3f, 0.3f}
                        : Vec3{1.0f, 1.0f, 1.0f};
        HUD::drawCrosshair(sw, sh, crossColor);

        if (m_hitMarkerTimer > 0.0f)
            HUD::drawHitMarker(sw, sh, m_hitMarkerTimer / 0.2f);

        HUD::drawHealthBar(sw, sh, m_localPlayer.health, m_localPlayer.maxHealth);

        // Summon portraits — top-left, well below floor/potion text
        {
            f32 portX = 10.0f;
            f32 portY = static_cast<f32>(sh) - 75.0f; // lower position, clear of other HUD
            f32 portH = 26.0f, gap = 3.0f;

            // Scan for summons
            Entity* combatDrone = nullptr;
            u32 swarmCount = 0;
            u32 turretCount = 0;

            for (u32 a = 0; a < m_entities.activeCount; a++) {
                u32 idx = m_entities.activeList[a];
                Entity& ent = m_entities.entities[idx];
                if (!(ent.flags & ENT_FRIENDLY)) continue;
                if (ent.flags & ENT_DEAD) continue;
                if (ent.npcClass != NpcClass::NONE) continue;

                if (ent.enemyType == EnemyType::SPIDER && ent.moveSpeed > 0.0f) {
                    combatDrone = &ent;
                } else if (ent.flags & ENT_UNTARGETABLE) {
                    swarmCount++;
                } else if (ent.moveSpeed <= 0.0f) {
                    turretCount++;
                }
            }

            u32 slot = 0;

            u8 matDrone  = MaterialSystem::getIdByName("icon_drone");
            u8 matSwarm  = MaterialSystem::getIdByName("icon_swarm");
            u8 matTurret = MaterialSystem::getIdByName("icon_turret");

            if (combatDrone) {
                f32 hpFrac = combatDrone->health / combatDrone->maxHealth;
                HUD::drawSummonPortrait(sw, sh, portX, portY - slot * (portH + gap),
                                         "Drone", {0.35f, 0.33f, 0.4f}, hpFrac, 1, matDrone);
                slot++;
            }
            if (swarmCount > 0) {
                HUD::drawSummonPortrait(sw, sh, portX, portY - slot * (portH + gap),
                                         "Swarm", {0.3f, 0.3f, 0.35f}, -1.0f, swarmCount, matSwarm);
                slot++;
            }
            if (turretCount > 0) {
                HUD::drawSummonPortrait(sw, sh, portX, portY - slot * (portH + gap),
                                         "Turret", {0.33f, 0.31f, 0.37f}, -1.0f, turretCount, matTurret);
                slot++;
            }
        }

        HUD::drawWeaponIndicator(sw, sh, m_quickbars[m_localPlayerIndex].activeSlot);

        // Energy bar
        HUD::drawEnergyBar(sw, sh, m_skillStates[0].energy, m_skillStates[0].maxEnergy);

        // Class skill bar — 4 slots at bottom-center
        {
            const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
            f32 slotW = 36.0f, slotH = 36.0f, gap = 4.0f;
            f32 totalW = 4 * slotW + 3 * gap;
            f32 barX = (static_cast<f32>(sw) - totalW) * 0.5f;
            f32 barY = 10.0f;

            for (u8 s = 0; s < 4; s++) {
                f32 x = barX + s * (slotW + gap);
                bool unlocked = (m_currentFloor >= cls.skillUnlockFloor[s]);
                bool selected = (s == m_activeClassSkill);
                bool upgraded = (m_currentFloor >= cls.skillUpgradeFloor[s]);

                // Background
                Vec3 bgCol = unlocked ? Vec3{0.15f, 0.15f, 0.2f} : Vec3{0.08f, 0.08f, 0.1f};
                if (selected && unlocked) bgCol = {0.2f, 0.25f, 0.35f};
                for (f32 fy = 0; fy < slotH; fy += 1.0f) {
                    DebugDraw::line({x, barY + fy, 0}, {x + slotW, barY + fy, 0}, bgCol);
                }

                // Border
                Vec3 borderCol = selected ? Vec3{0.4f, 0.9f, 0.5f} : Vec3{0.3f, 0.3f, 0.4f};
                if (!unlocked) borderCol = {0.15f, 0.15f, 0.2f};
                if (upgraded) borderCol = {0.9f, 0.8f, 0.3f}; // gold for upgraded
                DebugDraw::line({x, barY, 0}, {x + slotW, barY, 0}, borderCol);
                DebugDraw::line({x + slotW, barY, 0}, {x + slotW, barY + slotH, 0}, borderCol);
                DebugDraw::line({x + slotW, barY + slotH, 0}, {x, barY + slotH, 0}, borderCol);
                DebugDraw::line({x, barY + slotH, 0}, {x, barY, 0}, borderCol);

                // Slot number
                char num[2] = {static_cast<char>('1' + s), 0};
                Vec3 numCol = unlocked ? Vec3{0.8f, 0.8f, 0.8f} : Vec3{0.3f, 0.3f, 0.3f};
                FontSystem::drawText(sw, sh, x + 2.0f, barY + slotH - 12.0f, num, numCol, 1);

                // Cooldown overlay
                if (unlocked && m_classSkillStates[s].cooldownTimer > 0.0f) {
                    f32 cdFrac = m_classSkillStates[s].cooldownTimer / 2.0f; // rough normalization
                    if (cdFrac > 1.0f) cdFrac = 1.0f;
                    f32 cdH = slotH * cdFrac;
                    Vec3 cdCol = {0.1f, 0.1f, 0.15f};
                    for (f32 fy = 0; fy < cdH; fy += 1.0f) {
                        DebugDraw::line({x + 1, barY + fy + 1, 0}, {x + slotW - 1, barY + fy + 1, 0}, cdCol);
                    }
                }

                // "LOCKED" text for unavailable skills
                if (!unlocked) {
                    char lockTxt[8];
                    std::snprintf(lockTxt, sizeof(lockTxt), "F%u", cls.skillUnlockFloor[s]);
                    FontSystem::drawText(sw, sh, x + 8.0f, barY + 12.0f, lockTxt, {0.4f, 0.3f, 0.3f}, 1);
                }
            }
        }

        // Active skill display — right side of screen, shows current right-click skill name
        {
            const ClassDef& cls = kClassDefs[static_cast<u32>(m_playerClass)];
            u8 slot = m_activeClassSkill;
            bool unlocked = (m_currentFloor >= cls.skillUnlockFloor[slot]);
            const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, cls.skills[slot]);

            f32 rmbX = static_cast<f32>(sw) - 160.0f;
            f32 rmbY = 15.0f;

            // Background box
            Vec3 bgCol = unlocked ? Vec3{0.12f, 0.12f, 0.18f} : Vec3{0.06f, 0.06f, 0.08f};
            for (f32 fy = 0; fy < 30.0f; fy += 1.0f) {
                DebugDraw::line({rmbX, rmbY + fy, 0}, {rmbX + 150.0f, rmbY + fy, 0}, bgCol);
            }
            // Border
            Vec3 bCol = unlocked ? Vec3{0.3f, 0.7f, 0.4f} : Vec3{0.2f, 0.2f, 0.25f};
            if (m_classSkillStates[slot].cooldownTimer > 0.0f) bCol = {0.5f, 0.3f, 0.2f};
            DebugDraw::line({rmbX, rmbY, 0}, {rmbX + 150, rmbY, 0}, bCol);
            DebugDraw::line({rmbX + 150, rmbY, 0}, {rmbX + 150, rmbY + 30, 0}, bCol);
            DebugDraw::line({rmbX + 150, rmbY + 30, 0}, {rmbX, rmbY + 30, 0}, bCol);
            DebugDraw::line({rmbX, rmbY + 30, 0}, {rmbX, rmbY, 0}, bCol);

            // "RMB" label
            FontSystem::drawText(sw, sh, rmbX + 4, rmbY + 17, "RMB", {0.5f, 0.5f, 0.6f}, 1);

            // Skill name
            const char* skillName = sd ? sd->name : "???";
            Vec3 nameCol = unlocked ? Vec3{0.9f, 0.9f, 1.0f} : Vec3{0.4f, 0.4f, 0.4f};
            if (m_classSkillStates[slot].cooldownTimer > 0.0f) nameCol = {0.6f, 0.4f, 0.3f};
            FontSystem::drawText(sw, sh, rmbX + 35, rmbY + 17, skillName, nameCol, 1);

            // Cooldown text
            if (m_classSkillStates[slot].cooldownTimer > 0.0f) {
                char cdTxt[8];
                std::snprintf(cdTxt, sizeof(cdTxt), "%.1fs", m_classSkillStates[slot].cooldownTimer);
                FontSystem::drawText(sw, sh, rmbX + 35, rmbY + 5, cdTxt, {1.0f, 0.5f, 0.2f}, 1);
            }
        }

        // Minimap (top-right corner)
        Minimap::draw(sw, sh, m_grid, m_localPlayer.position, m_localPlayer.yaw, m_entities);

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

    // Backpack full notification — shown centered at 70% screen height, fades out
    if (m_fullBackpackNotifyTimer > 0.0f) {
        const char* fullText = "Backpack Full!";
        f32 fullW = FontSystem::textWidth(fullText, 2);
        f32 alpha = (m_fullBackpackNotifyTimer < 0.5f) ? m_fullBackpackNotifyTimer * 2.0f : 1.0f;
        Vec3 fullColor = {0.9f * alpha, 0.2f * alpha, 0.2f * alpha};
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - fullW) * 0.5f,
                             static_cast<f32>(sh) * 0.7f, fullText, fullColor, 2);
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

    // Chat log — left side of screen, above the quickbar
    {
        f32 chatX = 15.0f;
        f32 chatY = 80.0f; // above quickbar (which is at Y=20)
        for (u32 i = 0; i < MAX_CHAT_LINES; i++) {
            if (m_chatLog[i].timer <= 0.0f || m_chatLog[i].text[0] == '\0') continue;
            f32 alpha = (m_chatLog[i].timer < 2.0f) ? m_chatLog[i].timer * 0.5f : 1.0f;
            Vec3 col = m_chatLog[i].color * alpha;
            f32 lineY = chatY + static_cast<f32>(i) * 12.0f;
            FontSystem::drawText(sw, sh, chatX, lineY, m_chatLog[i].text, col, 1);
        }
    }
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

    if (m_menuSubState == 1) {
        // Single player sub-menu — replaces main menu options
        const char* subTitle = "Single Player";
        f32 stW = FontSystem::textWidth(subTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.55f, subTitle, {0.2f, 0.9f, 0.2f}, 2);

        bool hasSave = false;
        { FILE* f = std::fopen("save.dat", "rb"); if (f) { hasSave = true; std::fclose(f); } }

        static const char* subLabels[] = {"New Game", "Continue"};
        for (u32 i = 0; i < 2; i++) {
            f32 y = sh * 0.38f + (1 - i) * 50.0f;
            bool sel = (i == m_menuSubSelection);
            bool available = (i == 0) || hasSave;
            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.4f, 0.2f};
            if (!available) col = {0.2f, 0.2f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 250, 35, col, sel && available);
            Vec3 tc = available ? (sel ? Vec3{1,1,1} : Vec3{0.6f,0.6f,0.6f}) : Vec3{0.35f,0.35f,0.35f};
            f32 tw = FontSystem::textWidth(subLabels[i], 2);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 10.0f, subLabels[i], tc, 2);
        }

        const char* hint = "Up/Down, Enter to confirm, ESC to go back";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.15f, hint, {0.4f, 0.4f, 0.5f}, 1);
    } else if (m_menuSubState == 2) {
        // Class selection screen
        const char* subTitle = "Choose Your Class";
        f32 stW = FontSystem::textWidth(subTitle, 2);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - stW) * 0.5f, sh * 0.58f,
                             subTitle, {0.9f, 0.8f, 0.3f}, 2);

        u8 classCount = static_cast<u8>(PlayerClass::CLASS_COUNT);
        f32 listTop = sh * 0.50f;
        f32 spacing = 28.0f;

        for (u8 i = 0; i < classCount; i++) {
            const ClassDef& cls = kClassDefs[i];
            f32 y = listTop - i * spacing;
            bool sel = (i == m_menuSubSelection);

            Vec3 col = sel ? Vec3{0.3f, 1.0f, 0.4f} : Vec3{0.15f, 0.35f, 0.2f};
            HUD::drawMenuOption(sw, sh, y, 300, 24, col, sel);

            Vec3 tc = sel ? Vec3{1, 1, 1} : Vec3{0.55f, 0.55f, 0.55f};
            char label[64];
            std::snprintf(label, sizeof(label), "%s  (%.0f HP, %.0f EN)", cls.name, cls.baseHealth, cls.baseEnergy);
            f32 tw = FontSystem::textWidth(label, 1);
            FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - tw) * 0.5f, y + 7.0f, label, tc, 1);
        }

        // Show selected class description and skills
        if (m_menuSubSelection < classCount) {
            const ClassDef& sel = kClassDefs[m_menuSubSelection];
            f32 descY = sh * 0.50f - classCount * spacing - 20.0f;

            FontSystem::drawText(sw, sh, sw * 0.25f, descY, sel.description, {0.7f, 0.7f, 0.8f}, 1);
            descY -= 18.0f;

            char statLine[80];
            std::snprintf(statLine, sizeof(statLine), "HP: %.0f  Speed: %.1f  Energy: %.0f  Weapon: %s",
                          sel.baseHealth, sel.baseMoveSpeed, sel.baseEnergy, sel.startingWeaponName);
            FontSystem::drawText(sw, sh, sw * 0.25f, descY, statLine, {0.6f, 0.8f, 0.6f}, 1);
        }

        const char* hint2 = "Up/Down to select, Enter to confirm, ESC to go back";
        f32 hintW2 = FontSystem::textWidth(hint2, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW2) * 0.5f, sh * 0.08f, hint2, {0.4f, 0.4f, 0.5f}, 1);
    } else {
        // Main menu options
        static const char* labels[] = {"Single Player", "Host Game", "Join Game", "Exit Game"};
        Vec3 colors[] = {
            {0.2f, 0.9f, 0.2f},
            {0.2f, 0.5f, 1.0f},
            {1.0f, 0.7f, 0.2f},
            {0.7f, 0.2f, 0.2f},
        };

        for (u32 i = 0; i < 4; i++) {
            f32 y = sh * 0.25f + (3 - i) * 50.0f;
            Vec3 color = colors[i];
            bool selected = (i == m_menuSelection);
            if (!selected) color = color * 0.4f;
            HUD::drawMenuOption(sw, sh, y, 250, 35, color, selected);

            f32 textW = FontSystem::textWidth(labels[i], 2);
            f32 textX = (static_cast<f32>(sw) - textW) * 0.5f;
            FontSystem::drawText(sw, sh, textX, y + 10.0f, labels[i],
                selected ? Vec3{1,1,1} : Vec3{0.6f,0.6f,0.6f}, 2);
        }

        const char* hint = "Up/Down to select, Enter to confirm";
        f32 hintW = FontSystem::textWidth(hint, 1);
        FontSystem::drawText(sw, sh, (static_cast<f32>(sw) - hintW) * 0.5f, sh * 0.15f, hint, {0.4f, 0.4f, 0.5f}, 1);
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
