// engine_town.cpp — the outdoor TOWN hub earned by killing the Dungeon Engine.
//
// The town is the "outside" the exit portal always promised: an open grass plaza under real
// sky (interior cells simply omit CELL_CEILING — the mesher skips the lid and the sky-blue
// clear color shows), walled by low stone, populated by the friendly NPC cast, holding the
// shared ACCOUNT STASH chest and the to-dungeon portal that opens the Free-Play select.
//
// Build is DETERMINISTIC (no seed inputs) on the buildSourceChamber pattern, and entry rides
// the same sentinel-floor rails: the host broadcasts SV_LEVEL_SEED with TOWN_SENTINEL_FLOOR
// (98) and clients build the identical town (onLevelSeed routes to enterTownClient). Nothing
// about the town touches currentFloor or the save header — a cleared character keeps its
// pinned >50 marker floor on disk (see saveCharacter's town guard).

#include "engine/engine.h"
#include "platform/input.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "world/collision.h"
#include "game/game_constants.h"
#include "game/enemy_ai.h"
#include "renderer/material.h"
#include "renderer/minimap.h"
#include "net/net.h"
#include "net/server.h"
#include "audio/audio.h"
#include "core/log.h"
#include <cstdlib>

// Town layout constants — one place, shared by build + population so they can't drift.
namespace {
    constexpr u32 TOWN_W = 44, TOWN_D = 44;
    constexpr f32 TOWN_CS = 1.0f;
}

Vec3 Engine::buildTownLevel() {
    LevelGridSystem::init(m_level.grid, TOWN_W, TOWN_D, TOWN_CS);

    u8 grass = MaterialSystem::getIdByName("town_grass");
    u8 wall  = MaterialSystem::getIdByName("brick_wall");   // warm brick — dark stone reads as a void in daylight
    u8 plank = MaterialSystem::getIdByName("wood_plank");
    for (u32 z = 0; z < TOWN_D; z++) {
        for (u32 x = 0; x < TOWN_W; x++) {
            GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
            bool border = (x == 0 || z == 0 || x == TOWN_W - 1 || z == TOWN_D - 1);
            if (border) {
                c.flags = CELL_SOLID;              // low perimeter — the world ends politely
                c.wallMaterialId = wall;
            } else {
                // NO CELL_CEILING: the mesher builds no lid, so the town is open sky.
                c.flags = CELL_FLOOR;
                c.floorHeight   = 0;
                // ceilingHeight doubles as the WALL height of adjacent solid cells (the mesher
                // raises wall faces to the open side's ceilH) — 12 quarter-units = 3 m keeps the
                // perimeter/hut walls low so the sky owns the view; there is still no ceiling.
                c.ceilingHeight = 12;
                c.floorMaterialId = grass;
                c.wallMaterialId  = wall;
            }
        }
    }

    // A few solid hut footprints sketch a settlement without new meshes: plank-walled blocks
    // the player walks around. Positions are hand-placed around the plaza center.
    auto hut = [&](u32 hx, u32 hz, u32 w, u32 d) {
        for (u32 z = hz; z < hz + d; z++)
            for (u32 x = hx; x < hx + w; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags = CELL_SOLID;
                c.wallMaterialId = plank;
            }
    };
    // THE NORTH GATE — the way out into the overworld (Act 1). Always CARVED, for every hero,
    // because the town is deterministic geometry that host and client each build from the sentinel
    // seed: making the opening conditional on a save's unlock state would let two peers build
    // DIFFERENT towns and desync the moment one walked where the other saw a wall. The gate is
    // therefore always a hole; whether it takes you anywhere is decided by the transition
    // (updateZoneTransitions), which is host-authoritative and checks the Inferno clear.
    for (s32 o = -2; o <= 2; o++) {
        GridCell& g = LevelGridSystem::getCell(m_level.grid, static_cast<u32>(TOWN_W / 2 + o), 0);
        g.flags           = CELL_FLOOR;
        g.floorHeight     = 0;
        g.ceilingHeight   = 12;
        g.floorMaterialId = grass;
    }

    hut(8, 8, 5, 4);      // north-west lodge
    hut(30, 9, 4, 4);     // north-east hut
    hut(9, 30, 4, 5);     // south-west hut
    hut(31, 31, 3, 3);    // south-east shed

    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid,
                             0x70A11u,     // constant seed — deterministic prop/tile shading
                             m_level.sections, MAX_LEVEL_SECTIONS);
    LevelGridSystem::buildClearanceField(m_level.grid);
    Minimap::init(m_level.grid.width, m_level.grid.depth);
    Vec3 center = {(TOWN_W * 0.5f) * TOWN_CS, 0.0f, (TOWN_D * 0.5f) * TOWN_CS};
    LevelGridSystem::buildFlowField(m_level.grid, center);
    return center;
}

// Shared population (host AND client build their own copy — NPCs on the client are cosmetic;
// the authoritative ones replicate over snapshots exactly like dungeon NPCs, and the client's
// local spawns are overwritten by the mirror the same way).
void Engine::spawnTownContents(Vec3 center) {
    // --- The account stash: an oversized golden chest at the plaza's heart ---
    ItemInstance stash{};
    stash.defId = STASH_ID;
    stash.uid   = m_worldItems.nextUid++;
    WorldItemSystem::spawn(m_worldItems, stash, center + Vec3{0.0f, 0.0f, -3.0f}, &m_level.grid);

    // --- The to-dungeon portal, south of the plaza (where the player arrives) ---
    m_level.townPortalActive = true;
    m_level.townPortalPos    = center + Vec3{0.0f, 0.0f, 10.0f};   // 4m clear of the arrival spot
                                                                    // (spawn z = center+14; trigger r=2m)

    // --- Townsfolk: the companion cast at plaza posts (server-authoritative; clients mirror) ---
    if (m_netRole != NetRole::CLIENT) {
        const u8 floor = 1;   // town NPCs use base-floor stats; they never fight anyway
        const Vec3 posts[6] = {
            center + Vec3{-4.0f, 0.0f, -1.0f}, center + Vec3{ 4.0f, 0.0f, -1.0f},
            center + Vec3{ 0.0f, 0.0f,  4.0f}, center + Vec3{-9.0f, 0.0f,  7.0f},
            center + Vec3{ 9.0f, 0.0f,  7.0f}, center + Vec3{ 0.0f, 0.0f, -8.0f},
        };
        const NpcClass kinds[6] = {NpcClass::CLERIC, NpcClass::ROGUE, NpcClass::ARCHER,
                                   NpcClass::CLERIC, NpcClass::ROGUE, NpcClass::ARCHER};
        for (u32 n = 0; n < 6; n++) {
            EntityHandle h = spawnFriendlyNpc(posts[n], kinds[n], floor);
            Entity* npc = handleGet(m_entities, h);
            if (npc) npc->homePosition = posts[n];   // the post the town-mode AI holds
        }
    }
}

// Host/SP: enter the town — wipe world pools, build, place players, populate, broadcast.
void Engine::enterTown() {
    worldResetPools();
    Vec3 center = buildTownLevel();
    worldClearLevelFlags();
    m_level.inTown = true;   // no ordinary exit here; the town portal opens the Free-Play select

    // Players arrive at the south gate, facing the plaza (the stash straight ahead).
    // yaw 0 faces -Z, which from the south gate IS the plaza.
    Vec3 base = {center.x, 0.0f, center.z + 14.0f};
    worldPlaceLocalPlayers(base, 0.0f);
    // Seeding the host's own slot was MISSING here for the whole life of this function. It only
    // ever worked because the menu path calls startGame immediately before enterTown; reached any
    // other way (a cleared-save --town, the VICTORY roll-on) the seating loop below skips the
    // inactive host slot and the first frame stomps it to {0,0,0}/health-100 — inside the wall.
    worldSeedHostSlot();
    worldSeatNetPlayers(base);   // the town IS the respawn point while you are here

    spawnTownContents(center);
    worldFinishEntry(GameConst::TOWN_SENTINEL_FLOOR, /*peaceful=*/true);
    LOG_INFO("Entered the town (host).");
}

// Client: mirror of enterTown, driven by the sentinel-floor SV_LEVEL_SEED (see onLevelSeed).
void Engine::enterTownClient() {
    worldResetPools();
    Vec3 center = buildTownLevel();
    worldClearLevelFlags();
    m_level.inTown = true;

    worldPlaceLocalPlayers(Vec3{center.x, 0.0f, center.z + 14.0f}, 0.0f);   // face the plaza (-Z)
    spawnTownContents(center);   // stash chest + portal are local fixtures; NPCs mirror over snapshots
    // worldFinishEntry wires the CLIENT callbacks: a join-accept can route here INSTEAD of startGame
    // (joining a host who is at home), and without that wiring the join is connected but deaf — no
    // snapshots, no SV_EVENTs. Idempotent on the mid-session SV_LEVEL_SEED route.
    worldFinishEntry(GameConst::TOWN_SENTINEL_FLOOR, /*peaceful=*/true);
    LOG_INFO("Entered the town (client).");
}
