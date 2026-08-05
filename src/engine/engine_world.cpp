// engine_world.cpp — the shared ritual every NON-startGame world entry has to perform.
//
// The engine has four ways into a world that bypass startGame: the town (sentinel floor 98), the PvP
// arena (97), The Source (99) and — as of the overworld — the wilderness zones (52-96). Each one has
// to reset the pools, build its level, clear the LevelState flags, place the players, persist the lane
// alias, re-wire the net callbacks and broadcast the sentinel seed. That ritual was COPY-PASTED four
// times, and every single omission of a step has shipped as a bug:
//
//   * missing wireServerNet   -> a joiner connected but was never seated (enterTown, fixed 2026-07)
//   * missing wireClientNet   -> a joining client was "connected but deaf" (enterTownClient, same)
//   * missing host-slot seed  -> the host spawned inside the border wall with its HP reset
//                               (enterArena, "arena spawns out of bounds")
//   * missing lane persist    -> the next frame's swapInPlayer restored the STALE position, putting
//                               the hero outside the freshly built world (enterSourceChamber)
//
// Three of those four are the same class of mistake: a step that only matters on a path nobody tested,
// so it looks fine until someone reaches the world the other way. Adding a fifth entry point (zones)
// without collapsing the duplication would just multiply the trap, so the steps live here once.
//
// This is deliberately a SET OF SMALL HELPERS rather than one enterWorld(callback) monolith: the four
// worlds genuinely differ in how they place players (the arena seats each slot on its own numbered pad,
// the town lines everyone up at the south gate, The Source drops them at the centre), and threading
// that through a callback would be more indirection than the two lines it saves. What must never
// diverge is the ORDER and the COMPLETENESS of the surrounding steps, and that is what these capture.

#include "engine/engine.h"
#include "game/enemy_ai.h"
#include "platform/input.h"
#include "net/net.h"
#include "net/server.h"
#include "core/log.h"
#include "world/level_gen.h"

// Step 1 — every world entry starts from empty pools. Entities, projectiles and world items all
// belong to the world being left; carrying any of them across is how a boss's minion or a stale loot
// drop ends up standing in the town square.
void Engine::worldResetPools() {
    EntitySystem::init(m_entities);
    ProjectileSystem::init(m_projectiles);
    WorldItemSystem::init(m_worldItems);
}

// Step 2 — clear every "which special world am I in" flag. Callers set their own afterwards. Keeping
// the clear in one place means a new flag is turned OFF by all four entries the day it is added,
// instead of leaking into whichever world forgot it (the exit portal staying live in the town was
// exactly this).
void Engine::worldClearLevelFlags() {
    m_level.inTown             = false;
    m_level.inArena            = false;
    m_level.inSourceChamber    = false;
    m_level.inZone             = false;
    m_level.zoneFloor          = 0;
    m_level.floorDoorActive    = false;
    m_level.floorHasBoss       = false;
    m_level.sourcePortalActive = false;
    m_level.exitPortalActive   = false;
    // The TOWN's to-dungeon portal. It was cleared ad-hoc in startGame and enterArena but not here,
    // so it survived every entry that goes through this function — walk town -> overworld zone and
    // the dungeon portal came WITH you, rendered and interactable, in every zone of both acts
    // (Aaron: "there is an entrance to the dungeon in every area"). Taking it would have launched a
    // dungeon run from inside Act 1.
    //
    // This is the second time this exact flag-leak has bitten (the comment above already cites the
    // exit portal staying live in the town), which is the argument for the single clear: enterTown
    // re-asserts it a few lines later, and anything that does NOT re-assert it gets it off for free.
    m_level.townPortalActive   = false;

    // The HELLFORGE surcharge. lavaFloor is cleared ONLY by startGame, and it feeds
    // hellforgeHpMult/hellforgeDamageMult inside spawnFloorEnemies — the same spawn path an
    // overworld zone uses. So arriving in the acts after a molten dungeon floor silently handed
    // every zone enemy +50% HP and +30% damage, intermittently, depending on where you had been.
    // Exactly the shape of the townPortalActive leak above, and the reason this clear exists.

    // The HELLFORGE surcharge. lavaFloor is cleared ONLY by startGame, and it feeds
    // hellforgeHpMult/hellforgeDamageMult inside spawnFloorEnemies — the same spawn path an
    // overworld zone uses. So arriving in the acts after a molten dungeon floor silently handed
    // every zone enemy +50% HP and +30% damage, intermittently, depending on where you had been.
    // Measured: hpMult 3038 -> 4557 in a zone. Same shape as the townPortalActive leak above.
    m_level.lavaFloor          = false;

    // Layout style likewise belongs to the world you are IN. Left stale, a zone inherits the last
    // dungeon floor's style, and consumers read it: the AI gives CAVERN-style open floors a x1.5
    // detection bubble, and the autoplay/nav paths branch on the stacked styles. buildZoneLevel
    // records the style it actually generated (below); this is the neutral default for the town,
    // the arena and the Source chamber, none of which are dungeon layouts.
    m_level.layoutStyle        = LevelGen::LayoutStyle::BSP_ROOMS;
}

// Step 3 — THE BUG FIX, and the reason this file exists.
//
// startGame is the only path that activates and seeds the host's own NetPlayer slot (onPlayerJoin
// refuses slot 0 by design). Reaching a world any other way leaves m_players[activeNetSlot()] a
// default NetPlayer{}: inactive, position {0,0,0}, health 100. Every seating loop is gated on
// `active`, so it SKIPS the host, and the first gameUpdate's syncNetPlayerToLocalPlayer then copies
// {0,0,0}/health-100 over the placement — wedging the host in the border wall and resetting a
// Continue'd hero's HP.
//
// enterArena carries a 14-line comment about this; enterTown never got the fix and is saved only by
// luck (the menu happens to call startGame immediately before it). Called unconditionally by every
// entry now. Idempotent: a re-assert when a prior startGame already activated the slot is harmless,
// because the values come from m_localPlayer, which the caller has just placed.
void Engine::worldSeedHostSlot() {
    if (m_netRole != NetRole::SERVER) return;
    const u8   hostSlot = activeNetSlot();
    NetPlayer& host     = m_players[hostSlot];
    host.active        = true;
    host.slotIndex     = hostSlot;
    host.playerClass   = m_playerClasses[m_localPlayerIndex];
    host.baseMaxHealth = m_localPlayer.baseMaxHealth;
    host.maxHealth     = m_localPlayer.maxHealth;
    host.health        = m_localPlayer.health;
    host.moveSpeed     = m_localPlayer.moveSpeed;
    host.weaponState.currentWeapon = 0;
}

// Step 4 — place the local player(s) and PERSIST THE LANE ALIAS.
//
// m_localPlayer is a swap alias: every one of these entry points runs OUTSIDE the per-player pass, so
// a write to the alias alone is erased by the next frame's swapInPlayer, which restores the stale
// pre-transition position — in a freshly built grid that is usually outside the world. The explicit
// m_localPlayers[...] write is what makes the placement stick.
void Engine::worldPlaceLocalPlayers(Vec3 base, f32 yaw) {
    m_localPlayer.position    = base;
    m_localPlayer.yaw         = yaw;
    m_localPlayer.pitch       = 0.0f;
    m_localPlayer.invulnTimer = 1.0f;
    for (u8 lane = 0; lane < m_splitPlayerCount && lane < MAX_LOCAL_PLAYERS; lane++) {
        if (lane == m_localPlayerIndex) continue;
        m_localPlayers[lane].position    = base + Vec3{static_cast<f32>(lane) * 1.6f, 0.0f, 0.0f};
        m_localPlayers[lane].yaw         = yaw;
        m_localPlayers[lane].pitch       = 0.0f;
        m_localPlayers[lane].invulnTimer = 1.0f;
    }
    m_localPlayers[m_localPlayerIndex] = m_localPlayer;   // the persist — see the comment above
    snapCameraToPlayer();
}

// Step 5 — seat every active NetPlayer near `base`, and make that their RESPAWN anchor.
//
// spawnPosition matters as much as position: it is where every revive path teleports to, and it is
// otherwise written only by startGame. The Source shipped without it, which meant dying in the secret
// boss fight put a player outside the world with no way back.
void Engine::worldSeatNetPlayers(Vec3 base) {
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (!m_players[pi].active) continue;
        m_players[pi].position      = base + Vec3{static_cast<f32>(pi) * 1.2f - 0.6f, 0.0f, 0.0f};
        m_players[pi].spawnPosition = m_players[pi].position;
        m_players[pi].invulnTimer   = 1.0f;
        m_players[pi].isDead        = false;
    }
}

// Step 6 — finish the entry: peace/hostility mode, game state, and the net wiring + seed broadcast.
//
// wireServerNet/wireClientNet are idempotent, so calling them on the mid-session route (already wired)
// costs nothing; what they buy is the COLD route — a host that reached this world without startGame
// can still seat joiners, and a client that was routed here by SV_LEVEL_SEED instead of startGame is
// not left deaf. `sentinelFloor` is the world's identity on the wire; clients rebuild from it alone.
void Engine::worldFinishEntry(u8 sentinelFloor, bool peaceful) {
    EnemyAI::setTownMode(peaceful);
    m_gameState = GameState::IN_GAME;
    Input::setRelativeMouseMode(true);

    if (m_netRole == NetRole::SERVER) {
        wireServerNet();
        Net::broadcastLevelSeed(sentinelFloor, m_difficulty, m_level.levelSeed);
        Server::updateLevel(m_level.levelSeed, sentinelFloor, m_difficulty);
    } else if (m_netRole == NetRole::CLIENT) {
        wireClientNet();
    }
}
