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
#include "audio/audio.h" // AudioSystem::play / SfxId for client descent cue (onLevelSeed)
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

FrameAllocator s_frameAllocator;

// Global engine pointer — extern'd by split engine_*.cpp files
Engine* s_engine = nullptr;

// First-kill guaranteed drop flag (reset each floor in startGame)
bool s_firstKillDropGiven = false;


// ---------------------------------------------------------------------------
// Net callbacks (static — forwarded to engine)
// ---------------------------------------------------------------------------
void Engine::onSnapshot(const u8* data, u32 size) {
    Client::receiveSnapshot(data, size);
}

void Engine::onInput(u8 playerSlot, const u8* data, u32 size) {
    Server::receiveInput(playerSlot, data, size);
}

// Server-side CL_PICKUP_ITEM dispatch (N5). Decodes the requested uid and forwards to
// the instance handler, which validates proximity/ownership against authoritative state.
void Engine::onPickup(u8 playerSlot, const u8* data, u32 size) {
    if (!s_engine) return;
    if (s_engine->m_netRole != NetRole::SERVER) return; // only the host services pickups
    if (size < sizeof(PacketHeader) + 4) return;        // header(4) + uid(4)
    u32 uid;
    std::memcpy(&uid, data + sizeof(PacketHeader), 4);
    s_engine->handlePickupRequest(playerSlot, uid);
}

void Engine::onRespawn(u8 playerSlot) {
    if (!s_engine) return;
    if (s_engine->m_netRole != NetRole::SERVER) return; // only the host services respawns
    s_engine->handleRespawnRequest(playerSlot);
}

// Server-side CL_REQUEST_DESCEND dispatch. The instance handler re-validates the request
// (proximity to the floor door + boss-dead gate) and then runs the shared descent flow,
// which broadcasts SV_LEVEL_SEED to every client.
void Engine::onDescendRequest(u8 playerSlot) {
    if (!s_engine) return;
    if (s_engine->m_netRole != NetRole::SERVER) return; // only the host services descent requests
    s_engine->handleDescendRequest(playerSlot);
}

// Server-side CL_FIRE_WEAPON dispatch. Unpacks the payload (clientTick + claimed origin
// + yaw/pitch) and queues the fire request for the next per-tick handleWeaponFireForPlayer
// pass to consume — the client's claimed aim is what the projectile/hitscan/melee fires
// from, eliminating the prior "fires from drain-derived np.yaw stale by seconds" bug.
void Engine::onFireWeapon(u8 playerSlot, const u8* data, u32 size) {
    if (!s_engine) return;
    if (s_engine->m_netRole != NetRole::SERVER) return; // only the host fires authoritatively
    if (size < sizeof(PacketHeader) + 14) return;        // header(4) + tick(4) + pos×3(6) + yaw+pitch(4)
    u32 off = sizeof(PacketHeader);
    u32 clientTick;
    std::memcpy(&clientTick, data + off, 4); off += 4;
    u16 posXQ, posYQ, posZQ;
    std::memcpy(&posXQ, data + off, 2); off += 2;
    std::memcpy(&posYQ, data + off, 2); off += 2;
    std::memcpy(&posZQ, data + off, 2); off += 2;
    u16 yawQ, pitchQ;
    std::memcpy(&yawQ,   data + off, 2); off += 2;
    std::memcpy(&pitchQ, data + off, 2); off += 2;
    Vec3 claimedOrigin = {Quantize::unpackPos(posXQ),
                          Quantize::unpackPos(posYQ),
                          Quantize::unpackPos(posZQ)};
    f32 claimedYaw   = Quantize::unpackAngle(yawQ);
    f32 claimedPitch = Quantize::unpackAngle(pitchQ);
    s_engine->handleFireWeaponRequest(playerSlot, clientTick,
                                      claimedOrigin, claimedYaw, claimedPitch);
}

// ---------------------------------------------------------------------------
// Inventory sync (Continue-join). Wire layout mirrors engine_persist.cpp's
// per-player save block so we can simply memcpy the trivially-copyable structs.
// Versioned so future struct changes can be rejected cleanly.
// ---------------------------------------------------------------------------
static constexpr u32 INVENTORY_SYNC_VERSION = 1;

namespace {
struct InventorySyncBody {
    u32             version;
    PlayerInventory inv;
    QuickbarState   qb;
    SkillState      energySkill;     // m_skillStates[slot]
    u8              cls;             // PlayerClass
    u8              activeClassSkill;
    SkillState      classSkills[4];
    f32             health;
    f32             maxHealth;
};
} // anonymous

void Engine::sendInventorySync() {
    if (m_netRole != NetRole::CLIENT) return;
    // Build the body from the LOCAL split-screen lane 0 — the joiner is always lane 0
    // on a client (split-screen is disabled when networking).
    InventorySyncBody body{};
    body.version          = INVENTORY_SYNC_VERSION;
    body.inv              = m_inventories[m_localPlayerIndex];
    body.qb               = m_quickbars[m_localPlayerIndex];
    body.energySkill      = m_skillStates[m_localPlayerIndex];
    body.cls              = static_cast<u8>(m_playerClasses[m_localPlayerIndex]);
    body.activeClassSkill = m_activeClassSkills[m_localPlayerIndex];
    std::memcpy(body.classSkills, m_classSkillStatesPerPlayer[m_localPlayerIndex],
                sizeof(body.classSkills));
    body.health           = m_localPlayer.health;
    body.maxHealth        = m_localPlayer.maxHealth;

    // Total wire size: PacketHeader(4) + body(~sizeof(InventorySyncBody)).
    constexpr u32 totalSize = sizeof(PacketHeader) + sizeof(InventorySyncBody);
    u8 buf[totalSize];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_INVENTORY_SYNC;
    hdr->flags = 0;
    hdr->seq   = 0;
    std::memcpy(buf + sizeof(PacketHeader), &body, sizeof(body));
    Net::sendToServer(buf, totalSize, /*reliable=*/true);
    LOG_INFO("CL_INVENTORY_SYNC: sent %u B (class=%u hp=%.0f/%.0f)",
             totalSize, body.cls, body.health, body.maxHealth);
}

void Engine::onInventorySync(u8 playerSlot, const u8* data, u32 size) {
    if (!s_engine) return;
    if (s_engine->m_netRole != NetRole::SERVER) return;
    if (playerSlot == 0 || playerSlot >= MAX_PLAYERS) return;     // host never receives this
    if (size < sizeof(PacketHeader) + sizeof(InventorySyncBody)) {
        LOG_WARN("CL_INVENTORY_SYNC slot %u: short packet (%u B)", playerSlot, size);
        return;
    }
    InventorySyncBody body{};
    std::memcpy(&body, data + sizeof(PacketHeader), sizeof(body));
    if (body.version != INVENTORY_SYNC_VERSION) {
        LOG_WARN("CL_INVENTORY_SYNC slot %u: version mismatch %u vs %u — ignored",
                 playerSlot, body.version, INVENTORY_SYNC_VERSION);
        return;
    }

    // Adopt as-is. Trust model: client-authoritative inventory on join, mirroring
    // client-authoritative position. Anti-cheat would validate item-def IDs / damage
    // bounds here; out of scope for coop.
    s_engine->m_inventories[playerSlot] = body.inv;
    Inventory::recalculateStats(s_engine->m_inventories[playerSlot]); // rebuild bonus cache
    s_engine->m_quickbars[playerSlot]   = body.qb;
    s_engine->m_skillStates[playerSlot] = body.energySkill;

    NetPlayer& np = s_engine->m_players[playerSlot];
    if (body.cls < static_cast<u8>(PlayerClass::CLASS_COUNT))
        np.playerClass = static_cast<PlayerClass>(body.cls);
    np.maxHealth = body.maxHealth;
    np.health    = body.health;
    // Resolve the equipped weapon's mesh id so the snapshot's per-player weaponMeshId
    // reflects the loaded weapon on the next snap (serverNetPost line that maps from
    // m_inventories[*].equipped[WEAPON].defId → m_itemDefs[*].meshId).
    const ItemInstance& eqWpn = s_engine->m_inventories[playerSlot].equipped[
        static_cast<u32>(ItemSlot::WEAPON)];
    np.weaponState.weaponMeshId = (!isItemEmpty(eqWpn) && eqWpn.defId < s_engine->m_itemDefCount)
                                  ? s_engine->m_itemDefs[eqWpn.defId].meshId : 0;
    LOG_INFO("CL_INVENTORY_SYNC slot=%u adopted: class=%u hp=%.0f/%.0f",
             playerSlot, body.cls, body.health, body.maxHealth);
}

void Engine::onEvent(const u8* data, u32 size) {
    if (!s_engine) return;
    // SV_EVENT is a server→client packet. The host wires `Net::setOnEvent` only on
    // CLIENT (engine_startgame.cpp), so it can't physically receive its own broadcast
    // today — but make the contract explicit so a future regression doesn't quietly
    // double-spend an event handler on the host.
    if (s_engine->m_netRole != NetRole::CLIENT) return;
    if (size < sizeof(PacketHeader) + 1) return;

    u8 eventType = data[sizeof(PacketHeader)];
    switch (static_cast<NetEventType>(eventType)) {
        case NetEventType::HITSCAN_IMPACT: {
            // Remote player hitscan hit — spawn local impact spark with position + normal
            if (size < sizeof(PacketHeader) + 26) break;
            u32 off = sizeof(PacketHeader) + 1;
            Vec3 pos, nrm;
            std::memcpy(&pos.x, data + off, 4); off += 4;
            std::memcpy(&pos.y, data + off, 4); off += 4;
            std::memcpy(&pos.z, data + off, 4); off += 4;
            std::memcpy(&nrm.x, data + off, 4); off += 4;
            std::memcpy(&nrm.y, data + off, 4); off += 4;
            std::memcpy(&nrm.z, data + off, 4); off += 4;
            bool hitEntity = data[off] != 0;
            for (u32 fx = 0; fx < MAX_IMPACT_FX; fx++) {
                if (!s_engine->m_fx.impactFX[fx].active) {
                    s_engine->m_fx.impactFX[fx] = {pos, nrm, 0.3f, true, hitEntity};
                    break;
                }
            }
        } break;
        case NetEventType::DAMAGE_NUMBER: {
            // Host-replicated damage/heal number — unpack and spawn locally on the client.
            // Engine::spawnDamageNumber's broadcast branch is gated to NetRole::SERVER, so
            // calling it here from a CLIENT receive handler doesn't re-broadcast.
            //
            // Size check: payload after the 4-byte PacketHeader is 18 bytes
            //   (eventType(1) + pos(12) + amount(4) + flags(1)). Total wire packet = 22 B.
            // The earlier `+ 1 + 18` here counted the eventType byte twice — `size < 23`
            // rejected every legitimate 22-byte packet. Match the HITSCAN_IMPACT pattern:
            // the post-header byte count INCLUDES the eventType byte.
            if (size < sizeof(PacketHeader) + 18) break;
            u32 off = sizeof(PacketHeader) + 1;
            Vec3 pos;
            std::memcpy(&pos.x, data + off, 4); off += 4;
            std::memcpy(&pos.y, data + off, 4); off += 4;
            std::memcpy(&pos.z, data + off, 4); off += 4;
            f32 amount;
            std::memcpy(&amount, data + off, 4); off += 4;
            u8 flagsByte = data[off];
            bool isHeal = (flagsByte & 1) != 0;
            bool isCrit = (flagsByte & 2) != 0;
            s_engine->spawnDamageNumber(pos, amount, isHeal, isCrit);
        } break;
        case NetEventType::SKILL_CD_RESET: {
            // Server-authoritative skill cooldown reset (Wanderer Exploit Weakness on
            // marked-enemy kill, etc.). Server doesn't own our m_classSkillStates, so it
            // ships the skill id and we zero whichever slot has it equipped. We zero
            // both the active alias (m_classSkillStates) and the persistent backing
            // (m_classSkillStatesPerPlayer[0]) since a client is always single-player
            // (split-screen is mutually exclusive with NetRole != NONE).
            if (size < sizeof(PacketHeader) + 1 + 2) break;
            u16 sidRaw;
            std::memcpy(&sidRaw, data + sizeof(PacketHeader) + 1, 2);
            SkillId sid = static_cast<SkillId>(sidRaw);
            for (u32 s = 0; s < 4; s++) {
                if (s_engine->m_classSkillStates[s].activeSkill == sid)
                    s_engine->m_classSkillStates[s].cooldownTimer = 0.0f;
                if (s_engine->m_classSkillStatesPerPlayer[0][s].activeSkill == sid)
                    s_engine->m_classSkillStatesPerPlayer[0][s].cooldownTimer = 0.0f;
            }
        } break;
        default: break;
    }
}

// Server-authoritative mid-run floor descent. The host has already advanced to the
// new floor; this drives the CLIENT into the same FLOOR_TRANSITION -> startGame(DESCEND)
// path the host ran locally, so it regenerates the IDENTICAL next dungeon from the
// shared per-run seed (levelSeed + floor*7919 + difficulty*104729). Floor/HP/inventory
// stay server-authoritative via snapshots; this only resyncs the LEVEL the client builds.
void Engine::onLevelSeed(u8 floor, u8 difficulty, u32 seed) {
    if (!s_engine) return;
    // Only a remote client follows a pushed descent. The host advances itself in
    // updateFloorDoor; a NONE role never receives net packets. Guarding here keeps the
    // host's local descend feel exactly as-is even though it set the same callback path.
    if (s_engine->m_netRole != NetRole::CLIENT) return;
    // Only adopt a level seed while we're actually in (or transitioning between) levels.
    // A stray SV_LEVEL_SEED arriving during MENU / LOBBY / CONNECTING / GAME_OVER would
    // jump straight into FLOOR_TRANSITION and 2 s later call startGame(DESCEND) against
    // uninitialized engine state. Reliable ordering + the host only broadcasting to
    // ACTIVE peers makes this hard to hit naturally, but a hostile/buggy server defeats
    // both — close the contract explicitly.
    if (s_engine->m_gameState != GameState::IN_GAME &&
        s_engine->m_gameState != GameState::FLOOR_TRANSITION) {
        LOG_WARN("Net: SV_LEVEL_SEED dropped — wrong game state");
        return;
    }
    // Ignore stale/duplicate descents (reliable channel shouldn't dup, but be safe).
    // Compare announced (floor, difficulty) against current — only act on a STRICTLY newer
    // descent. This both rejects a dup for the current floor and accepts a genuinely newer
    // descent that arrives mid-transition, instead of just gating on FLOOR_TRANSITION.
    const u32 announced = (static_cast<u32>(difficulty) << 8) | floor;
    const u32 current   = (static_cast<u32>(s_engine->m_difficulty) << 8)
                        | static_cast<u32>(s_engine->m_level.currentFloor);
    if (announced <= current) return;

    // Adopt the host's authoritative level coordinates. seed is normally unchanged
    // across floors, but trusting the server's value self-corrects a client that
    // somehow missed SV_JOIN_ACCEPT. Difficulty rises on the floor-50->1 loop.
    s_engine->m_level.levelSeed    = seed;
    s_engine->m_level.currentFloor = floor;
    s_engine->m_difficulty         = difficulty;
    s_engine->m_level.savedFloor   = floor;
    s_engine->m_level.savedSeed    = seed;

    // Enter the shared transition: the FLOOR_TRANSITION handler ticks the timer down
    // then calls startGame(GameStart::DESCEND), which rebuilds the level from the
    // coordinates set above. Same timer as the host's non-difficulty-loop descent.
    s_engine->m_transition.snapshotKills = s_engine->m_transition.floorKillCount;
    s_engine->m_transition.snapshotTime  = s_engine->m_transition.floorTime;
    s_engine->m_transition.timer = 2.0f;
    s_engine->m_gameState = GameState::FLOOR_TRANSITION;
    AudioSystem::play(SfxId::LEVEL_UP);
    LOG_INFO("Client following host descent to floor %u (diff=%u)", floor, difficulty);
}

void Engine::onPlayerJoin(u8 playerSlot, u8 classId) {
    if (!s_engine) return;
    if (playerSlot < MAX_PLAYERS) {
        // Ignore a duplicate/retransmitted JOIN for an already-active slot, and never
        // re-init the host (slot 0, set up in startGame) — either would wipe live state.
        if (playerSlot == 0 || s_engine->m_players[playerSlot].active) return;
        // Honor the class the joiner picked in their lobby (sent in CL_JOIN_REQUEST).
        // Anything out of range (or a pre-class-byte request that sends 0xFF) falls
        // back to Warrior so the server never indexes kClassDefs out of bounds.
        PlayerClass joinClass = (classId < static_cast<u8>(PlayerClass::CLASS_COUNT))
                                ? static_cast<PlayerClass>(classId)
                                : PlayerClass::WARRIOR;
        const ClassDef& cls = kClassDefs[static_cast<u32>(joinClass)];
        NetPlayer& np = s_engine->m_players[playerSlot];
        np.active = true;
        np.slotIndex = playerSlot;
        // Fresh input buffer for this slot — a previous occupant's stale high ticks would
        // otherwise freeze this joiner via the monotonic-tick guard (H6).
        Server::resetInputBuffer(playerSlot);
        // Phase 1.1 — Mirror the dedup ring reset for CL_FIRE_WEAPON. A previous occupant
        // could have left clientTicks in the ring that would now spuriously match the
        // joiner's first few fires and silently drop them.
        s_engine->resetFireDedup(playerSlot);
        // Health from the chosen class scaled by the host's per-floor growth, so a mid-run
        // joiner isn't dramatically under-statted on a deep floor. Use the host's
        // maxHealth/baseHealth ratio as the growth factor (self-corrects whatever formula the
        // run is using). Falls back to 1.0 if the host's class lookup is degenerate.
        const NetPlayer& host = s_engine->m_players[0];
        const ClassDef& hostCls = kClassDefs[static_cast<u32>(host.playerClass)];
        const f32 growth = (hostCls.baseHealth > 0.0f) ? (host.maxHealth / hostCls.baseHealth) : 1.0f;
        np.maxHealth = cls.baseHealth * (growth > 1.0f ? growth : 1.0f);
        np.health = np.maxHealth;
        np.position = s_engine->m_players[0].spawnPosition; // spawn at host's spawn
        np.spawnPosition = np.position;
        np.weaponState.currentWeapon = 0;
        np.weaponState.cooldownTimer = 0.0f;
        np.isDead = false;
        np.invulnTimer = 2.5f; // spawn protection
        np.playerClass = joinClass; // class chosen by the joining player
        // Seed moveSpeed from the joiner's class — NetPlayer's default 6.0 f isn't right for
        // every class, and `np.moveSpeed` is read by chain-damage attribution and (more
        // importantly) any future code that consults it. Mirrors the class-table read used
        // by m_localPlayer.moveSpeed at startup.
        np.moveSpeed = cls.baseMoveSpeed;
        // Defensive zero-init of new server-only fields. Today onPlayerLeft does
        // m_players[i] = NetPlayer{} so defaults already apply, but reactivating a slot
        // without going through onPlayerLeft (e.g. some future re-init path) would
        // leave Wanderer / ring-passive timers carrying values from the previous occupant.
        np.shadowDanceTimer  = 0.0f;
        np.markTimer         = 0.0f;
        np.markSpeedStacks   = 0;
        for (u32 ms = 0; ms < 20; ms++) np.markSpeedTimers[ms] = 0.0f;
        np.soulHarvestTimer  = 0.0f;
        np.soulHarvestStacks = 0;
        np.smokeTimer        = 0.0f;
        np.secondWindCooldown = 0.0f;
        np.lifesaverArmed    = true;  // one-shot near-death i-frame ready on (re)join

        // Initialize inventory, skill states, and quickbar for the new player
        Inventory::init(s_engine->m_inventories[playerSlot]);
        s_engine->m_skillStates[playerSlot] = SkillState{};
        // Seed the joiner's energy ceiling from its class so skills are usable on join.
        s_engine->m_skillStates[playerSlot].maxEnergy = cls.baseEnergy;
        s_engine->m_skillStates[playerSlot].energy    = cls.baseEnergy;
        s_engine->m_bootSkillStates[playerSlot] = SkillState{};
        s_engine->m_helmetSkillStates[playerSlot] = SkillState{};
        Quickbar::init(s_engine->m_quickbars[playerSlot], s_engine->m_inventories[playerSlot]);

        // Give the class's deterministic starting weapon (same logic as host startup,
        // now driven by the joiner's real class instead of a forced Warrior).
        for (u32 di = 0; di < s_engine->m_itemDefCount; di++) {
            if (std::strcmp(s_engine->m_itemDefs[di].name, cls.startingWeaponName) == 0) {
                ItemInstance startWpn{};
                startWpn.defId = static_cast<u16>(di);
                startWpn.damage = s_engine->m_itemDefs[di].baseDamage;
                startWpn.rarity = Rarity::COMMON;
                startWpn.itemLevel = 1;
                startWpn.uid = static_cast<u16>(std::rand());
                if (Inventory::addToBackpack(s_engine->m_inventories[playerSlot], startWpn)) {
                    Inventory::equip(s_engine->m_inventories[playerSlot], 0, s_engine->m_itemDefs);
                    Quickbar::syncWeaponSlot(s_engine->m_quickbars[playerSlot],
                                              s_engine->m_inventories[playerSlot]);
                }
                break;
            }
        }

        LOG_INFO("Engine: player %u joined, spawned at (%.1f, %.1f, %.1f)",
                 playerSlot, np.position.x, np.position.y, np.position.z);
    }
}

void Engine::onPlayerLeft(u8 playerSlot) {
    if (!s_engine) return;
    // Never reset slot 0 (the listen-server host) — it doesn't leave via this path,
    // and wiping it would destroy the authoritative host player.
    if (playerSlot > 0 && playerSlot < MAX_PLAYERS) {
        // Fully reset the slot's NetPlayer (back to default-constructed) so leftover
        // state — stale lock-on (lockActive/lockIndex), status timers, isDead, velocity —
        // can't linger while the slot is inactive or bleed into a future rejoin.
        // onPlayerJoin re-inits inventory/skills/quickbar separately, so a clean
        // NetPlayer here is sufficient for a correct rejoin.
        s_engine->m_players[playerSlot] = NetPlayer{};
        Server::resetInputBuffer(playerSlot); // don't leave stale ticks for the next occupant (H6)
        s_engine->resetFireDedup(playerSlot); // Phase 1.1: ditto for CL_FIRE_WEAPON dedup

        // Audit-#7 leaver sweep. Without this, a still-flying projectile / unclaimed
        // exclusive drop / pending meteor / friendly minion the leaver owned would
        // either credit a future rejoiner of the same net slot (loot ownership, on-kill
        // procs), or keep ticking in a phantom state. Each pool keys ownership on the
        // net slot we just freed.

        // 1) Projectiles owned by the leaver — orphan to FFA so a kill credits nobody.
        for (u32 pi = 0; pi < MAX_PROJECTILES; pi++) {
            Projectile& p = s_engine->m_projectiles.projectiles[pi];
            if (p.active && p.fromPlayer && p.ownerSlot == playerSlot) p.ownerSlot = 0xFF;
        }

        // 2) World items still inside the leaver's exclusive window — release to FFA so
        // remaining players can grab the loot instead of it ageing out untouched.
        for (u32 wi = 0; wi < MAX_WORLD_ITEMS; wi++) {
            WorldItem& w = s_engine->m_worldItems.items[wi];
            if (w.active && w.ownerSlot == playerSlot && w.exclusiveTimer > 0.0f) {
                w.ownerSlot = 0xFF;
                w.exclusiveTimer = 0.0f;
            }
        }

        // 3) Friendly minions tethered to the leaver — despawn instead of leaving them
        // following the host (which is what the N4 anchor fallback would now do).
        for (u32 ei = 0; ei < s_engine->m_entities.activeCount; ei++) {
            u32 idx = s_engine->m_entities.activeList[ei];
            Entity& e = s_engine->m_entities.entities[idx];
            if ((e.flags & ENT_FRIENDLY) && !(e.flags & ENT_DEAD) &&
                e.ownerNetSlot == playerSlot) {
                e.flags |= ENT_DEAD;
                e.deathTimer = 0.01f;
            }
        }

        // 4) SkillSystem statics (Holy Bombardment / Holy Nova / Overcharge / pending
        // meteors credited to this slot).
        SkillSystem::resetSlotState(playerSlot);

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

        // Poll input once per rendered frame — decoupled from physics tick rate
        Input::update();
        m_accumulator += frameTime;
        m_firstTick = true;
        while (m_accumulator >= FIXED_DT) {
            update(static_cast<f32>(FIXED_DT));
            m_accumulator -= FIXED_DT;
            m_updateCount++;
            // After first tick, consume pressed edges so isKeyPressed/isActionPressed
            // return false on subsequent ticks. Fixes all multi-fire input bugs.
            if (m_firstTick) {
                Input::consumePressedState();
                m_firstTick = false;
            }
        }

        render(static_cast<f32>(m_accumulator / FIXED_DT));
        m_frameCount++;

        // Record frame time for profiler
        profilerRecordFrame(frameTime * 1000.0);

        m_statsTimer += frameTime;
        if (m_statsTimer >= 1.0) {
            if (m_gameState == GameState::IN_GAME) logStats();
            m_displayFps   = m_frameCount;
            m_statsTimer  -= 1.0;
            m_updateCount  = 0;
            m_frameCount   = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Split-screen player swap — copy per-player arrays ↔ active aliases.
//
// M1 (drift-proofing): the alias↔array field list lives in ONE place — the X-macro below.
// swapInPlayer and swapOutPlayer expand it in opposite directions, so it is impossible to
// add (or rename) a per-player field that gets restored in one direction but not the other.
// Each entry is (active-alias, per-player-array); a plain assignment must be valid for both.
// Array-typed fields (m_classSkillStates) can't use operator= and are listed separately.
// ---------------------------------------------------------------------------
#define LOCAL_PLAYER_SWAP_FIELDS(X)        \
    X(m_localPlayer,      m_localPlayers)   \
    X(m_camera,           m_cameras)        \
    X(m_viewmodelState,   m_viewmodelStates)\
    X(m_playerClass,      m_playerClasses)  \
    X(m_activeClassSkill, m_activeClassSkills) \
    X(m_armorAura,        m_armorAuras)     \
    X(m_weaponProc,       m_weaponProcs)    \
    X(m_ringPassive,      m_ringPassives)   \
    X(m_inventoryOpen,    m_inventoryOpenArr) \
    X(m_hitMarkerTimer,   m_hitMarkerTimers)\
    X(m_potionCooldown,   m_potionCooldowns)\
    X(m_invCursorPanel,   m_invCursorPanels)\
    X(m_invCursorIndex,   m_invCursorIndices) \
    X(m_dragState,        m_dragStates)     \
    X(m_dblClickState,    m_dblClickStates)

void Engine::swapInPlayer(u8 idx) {
    #define X(alias, arr) alias = arr[idx];
    LOCAL_PLAYER_SWAP_FIELDS(X)
    #undef X
    // Array field — paired manually (must mirror the swapOut memcpy below).
    std::memcpy(m_classSkillStates, m_classSkillStatesPerPlayer[idx], sizeof(m_classSkillStates));
    m_localPlayerIndex = idx;
}

void Engine::swapOutPlayer(u8 idx) {
    #define X(alias, arr) arr[idx] = alias;
    LOCAL_PLAYER_SWAP_FIELDS(X)
    #undef X
    std::memcpy(m_classSkillStatesPerPlayer[idx], m_classSkillStates, sizeof(m_classSkillStates));
}
#undef LOCAL_PLAYER_SWAP_FIELDS

// (M5) Place the split-screen pair at the current spawn. P0 is taken from the active alias
// (the freshly-built spawn position/orientation); P1 is placed one metre to the side facing
// the same way, briefly invulnerable, and both NetPlayer spawnPositions are recorded for
// respawn. This consolidates the near-identical reposition blocks that were copy-pasted in
// the floor-transition, continue-load, and new-run-with-P2 paths. No-op outside split-screen.
void Engine::positionLocalPlayersAtSpawn() {
    if (m_splitPlayerCount <= 1) return;
    m_localPlayers[0]            = m_localPlayer;          // P0 = current active alias
    m_cameras[0]                 = m_camera;
    m_players[0].spawnPosition   = m_localPlayer.position;
    m_localPlayers[1].position   = m_localPlayer.position + Vec3{1.0f, 0.0f, 0.0f};
    m_localPlayers[1].velocity   = {0, 0, 0};
    m_localPlayers[1].yaw        = m_localPlayer.yaw;
    m_localPlayers[1].eyeHeight  = m_localPlayer.eyeHeight;
    m_localPlayers[1].invulnTimer = 2.5f;                  // brief spawn protection for P1
    m_players[1].spawnPosition   = m_localPlayers[1].position;
}

// ---------------------------------------------------------------------------
// Sync helpers between Player and NetPlayer
// ---------------------------------------------------------------------------
void Engine::syncLocalPlayerToNetPlayer() {
    NetPlayer& np = m_players[activeNetSlot()]; // net slot, not lane (client slot may be >=1)
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
    // Status effects
    np.invulnTimer      = m_localPlayer.invulnTimer;
    np.damageReduction  = m_localPlayer.damageReduction;
    np.slowTimer        = m_localPlayer.slowTimer;
    np.poisonTimer      = m_localPlayer.poisonTimer;
    np.poisonDps        = m_localPlayer.poisonDps;
    np.burnTimer        = m_localPlayer.burnTimer;
    np.burnDps          = m_localPlayer.burnDps;
    np.freezeTimer      = m_localPlayer.freezeTimer;
    np.blocking         = m_localPlayer.blocking;
    np.blockTimer       = m_localPlayer.blockTimer;
    np.ringPassive      = static_cast<SkillId>(m_localPlayer.ringPassive);
}

void Engine::syncNetPlayerToLocalPlayer() {
    const NetPlayer& np = m_players[activeNetSlot()]; // net slot, not lane (client slot may be >=1)
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
    // Derive forward vector from yaw/pitch so weapon fire aims correctly
    m_localPlayer.forward = normalize(Vec3{
        -sinf(np.yaw) * cosf(np.pitch),
         sinf(np.pitch),
        -cosf(np.yaw) * cosf(np.pitch)
    });
    // Status effects
    m_localPlayer.invulnTimer      = np.invulnTimer;
    m_localPlayer.damageReduction  = np.damageReduction;
    m_localPlayer.slowTimer        = np.slowTimer;
    m_localPlayer.poisonTimer      = np.poisonTimer;
    m_localPlayer.poisonDps        = np.poisonDps;
    m_localPlayer.burnTimer        = np.burnTimer;
    m_localPlayer.burnDps          = np.burnDps;
    m_localPlayer.freezeTimer      = np.freezeTimer;
    m_localPlayer.blocking         = np.blocking;
    m_localPlayer.blockTimer       = np.blockTimer;
    m_localPlayer.ringPassive      = static_cast<u8>(np.ringPassive);
}

// R7-3 / TA-3: SERVER target bridge. The AI/projectile/skill systems operate on `Player&`,
// but remote players are `NetPlayer`s — so we build throwaway Player "views" of a remote and
// copy the mutated fields back afterwards. The per-field copy list lives ONCE in these two
// free helpers so the array adapter (AI/projectile path) and the single-slot helpers (skill
// path) never duplicate it. Wanderer-only concepts NetPlayer lacks (smokeTimer, deflectTimer,
// dodgeState, curseStacks, ...) stay at the Player default-zero — graceful (no stealth, no
// deflect, detectable) — and, having no NetPlayer home, simply don't persist across the call.

// Seed `v` from `np` with every field the AI/projectile/skill paths read. Position/yaw/pitch
// are included because skills (PhaseDash, ShadowStrike/Step) read the CURRENT transform to
// compute their destination; the AI/projectile path reads position too. lifesaverArmed (TA-1)
// is mirrored so the one-shot near-death i-frame stays one-shot for remotes.
static void seedRemoteView(const NetPlayer& np, Player& v) {
    v = Player{};                                        // default-zero Wanderer-only fields
    v.position        = np.position;
    v.velocity        = np.velocity;
    v.yaw             = np.yaw;
    v.pitch           = np.pitch;
    v.eyeHeight       = np.eyeHeight;
    v.health          = np.health;
    v.maxHealth       = np.maxHealth;
    // Shared status the damage/skill paths read/write (NetPlayer-backed).
    v.invulnTimer     = np.invulnTimer;
    v.damageReduction = np.damageReduction;
    v.slowTimer       = np.slowTimer;
    v.poisonTimer     = np.poisonTimer;
    v.poisonDps       = np.poisonDps;
    v.burnTimer       = np.burnTimer;
    v.burnDps         = np.burnDps;
    v.freezeTimer     = np.freezeTimer;
    v.blocking        = np.blocking;
    v.blockTimer      = np.blockTimer;
    v.lifesaverArmed  = np.lifesaverArmed; // TA-1: mirror so the i-frame isn't re-armed each frame
    // M-3/M-4: ring-passive state on a remote — Soul Harvest stacks drive the gun-damage /
    // move-speed bonus the AI/combat code reads from the Player view; smokeTimer drives the
    // Phase Strike stealth check in enemy_ai_states.cpp. Without mirroring these, M8 credits
    // stacks/timer onto the NetPlayer but the remote view zero-defaults them and nothing reads.
    v.soulHarvestStacks  = np.soulHarvestStacks;
    v.soulHarvestTimer   = np.soulHarvestTimer;
    v.smokeTimer         = np.smokeTimer;
    v.secondWindCooldown = np.secondWindCooldown;
    // Wanderer death-preamble state (next-batch): mirror so a remote Wanderer's Shadow Dance
    // and mark-prey stacks credit-and-read through the view consistently.
    v.shadowDanceTimer   = np.shadowDanceTimer;
    v.markTimer          = np.markTimer;
    v.markSpeedStacks    = np.markSpeedStacks;
    for (u32 ms = 0; ms < 20; ms++) v.markSpeedTimers[ms] = np.markSpeedTimers[ms];
}

// Copy back every view field the AI/projectile/skill paths can mutate. Position is written back
// so dash/blink skills actually move the remote; serverNetPost then ticks DoT/death and the
// snapshot carries the result to the owning client.
static void writeBackRemoteView(const Player& v, NetPlayer& np) {
    np.position         = v.position;       // TA-3: dash/blink skills move the casting remote
    np.health           = v.health;         // damage / Blood Nova self-cost / heals
    np.invulnTimer      = v.invulnTimer;    // lifesaver near-death i-frame + Divine Shield
    np.slowTimer        = v.slowTimer;
    np.poisonTimer      = v.poisonTimer;
    np.poisonDps        = v.poisonDps;
    np.burnTimer        = v.burnTimer;
    np.burnDps          = v.burnDps;
    np.freezeTimer      = v.freezeTimer;
    np.damageFlashTimer = v.damageFlashTimer; // drives the remote damage-flash render
    np.lifesaverArmed   = v.lifesaverArmed;   // TA-1: persist consume/re-arm across frames
    // Persist ring-passive state mutations across frames (e.g., a stack decay that some skill
    // path applied to the view) — mirrors the seed list above.
    np.soulHarvestStacks  = v.soulHarvestStacks;
    np.soulHarvestTimer   = v.soulHarvestTimer;
    np.smokeTimer         = v.smokeTimer;
    np.secondWindCooldown = v.secondWindCooldown;
    // Wanderer death-preamble state (next-batch) — same persistence pattern.
    np.shadowDanceTimer   = v.shadowDanceTimer;
    np.markTimer          = v.markTimer;
    np.markSpeedStacks    = v.markSpeedStacks;
    for (u32 ms = 0; ms < 20; ms++) np.markSpeedTimers[ms] = v.markSpeedTimers[ms];
}

// Array form (AI + projectile pass): build a view of every ACTIVE, non-dead REMOTE NetPlayer
// (skip the local host slot, whose damage flows through m_localPlayer in gameUpdate). `views[k]`
// is the view for `ptrs[k]`; `slots[k]` records its NetPlayer index for applyRemotePlayerViews.
u32 Engine::buildRemotePlayerViews(Player* views, Player** ptrs, u8* slots) {
    u32 count = 0;
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (i == m_localPlayerIndex) continue;           // host slot handled via m_localPlayer
        const NetPlayer& np = m_players[i];
        if (!np.active || np.isDead) continue;           // only live remotes are targetable
        slots[count] = static_cast<u8>(i);
        seedRemoteView(np, views[count]);
        ptrs[count] = &views[count];
        count++;
    }
    return count;
}

// Array form: copy the mutated view fields back into the matching remote NetPlayers.
void Engine::applyRemotePlayerViews(const Player* views, const u8* slots, u32 count) {
    for (u32 k = 0; k < count; k++)
        writeBackRemoteView(views[k], m_players[slots[k]]);
}

// TA-3 single-slot form (skill path): seed `out` from one specific remote NetPlayer so the
// skill computes against THAT guest's current transform/health, not the host's m_localPlayer.
void Engine::buildRemotePlayerView(u8 slot, Player& out) {
    seedRemoteView(m_players[slot], out);
}

// TA-3 single-slot form: write a remote's skill-mutated view back into its NetPlayer.
void Engine::applyRemotePlayerView(const Player& v, u8 slot) {
    writeBackRemoteView(v, m_players[slot]);
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
