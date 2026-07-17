// engine_arena.cpp — the PvP ARENA (Arena mode, sentinel floor 97).
//
// A deterministic open-sky colosseum where players fight each other: FFA deathmatch,
// first to Arena::KILL_TARGET kills, 3 s auto-respawn (rules: game/arena.h). Built on the
// engine_town.cpp pattern — same sentinel-floor rails (host broadcasts SV_LEVEL_SEED with
// ARENA_SENTINEL_FLOOR, clients build the identical arena), same daylight rendering branch.
//
// The layout is a Quake/Combat-Hall-style multi-tier deathmatch map, symmetric under both diagonal
// mirrors so no spawn corner is favored. Combat/LOS/raycast/DDA all read the grid geometry with zero
// new systems; the vertical play rides two opt-in cell flags (CELL_LEDGE gates, CELL_JUMPPAD launches):
//   - TIER 0 (ground pit): four corner spawn bays, each with a crate for cover, and one glowing
//     JUMP PAD per diagonal quadrant on the run-in from spawn.
//   - TIER 1 (1.5 m tower): a central 6x6 platform reached by four cardinal LEDGE ramps (pit->tower).
//   - TIER 2 (3.0 m crown): a 2x2 pinnacle at the very centre — the commanding vantage — ringed by
//     jump pads so it's reachable only by being flung up (two-stage ascent) or a pad arc.
// Jump pads fling a grounded body upward (Collision::JUMPPAD_LAUNCH, a velocity.y impulse like the
// jump — see CELL_JUMPPAD), so they replicate in co-op with no wire change. Enemies never jump, hence
// pads are PvP-only (the boss "arenas" in engine_spawn.cpp use plain walkable tiers instead).
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

    // COMBAT-HALL vertical layout (Quake / Metroid-Prime-Hunters style). Three tiers connected by
    // walkable ramps and jump pads. Every placement is symmetric under x->35-x, z->35-z AND the
    // diagonal swap, so all four spawn corners face identical geometry — no corner is favoured.
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

    // --- TIER 1+2: the central tower and its crown ---------------------------------------------
    // Tower: a 6x6 platform at 1.5 m (qh 6). Its outer frame is the walkable high ground reached by
    // the four ramps; the inner cells become the crown launch-ring below.
    raise(15, 15, 6, 6, 6);
    // Crown launch-ring: the inner 4x4 of the tower is JUMP PADS at 1.5 m — you can't walk up to the
    // crown, you step onto a pad and get flung onto it (the two-stage ascent). The centre 2x2 is
    // overwritten by the crown next, leaving a 12-cell ring of pads hugging the crown's base.
    pad(16, 16, 4, 4, 6);
    // Crown: a 2x2 pinnacle at 3.0 m (qh 12) — the map's commanding vantage. Too high to jump or ramp
    // to (2x the walk-jump apex); reachable ONLY by the ring pads around its base or a pad arc.
    raise(17, 17, 2, 2, 12);

    // --- Four cardinal ramps: pit -> tower (2 lanes wide, on the centre lanes 17/18) ------------
    ramp(21, 17,  1,  0, 0,  1, 6, 6);   // east  (x 21..26)
    ramp(14, 17, -1,  0, 0,  1, 6, 6);   // west  (x 14..9)
    ramp(17, 14,  0, -1, 1,  0, 6, 6);   // north (z 14..9)
    ramp(17, 21,  0,  1, 1,  0, 6, 6);   // south (z 21..26)

    // --- TIER 0: ground pads + cover ------------------------------------------------------------
    // One 2x2 launch pad per diagonal quadrant, on the spawn->centre path: run in from your corner
    // and pad up onto the tower frame (or arc for the crown).
    pad(10, 10, 2, 2, 0); pad(24, 10, 2, 2, 0);
    pad(10, 24, 2, 2, 0); pad(24, 24, 2, 2, 0);
    // Crate clusters: one 2x2 plank block per quadrant, shielding its spawn pad from the centre.
    solid( 8,  8, 2, 2, plank); solid(26,  8, 2, 2, plank);
    solid( 8, 26, 2, 2, plank); solid(26, 26, 2, 2, plank);

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
