// engine_arena.cpp — the PvP ARENA (Arena mode, sentinel floor 97).
//
// A deterministic open-sky colosseum where players fight each other: FFA deathmatch,
// first to Arena::KILL_TARGET kills, 3 s auto-respawn (rules: game/arena.h). Built on the
// engine_town.cpp pattern — same sentinel-floor rails (host broadcasts SV_LEVEL_SEED with
// ARENA_SENTINEL_FLOOR, clients build the identical arena), same daylight rendering branch.
//
// The layout is a two-story Quake / Metroid-Prime-Hunters COMBAT HALL (44x44, 4-fold rotational
// symmetry — every spawn corner faces identical geometry):
//   - TIER 0 (ground): the open pit with crate cover, wall-midpoint jump pads, and the covered
//     perimeter ARCADE under the balcony (spawn bays live here, in cover, out of sniper LOS).
//   - TIER 1 (1.5 m): the central tower, reached by four cardinal LEDGE ramps.
//   - TIER 2 (3.0 m): TWO dueling vantages — the perimeter SNIPER BALCONY (CELL_PLATFORM slabs:
//     stand on it, walk under it; open inner edge to drop/fire from; corner slab stairwells and
//     the midpoint pads to get up) and the tower's crown at the same height across the map.
// Verticality rides three opt-in cell flags: CELL_LEDGE (jump-gated risers), CELL_JUMPPAD
// (launch pads), CELL_PLATFORM (walk-under slabs). All are deterministic level geometry built
// from the seed on every peer, so co-op needs NO wire change (posY + onGround are snapshotted).
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
    constexpr u32 ARENA_W = 44, ARENA_D = 44;
    constexpr f32 ARENA_CS = 1.0f;

    // Spawn pads live in the ARCADE — the covered ground story under the perimeter balcony — one
    // per wall, rotationally symmetric ((x,z) -> (43-z, x)), each tucked beside a support column
    // and near a corner stairwell: you respawn in cover, out of every balcony sightline, with the
    // stairs and the pit both a few steps away.
    constexpr Vec3 kArenaPads[MAX_PLAYERS] = {
        { 1.5f, 0.0f, 10.5f},   // west arcade,  beside the column at (2,10)
        {33.5f, 0.0f,  1.5f},   // north arcade, beside the column at (33,2)
        {42.5f, 0.0f, 33.5f},   // east arcade,  beside the column at (41,33)
        {10.5f, 0.0f, 42.5f},   // south arcade, beside the column at (10,41)
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

    u8 sand   = MaterialSystem::getIdByName("arena_sand");
    u8 brick  = MaterialSystem::getIdByName("brick_wall");  // warm perimeter (dark stone reads as void in daylight)
    u8 stone  = MaterialSystem::getIdByName("stone_wall");  // tower / ramp / crown risers
    u8 plank  = MaterialSystem::getIdByName("wood_plank");  // crate clusters
    u8 padMat = MaterialSystem::getIdByName("arena_pad");   // glowing jump-pad tiles

    for (u32 z = 0; z < ARENA_D; z++) {
        for (u32 x = 0; x < ARENA_W; x++) {
            GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
            bool border = (x == 0 || z == 0 || x == ARENA_W - 1 || z == ARENA_D - 1);
            if (border) {
                c.flags = CELL_SOLID;
                c.wallMaterialId = brick;
            } else {
                // NO CELL_CEILING — open sky (the mesher skips the lid, the daylight branch
                // lights it). ceilingHeight doubles as adjacent walls' height: 20 quarter-units
                // = 5 m, tall enough that a balcony jump can't clear the perimeter.
                c.flags = CELL_FLOOR;
                c.floorHeight     = 0;
                c.ceilingHeight   = 20;   // 5 m walls: a balcony jump (3.0 + 0.8 m) cannot clear them
                c.floorMaterialId = sand;
                c.wallMaterialId  = brick;
            }
        }
    }

    // Two-story COMBAT-HALL layout (Quake / Metroid-Prime-Hunters style). Placement is 4-fold
    // rotationally symmetric about the arena centre (the balcony/stairwells/columns via the rotCell
    // helper below, the centre/pads/crates by mirror pairs), so all four spawn corners face
    // identical geometry — no corner is favoured.
    auto solid = [&](u32 sx, u32 sz, u32 w, u32 d, u8 mat) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags = CELL_SOLID;
                c.wallMaterialId = mat;
            }
    };
    // Raise a block into a jump-gated platform: qh = floor height in quarter-units (×0.25 = m),
    // CELL_LEDGE so a body must JUMP or ramp onto it (collision refuses the walk-up — see
    // STEP_UP_HEIGHT). Stone risers, sand top; the mesher draws the riser faces.
    auto raise = [&](u32 sx, u32 sz, u32 w, u32 d, u8 qh) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags = static_cast<u8>(CELL_FLOOR | CELL_LEDGE);
                c.floorHeight     = qh;
                c.floorMaterialId = sand;
                c.wallMaterialId  = stone;   // riser face material
            }
    };
    // Mark a block as JUMP PADS at height qh: standing on one flings you up JUMPPAD_LAUNCH (Quake pad).
    // Glowing tiles so the launch is legible; stone risers under any raised pad ring.
    auto pad = [&](u32 sx, u32 sz, u32 w, u32 d, u8 qh) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags = static_cast<u8>(CELL_FLOOR | CELL_JUMPPAD);
                c.floorHeight     = qh;
                c.floorMaterialId = padMat;
                c.wallMaterialId  = stone;
            }
    };
    // A 2-lane LEDGE STAIRCASE (ramp): `len` cells from (sx,sz) stepping by (dx,dz), height starting
    // at qhTop and dropping ONE quarter-unit per cell — so the inner end meets a tier at qhTop and the
    // outer end meets the pit at qhTop-len+1. 0.25 m steps stay under STEP_UP_HEIGHT, so you climb it
    // from the pit end one step at a time; the LEDGE flag walls off the sides (no snap-up cheese). The
    // second lane is offset by (px,pz).
    auto ramp = [&](u32 sx, u32 sz, s32 dx, s32 dz, s32 px, s32 pz, u8 qhTop, u32 len) {
        for (u32 i = 0; i < len; i++) {
            u8  qh = static_cast<u8>(qhTop - i);
            u32 cx = static_cast<u32>(static_cast<s32>(sx) + dx * static_cast<s32>(i));
            u32 cz = static_cast<u32>(static_cast<s32>(sz) + dz * static_cast<s32>(i));
            raise(cx, cz, 1, 1, qh);
            raise(static_cast<u32>(static_cast<s32>(cx) + px),
                  static_cast<u32>(static_cast<s32>(cz) + pz), 1, 1, qh);
        }
    };

    // Mark a block as PLATFORM SLABS: a second story floating topQ*0.25 m over the cell's normal
    // ground floor, which stays walkable beneath (the arcade). Plank walkway, stone rim faces.
    auto plat = [&](u32 sx, u32 sz, u32 w, u32 d, u8 topQ) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags           = static_cast<u8>(CELL_FLOOR | CELL_PLATFORM);
                c.platHeight      = topQ;
                c.platMaterialId  = plank;
                c.floorMaterialId = sand;    // the arcade ground beneath stays sand
                c.wallMaterialId  = stone;
            }
    };
    // Rotate a cell k*90° about the arena centre (one turn: (x,z) -> (W-1-z, x)). Building all
    // four corners/walls from ONE template through this guarantees perfect 4-fold symmetry — no
    // spawn corner ever faces different geometry.
    auto rotCell = [&](u32 x, u32 z, u32 k, u32& ox, u32& oz) {
        ox = x; oz = z;
        for (u32 i = 0; i < k; i++) { u32 t = ox; ox = ARENA_W - 1 - oz; oz = t; }
    };

    // --- SECOND STORY: the perimeter SNIPER BALCONY @ 3.0 m (the Combat-Hall signature) --------
    // A 2-cell walkway hugging every wall; open inner edge (drop off / fire into the pit
    // anywhere), covered arcade beneath (underside 2.5 m — 0.7 m of headroom over a body).
    plat(1,  1, 42,  2, 12);   // north band (z 1..2)
    plat(1, 41, 42,  2, 12);   // south band
    plat(1,  3,  2, 38, 12);   // west band  (x 1..2)
    plat(41, 3,  2, 38, 12);   // east band

    // Corner STAIRWELLS: an L-switchback of graduated slabs (0.25 m steps — walkable under
    // STEP_UP_HEIGHT), arcade -> balcony, overwriting band cells. The quiet route up; the pads
    // below are the fast, loud one. Both lanes of each leg step together.
    for (u32 k = 0; k < 4; k++) {
        u32 ox, oz;
        for (u32 i = 0; i < 6; i++)                       // leg A: h 0.25..1.5 m
            for (u32 lane = 1; lane <= 2; lane++) {
                rotCell(8 - i, lane, k, ox, oz);
                plat(ox, oz, 1, 1, static_cast<u8>(1 + i));
            }
        for (u32 cx2 = 1; cx2 <= 2; cx2++)                // corner landing @ 1.5 m
            for (u32 cz2 = 1; cz2 <= 2; cz2++) {
                rotCell(cx2, cz2, k, ox, oz);
                plat(ox, oz, 1, 1, 6);
            }
        for (u32 i = 0; i < 6; i++)                       // leg B: h 1.75..3.0 m, meets the band
            for (u32 lane = 1; lane <= 2; lane++) {
                rotCell(lane, 3 + i, k, ox, oz);
                plat(ox, oz, 1, 1, static_cast<u8>(7 + i));
            }
    }

    // Support COLUMNS: full-height solid pillars on the balcony's inner-edge row — arcade cover
    // below, pillars to strafe around above (the walkway narrows to one cell at each), and the
    // structure that visually carries the slab. Mirror-symmetric pairs (10,33) and (16,27).
    {
        static constexpr u32 kColX[4] = {10, 16, 27, 33};
        for (u32 k = 0; k < 4; k++)
            for (u32 ci = 0; ci < 4; ci++) {
                u32 ox, oz;
                rotCell(kColX[ci], 2, k, ox, oz);
                solid(ox, oz, 1, 1, stone);
            }
    }

    // Wall-midpoint JUMP PADS: pit -> balcony (launch apex 3.6 m; air-steer onto the 3.0 m band
    // edge). Never ON or UNDER a slab — a pad launch must own its full arc.
    pad(21,  4, 2, 2, 0); pad(38, 21, 2, 2, 0);
    pad(21, 38, 2, 2, 0); pad( 4, 21, 2, 2, 0);

    // --- CENTER: tower + crown (solid-riser tiers, as before, shifted to the 44x44 centre).
    // The crown now sits at BALCONY height so the two commanding vantages duel across the map.
    raise(19, 19, 6, 6, 6);            // tower @ 1.5 m, reached by the four ramps
    pad(20, 20, 4, 4, 6);              // crown launch-ring (12 pad cells after the crown overwrite)
    raise(21, 21, 2, 2, 12);           // crown @ 3.0 m — level with the sniper balcony
    ramp(25, 21,  1,  0, 0,  1, 6, 6); // east  (x 25..30)
    ramp(18, 21, -1,  0, 0,  1, 6, 6); // west  (x 18..13)
    ramp(21, 18,  0, -1, 1,  0, 6, 6); // north (z 18..13)
    ramp(21, 25,  0,  1, 1,  0, 6, 6); // south (z 25..30)

    // --- Pit cover: one crate cluster per diagonal quadrant (mirror pairs 10 <-> 32) -----------
    solid(10, 10, 2, 2, plank); solid(32, 10, 2, 2, plank);
    solid(10, 32, 2, 2, plank); solid(32, 32, 2, 2, plank);

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
    for (u32 i = 0; i < ARENA_FEED_LINES; i++) m_arenaFeed[i] = ArenaFeedEntry{};

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
        // A host can reach the arena WITHOUT startGame (Continue hero, --arena dev door) —
        // wire the server callbacks or joiners connect but never get seated (no onPlayerJoin).
        wireServerNet();
        Net::broadcastLevelSeed(GameConst::ARENA_SENTINEL_FLOOR, m_difficulty, m_level.levelSeed);
        Server::updateLevel(m_level.levelSeed, GameConst::ARENA_SENTINEL_FLOOR, m_difficulty);
    }
    LOG_INFO("Entered the ARENA (host) — first to %u.", Arena::KILL_TARGET);
}

// Client: mirror of enterArena, driven by the sentinel-floor SV_LEVEL_SEED / join-accept.
// Our real position/score arrive from the server (snapshot + ARENA_SCORES).
void Engine::enterArenaClient() {
    // Join-accept routed here INSTEAD of startGame, so the client-side net wiring (snapshot,
    // SV_EVENT, clock sync) must happen here or this client is connected but deaf.
    wireClientNet();
    enterArenaCommon();
    LOG_INFO("Entered the ARENA (client).");
}

// ---------------------------------------------------------------------------------------------
// The deathmatch loop — kill credit, auto-respawn, match end. Authority-only except where
// noted (the client mirrors score/feed/over via the ARENA_* events, engine.cpp onEvent).
// ---------------------------------------------------------------------------------------------

#include "audio/audio.h"
#include "net/packet.h"
#include <cstring>

extern Engine* s_engine;

// Push one line into the kill-feed ring (newest first). Runs on every machine: the authority
// pushes directly from arenaHandleDeath, clients from the ARENA_KILL event.
void Engine::arenaPushFeed(u8 killerSlot, u8 victimSlot) {
    for (u32 i = ARENA_FEED_LINES - 1; i > 0; i--) m_arenaFeed[i] = m_arenaFeed[i - 1];
    m_arenaFeed[0] = {killerSlot, victimSlot, 4.0f};
}

// SERVER: ship the full per-slot score table — the mid-join refresh (join-accept is a fixed
// legacy layout, so scores ride this additive event instead). toSlot 0xFF = broadcast.
void Engine::arenaSendScores(u8 toSlot) {
    if (m_netRole != NetRole::SERVER) return;
    u8 buf[sizeof(PacketHeader) + 1 + MAX_PLAYERS];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type = NetPacketType::SV_EVENT;
    hdr->flags = 0;
    hdr->seq = 0;
    u32 off = sizeof(PacketHeader);
    buf[off++] = static_cast<u8>(NetEventType::ARENA_SCORES);
    for (u32 i = 0; i < MAX_PLAYERS; i++)
        buf[off++] = static_cast<u8>(m_arenaScore.kills[i] > 255 ? 255 : m_arenaScore.kills[i]);
    if (toSlot == 0xFF) Net::broadcastReliable(buf, off);
    else                Net::sendReliable(toSlot, buf, off);
}

// AUTHORITY: a combatant died. Credits the killer, feeds every peer's HUD, starts the victim's
// respawn clock, and ends the match on the KILL_TARGET-th kill. Death sites (the serverNetPost
// remote check and the local-lane death path) call this exactly once per death.
void Engine::arenaHandleDeath(u8 victimSlot, u8 killerSlot) {
    if (!m_level.inArena || m_arenaOverTimer > 0.0f) return;
    m_arenaRespawn[victimSlot] = Arena::RESPAWN_DELAY;
    arenaPushFeed(killerSlot, victimSlot);
    u8 winner = 0xFF;
    bool over = Arena::recordKill(m_arenaScore, killerSlot, winner);
    LOG_INFO("Arena: player %u slew player %u (%u/%u)", killerSlot, victimSlot,
             killerSlot < MAX_PLAYERS ? m_arenaScore.kills[killerSlot] : 0, Arena::KILL_TARGET);
    if (m_netRole == NetRole::SERVER && killerSlot < MAX_PLAYERS) {
        u8 buf[sizeof(PacketHeader) + 4];
        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
        hdr->type = NetPacketType::SV_EVENT;
        hdr->flags = 0;
        hdr->seq = 0;
        u32 off = sizeof(PacketHeader);
        buf[off++] = static_cast<u8>(NetEventType::ARENA_KILL);
        buf[off++] = killerSlot;
        buf[off++] = victimSlot;
        buf[off++] = static_cast<u8>(m_arenaScore.kills[killerSlot]);
        Net::broadcastReliable(buf, off);
    }
    if (over) beginArenaOver(winner);
}

// AUTHORITY: revive a slot on the pad farthest from its living enemies (the handleRespawnRequest
// field-set: full HP, zeroed velocity, 1.5 s spawn protection).
void Engine::arenaRespawnSlot(u8 slot) {
    Vec3 hostiles[MAX_PLAYERS];
    u32  hostileCount = 0;
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (pi == slot) continue;
        if (pi < m_splitPlayerCount) {
            if (!m_playerDead[pi]) hostiles[hostileCount++] = m_localPlayers[pi].position;
        } else if (m_players[pi].active && !m_players[pi].isDead) {
            hostiles[hostileCount++] = m_players[pi].position;
        }
    }
    u32 pad = Arena::farthestPad(kArenaPads, MAX_PLAYERS, hostiles, hostileCount);
    Vec3 pos = kArenaPads[pad];
    f32  yaw = padYawToCenter(pos);

    NetPlayer& np = m_players[slot];
    if (np.active) {
        np.health        = np.maxHealth;
        np.position      = pos;
        np.spawnPosition = pos;
        np.velocity      = {0, 0, 0};
        np.yaw           = yaw;
        np.invulnTimer   = 1.5f;
        np.isDead        = false;
        np.lastHitByPlayerSlot = 0xFF;
    }
    if (slot < m_splitPlayerCount) {
        Player& lane = m_localPlayers[slot];
        lane.health        = lane.maxHealth;
        lane.position      = pos;
        lane.velocity      = {0, 0, 0};
        lane.yaw           = yaw;
        lane.pitch         = 0.0f;
        lane.invulnTimer   = 1.5f;
        lane.hurtVignette  = 0.0f;
        lane.damageFlashTimer = 0.0f;
        lane.lastHitByPlayerSlot = 0xFF;
        m_playerDead[slot] = false;
        // Outside-swap rule: if this lane is the current alias, refresh it too.
        if (slot == m_localPlayerIndex) m_localPlayer = lane;
    }
}

// Per-tick arena bookkeeping, every role. Called from update() right after the PvP window
// closes. Authority ticks respawn clocks; every peer ticks the match-over banner and tears
// itself down when it expires (each peer was told via ARENA_OVER — nobody waits on anybody).
void Engine::arenaTick(f32 dt) {
    if (!m_level.inArena || m_gameState != GameState::IN_GAME) return;

    // Kill-feed TTLs decay everywhere (render-side data, engine-side clock).
    for (u32 i = 0; i < ARENA_FEED_LINES; i++)
        if (m_arenaFeed[i].ttl > 0.0f) m_arenaFeed[i].ttl -= dt;

    if (m_arenaOverTimer > 0.0f) {
        m_arenaOverTimer -= dt;
        if (m_arenaOverTimer <= 0.0f) arenaLeaveToMenu();
        return;   // match decided: no respawns, scores frozen
    }

    if (m_netRole == NetRole::CLIENT) {
        // Cosmetic countdown for OUR OWN death (the server's clock is authoritative; ours just
        // feeds the "Respawning in N" overlay). Started when we first see ourselves dead.
        u8 slot = activeNetSlot();
        if (m_playerDead[m_localPlayerIndex]) {
            if (m_arenaRespawn[slot] <= 0.0f) m_arenaRespawn[slot] = Arena::RESPAWN_DELAY;
            m_arenaRespawn[slot] -= dt;
        } else {
            m_arenaRespawn[slot] = 0.0f;
        }
        return;
    }

    // AUTHORITY: tick every dead combatant's clock and revive at zero.
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (m_arenaRespawn[pi] <= 0.0f) continue;
        bool exists = (pi < m_splitPlayerCount) || m_players[pi].active;
        if (!exists) { m_arenaRespawn[pi] = 0.0f; continue; }   // combatant left mid-death
        m_arenaRespawn[pi] -= dt;
        if (m_arenaRespawn[pi] <= 0.0f) arenaRespawnSlot(static_cast<u8>(pi));
    }
}

// Match decided. Broadcast FIRST, then flip local state — the CREDITS ordering rule: a
// host-local flip that stops the world before the packet leaves is how clients hang.
void Engine::beginArenaOver(u8 winner) {
    if (m_netRole == NetRole::SERVER) {
        u8 buf[sizeof(PacketHeader) + 2 + MAX_PLAYERS];
        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
        hdr->type = NetPacketType::SV_EVENT;
        hdr->flags = 0;
        hdr->seq = 0;
        u32 off = sizeof(PacketHeader);
        buf[off++] = static_cast<u8>(NetEventType::ARENA_OVER);
        buf[off++] = winner;
        for (u32 i = 0; i < MAX_PLAYERS; i++)
            buf[off++] = static_cast<u8>(m_arenaScore.kills[i] > 255 ? 255 : m_arenaScore.kills[i]);
        Net::broadcastReliable(buf, off);
    }
    m_arenaWinner    = winner;
    m_arenaOverTimer = 8.0f;
    LOG_INFO("Arena: match over — player %u wins.", winner);
}

// Tear down to the main menu (the pause-quit path MINUS the save — the arena never saves;
// a character leaves exactly as it entered). Runs independently on every peer.
void Engine::arenaLeaveToMenu() {
    m_menu.confirmQuit      = false;
    m_menu.optionsFromPause = false;
    if (m_netRole != NetRole::NONE) {
        Net::disconnect();
        m_netRole = NetRole::NONE;
    }
    m_level.inArena    = false;
    m_menu.arena       = false;
    m_splitPlayerCount = 1;
    Input::setSplitScreen(false);
    m_gameState = GameState::MENU;
    m_menu.subState    = 0;
    m_menu.selection   = 0;
    AudioSystem::stopMusic();
    Input::setRelativeMouseMode(false);
}
