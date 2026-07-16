// Engine asset initialisation — loads meshes, materials, and JSON content defs,
// then resolves all visual references (mesh IDs, material IDs) after init.
// Called by Engine::init() as the first major init phase, before callbacks.

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "audio/audio.h"

#include "engine/engine.h"
#include "engine/asset_manifest.h"
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
#include "game/floor_event_loader.h"
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

    // Load the OBJ meshes named by the shared manifest (src/engine/asset_manifest.h).
    //
    // A mesh that fails to load here used to be skipped in total silence, which is how an entity
    // whose .obj was never generated could ship looking like a 0.3-scale cube. Every failure is now
    // an ERROR: the manifest is the contract, and a missing file means the asset build is broken.
    {
        u32 missing = 0;
        for (u32 mi = 0; mi < kMeshAssetCount; mi++) {
            const MeshAsset& entry = kMeshAssets[mi];
            if (m_meshDefCount >= MAX_MESH_DEFS) {
                // Unreachable — asset_manifest.h static_asserts the table fits. Kept as a runtime
                // backstop because the consequence (the tail of the table silently vanishing) is
                // exactly the failure mode this whole file is defending against.
                LOG_ERROR("Mesh registry full at %u — '%s' and everything after it was DROPPED",
                          m_meshDefCount, entry.name);
                break;
            }
            AABB bounds;
            // Measure body-part regions only for player body meshes (armor auto-fit reads them).
            bool isBody = std::strncmp(entry.name, "player_", 7) == 0;
            BodyRegions regions;
            Mesh mesh = ObjLoader::load(ASSET_PATH(entry.path), &bounds,
                                        isBody ? &regions : nullptr);
            if (mesh.vao == 0) {
                LOG_ERROR("Mesh MISSING: '%s' (%s) — run tools/build_assets.py; this asset will "
                          "render as a fallback cube", entry.name, entry.path);
                missing++;
                continue;
            }
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
        if (missing > 0)
            LOG_ERROR("Asset build incomplete: %u of %u meshes missing. Run tools/build_assets.py",
                      missing, kMeshAssetCount);
    }


    // --- Decoration props: load CPU geometry once and hand to the level mesher --------------
    // These small meshes are BAKED into floor sections (LevelMeshSystem::buildAll) rather than
    // drawn as entities, so any number of them costs no extra draw calls and — unlike the prop
    // ENTITIES in spawnFloorDecorations — they have no collision, so they can sit in room
    // interiors without blocking movement or item pickups (which is why bones/rubble live here,
    // not there). Tint-only materials (prop_iron/bone/wood) give a clean solid voxel look.
    {
        LevelMeshSystem::clearPropMeshes();
        for (u32 pi = 0; pi < kPropAssetCount; pi++) {
            const PropAsset& pe = kPropAssets[pi];
            std::vector<Vertex> verts;
            std::vector<u32>    indices;
            // Load only to harvest CPU verts; the returned GPU mesh is unused (destroy it so we
            // don't leak a VAO/VBO we never draw).
            Mesh tmp = ObjLoader::load(ASSET_PATH(pe.path), nullptr, nullptr, &verts, &indices);
            if (verts.empty() || indices.empty()) {
                // Not "skip cleanly" any more: the manifest promised this file, so its absence means
                // the asset build is broken — say so instead of quietly meshing a floor without it.
                LOG_ERROR("Prop mesh MISSING: %s — run tools/build_assets.py", pe.path);
                if (tmp.vao) MeshSystem::destroy(tmp);
                continue;
            }
            u8 matId = MaterialSystem::getIdByName(pe.material);
            LevelMeshSystem::addPropMesh(verts.data(), (u32)verts.size(),
                                         indices.data(), (u32)indices.size(), matId, pe.radius);
            if (tmp.vao) MeshSystem::destroy(tmp);
        }
    }

    // Build limb meshes AFTER OBJ meshes are loaded (needs valid meshDefCount).
    //
    // The registry has TWO producers — the OBJ manifest above and LimbSystem here — so its capacity
    // must cover both. The manifest's own static_assert only ever checked the first, which is how
    // the limb meshes came to overflow a hardcoded cap in total silence and hand every spider a pair
    // of cube mandibles. This assert is the one that would have caught it.
    static_assert(kLimbMeshReserve == LimbSystem::LIMB_MESH_COUNT,
                  "asset_manifest.h reserves a different number of limb slots than LimbSystem "
                  "actually registers — the registry budget is wrong and the tail will become cubes.");
    LimbSystem::init(m_meshDefs, m_meshDefCount, MAX_MESH_DEFS);
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
    m_goblinMeshId   = findMeshByName("goblin");
    m_shrineMeshId   = findMeshByName("shrine");
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

    // Floor events (the loot goblin, and whatever follows it). A missing/!malformed file leaves the
    // table empty, which simply means no floor events — not a crash.
    FloorEventLoader::load(ASSET_PATH("assets/config/events.json"), m_floorEvents);

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

    // Resolve pet consumables to their enemy defs (items.json petEnemy name → enemy index) and
    // build the reverse map the 1-in-10000 kill roll uses (enemy index → pet item def). Runs after
    // BOTH tables are loaded; an unmatched name degrades to "that enemy drops no pet" — loudly,
    // because a typo here is exactly the silent sync trap the cross-JSON unit test also pins.
    for (u32 i = 0; i < MAX_ENEMY_DEFS; i++) m_petItemForEnemy[i] = 0xFFFF;
    for (u32 i = 0; i < m_itemDefCount; i++) {
        ItemDef& d = m_itemDefs[i];
        if (!d.petSummon || d.petEnemyName[0] == '\0') continue;
        bool matched = false;
        for (u32 e = 0; e < m_enemyDefs.count; e++) {
            if (std::strcmp(d.petEnemyName, m_enemyDefs.defs[e].name) == 0) {
                d.petEnemyIdx        = static_cast<u8>(e);
                m_petItemForEnemy[e] = static_cast<u16>(i);
                matched = true;
                break;
            }
        }
        if (!matched)
            LOG_WARN("ItemDef '%s': petEnemy '%s' matches no enemies.json entry — pet unobtainable",
                     d.name, d.petEnemyName);
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
