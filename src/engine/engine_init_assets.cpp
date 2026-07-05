// Engine asset initialisation — loads meshes, materials, and JSON content defs,
// then resolves all visual references (mesh IDs, material IDs) after init.
// Called by Engine::init() as the first major init phase, before callbacks.

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
#include "game/boss_def.h"
#include "game/boss_ai.h"
#include "game/boss_loader.h"
#include "game/enemy_loader.h"
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
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;

void Engine::initAssets() {
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
            {"chakram",        "assets/meshes/chakram.obj"},
            {"infinity_chakram", "assets/meshes/infinity_chakram.obj"},
            {"gloves",         "assets/meshes/gloves.obj"},
            {"helmet",         "assets/meshes/helmet.obj"},
            {"armor",          "assets/meshes/armor.obj"},
            {"boots",          "assets/meshes/boots.obj"},
            // Armor tier variants (MEDIUM reuses the bare meshes above; chest MEDIUM = "armor").
            // Resolved per armor ItemDef into ItemDef.tierMeshId; rendered on the body / inspect screen.
            {"helmet_light",   "assets/meshes/helmet_light.obj"},
            {"helmet_heavy",   "assets/meshes/helmet_heavy.obj"},
            {"chest_light",    "assets/meshes/chest_light.obj"},
            {"chest_heavy",    "assets/meshes/chest_heavy.obj"},
            {"boots_light",    "assets/meshes/boots_light.obj"},
            {"boots_heavy",    "assets/meshes/boots_heavy.obj"},
            {"gloves_light",   "assets/meshes/gloves_light.obj"},
            {"gloves_heavy",   "assets/meshes/gloves_heavy.obj"},
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
            {"web_wall",       "assets/meshes/web_wall.obj"},
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
            // New enemy meshes (roster rework)
            {"hellhound",      "assets/meshes/hellhound.obj"},
            {"wraith",         "assets/meshes/wraith.obj"},
            {"sentinel",       "assets/meshes/sentinel.obj"},
            {"cave_troll",     "assets/meshes/cave_troll.obj"},
            {"pit_fiend",      "assets/meshes/pit_fiend.obj"},
            {"pit_fiend_wing","assets/meshes/pit_fiend_wing.obj"},
            {"hellforge_smith","assets/meshes/hellforge_smith.obj"},
            {"succubus",       "assets/meshes/succubus.obj"},
            {"abyssal_titan",  "assets/meshes/abyssal_titan.obj"},
            {"entropy_weaver", "assets/meshes/entropy_weaver.obj"},
            // New boss meshes (visual rework) — each major boss now has a dedicated OBJ
            {"lich",           "assets/meshes/lich.obj"},
            {"warden",         "assets/meshes/warden.obj"},
            {"spider_queen",   "assets/meshes/spider_queen.obj"},
            {"korvath",        "assets/meshes/korvath.obj"},
            {"azhar",          "assets/meshes/azhar.obj"},
            {"diabro",         "assets/meshes/diabro.obj"},
            {"nyx",            "assets/meshes/nyx.obj"},
            {"reaper",         "assets/meshes/reaper.obj"},
            // Secret superboss: The Dungeon Engine + its source-shard pickup key.
            {"engine",         "assets/meshes/engine.obj"},
            {"shard",          "assets/meshes/shard.obj"},
            // Player class meshes — resolved by name from ClassDef.meshName in the renderer.
            // Distinct from the NPC meshes above so player and town-NPC visuals can diverge.
            // The renderer falls back to "human" if any of these fails to load (e.g. the
            // generator hasn't been run yet), so this is safe to register pre-asset-build.
            {"player_warrior",         "assets/meshes/player_warrior.obj"},
            {"player_ranger",          "assets/meshes/player_ranger.obj"},
            {"player_sorcerer",        "assets/meshes/player_sorcerer.obj"},
            {"player_rogue",           "assets/meshes/player_rogue.obj"},
            {"player_paladin",         "assets/meshes/player_paladin.obj"},
            {"player_combat_engineer", "assets/meshes/player_combat_engineer.obj"},
            {"player_marksman",        "assets/meshes/player_marksman.obj"},
            {"player_tinkerer",        "assets/meshes/player_tinkerer.obj"},
            {"player_wanderer",        "assets/meshes/player_wanderer.obj"},
        };
        for (auto& entry : kMeshes) {
            if (m_meshDefCount >= MAX_MESH_DEFS) break;
            AABB bounds;
            // Measure body-part regions only for player body meshes (armor auto-fit reads them).
            bool isBody = std::strncmp(entry.name, "player_", 7) == 0;
            BodyRegions regions;
            Mesh mesh = ObjLoader::load(ASSET_PATH(entry.path), &bounds,
                                        isBody ? &regions : nullptr);
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
                if (isBody && regions.valid) m_bodyRegions[m_meshDefCount] = regions;
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
    LimbSystem::setPitFiendWingMeshId(findMeshByName("pit_fiend_wing"));

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
    m_meshIdShard      = findMeshByName("shard");  // source-shard pickup (secret superboss key)
    SkillSystem::setArrowMeshIds(m_meshIdArrow, m_meshIdBolt);
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

    // Boss definitions — loaded after skills so skill names resolve to IDs
    if (!BossLoader::load(ASSET_PATH("assets/config/bosses.json"), m_bossDefs)) {
        LOG_WARN("Failed to load boss defs — using empty table");
        m_bossDefs.count = 0;
    }
    // Provide boss def table to EnemyAI for personality-driven boss behavior
    EnemyAI::setBossDefTable(&m_bossDefs);
    // Resolve skeleton minion visuals once so boss summon abilities (Sethrak) can
    // spawn proper-looking skeletons — AI code can't resolve asset name strings.
    EnemyAI::setSkeletonVisuals(findMeshByName("skeleton"), MaterialSystem::getIdByName("skeleton_skin"));

    // Enemy definitions — loaded after materials so material names resolve
    if (!EnemyLoader::load(ASSET_PATH("assets/config/enemies.json"), m_enemyDefs)) {
        LOG_WARN("Failed to load enemy defs — using fallback kTier arrays");
        m_enemyDefs.count = 0;
    }

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
        // Blinks land instantly — zero camera-interp delta so the view doesn't
        // smear/whip across the room (esp. now that rogue blinks also rotate yaw).
        s_engine->snapCameraToPlayer();
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
    SkillSystem::setReloadCallback([]() {
        if (!s_engine) return;
        WeaponState& ws = s_engine->m_players[s_engine->activeNetSlot()].weaponState; // local player's net slot
        const ItemInstance& eqWpn = s_engine->m_inventories[s_engine->m_localPlayerIndex]
            .equipped[static_cast<u32>(ItemSlot::WEAPON)];
        if (!isItemEmpty(eqWpn)) {
            WeaponDef wpn = Inventory::getWeaponFromItem(
                s_engine->m_inventories[s_engine->m_localPlayerIndex],
                s_engine->m_itemDefs, eqWpn);
            ws.currentClip = wpn.clipSize;
            ws.reloading = false;
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

    // Resolve per-slot tier mesh IDs for armor items (helmet/chest/boots/gloves × light/medium/heavy).
    // MEDIUM tier reuses the base mesh name (chest slot uses "armor" to match the existing mesh);
    // LIGHT/HEAVY append "_light"/"_heavy". Falls back to the item's own meshId if the tier mesh
    // isn't registered (e.g. during development when only some tiers have generated meshes).
    for (u32 i = 0; i < m_itemDefCount; i++) {
        ItemDef& d = m_itemDefs[i];
        if (d.slot == ItemSlot::HELMET || d.slot == ItemSlot::ARMOR ||
            d.slot == ItemSlot::BOOTS  || d.slot == ItemSlot::GLOVES) {
            const char* base = (d.slot == ItemSlot::HELMET) ? "helmet"
                             : (d.slot == ItemSlot::ARMOR)  ? "chest"
                             : (d.slot == ItemSlot::BOOTS)  ? "boots" : "gloves";
            ArmorTier tier = armorTierFromMaterial(d.materialName);
            char tierMeshName[40];
            if (tier == ArmorTier::MEDIUM)
                std::snprintf(tierMeshName, sizeof(tierMeshName), "%s",
                              (d.slot == ItemSlot::ARMOR) ? "armor" : base);
            else
                std::snprintf(tierMeshName, sizeof(tierMeshName), "%s%s", base,
                              tier == ArmorTier::LIGHT ? "_light" : "_heavy");

            // Walk the mesh registry for a name match (same pattern as meshId resolution above)
            u8 found = d.meshId; // fallback to the item's own mesh if tier mesh not registered
            for (u32 m = 0; m < m_meshDefCount; m++) {
                if (std::strcmp(tierMeshName, m_meshDefs[m].name) == 0) {
                    found = static_cast<u8>(m);
                    break;
                }
            }
            d.tierMeshId = found;
        }
    }

    // Resolve enemy def visuals (material names → IDs, mesh names → mesh registry IDs)
    EnemyLoader::resolveVisuals(m_enemyDefs);
    for (u32 i = 0; i < m_enemyDefs.count; i++) {
        if (m_enemyDefs.defs[i].meshName[0] != '\0') {
            for (u32 m = 0; m < m_meshDefCount; m++) {
                if (std::strcmp(m_enemyDefs.defs[i].meshName, m_meshDefs[m].name) == 0) {
                    m_enemyDefs.defs[i].meshId = static_cast<u8>(m);
                    break;
                }
            }
        }
    }

    // Resolve boss def visuals (material + mesh)
    BossLoader::resolveVisuals(m_bossDefs);
    for (u32 i = 0; i < m_bossDefs.count; i++) {
        if (m_bossDefs.defs[i].meshName[0] != '\0') {
            for (u32 m = 0; m < m_meshDefCount; m++) {
                if (std::strcmp(m_bossDefs.defs[i].meshName, m_meshDefs[m].name) == 0) {
                    m_bossDefs.defs[i].meshId = static_cast<u8>(m);
                    break;
                }
            }
        }
        if (m_bossDefs.defs[i].weaponName[0] != '\0') {
            for (u32 m = 0; m < m_meshDefCount; m++) {
                if (std::strcmp(m_bossDefs.defs[i].weaponName, m_meshDefs[m].name) == 0) {
                    m_bossDefs.defs[i].weaponMeshId = static_cast<u8>(m);
                    break;
                }
            }
        }
    }

    // Instanced projectile renderer — batches projectiles by mesh for minimal draw calls
    ProjectileRenderer::init();

    // Particle system init — must come after MaterialSystem::init so mat IDs are valid
    ParticleSystem::init(m_particles);
    ParticleSystem::initBatchBuffers(m_particles);
    m_particleBlobMatId  = MaterialSystem::getIdByName("particle_blob");
    m_particleSparkMatId = MaterialSystem::getIdByName("particle_spark");
}
