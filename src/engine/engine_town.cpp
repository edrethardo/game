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
    EntitySystem::init(m_entities);
    ProjectileSystem::init(m_projectiles);
    WorldItemSystem::init(m_worldItems);
    Vec3 center = buildTownLevel();

    m_level.inTown             = true;
    m_level.inSourceChamber    = false;
    m_level.floorDoorActive    = false;   // no ordinary exit; the town portal opens the select
    m_level.floorHasBoss       = false;
    m_level.sourcePortalActive = false;
    m_level.exitPortalActive   = false;

    // Players arrive at the south gate, facing the plaza (the stash straight ahead).
    Vec3 base = {center.x, 0.0f, center.z + 14.0f};
    const f32 facePlaza = 0.0f;   // yaw 0 faces -Z — from the south gate that is the plaza
    m_localPlayer.position    = base;
    m_localPlayer.yaw         = facePlaza;
    m_localPlayer.pitch       = 0.0f;
    m_localPlayer.invulnTimer = 1.0f;
    for (u8 lane = 0; lane < m_splitPlayerCount && lane < MAX_LOCAL_PLAYERS; lane++) {
        if (lane == m_localPlayerIndex) continue;
        m_localPlayers[lane].position = base + Vec3{(f32)lane * 1.6f, 0.0f, 0.0f};
        m_localPlayers[lane].yaw      = facePlaza;
        m_localPlayers[lane].pitch    = 0.0f;
    }
    // Every enterTown call site (launch flag, menu Continue, the VICTORY handler) runs OUTSIDE
    // the per-player swap, so the alias write above would be erased by next frame's
    // swapInPlayer — persist the lane array explicitly (the applyClassToLane0 pattern).
    m_localPlayers[m_localPlayerIndex] = m_localPlayer;
    snapCameraToPlayer();
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (!m_players[pi].active) continue;
        m_players[pi].position      = base + Vec3{(f32)pi * 1.2f - 0.6f, 0.0f, 0.0f};
        m_players[pi].spawnPosition = m_players[pi].position;   // town IS the respawn point here
        m_players[pi].invulnTimer   = 1.0f;
        m_players[pi].isDead        = false;
    }

    spawnTownContents(center);
    EnemyAI::setTownMode(true);   // townsfolk hold posts + small talk (cleared by startGame)
    m_gameState = GameState::IN_GAME;
    Input::setRelativeMouseMode(true);

    if (m_netRole == NetRole::SERVER) {
        // Same gap as the arena: a cleared-hero Continue host reaches the town WITHOUT
        // startGame, so the server callbacks (onPlayerJoin!) were never wired on that path —
        // a joiner connected but was never seated. wireServerNet is idempotent.
        wireServerNet();
        Net::broadcastLevelSeed(GameConst::TOWN_SENTINEL_FLOOR, m_difficulty, m_level.levelSeed);
        Server::updateLevel(m_level.levelSeed, GameConst::TOWN_SENTINEL_FLOOR, m_difficulty);
    }
    LOG_INFO("Entered the town (host).");
}

// Client: mirror of enterTown, driven by the sentinel-floor SV_LEVEL_SEED (see onLevelSeed).
void Engine::enterTownClient() {
    // A join-accept can route here INSTEAD of startGame (joining a host who is at home) —
    // wire the client net callbacks or the join is connected but deaf (no snapshots, no
    // SV_EVENTs). Harmless for the mid-session SV_LEVEL_SEED route (already wired; idempotent).
    if (m_netRole == NetRole::CLIENT) wireClientNet();
    EntitySystem::init(m_entities);
    ProjectileSystem::init(m_projectiles);
    WorldItemSystem::init(m_worldItems);
    Vec3 center = buildTownLevel();

    m_level.inTown             = true;
    m_level.inSourceChamber    = false;
    m_level.floorDoorActive    = false;
    m_level.floorHasBoss       = false;
    m_level.sourcePortalActive = false;
    m_level.exitPortalActive   = false;

    Vec3 base = {center.x, 0.0f, center.z + 14.0f};
    m_localPlayer.position    = base;
    m_localPlayer.yaw         = 0.0f;   // face the plaza (-Z)
    m_localPlayer.pitch       = 0.0f;
    m_localPlayer.invulnTimer = 1.0f;
    m_localPlayers[m_localPlayerIndex] = m_localPlayer;   // outside the swap — persist (see enterTown)
    snapCameraToPlayer();

    spawnTownContents(center);   // stash chest + portal are local fixtures; NPCs mirror over snapshots
    EnemyAI::setTownMode(true);
    m_gameState = GameState::IN_GAME;
    Input::setRelativeMouseMode(true);
    LOG_INFO("Entered the town (client).");
}
