// engine_arena.cpp — the PvP ARENA (Arena mode, sentinel floor 97).
//
// A deterministic open-sky colosseum where players fight each other: FFA deathmatch,
// first to Arena::KILL_TARGET kills, 3 s auto-respawn (rules: game/arena.h). Built on the
// engine_town.cpp pattern — same sentinel-floor rails (host broadcasts SV_LEVEL_SEED with
// ARENA_SENTINEL_FLOOR, clients build the identical arena), same daylight rendering branch.
//
// The layout is symmetric under both diagonal mirrors so no spawn corner is favored, and
// every piece of cover is plain CELL_SOLID — collision, combat-query LOS, hitscan raycasts
// and projectile DDA all respect it with zero new systems:
//   - a 4-pillar rotunda around the contested center,
//   - a 2x2 crate cluster shielding each corner spawn pad,
//   - four short wall segments breaking the mid-field sightlines.
// No enemies, no loot, no portals spawn here; the exit is the pause menu ("Leave Arena").

#include "engine/engine.h"
#include "platform/input.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "game/game_constants.h"
#include "game/enemy_ai.h"
#include "renderer/material.h"
#include "renderer/minimap.h"
#include "net/net.h"
#include "net/server.h"
#include "core/log.h"
#include <cmath>

// The pure rules assume exactly MAX_PLAYERS combatants — pin it here, at the engine boundary.
static_assert(Arena::MAX_COMBATANTS == MAX_PLAYERS,
              "Arena::MAX_COMBATANTS must track MAX_PLAYERS");

// Arena layout constants — one place, shared by build + spawn placement so they can't drift.
namespace {
    constexpr u32 ARENA_W = 36, ARENA_D = 36;
    constexpr f32 ARENA_CS = 1.0f;

    // Corner spawn pads (world coords, cell centers), one per net slot. Each pad sits behind
    // its corner's crate cluster relative to the center — you respawn in cover, not in a lane.
    constexpr Vec3 kArenaPads[MAX_PLAYERS] = {
        { 4.5f, 0.0f,  4.5f}, {31.5f, 0.0f,  4.5f},
        { 4.5f, 0.0f, 31.5f}, {31.5f, 0.0f, 31.5f},
    };

    // Yaw that faces the arena center from a pad. Forward is {-sin(yaw), 0, -cos(yaw)}
    // (the engine-wide convention), so yaw = atan2(-dx, -dz).
    f32 padYawToCenter(const Vec3& pad) {
        f32 dx = (ARENA_W * 0.5f) * ARENA_CS - pad.x;
        f32 dz = (ARENA_D * 0.5f) * ARENA_CS - pad.z;
        return std::atan2(-dx, -dz);
    }
}

Vec3 Engine::arenaPad(u8 slot) const { return kArenaPads[slot % MAX_PLAYERS]; }

Vec3 Engine::buildArenaLevel() {
    LevelGridSystem::init(m_level.grid, ARENA_W, ARENA_D, ARENA_CS);

    u8 sand  = MaterialSystem::getIdByName("arena_sand");
    u8 brick = MaterialSystem::getIdByName("brick_wall");   // warm perimeter (dark stone reads as void in daylight)
    u8 stone = MaterialSystem::getIdByName("stone_wall");   // rotunda pillars + wall segments
    u8 plank = MaterialSystem::getIdByName("wood_plank");   // crate clusters

    for (u32 z = 0; z < ARENA_D; z++) {
        for (u32 x = 0; x < ARENA_W; x++) {
            GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
            bool border = (x == 0 || z == 0 || x == ARENA_W - 1 || z == ARENA_D - 1);
            if (border) {
                c.flags = CELL_SOLID;
                c.wallMaterialId = brick;
            } else {
                // NO CELL_CEILING — open sky (the mesher skips the lid, the daylight branch
                // lights it). ceilingHeight doubles as adjacent walls' height: 16 quarter-units
                // = 4 m, one meter taller than the town so the fight feels walled-in.
                c.flags = CELL_FLOOR;
                c.floorHeight     = 0;
                c.ceilingHeight   = 16;
                c.floorMaterialId = sand;
                c.wallMaterialId  = brick;
            }
        }
    }

    // Cover. All placements mirror under x -> 35-x and z -> 35-z (and the diagonal swap),
    // so all four spawn corners see identical geometry.
    auto solid = [&](u32 sx, u32 sz, u32 w, u32 d, u8 mat) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags = CELL_SOLID;
                c.wallMaterialId = mat;
            }
    };
    // Rotunda: four 1x1 stone pillars around the center plaza.
    solid(16, 16, 1, 1, stone); solid(19, 16, 1, 1, stone);
    solid(16, 19, 1, 1, stone); solid(19, 19, 1, 1, stone);
    // Crate clusters: one 2x2 plank block per quadrant, shielding its spawn pad.
    solid( 8,  8, 2, 2, plank); solid(26,  8, 2, 2, plank);
    solid( 8, 26, 2, 2, plank); solid(26, 26, 2, 2, plank);
    // Sightline breakers: four 1x4 stone segments between rotunda and mid-field.
    solid(12, 16, 1, 4, stone); solid(23, 16, 1, 4, stone);
    solid(16, 12, 4, 1, stone); solid(16, 23, 4, 1, stone);

    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid,
                             0xA12E7Au,    // constant seed — deterministic tile shading on every peer
                             m_level.sections, MAX_LEVEL_SECTIONS);
    LevelGridSystem::buildClearanceField(m_level.grid);
    Minimap::init(m_level.grid.width, m_level.grid.depth);
    Vec3 center = {(ARENA_W * 0.5f) * ARENA_CS, 0.0f, (ARENA_D * 0.5f) * ARENA_CS};
    LevelGridSystem::buildFlowField(m_level.grid, center);
    return center;
}

// Deliberately empty in v1: no stash, no portals, no NPCs, no loot — the arena is a progression
// firewall. Kept as a function (town symmetry) so future contested pickups have a home.
void Engine::spawnArenaContents(Vec3 /*center*/) {}

// Shared body of enterArena/enterArenaClient: wipe pools, build, reset match state, place the
// local lanes on their pads. Everything both sides do identically lives here.
void Engine::enterArenaCommon() {
    EntitySystem::init(m_entities);
    ProjectileSystem::init(m_projectiles);
    WorldItemSystem::init(m_worldItems);
    Vec3 center = buildArenaLevel();

    m_level.inArena            = true;
    m_level.inTown             = false;
    m_level.inSourceChamber    = false;
    m_level.floorDoorActive    = false;
    m_level.floorHasBoss       = false;
    m_level.townPortalActive   = false;
    m_level.sourcePortalActive = false;
    m_level.exitPortalActive   = false;

    m_arenaScore     = Arena::Score{};
    m_arenaWinner    = 0xFF;
    m_arenaOverTimer = 0.0f;
    for (u32 i = 0; i < MAX_PLAYERS; i++) m_arenaRespawn[i] = 0.0f;

    // Local lanes onto their pads (lane index == net slot for host/SP locals). The alias write
    // happens OUTSIDE the per-player swap at every call site, so persist the lane array
    // explicitly or next frame's swapInPlayer erases the placement (the enterTown rule).
    for (u8 lane = 0; lane < m_splitPlayerCount && lane < MAX_LOCAL_PLAYERS; lane++) {
        Player& p = (lane == m_localPlayerIndex) ? m_localPlayer : m_localPlayers[lane];
        p.position    = kArenaPads[lane];
        p.yaw         = padYawToCenter(kArenaPads[lane]);
        p.pitch       = 0.0f;
        p.invulnTimer = 1.0f;
        p.lastHitByPlayerSlot = 0xFF;
    }
    m_localPlayers[m_localPlayerIndex] = m_localPlayer;
    snapCameraToPlayer();

    spawnArenaContents(center);
    EnemyAI::setTownMode(false);
    m_gameState = GameState::IN_GAME;
    Input::setRelativeMouseMode(true);
}

// Host/SP: enter the arena and (if hosting) pull every connected client in with us.
void Engine::enterArena() {
    enterArenaCommon();

    // Seat every active NetPlayer on its own pad; the pad is also its respawn point.
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (!m_players[pi].active) continue;
        m_players[pi].position      = kArenaPads[pi];
        m_players[pi].yaw           = padYawToCenter(kArenaPads[pi]);
        m_players[pi].spawnPosition = kArenaPads[pi];
        m_players[pi].invulnTimer   = 1.0f;
        m_players[pi].isDead        = false;
    }

    if (m_netRole == NetRole::SERVER) {
        Net::broadcastLevelSeed(GameConst::ARENA_SENTINEL_FLOOR, m_difficulty, m_level.levelSeed);
        Server::updateLevel(m_level.levelSeed, GameConst::ARENA_SENTINEL_FLOOR, m_difficulty);
    }
    LOG_INFO("Entered the ARENA (host) — first to %u.", Arena::KILL_TARGET);
}

// Client: mirror of enterArena, driven by the sentinel-floor SV_LEVEL_SEED / join-accept.
// Our real position/score arrive from the server (snapshot + ARENA_SCORES).
void Engine::enterArenaClient() {
    enterArenaCommon();
    LOG_INFO("Entered the ARENA (client).");
}
