// Engine module — see engine.h for class definition.
// Split from engine.cpp for manageability. All methods are Engine:: members.

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "audio/audio.h"

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
#include "renderer/projectile_renderer.h"
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
// Player class definitions — 8 classes with 4 skills each
// ---------------------------------------------------------------------------
const ClassDef kClassDefs[static_cast<u32>(PlayerClass::CLASS_COUNT)] = {
    // WARRIOR — Melee Tank
    {"Warrior", "Heavy melee fighter with crowd control",
     150.0f, 5.0f, 80.0f, "Iron Sword",
     {SkillId::THUNDERCLAP, SkillId::WAR_CRY, SkillId::WHIRLWIND, SkillId::EARTHQUAKE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::MELEE},

    // RANGER — Ranged DPS
    {"Ranger", "Agile archer with poison and piercing shots",
     80.0f, 6.5f, 100.0f, "Short Bow",
     {SkillId::MULTI_SHOT, SkillId::RAIN_OF_ARROWS, SkillId::POISON_ARROW, SkillId::SHADOW_SHOT},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::PROJECTILE},

    // SORCERER — Glass Cannon
    {"Sorcerer", "Devastating elemental magic, fragile body",
     70.0f, 5.5f, 150.0f, "Wand of Sparks",
     {SkillId::FIREBALL, SkillId::FROZEN_ORB, SkillId::CHAIN_LIGHTNING, SkillId::METEOR_STRIKE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::PROJECTILE},

    // ROGUE — Hit-and-Run
    {"Rogue", "Fast assassin with teleports and poison",
     85.0f, 7.0f, 100.0f, "Rusty Dagger",
     {SkillId::KNIFE_BURST, SkillId::PHASE_DASH, SkillId::POISON_CLOUD, SkillId::SHADOW_STRIKE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::MELEE},

    // PALADIN — Wrathful Holy Warrior
    {"Paladin", "Wrathful holy warrior raining judgment",
     130.0f, 5.0f, 90.0f, "Iron Sword",
     {SkillId::HOLY_SMITE, SkillId::HOLY_BOMBARDMENT, SkillId::HOLY_NOVA, SkillId::DIVINE_JUDGMENT},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::MELEE},

    // COMBAT ENGINEER — Gadget Specialist
    {"Combat Engineer", "Turrets, tesla coils, and gadgets",
     100.0f, 5.5f, 120.0f, "Pistol",
     {SkillId::SHOCK_BOLT, SkillId::DEPLOY_TURRET, SkillId::TESLA_COIL, SkillId::MECH_OVERDRIVE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::HITSCAN},

    // MARKSMAN — Precision Sniper
    {"Marksman", "Anti-materiel sniper with devastating penetrating shots",
     75.0f, 6.0f, 100.0f, "Revolver",
     {SkillId::AIMED_SHOT, SkillId::EXPLOSIVE_ROUND, SkillId::OVERCHARGED_MAGAZINE, SkillId::HEADSHOT},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::HITSCAN},

    // TINKERER — Minion Master
    {"Tinkerer", "Drone commander with stun grenades",
     90.0f, 5.5f, 110.0f, "Pistol",
     {SkillId::COMBAT_DRONE, SkillId::SWARM_DRONES, SkillId::STUN_GRENADE, SkillId::DEPLOY_TURRET},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::HITSCAN},
};


// ---------------------------------------------------------------------------
// Engine lifecycle
// ---------------------------------------------------------------------------
void Engine::init() {
    s_engine = this;

    Log::init();
    LOG_INFO("Engine initializing...");

    if (!Window::init("Curse of the Dungeon Engine", 1280, 720)) {
        LOG_ERROR("Failed to initialize window");
        return;
    }

    AudioSystem::init(); // non-fatal — game works without audio
    AudioSystem::setMusicVolume(0.3f); // ambient music sits below SFX

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
    // Font UI scale is set dynamically per-frame based on viewport height
    // (see renderHUD). No fixed scale needed here.
    ItemIconSystem::init();
    // NOTE: LimbSystem::init is called later, after OBJ meshes are loaded

    // Shaders
    m_basicShader = ShaderSystem::load(ASSET_PATH("assets/shaders/basic.vert"),
                                       ASSET_PATH("assets/shaders/basic.frag"));
    m_unlitShader = ShaderSystem::load(ASSET_PATH("assets/shaders/unlit.vert"),
                                       ASSET_PATH("assets/shaders/unlit.frag"));

    // Materials (loads textures from assets/materials.json)
    MaterialSystem::init(ASSET_PATH("assets/materials.json"));

    // Meshes
    m_cubeMesh = MeshSystem::createCube();
    m_quadMesh = MeshSystem::createQuad();

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
            {"arrow",          "assets/meshes/arrow.obj"},
            {"bolt",           "assets/meshes/bolt.obj"},
            {"skeleton_arm",   "assets/meshes/skeleton_arm.obj"},
            {"skeleton_leg",   "assets/meshes/skeleton_leg.obj"},
            {"bat_wing_mesh",  "assets/meshes/bat_wing_mesh.obj"},
            {"butcher_arm",    "assets/meshes/butcher_arm.obj"},
            {"butcher_leg",    "assets/meshes/butcher_leg.obj"},
            {"bat_foot",       "assets/meshes/bat_foot.obj"},
            {"andariel",       "assets/meshes/andariel.obj"},
            {"spider_leg_pair","assets/meshes/spider_leg_pair.obj"},
            {"claymore",       "assets/meshes/claymore.obj"},
            {"turret",         "assets/meshes/turret.obj"},
            {"gargoyle",       "assets/meshes/gargoyle.obj"},
            {"necromancer",    "assets/meshes/necromancer.obj"},
            {"shaman",         "assets/meshes/shaman.obj"},
            {"herald",         "assets/meshes/herald.obj"},
        };
        for (auto& entry : kMeshes) {
            if (m_meshDefCount >= MAX_MESH_DEFS) break;
            AABB bounds;
            Mesh mesh = ObjLoader::load(ASSET_PATH(entry.path), &bounds);
            if (mesh.vao != 0) {
                // Resolve usemtl material names → IDs now that MaterialSystem is initialised.
                // MaterialSystem::init() runs before this loop so getIdByName is safe to call.
                for (u8 g = 0; g < mesh.materialGroupCount; g++) {
                    mesh.materials[g].materialId =
                        MaterialSystem::getIdByName(mesh.materials[g].materialName);
                }
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
    // Override box limb meshes with OBJ voxel limbs for better visuals
    LimbSystem::setObjMeshIds(
        findMeshByName("skeleton_arm"),
        findMeshByName("skeleton_leg"),
        findMeshByName("bat_wing_mesh"),
        findMeshByName("butcher_arm"),
        findMeshByName("butcher_leg"),
        findMeshByName("bat_foot"),
        findMeshByName("spider_leg_pair")
    );

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
    m_meshIdArrow      = findMeshByName("arrow");
    m_meshIdBolt       = findMeshByName("bolt");
    m_matIdBatWing     = MaterialSystem::getIdByName("bat_wing");

    // Weapons
    initWeaponTable(m_weaponDefs, m_weaponDefCount);

    // Item/loot system — log warnings and zero out tables rather than crashing on bad JSON
    if (!ItemLoader::loadItemDefs(ASSET_PATH("assets/config/items.json"), m_itemDefs, m_itemDefCount)) {
        LOG_WARN("Failed to load item defs — using empty table");
        m_itemDefCount = 0;
    }
    if (!ItemLoader::loadAffixDefs(ASSET_PATH("assets/config/affixes.json"), m_affixDefs, m_affixDefCount)) {
        LOG_WARN("Failed to load affix defs — using empty table");
        m_affixDefCount = 0;
    }
    if (!ItemLoader::loadSkillDefs(ASSET_PATH("assets/config/skills.json"), m_skillDefs, m_skillDefCount)) {
        LOG_WARN("Failed to load skill defs — using empty table");
        m_skillDefCount = 0;
    }
    SkillSystem::init();

    // Register skill visual FX callbacks
    SkillSystem::setNovaCallback([](Vec3 position, f32 radius, Vec3 color) {
        if (!s_engine) return;
        for (u32 i = 0; i < MAX_NOVA_FX; i++) {
            if (!s_engine->m_fx.novaFX[i].active) {
                s_engine->m_fx.novaFX[i] = {position, radius, 0.6f, true, color};
                return;
            }
        }
    });
    SkillSystem::setDashCallback([](Vec3 start, Vec3 end) {
        if (!s_engine) return;
        for (u32 i = 0; i < MAX_DASH_FX; i++) {
            if (!s_engine->m_fx.dashFX[i].active) {
                s_engine->m_fx.dashFX[i] = {start, end, 0.5f, true};
                return;
            }
        }
    });
    SkillSystem::setBeamCallback([](Vec3 start, Vec3 end, Vec3 color) {
        if (!s_engine) return;
        for (u32 i = 0; i < MAX_BEAM_FX; i++) {
            if (!s_engine->m_fx.beamFX[i].active) {
                s_engine->m_fx.beamFX[i] = {start, end, color, 0.3f, true};
                return;
            }
        }
    });
    SkillSystem::setScorchCallback([](Vec3 position, f32 radius, f32 duration, f32 dps) {
        if (!s_engine) return;
        for (u32 i = 0; i < MAX_SCORCH; i++) {
            if (!s_engine->m_fx.scorchZones[i].active) {
                s_engine->m_fx.scorchZones[i] = {position, radius, duration, dps, true};
                return;
            }
        }
        // Also spawn a fire FX for the visual
        for (u32 i = 0; i < MAX_FIRE_FX; i++) {
            if (!s_engine->m_fx.fireFX[i].active) {
                s_engine->m_fx.fireFX[i] = {position, radius, duration, true};
                return;
            }
        }
    });
    SkillSystem::setChainCallback([](const Vec3* points, u8 count) {
        if (!s_engine || count < 2) return;
        for (u32 i = 0; i < Engine::MAX_CHAIN_FX; i++) {
            if (!s_engine->m_fx.chainFX[i].active) {
                Engine::ChainFX& fx = s_engine->m_fx.chainFX[i];
                fx.pointCount = (count > Engine::MAX_CHAIN_POINTS) ? Engine::MAX_CHAIN_POINTS : count;
                for (u8 p = 0; p < fx.pointCount; p++) fx.points[p] = points[p];
                fx.timer = 0.8f;
                fx.active = true;
                return;
            }
        }
    });

    // Set bolt mesh/material IDs for shock bolt projectiles
    SkillSystem::setBoltMeshId(findMeshByName("bolt"), MaterialSystem::getIdByName("shock_bolt"));

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

    // Instanced projectile renderer — batches projectiles by mesh for minimal draw calls
    ProjectileRenderer::init();

    // Particle system init — must come after MaterialSystem::init so mat IDs are valid
    ParticleSystem::init(m_particles);
    m_particleBlobMatId  = MaterialSystem::getIdByName("particle_blob");
    m_particleSparkMatId = MaterialSystem::getIdByName("particle_spark");

    // Wire particle pool and screen shake into combat, skill, and projectile systems
    Combat::setFXTargets(&m_particles, &m_camera.shake);
    SkillSystem::setFXTargets(&m_particles, &m_camera.shake);
    extern void ProjectileSystem_setTrailPool(ParticlePool* pool);
    ProjectileSystem_setTrailPool(&m_particles);

    // Apply energy_flat affix bonus to SkillState.maxEnergy on equip/unequip
    Inventory::setStatsChangedCallback([](PlayerInventory& inv) {
        if (!s_engine) return;
        SkillState& ss = s_engine->m_skillStates[s_engine->m_localPlayerIndex];
        f32 baseEnergy = kClassDefs[static_cast<u32>(s_engine->m_playerClass)].baseEnergy;
        f32 oldMax = ss.maxEnergy;
        ss.maxEnergy = baseEnergy + inv.bonusEnergyFlat;
        // Preserve energy percentage so equipping doesn't drain/fill
        if (oldMax > 0.0f) ss.energy = ss.energy * (ss.maxEnergy / oldMax);
        if (ss.energy > ss.maxEnergy) ss.energy = ss.maxEnergy;
    });

    Combat::setDamageNumberCallback([](Vec3 pos, f32 amount) {
        if (!s_engine) return;
        s_engine->spawnDamageNumber(pos, amount);
    });

    Combat::setDeathCallback([](EntityPool& pool, u16 entityIndex, Vec3 position) {
        if (!s_engine) return;

        // Remove from squad so roles are reassigned before next AI tick
        SquadSystem::onMemberDeath(s_engine->m_level.squads, entityIndex, pool);

        // Friendly NPC death speech — set before loot drop so it's visible
        if (pool.entities[entityIndex].flags & ENT_FRIENDLY) {
            pool.entities[entityIndex].speechText = "Avenge... me...";
            pool.entities[entityIndex].speechTimer = 4.0f;
        }

        // Track hostile kills for floor transition screen
        if (!(pool.entities[entityIndex].flags & ENT_FRIENDLY)) {
            s_engine->m_transition.floorKillCount++;
            AudioSystem::playAt(SfxId::ENEMY_DEATH, position, s_engine->m_localPlayer.position);
        }

        // Floors 1-3: first hostile kill guarantees a magic (green) quality drop
        if (!s_firstKillDropGiven && s_engine->m_level.currentFloor <= 3 &&
            !(pool.entities[entityIndex].flags & ENT_FRIENDLY)) {
            s_firstKillDropGiven = true;
            u8 lvl = pool.entities[entityIndex].level;
            if (lvl < 1) lvl = 1;
            ItemInstance item = ItemGen::rollItem(lvl, s_engine->m_itemDefs,
                                                   s_engine->m_itemDefCount,
                                                   s_engine->m_affixDefs,
                                                   s_engine->m_affixDefCount);
            if (!isItemEmpty(item)) {
                // Force to at least MAGIC rarity
                if (item.rarity < Rarity::MAGIC) item.rarity = Rarity::MAGIC;
                // Re-roll affixes for magic quality (1-2 affixes)
                if (item.affixCount == 0) {
                    ItemGen::rollAffixes(item, lvl, s_engine->m_itemDefs[item.defId].slot,
                                          s_engine->m_affixDefs, s_engine->m_affixDefCount);
                }
                WorldItemSystem::spawn(s_engine->m_worldItems, item,
                                       position + Vec3{0, 0.5f, 0});
            }
            return; // skip normal drop logic for this kill
        }

        // Hostile enemies only drop loot; chance scales with floor depth
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

            // Chance to drop a globe (restores both HP and energy on pickup)
            if ((std::rand() % 100) < static_cast<int>(GameConst::GLOBE_DROP_CHANCE * 100.0f)) {
                ItemInstance globe;
                globe.defId = GLOBE_HEALTH_ID; // single globe type
                globe.uid   = s_engine->m_worldItems.nextUid++;
                WorldItemSystem::spawn(s_engine->m_worldItems, globe,
                                       position + Vec3{0.2f, 0.5f, 0.0f});
            }
        }

        // --- Ring on-kill passives ---
        if (s_engine->m_ringPassive != SkillId::NONE && !(pool.entities[entityIndex].flags & ENT_FRIENDLY)) {
            // Soul Harvest: +5% speed, +3% damage per stack for 10s (max 5)
            if (s_engine->m_ringPassive == SkillId::SOUL_HARVEST) {
                Player& p = s_engine->m_localPlayer;
                if (p.soulHarvestStacks < 5) p.soulHarvestStacks++;
                p.soulHarvestTimer = 5.0f; // 5s window to get next kill or stacks reset
                // Speed bonus applied via moveSpeed multiplier
            }
            // Phase Strike (Shadow Ring): 20% on kill, drop smoke bomb — 0.5s stealth + aggro reset
            if (s_engine->m_ringPassive == SkillId::PHASE_STRIKE && (std::rand() % 100) < 20) {
                Player& p = s_engine->m_localPlayer;
                p.smokeTimer = 0.5f;
                // Reset aggro on all enemies within 6m
                for (u32 si = 0; si < MAX_ENTITIES; si++) {
                    Entity& se = pool.entities[si];
                    if (!(se.flags & ENT_ACTIVE) || (se.flags & ENT_DEAD)) continue;
                    if (se.flags & ENT_FRIENDLY) continue;
                    Vec3 diff = se.position - position;
                    if (diff.x * diff.x + diff.z * diff.z < 6.0f * 6.0f) {
                        se.aiState = AIState::IDLE;
                        se.velocity = {0, 0, 0};
                    }
                }
                // Dark grey smoke nova at kill position
                for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                    if (!s_engine->m_fx.novaFX[ni].active) {
                        s_engine->m_fx.novaFX[ni] = {position, 4.0f, 1.2f, true, {0.3f, 0.3f, 0.35f}};
                        break;
                    }
                }
            }
            // Void Kill: 15% chance to spawn void zone on corpse
            if (s_engine->m_ringPassive == SkillId::VOID_KILL && (std::rand() % 100) < 15) {
                // AoE void damage to nearby enemies
                EntityHandle aoeHits[MAX_ENTITIES];
                f32 aoeDists[MAX_ENTITIES];
                u32 aoeCount = CombatQuery::queryConeSorted(
                    pool, position, {0,-1,0}, -1.0f, 3.0f,
                    aoeHits, aoeDists, MAX_ENTITIES);
                for (u32 h = 0; h < aoeCount; h++) {
                    Entity* ve = handleGet(pool, aoeHits[h]);
                    if (!ve || (ve->flags & ENT_DEAD) || (ve->flags & ENT_FRIENDLY)) continue;
                    f32 missingHp = ve->maxHealth - ve->health;
                    Combat::applyDamage(pool, aoeHits[h], 10.0f + missingHp * 0.6f);
                }
                // Dark purple nova visual
                for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                    if (!s_engine->m_fx.novaFX[ni].active) {
                        s_engine->m_fx.novaFX[ni] = {position, 3.0f, 1.0f, true, {0.4f, 0.1f, 0.6f}};
                        break;
                    }
                }
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
        if (LevelGridSystem::worldToGrid(s_engine->m_level.grid, position, gx, gz) &&
            !LevelGridSystem::isSolid(s_engine->m_level.grid, gx, gz)) {
            fxPos.y = LevelGridSystem::getFloorHeight(s_engine->m_level.grid, gx, gz) + 0.1f;
        }
        for (u32 i = 0; i < Engine::MAX_FIRE_FX; i++) {
            if (!s_engine->m_fx.fireFX[i].active) {
                s_engine->m_fx.fireFX[i] = {fxPos, radius, 1.0f, true};
                break;
            }
        }
        // Fireball splash particles — fiery burst at impact point
        ParticleSystem::spawnExplosion(s_engine->m_particles, position, radius);
        s_engine->m_camera.shake.trigger(0.06f, 0.3f);
    });

    // Projectile hit callback — triggers weapon on-hit procs for projectile weapons
    ProjectileSystem::setHitCallback([](Vec3 position, EntityHandle target) {
        if (!s_engine) return;
        if (s_engine->m_weaponProc == SkillId::NONE) return;

        u32 procRoll = static_cast<u32>(std::rand()) % 100;
        u32 procChance = 20;
        if (s_engine->m_weaponProc == SkillId::VOID_ZONE)       procChance = 5;
        if (s_engine->m_weaponProc == SkillId::FROZEN_ORB)      procChance = 15;
        if (s_engine->m_weaponProc == SkillId::CHAIN_LIGHTNING)  procChance = 25;
        if (s_engine->m_weaponProc == SkillId::METEOR_STRIKE)    procChance = 10;
        if (s_engine->m_weaponProc == SkillId::BLOOD_NOVA)       procChance = 20;
        if (s_engine->m_weaponProc == SkillId::SHADOW_RICOCHET)  procChance = 30;

        LOG_INFO("Projectile hit: weaponProc=%u, roll=%u/%u",
                 static_cast<u32>(s_engine->m_weaponProc), procRoll, procChance);

        if (procRoll >= procChance) return;

        const SkillDef* sd = SkillSystem::findSkillDef(s_engine->m_skillDefs,
                                                         s_engine->m_skillDefCount,
                                                         s_engine->m_weaponProc);
        if (!sd) { LOG_WARN("  Proc skill def not found!"); return; }

        LOG_INFO("WEAPON PROC triggered! skill=%u at (%.1f, %.1f, %.1f)",
                 static_cast<u32>(s_engine->m_weaponProc), position.x, position.y, position.z);

        switch (s_engine->m_weaponProc) {
            case SkillId::VOID_ZONE: {
                // AoE void zone — hits all enemies in radius with flat + 60% missing HP
                EntityHandle aoeHits[MAX_ENTITIES];
                f32 aoeDists[MAX_ENTITIES];
                u32 aoeCount = CombatQuery::queryConeSorted(
                    s_engine->m_entities, position, {0,-1,0}, -1.0f, sd->radius,
                    aoeHits, aoeDists, MAX_ENTITIES);
                for (u32 h = 0; h < aoeCount; h++) {
                    Entity* ve = handleGet(s_engine->m_entities, aoeHits[h]);
                    if (!ve || (ve->flags & ENT_DEAD) || (ve->flags & ENT_FRIENDLY)) continue;
                    f32 missingHp = ve->maxHealth - ve->health;
                    f32 voidDmg = sd->damage + missingHp * 0.6f;
                    Combat::applyDamage(s_engine->m_entities, aoeHits[h], voidDmg);
                }
                LOG_INFO("  VOID ZONE AoE: hit %u enemies", aoeCount);
                // Large dark-purple nova + scorch zone for visibility
                for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                    if (!s_engine->m_fx.novaFX[ni].active) {
                        s_engine->m_fx.novaFX[ni] = {position, 3.0f, 1.2f, true, {0.4f, 0.1f, 0.6f}};
                        break;
                    }
                }
                // Also spawn a dark scorch zone on the ground
                for (u32 si = 0; si < Engine::MAX_SCORCH; si++) {
                    if (!s_engine->m_fx.scorchZones[si].active) {
                        s_engine->m_fx.scorchZones[si] = {position, 3.0f, 1.5f, 0.0f, true};
                        break;
                    }
                }
            } break;
            case SkillId::FROZEN_ORB: {
                Vec3 dir = s_engine->m_localPlayer.forward;
                u16 orbIdx = ProjectileSystem::spawn(s_engine->m_projectiles, position, dir,
                    sd->projectileSpeed, sd->damage, sd->radius, sd->duration, true);
                if (orbIdx != 0xFFFF) s_engine->m_projectiles.projectiles[orbIdx].projFlags = PROJ_ORB;
            } break;
            case SkillId::CHAIN_LIGHTNING: {
                SkillState tempSS;
                tempSS.activeSkill = SkillId::CHAIN_LIGHTNING;
                tempSS.energy = 999.0f; tempSS.maxEnergy = 999.0f;
                SkillSystem::tryActivate(tempSS, s_engine->m_skillDefs, s_engine->m_skillDefCount,
                    position, s_engine->m_localPlayer.forward, 0,
                    s_engine->m_projectiles, s_engine->m_entities,
                    s_engine->m_level.grid, s_engine->m_localPlayer);
            } break;
            case SkillId::BLOOD_NOVA: {
                EntityHandle hits[MAX_ENTITIES];
                f32 dists[MAX_ENTITIES];
                u32 hitCount = CombatQuery::queryConeSorted(
                    s_engine->m_entities, position, {0,0,-1}, -1.0f, sd->radius,
                    hits, dists, MAX_ENTITIES);
                for (u32 h = 0; h < hitCount; h++)
                    Combat::applyDamage(s_engine->m_entities, hits[h], sd->damage * 0.5f);
                for (u32 ni = 0; ni < Engine::MAX_NOVA_FX; ni++) {
                    if (!s_engine->m_fx.novaFX[ni].active) {
                        s_engine->m_fx.novaFX[ni] = {position, sd->radius, 0.6f, true, {1.0f, 0.15f, 0.1f}};
                        break;
                    }
                }
            } break;
            case SkillId::METEOR_STRIKE: {
                extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
                for (u32 mi = 0; mi < MAX_PENDING_METEORS; mi++) {
                    if (!s_meteors[mi].active) {
                        s_meteors[mi] = {position, sd->damage, sd->radius, sd->delay, true};
                        break;
                    }
                }
            } break;
            case SkillId::SHADOW_RICOCHET: {
                // Find 2 nearest enemies (excluding the one we just hit) and fire
                // shadow bolts at them. Each bolt is a normal player projectile so
                // it can re-trigger the proc on hit (10% chance → natural decay).
                EntityHandle nearby[8];
                f32 nearDists[8];
                u32 found = CombatQuery::queryConeSorted(
                    s_engine->m_entities, position, {0,-1,0}, -1.0f, 12.0f,
                    nearby, nearDists, 8);
                u32 spawned = 0;
                for (u32 h = 0; h < found && spawned < 2; h++) {
                    if (nearby[h].index == target.index) continue; // skip primary target
                    Entity* ne = handleGet(s_engine->m_entities, nearby[h]);
                    if (!ne || (ne->flags & ENT_DEAD) || (ne->flags & ENT_FRIENDLY)) continue;
                    Vec3 toEnemy = ne->position - position;
                    f32 dist = length(toEnemy);
                    if (dist < 0.1f) continue;
                    Vec3 dir = toEnemy * (1.0f / dist);
                    // Shadow bolt: 60% of weapon damage, fast, small radius
                    f32 boltDmg = sd->damage * 0.6f;
                    u16 idx = ProjectileSystem::spawn(s_engine->m_projectiles, position, dir,
                        20.0f, boltDmg, 0.1f, 2.0f, true);
                    if (idx != 0xFFFF) {
                        s_engine->m_projectiles.projectiles[idx].projFlags = PROJ_VOID;
                    }
                    spawned++;
                }
                LOG_INFO("  SHADOW RICOCHET: spawned %u bolts", spawned);
            } break;
            default: break;
        }
    });

    // Floating damage numbers for projectile hits
    ProjectileSystem::setDamageNumberCallback([](Vec3 position, f32 damage) {
        if (!s_engine) return;
        s_engine->spawnDamageNumber(position, damage);
    });

    // Perfect block callback — legendary shield triggers stun bash
    Combat::setPerfectBlockCallback([](Player& player) {
        if (!s_engine) return;
        // Check if offhand is a legendary shield
        const ItemInstance& shield = s_engine->m_inventories[s_engine->m_localPlayerIndex].equipped[static_cast<u32>(ItemSlot::OFFHAND)];
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
                if (!s_engine->m_fx.novaFX[ni].active) {
                    s_engine->m_fx.novaFX[ni] = {player.position, 3.0f, 0.4f, true, {0.8f, 0.8f, 1.0f}};
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
            // Swarm drone — limit to 2 active (3 after floor 20 upgrade)
            u32 swarmCount = 0;
            u32 swarmMax = (s_engine->m_level.currentFloor >= 20) ? 3 : 2;
            for (u32 a = 0; a < pool.activeCount; a++) {
                u32 si = pool.activeList[a];
                Entity& se = pool.entities[si];
                if ((se.flags & ENT_UNTARGETABLE) && (se.flags & ENT_FRIENDLY) && !(se.flags & ENT_DEAD))
                    swarmCount++;
            }
            if (swarmCount >= swarmMax) return; // at capacity

            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.15f, 0.1f, 0.15f}, true, 9999.0f, 5.0f, 12.0f, 8.0f, 1.5f, 3.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->flags        |= ENT_FRIENDLY | ENT_FLYING | ENT_UNTARGETABLE;
                e->enemyType     = EnemyType::GENERIC;
                e->meshId        = s_engine->m_meshIdBat;
                e->materialId    = 49; // prop_iron
                e->npcWeaponType = WeaponType::HITSCAN;
                e->aiState       = AIState::IDLE;
            }
        } else if (type == 2) {
            // Mobile turret bot — armored body on tank treads, follows player when idle.
            // HP scales with floor depth so turrets stay relevant in later tiers.
            f32 baseHp = 160.0f;
            f32 floorMult = 1.0f + (s_engine->m_level.currentFloor - 1) * 0.06f;
            EntityHandle h = EntitySystem::spawn(pool, position,
                {0.2f, 0.3f, 0.2f}, false, baseHp * floorMult, 3.0f, 15.0f, 10.0f, 1.5f, 12.0f);
            Entity* e = handleGet(pool, h);
            if (e) {
                e->flags        |= ENT_FRIENDLY;
                e->enemyType     = EnemyType::GENERIC;
                e->meshId        = s_engine->findMeshByName("turret");
                e->materialId    = MaterialSystem::getIdByName("turret_skin");
                e->npcWeaponType = WeaponType::PROJECTILE;
                e->npcProjectileSpeed  = 30.0f;
                e->npcProjectileRadius = 0.06f;
                e->aiState       = AIState::IDLE;
                e->baseMoveSpeed      = e->moveSpeed;
                e->baseAttackCooldown = e->attackCooldown;
                // Spark burst on deploy — visual feedback
                ParticleSystem::spawnSparks(s_engine->m_particles, position, {0, 1, 0}, 8);
            }
        }
    });

    // Init networking
    Net::init();

    // Start in menu
    m_gameState = GameState::MENU;
    m_menu.selection = 0;
    m_running = true;
    m_accumulator = 0.0;
    m_statsTimer = 0.0;
    m_updateCount = 0;
    m_frameCount = 0;

    // Load global difficulty unlock so the menu can gray out locked tiers
    {
        FILE* f = std::fopen("difficulty_unlock.dat", "rb");
        if (f) {
            (void)std::fread(&m_highestUnlocked, 1, 1, f);
            std::fclose(f);
            if (m_highestUnlocked > 2) m_highestUnlocked = 0; // sanitize bad data
        }
    }

    LOG_INFO("Engine initialized — Phase 4 multiplayer ready");
}


void Engine::shutdown() {
    LOG_INFO("Engine shutting down...");

    AudioSystem::shutdown();
    Net::shutdown();

    // Destroy loaded OBJ meshes (skip index 0 = cube, destroyed below)
    for (u32 i = 1; i < m_meshDefCount; i++) {
        if (m_meshDefs[i].mesh.vao) MeshSystem::destroy(m_meshDefs[i].mesh);
    }

    MeshSystem::destroy(m_cubeMesh);
    MeshSystem::destroy(m_quadMesh);
    MeshSystem::destroy(m_handMesh);
    LevelMeshSystem::destroyAll(m_level.sections, m_level.sectionCount);
    LevelGridSystem::shutdown(m_level.grid);

    MaterialSystem::shutdown();
    ShaderSystem::destroy(m_basicShader);
    ShaderSystem::destroy(m_unlitShader);

    FontSystem::shutdown();
    ItemIconSystem::shutdown();
    HUD::shutdown();
    ProjectileRenderer::shutdown();
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

