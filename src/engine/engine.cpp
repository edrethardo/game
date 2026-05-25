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

void Engine::onEvent(const u8* data, u32 size) {
    if (!s_engine) return;
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
    // Ignore stale/duplicate descents (reliable channel shouldn't dup, but be safe):
    // if we're already on (or past) the announced floor, do nothing.
    if (s_engine->m_gameState == GameState::FLOOR_TRANSITION) return;

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
        // Health from the chosen class (not a hardcoded 100) so warriors/tanks aren't
        // shortchanged and squishier classes aren't over-tanked on join.
        np.health = cls.baseHealth;
        np.maxHealth = cls.baseHealth;
        np.position = s_engine->m_players[0].spawnPosition; // spawn at host's spawn
        np.spawnPosition = np.position;
        np.weaponState.currentWeapon = 0;
        np.weaponState.cooldownTimer = 0.0f;
        np.isDead = false;
        np.invulnTimer = 2.5f; // spawn protection
        np.playerClass = joinClass; // class chosen by the joining player

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
        //
        // Entity/projectile cleanup is intentionally NOT done: per-net-slot ownership
        // isn't tracked (Entity.ownerLocalPlayer is a SPLIT-SCREEN local index, not a
        // net slot), and lock-on only ever targets entities (NPCs), never other players —
        // so no other player's lock can point at the leaver. Cleaning entities owned by
        // a departed net player needs a real ownership field; deferred (see report).
        s_engine->m_players[playerSlot] = NetPlayer{};
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
// Split-screen player swap — copy per-player arrays ↔ active aliases
// ---------------------------------------------------------------------------
void Engine::swapInPlayer(u8 idx) {
    m_localPlayer      = m_localPlayers[idx];
    m_camera           = m_cameras[idx];
    m_viewmodelState   = m_viewmodelStates[idx];
    m_playerClass      = m_playerClasses[idx];
    m_activeClassSkill = m_activeClassSkills[idx];
    std::memcpy(m_classSkillStates, m_classSkillStatesPerPlayer[idx], sizeof(m_classSkillStates));
    m_armorAura        = m_armorAuras[idx];
    m_weaponProc       = m_weaponProcs[idx];
    m_ringPassive      = m_ringPassives[idx];
    m_inventoryOpen    = m_inventoryOpenArr[idx];
    m_hitMarkerTimer   = m_hitMarkerTimers[idx];
    m_potionCooldown   = m_potionCooldowns[idx];
    m_invCursorPanel   = m_invCursorPanels[idx];
    m_invCursorIndex   = m_invCursorIndices[idx];
    m_localPlayerIndex = idx;
}

void Engine::swapOutPlayer(u8 idx) {
    m_localPlayers[idx]      = m_localPlayer;
    m_cameras[idx]           = m_camera;
    m_viewmodelStates[idx]   = m_viewmodelState;
    m_playerClasses[idx]     = m_playerClass;
    m_activeClassSkills[idx] = m_activeClassSkill;
    std::memcpy(m_classSkillStatesPerPlayer[idx], m_classSkillStates, sizeof(m_classSkillStates));
    m_armorAuras[idx]        = m_armorAura;
    m_weaponProcs[idx]       = m_weaponProc;
    m_ringPassives[idx]      = m_ringPassive;
    m_inventoryOpenArr[idx]  = m_inventoryOpen;
    m_hitMarkerTimers[idx]   = m_hitMarkerTimer;
    m_potionCooldowns[idx]   = m_potionCooldown;
    m_invCursorPanels[idx]   = m_invCursorPanel;
    m_invCursorIndices[idx]  = m_invCursorIndex;
}

// ---------------------------------------------------------------------------
// Sync helpers between Player and NetPlayer
// ---------------------------------------------------------------------------
void Engine::syncLocalPlayerToNetPlayer() {
    NetPlayer& np = m_players[m_localPlayerIndex];
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
    const NetPlayer& np = m_players[m_localPlayerIndex];
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
