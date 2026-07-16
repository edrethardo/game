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
#include "platform/steam.h"
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

// --- The Dungeon Engine secret superboss: session-only state (NEVER serialized) ---
// Same lifetime contract as s_firstKillDropGiven above: a file-scope global, extern'd into the few
// TUs that touch it, absent from engine_persist.cpp. It MUST survive a DESCEND (so shards persist
// across the floor-50 → next-tier loop) but reset on a true run boundary (New Game / load / menu).
// s_sourceShards is a 10-bit set keyed by floor/5-1 (floor 5→bit0 … floor 50→bit9); complete = 0x3FF.
u16 s_sourceShards = 0;
// Set when the Engine is slain, so the VICTORY screen can show the secret ending instead of the
// ordinary Hell-clear text. Reset alongside s_sourceShards.
bool s_engineSlain = false;


// ---------------------------------------------------------------------------
// Net callbacks (static — forwarded to engine)
// ---------------------------------------------------------------------------
void Engine::onSnapshot(const u8* data, u32 size) {
    // M1.6: pass m_clockSync so receiveSnapshot can refine the tick estimate on each
    // successful deserialize (ClockSyncOps::onSnapshotReceived, P controller gain 0.1).
    if (!s_engine) return;
    Client::receiveSnapshot(data, size, s_engine->m_clockSync);
    // D7.2 — After a successful deserialize, copy the latest snapshot into the
    // client-side baseline so D7.3 can reconstruct unchanged slots from a delta
    // packet. We read getLatestSnapshot() immediately — if receiveSnapshot accepted
    // and pushed the new snapshot, the newest is our deserialized result; if it was
    // discarded as stale or OOO, getLatestSnapshot() returns the prior newest, which
    // is still the correct baseline for the next delta. The copy is cheap (~50 KB
    // memcpy) and happens at most once per snapshot tick (60 Hz), well within budget.
    const WorldSnapshot* latest = Client::getLatestSnapshot();
    if (latest) {
        s_engine->m_lastAppliedSnap = *latest;
    }
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

// CL_USE_PET — a client clicked a pet consumable. Validate against the server's synced copy of
// that slot's inventory (a forged defId without the item must be a no-op) before toggling; the
// summoned/dismissed entity reaches the client back through the ordinary snapshot.
void Engine::onUsePet(u8 playerSlot, u16 itemDefId) {
    if (!s_engine) return;
    if (s_engine->m_netRole != NetRole::SERVER) return;
    if (playerSlot >= MAX_PLAYERS || itemDefId >= s_engine->m_itemDefCount) return;
    if (!s_engine->m_itemDefs[itemDefId].petSummon) return;

    const PlayerInventory& inv = s_engine->m_inventories[playerSlot];
    for (u8 b = 0; b < MAX_INVENTORY_ITEMS; b++) {
        const ItemInstance& it = inv.backpack[b];
        if (!isItemEmpty(it) && it.defId == itemDefId) {
            s_engine->togglePetCompanion(playerSlot, itemDefId);
            return;
        }
    }
}

// CL_INTERACT_ENTITY — a client pressed E on a dormant mimic "chest". The pool index is
// untrusted network input, so it must PROVE it names an active, still-dormant mimic within
// interact reach of THAT player's authoritative position before anything wakes. Reach gets
// +1.0 m of slack over the local rule: the client aimed against its own predicted position,
// and a request ~RTT/2 in flight must not go dead because the player kept walking. The wake
// travels back to everyone as replicated aiState in the next snapshot — no reply packet.
void Engine::onEntityInteract(u8 playerSlot, u8 poolIdx) {
    if (!s_engine) return;
    if (s_engine->m_netRole != NetRole::SERVER) return;
    if (playerSlot >= MAX_PLAYERS || poolIdx >= MAX_ENTITIES) return;

    Entity& e = s_engine->m_entities.entities[poolIdx];
    if (!(e.flags & ENT_ACTIVE) || (e.flags & ENT_DEAD)) return;
    if (e.enemyType != EnemyType::MIMIC || e.aiState != AIState::DORMANT) return;

    const NetPlayer& np = s_engine->m_players[playerSlot];
    if (!np.active) return;
    const Vec3 d = e.position - np.position;
    const f32 maxReach = GameConst::INTERACT_RANGE + 1.0f;
    if (d.x * d.x + d.z * d.z > maxReach * maxReach) return;

    EnemyAI::wakeAmbusher(e);
}

// R11 — Server-side CL_DROP_ITEM dispatch. Wire layout:
//   header(4) + u8 slotKind + u8 slotIndex + Vec3 dropPos(12) + ItemInstance(sizeof ItemInstance)
// slotKind: 0=backpack, 1=equipment slot. The full ItemInstance rides the packet so we
// spawn the world item with the client's rolled stats — server doesn't keep its own
// authoritative copy (co-op trust model, matches CL_INVENTORY_SYNC).
void Engine::onDropItem(u8 playerSlot, const u8* data, u32 size) {
    if (!s_engine) return;
    if (s_engine->m_netRole != NetRole::SERVER) return;
    constexpr u32 kFixed = sizeof(PacketHeader) + 1 + 1 + 12;
    if (size < kFixed + sizeof(ItemInstance)) return;
    const u8* p = data + sizeof(PacketHeader);
    u8 slotKind  = p[0];
    u8 slotIndex = p[1];
    Vec3 dropPos;
    std::memcpy(&dropPos, p + 2, 12);
    ItemInstance it;
    std::memcpy(&it, p + 2 + 12, sizeof(ItemInstance));
    s_engine->handleDropRequest(playerSlot, slotKind, slotIndex, it, dropPos);
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
static constexpr u32 INVENTORY_SYNC_VERSION = 2; // v2: PlayerInventory gained GLOVES slot + attack-speed cache (save v3)

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

void Engine::sendInventorySync(u8 lane, u8 targetSlot) {
    if (m_netRole != NetRole::CLIENT) return;
    if (lane >= MAX_LOCAL_PLAYERS) return;
    // Build the body from the per-lane backing arrays (not the swapped-in alias) so online couch
    // co-op can push BOTH local players' inventories — one call per lane, each tagged with the
    // server slot it belongs to. targetSlot rides as a trailing byte the server validates by owner.
    InventorySyncBody body{};
    body.version          = INVENTORY_SYNC_VERSION;
    body.inv              = m_inventories[lane];
    body.qb               = m_quickbars[lane];
    body.energySkill      = m_skillStates[lane];
    body.cls              = static_cast<u8>(m_playerClasses[lane]);
    body.activeClassSkill = m_activeClassSkills[lane];
    std::memcpy(body.classSkills, m_classSkillStatesPerPlayer[lane], sizeof(body.classSkills));
    body.health           = m_localPlayers[lane].health;
    body.maxHealth        = m_localPlayers[lane].maxHealth;

    // Wire: PacketHeader(4) + body + targetSlot(1). The trailing slot byte keeps the body offset
    // unchanged, so onInventorySync's deserialize is untouched.
    constexpr u32 totalSize = sizeof(PacketHeader) + sizeof(InventorySyncBody) + 1;
    u8 buf[totalSize];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_INVENTORY_SYNC;
    hdr->flags = 0;
    hdr->seq   = 0;
    std::memcpy(buf + sizeof(PacketHeader), &body, sizeof(body));
    buf[sizeof(PacketHeader) + sizeof(InventorySyncBody)] = targetSlot;
    Net::sendToServer(buf, totalSize, /*reliable=*/true);
    LOG_INFO("CL_INVENTORY_SYNC: lane %u -> slot %u (class=%u hp=%.0f/%.0f)",
             lane, targetSlot, body.cls, body.health, body.maxHealth);
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
        case NetEventType::NOVA_FX: {
            // A nova ring fired by server-authoritative code (Blood Nova armor aura) — including
            // THIS client's own, which it cannot predict (it never sees the melee hit that
            // triggered it). Cosmetic only; the damage and the health cost are the server's.
            // Post-header byte count INCLUDES the eventType byte: 1 + 12 + 4 + 12 = 29.
            if (size < sizeof(PacketHeader) + 29) break;
            u32 off = sizeof(PacketHeader) + 1;
            Vec3 pos, color;
            f32  radius;
            std::memcpy(&pos.x,   data + off, 4); off += 4;
            std::memcpy(&pos.y,   data + off, 4); off += 4;
            std::memcpy(&pos.z,   data + off, 4); off += 4;
            std::memcpy(&radius,  data + off, 4); off += 4;
            std::memcpy(&color.x, data + off, 4); off += 4;
            std::memcpy(&color.y, data + off, 4); off += 4;
            std::memcpy(&color.z, data + off, 4); off += 4;
            s_engine->emitNovaFX(pos, radius, color);  // broadcast branch is SERVER-gated — no echo
        } break;
        case NetEventType::EXIT_PORTAL: {
            // The Engine is dead — the way out exists. Mirror the level state locally so the
            // portal renders and resolveInteractTargets can offer the enter (the actual enter
            // is still server-validated via CL_REQUEST_DESCEND).
            if (size < sizeof(PacketHeader) + 13) break;
            u32 off = sizeof(PacketHeader) + 1;
            Vec3 pos;
            std::memcpy(&pos.x, data + off, 4); off += 4;
            std::memcpy(&pos.y, data + off, 4); off += 4;
            std::memcpy(&pos.z, data + off, 4); off += 4;
            s_engine->m_level.exitPortalActive = true;
            s_engine->m_level.exitPortalPos    = pos;
        } break;
        case NetEventType::CREDITS: {
            // The run's ending — roll the same credits the host rolls. startCredits guards on
            // IN_GAME/GAME_OVER so a stray packet in a menu state is a no-op.
            if (size < sizeof(PacketHeader) + 2) break;
            const bool engineSlain = data[sizeof(PacketHeader) + 1] != 0;
            s_engine->startCredits(engineSlain);
        } break;
        case NetEventType::SPEECH: {
            // NPC speech (bubble + chat line) resolved and shipped by the server — see the
            // NetEventType doc. Wire strings are untrusted input: every length byte is checked
            // against the remaining payload and capped to the local buffers before any copy.
            u32 off = sizeof(PacketHeader) + 1;
            if (size < off + 6) break;               // poolIdx + rgb + nameLen + lineLen minimum
            u8  poolIdx = data[off++];
            f32 r = data[off++] / 255.0f;
            f32 g = data[off++] / 255.0f;
            f32 b = data[off++] / 255.0f;
            u8 nameLen = data[off++];
            if (nameLen > 23 || size < off + nameLen + 1u) break;
            char name[24];
            std::memcpy(name, data + off, nameLen); name[nameLen] = '\0'; off += nameLen;
            u8 lineLen = data[off++];
            if (lineLen == 0 || lineLen > 63 || size < off + lineLen) break;
            // Park the line in the speaker's OWN per-slot buffer (m_guestSpeech is keyed by the
            // server pool index) so speechText points at storage that outlives this packet and
            // that no OTHER entity's speech can rewrite while the bubble is live. A malformed
            // poolIdx still gets its chat line — parked in the overflow scratch — just no bubble.
            static char s_speechScratch[64];
            char* line = (poolIdx < MAX_ENTITIES) ? s_engine->m_guestSpeech[poolIdx]
                                                  : s_speechScratch;
            std::memcpy(line, data + off, lineLen); line[lineLen] = '\0';
            s_engine->addChatMessage(name, line, {r, g, b});
            // Bubble: renderSpeechBubbles reads the INTERP pool on a client, and that pool is
            // keyed by the server's entity pool index — exactly what poolIdx is. The speech
            // decay loop (tickSharedSystems) picks the same pool, so the bubble fades on
            // schedule; its chat-log branch is !CLIENT-gated, so no double-post.
            if (poolIdx < MAX_ENTITIES) {
                Entity& spk = s_engine->m_renderInterp.entities.entities[poolIdx];
                spk.speechText  = line;
                spk.speechTimer = 2.4f;   // same display window the authoritative side uses
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
        case NetEventType::PROJECTILE_SPLASH: {
            // Host-replicated projectile AoE splash — the client's ProjectileSystem::update
            // is gated off (N4), so the splash callback never fires locally. Replay the same
            // FX via spawnSplashFX (it re-runs the floor-snap from the raw position).
            // Payload after the 4-byte header: eventType(1) + pos(12) + radius(4) = 17 B.
            if (size < sizeof(PacketHeader) + 17) break;
            u32 off = sizeof(PacketHeader) + 1;
            Vec3 pos;
            std::memcpy(&pos.x, data + off, 4); off += 4;
            std::memcpy(&pos.y, data + off, 4); off += 4;
            std::memcpy(&pos.z, data + off, 4); off += 4;
            f32 radius;
            std::memcpy(&radius, data + off, 4); off += 4;
            s_engine->spawnSplashFX(pos, radius);
        } break;
        case NetEventType::METEOR: {
            // A meteor cast by SOMEONE ELSE (the host, or another guest), relayed so we can see it.
            // Our OWN meteors never arrive here — we predicted those locally and the server skips
            // the caster when relaying (broadcastReliableExcept), so this can't double-spawn ours.
            // Spawn a VISUAL-ONLY PendingMeteor: the client-side meteor tick (tickSharedSystems)
            // advances its telegraph and detonates it into FX. damage=0 — the server owns the
            // damage, and the client's meteor tick never applies any.
            // Payload after the 4-byte header: eventType(1) + pos(12) + radius(4) + delay(4) = 21 B.
            if (size < sizeof(PacketHeader) + 21) break;
            u32 off = sizeof(PacketHeader) + 1;
            Vec3 pos;
            std::memcpy(&pos.x, data + off, 4); off += 4;
            std::memcpy(&pos.y, data + off, 4); off += 4;
            std::memcpy(&pos.z, data + off, 4); off += 4;
            f32 radius, delay;
            std::memcpy(&radius, data + off, 4); off += 4;
            std::memcpy(&delay,  data + off, 4); off += 4;
            extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
            for (u32 mi = 0; mi < MAX_PENDING_METEORS; mi++) {
                if (!s_meteors[mi].active) {
                    s_meteors[mi] = {pos, 0.0f /*damage: server-authoritative*/, radius, delay, true};
                    break;
                }
            }
        } break;
        default: break;
    }
}

// Relay a meteor to clients (SV_EVENT::METEOR). exceptSlot skips the client that already PREDICTED
// this meteor (echoing it back would double-spawn); 0xFF = send to everyone (host-cast meteors).
// No-op off SERVER. Mirrors the PROJECTILE_SPLASH broadcast idiom.
void Engine::broadcastMeteorEvent(Vec3 position, f32 radius, f32 delay, u8 exceptSlot) {
    if (m_netRole != NetRole::SERVER) return;
    u8 buf[sizeof(PacketHeader) + 21]; // hdr(4) + eventType(1) + pos(12) + radius(4) + delay(4)
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::SV_EVENT;
    hdr->flags = 0;
    hdr->seq   = 0;
    u32 off = sizeof(PacketHeader);
    buf[off++] = static_cast<u8>(NetEventType::METEOR);
    std::memcpy(buf + off, &position.x, 4); off += 4;
    std::memcpy(buf + off, &position.y, 4); off += 4;
    std::memcpy(buf + off, &position.z, 4); off += 4;
    std::memcpy(buf + off, &radius,     4); off += 4;
    std::memcpy(buf + off, &delay,      4); off += 4;
    if (exceptSlot == 0xFF) Net::broadcastReliable(buf, off);
    else                    Net::broadcastReliableExcept(exceptSlot, buf, off);
}

// Spawn an expanding nova ring locally AND — on the SERVER — replicate it to every client, so a
// nova triggered by server-authoritative code (the Blood Nova armor aura) is visible to the player
// it belongs to. Safe to call from the CLIENT's SV_EVENT handler: the broadcast is SERVER-gated,
// so replaying a received event can't echo it back.
void Engine::emitNovaFX(Vec3 position, f32 radius, Vec3 color) {
    for (u32 i = 0; i < MAX_NOVA_FX; i++) {
        if (!m_fx.novaFX[i].active) {
            m_fx.novaFX[i] = {position, radius, 0.6f, true, color};
            break;
        }
    }
    if (m_netRole != NetRole::SERVER) return;

    u8 buf[sizeof(PacketHeader) + 29]; // hdr(4) + eventType(1) + pos(12) + radius(4) + color(12)
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::SV_EVENT;
    hdr->flags = 0;
    hdr->seq   = 0;
    u32 off = sizeof(PacketHeader);
    buf[off++] = static_cast<u8>(NetEventType::NOVA_FX);
    std::memcpy(buf + off, &position.x, 4); off += 4;
    std::memcpy(buf + off, &position.y, 4); off += 4;
    std::memcpy(buf + off, &position.z, 4); off += 4;
    std::memcpy(buf + off, &radius,     4); off += 4;
    std::memcpy(buf + off, &color.x,    4); off += 4;
    std::memcpy(buf + off, &color.y,    4); off += 4;
    std::memcpy(buf + off, &color.z,    4); off += 4;
    Net::broadcastReliable(buf, off);
}

// The post-Engine EXIT portal: set the level state locally and — on the SERVER — replicate it so
// clients can see and use it (contrast the Source ENTRY portal, which is host-only-visible by
// design). Sent once; a client joining after this is an accepted gap (the fight is the endgame).
void Engine::spawnExitPortal(Vec3 pos) {
    m_level.exitPortalActive = true;
    m_level.exitPortalPos    = pos;
    LOG_INFO("Exit portal spawned at (%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);
    if (m_netRole != NetRole::SERVER) return;

    u8 buf[sizeof(PacketHeader) + 13]; // hdr(4) + eventType(1) + pos(12)
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::SV_EVENT;
    hdr->flags = 0;
    hdr->seq   = 0;
    u32 off = sizeof(PacketHeader);
    buf[off++] = static_cast<u8>(NetEventType::EXIT_PORTAL);
    std::memcpy(buf + off, &pos.x, 4); off += 4;
    std::memcpy(buf + off, &pos.y, 4); off += 4;
    std::memcpy(buf + off, &pos.z, 4); off += 4;
    Net::broadcastReliable(buf, off);
}

// Server/SP entry into the run's ending: broadcast FIRST so every client rolls the same credits
// (THE fix for "the client just hangs when the run ends" — the old flow flipped the host's local
// m_gameState and went silent), then run the identical local flip.
void Engine::beginCreditsSequence(bool engineSlain) {
    if (m_gameState == GameState::CREDITS || m_gameState == GameState::VICTORY) return; // once
    if (m_netRole == NetRole::SERVER) {
        u8 buf[sizeof(PacketHeader) + 2]; // hdr(4) + eventType(1) + engineSlain(1)
        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
        hdr->type  = NetPacketType::SV_EVENT;
        hdr->flags = 0;
        hdr->seq   = 0;
        u32 off = sizeof(PacketHeader);
        buf[off++] = static_cast<u8>(NetEventType::CREDITS);
        buf[off++] = engineSlain ? 1 : 0;
        Net::broadcastReliable(buf, off);
    }
    startCredits(engineSlain);
}

// The LOCAL credits flip every machine runs (host directly, clients from SV_EVENT::CREDITS).
// Leads into the existing VICTORY screen; the net session stays connected (Net::poll runs in
// every GameState) and is torn down when VICTORY returns to the menu.
void Engine::startCredits(bool engineSlain) {
    if (m_gameState != GameState::IN_GAME && m_gameState != GameState::GAME_OVER) return;
    s_engineSlain = engineSlain;   // client copy — steers credits/victory text to the ending variant
    m_level.exitPortalActive = false;
    m_creditsScroll = 0.0f;
    m_gameState = GameState::CREDITS;
    AudioSystem::stopMusic();
    Input::setRelativeMouseMode(false);
    LOG_INFO("Credits rolling (%s ending)", engineSlain ? "secret" : "standard");
}

// Blood Nova worn as EQUIPMENT — the health-sacrificing nova the tooltip has always promised.
// Shared by every non-weapon path that fires it (hence health/cooldown by reference: a Player and a
// NetPlayer have no common base, but both own an f32 health and an f32 cooldown):
//   • Demonhide Cuirass (armor) — retaliates when the wearer is STRUCK.
//   • Aegis of Blood (offhand)  — erupts on a PERFECT BLOCK.
// The CALLER owns the trigger condition; this owns the cost, the cooldown, the damage and the ring.
//
// Neither used to do anything of the sort: the armor applied a 1 dps / 0.5 s poison within 3 m, and
// the shield did the same generic freeze-bash as every other legendary shield. Both shared nothing
// with Blood Nova but the name printed in their tooltip.
//
// SERVER/SP ONLY. On a CLIENT the entity pool is the N4 ghost sim, so damage applied here would be
// meaningless; the guest gets its ring from the replicated NOVA_FX event instead.
//
// Returns true if it detonated (caller may log).
bool Engine::detonateBloodNova(Vec3 origin, u8 ownerSlot, f32& health, f32& cooldown) {
    if (m_netRole == NetRole::CLIENT) return false;
    if (cooldown > 0.0f)     return false;   // BLOOD_NOVA_ARMOR_COOLDOWN_SEC — anti-multi-hit only
    if (health <= 0.0f)      return false;   // corpses don't erupt

    const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, SkillId::BLOOD_NOVA);
    if (!sd) return false;

    // 20% of CURRENT health — a fraction, so it decays asymptotically and can never itself be
    // lethal. The floor guard mirrors SkillSystem::tryActivate's refuse-to-suicide rule: an
    // automatic passive the player cannot decline must never be the thing that kills them (it
    // would turn an otherwise survivable hit lethal, which no tooltip warns about).
    const f32 cost = health * BLOOD_NOVA_ARMOR_COST_PCT;
    if (health <= cost + 1.0f) return false;
    health -= cost;

    // Same 360° query the active skill and the weapon proc use (cosAngle -1 = all directions),
    // at the skill def's full damage and radius — so all three roles of Blood Nova finally agree.
    EntityHandle hits[MAX_ENTITIES];
    f32          dists[MAX_ENTITIES];
    u32 hitCount = CombatQuery::queryConeSorted(m_entities, origin, {0.0f, 0.0f, -1.0f}, -1.0f,
                                                sd->radius, hits, dists, MAX_ENTITIES);
    Combat::setAttackingPlayer(ownerSlot);    // credit kills to the wearer
    for (u32 h = 0; h < hitCount; h++)
        Combat::applyDamage(m_entities, hits[h], sd->damage);

    // NOT sd->cooldown: the 5 s in skills.json gates the ACTIVE cast, where the player chooses to
    // pay. Worn as equipment the nova is a reflex, so it re-arms almost immediately (see the
    // constant's comment) and the health cost is the real limiter.
    cooldown = BLOOD_NOVA_ARMOR_COOLDOWN_SEC;
    emitNovaFX(origin, sd->radius, {1.0f, 0.15f, 0.1f});   // the red blood ring
    return true;
}

// The LOCAL player just rolled a weapon on-hit PROC meteor. Each player predicts their own (the
// roll is a local std::rand() the other side can't reproduce), so:
//   • CLIENT — spawn a PREDICTED, visual-only meteor right now (instant telegraph; the client-side
//     meteor tick animates + detonates it and applies no damage), and tell the server via CL_METEOR
//     so it spawns the single authoritative damaging meteor and relays it to the other players.
//   • SERVER (host) — spawn the real damaging meteor and relay it to every client.
//   • Singleplayer — just spawn it.
void Engine::predictProcMeteor(Vec3 position, f32 damage, f32 radius, f32 delay) {
    const bool isClient = (m_netRole == NetRole::CLIENT);
    extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
    for (u32 mi = 0; mi < MAX_PENDING_METEORS; mi++) {
        if (!s_meteors[mi].active) {
            PendingMeteor pm;
            pm.position = position;
            // On CLIENT the local copy is a pure visual prediction — zero damage makes the "server
            // owns damage" contract explicit (its meteor tick never applies any regardless).
            pm.damage   = isClient ? 0.0f : damage;
            pm.radius   = radius;
            pm.timer    = delay;
            pm.active   = true;
            pm.caster   = static_cast<u8>(m_localPlayerIndex);
            s_meteors[mi] = pm;
            break;
        }
    }
    if (isClient)                          sendMeteorRequest(position, radius, delay, damage);
    else if (m_netRole == NetRole::SERVER) broadcastMeteorEvent(position, radius, delay, 0xFF);
}

// Client → server: "I predicted a proc meteor here." Reliable — a dropped one costs the shot its
// damage entirely (the client would show a telegraph that never hurts anything).
void Engine::sendMeteorRequest(Vec3 position, f32 radius, f32 delay, f32 damage) {
    if (m_netRole != NetRole::CLIENT) return;
    u8 buf[sizeof(PacketHeader) + 25]; // hdr(4) + pos(12) + radius(4) + delay(4) + damage(4) + targetSlot(1)
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type  = NetPacketType::CL_METEOR;
    hdr->flags = 0;
    hdr->seq   = 0;
    u32 off = sizeof(PacketHeader);
    std::memcpy(buf + off, &position.x, 4); off += 4;
    std::memcpy(buf + off, &position.y, 4); off += 4;
    std::memcpy(buf + off, &position.z, 4); off += 4;
    std::memcpy(buf + off, &radius,     4); off += 4;
    std::memcpy(buf + off, &delay,      4); off += 4;
    std::memcpy(buf + off, &damage,     4); off += 4;
    buf[off++] = activeNetSlot();   // v18: which local lane cast it (kill credit / on-kill procs)
    Net::sendToServer(buf, off, /*reliable=*/true);
}

void Engine::onMeteor(u8 playerSlot, const u8* data, u32 size) {
    if (s_engine) s_engine->handleMeteorRequest(playerSlot, data, size);
}

// Server: a client PREDICTED a proc meteor and told us. Spawn the single AUTHORITATIVE (damaging)
// meteor credited to that caster, then relay it to the OTHER clients so they see it too — never
// back to the caster, which already has its own prediction on screen.
void Engine::handleMeteorRequest(u8 playerSlot, const u8* data, u32 size) {
    if (m_netRole != NetRole::SERVER) return;
    if (playerSlot >= MAX_PLAYERS || !m_players[playerSlot].active) return;
    if (size < sizeof(PacketHeader) + 24) return;

    u32 off = sizeof(PacketHeader);
    Vec3 pos;
    std::memcpy(&pos.x, data + off, 4); off += 4;
    std::memcpy(&pos.y, data + off, 4); off += 4;
    std::memcpy(&pos.z, data + off, 4); off += 4;
    f32 radius, delay, damage;
    std::memcpy(&radius, data + off, 4); off += 4;
    std::memcpy(&delay,  data + off, 4); off += 4;
    std::memcpy(&damage, data + off, 4); off += 4;

    // Sanity-gate the client-supplied values against the authoritative METEOR_STRIKE def so a
    // malformed/hostile packet can't drop a world-sized nuke. Co-op, so this is a sanity bound,
    // not a full anti-cheat: the roll itself is deliberately the client's to own.
    const SkillDef* sd = SkillSystem::findSkillDef(m_skillDefs, m_skillDefCount, SkillId::METEOR_STRIKE);
    if (!sd) return;
    const f32 maxDamage = sd->damage * 10.0f;   // generous headroom for item-level scaling
    if (!(damage > 0.0f) || damage > maxDamage) damage = sd->damage;
    if (!(radius > 0.0f) || radius > sd->radius * 4.0f) radius = sd->radius;
    if (!(delay  > 0.0f) || delay  > 10.0f)             delay  = sd->delay;
    // The meteor must land near the caster's authoritative position (the proc fires at a hit point
    // in front of them, so allow the weapon's reach plus slack).
    Vec3 d = pos - m_players[playerSlot].position;
    if (d.x*d.x + d.y*d.y + d.z*d.z > 80.0f * 80.0f) return;

    extern PendingMeteor s_meteors[MAX_PENDING_METEORS];
    for (u32 mi = 0; mi < MAX_PENDING_METEORS; mi++) {
        if (!s_meteors[mi].active) {
            PendingMeteor pm;
            pm.position = pos;
            pm.damage   = damage;
            pm.radius   = radius;
            pm.timer    = delay;
            pm.active   = true;
            pm.caster   = playerSlot;   // credit kills / route the D2 AoE lag-comp rewind
            s_meteors[mi] = pm;
            break;
        }
    }
    // Relay to the other players — skip the caster, which already predicted this exact meteor.
    broadcastMeteorEvent(pos, radius, delay, playerSlot);
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
    // Sentinel floor 99 = the host entered The Source (secret superboss). Build the same
    // deterministic chamber and follow — do NOT touch currentFloor or run normal level gen. The
    // Engine + its summoned waves are server-authoritative and arrive via snapshots.
    if (floor == GameConst::SOURCE_SENTINEL_FLOOR) {
        s_engine->enterSourceChamberClient();
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
        // Netcode-audit 2026-07 — the per-slot net trackers must not survive slot recycling
        // either. A stale ack from the previous occupant can still sit inside the 32-deep
        // snapshot history on a fast rejoin, making the server delta this joiner against a
        // baseline it never decoded (self-heals in ≤533 ms, but violates the named-baseline
        // invariant); the activation watermark would sit thousands of ticks above the joiner's
        // fresh clientTick frame. The starve counter is already re-zeroed by the drain loop's
        // !active branch — reset it here too so all three trackers share one lifecycle.
        s_engine->m_clientAckedSnap[playerSlot]    = 0;
        s_engine->m_lastActivationTick[playerSlot] = 0;
        s_engine->m_starvedRepeats[playerSlot]     = 0;
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
        np.invulnTimer = 2.0f; // spawn protection
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
        np.overdriveTimer    = 0.0f;   // Mech Overdrive / War Cry buff must not survive a slot recycle
        // Wanderer kit — a recycled slot must not inherit the previous occupant's absorb
        // pool (a phantom burst on the joiner's first deflect) or leftover stacks/ult.
        np.deflectTimer      = 0.0f;
        np.deflectAbsorbed   = 0.0f;
        np.deflectHitCount   = 0;
        np.deflectSpeedTimer = 0.0f;
        np.deathsDanceTimer  = 0.0f;
        np.counterStacks     = 0;
        for (u32 ct = 0; ct < 5; ct++) np.counterTimers[ct] = 0.0f;
        np.adrenalineUpgraded = false;
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
                if (Inventory::addToBackpack(s_engine->m_inventories[playerSlot], startWpn) >= 0) {
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
    // A slot filled — refresh slots_free and, if we just hit MAX_PLAYERS, close the lobby to joiners.
    s_engine->updateSteamLobbyRoster();
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
    // A slot freed — re-advertise the lobby as joinable and refresh slots_free.
    s_engine->updateSteamLobbyRoster();
}

// Reflect the live roster into the Steam lobby so matchmaking doesn't hand out a full game.
// Only the Steam-relay host that actually owns a lobby does anything here: clients (not SERVER)
// and ENet/LAN hosts (no lobby) fall through as no-ops. setLobbyData/setLobbyJoinable are
// owner-only Steam calls, and the m_netRole guard keeps us from calling them as a client.
// Host: publish everything a browser row / code lookup needs, right after the lobby is created.
//   • version    — the join filter (a mismatched build must never be listed to us)
//   • name       — the HOST'S Steam persona. This used to be a hardcoded title, identical for every
//                  lobby, which is exactly why the browser was useless: every row read the same.
//   • code       — the 4-glyph share code. A RANDOM lookup key, not an encoding of the 64-bit lobby
//                  id (4 glyphs = 20 bits can't hold 64): a joiner finds us by asking Steam for the
//                  lobby whose "code" matches. Must exist before anyone can join by code.
//   • private    — what makes an unlisted game unlisted. The browser query filters private=="0"; a
//                  code lookup ignores it. (The lobby itself stays searchable — that's the trade a
//                  short code requires; see lobby_code.h.)
//   • roster/floor/difficulty — via updateSteamLobbyRoster.
void Engine::publishLobbyIdentity() {
    if (m_netRole != NetRole::SERVER) return;
    if (Steam::currentLobbyId() == 0) return;

    char ver[16];
    std::snprintf(ver, sizeof(ver), "%u", PROTOCOL_VERSION);
    Steam::setLobbyData("version", ver);

    const char* persona = Steam::localPersonaName();
    char lobbyName[80];
    if (persona && persona[0]) std::snprintf(lobbyName, sizeof(lobbyName), "%s's Game", persona);
    else                       std::snprintf(lobbyName, sizeof(lobbyName), "Dungeon Engine Game");
    Steam::setLobbyData("name", lobbyName);

    LobbyCode::generate(static_cast<u32>(std::rand()), m_lobbyCode, sizeof(m_lobbyCode));
    Steam::setLobbyData("code", m_lobbyCode);
    Steam::setLobbyData("private", m_menu.hostPrivate ? "1" : "0");

    LOG_INFO("Steam: hosting a %s lobby - share code %s",
             m_menu.hostPrivate ? "PRIVATE (code/invite only)" : "public", m_lobbyCode);

    // Roster + floor/difficulty, so the browser shows an accurate row immediately (before any join).
    updateSteamLobbyRoster();
}

void Engine::updateSteamLobbyRoster() {
    if (m_netRole != NetRole::SERVER) return;      // only the host owns the lobby
    if (Steam::currentLobbyId() == 0) return;      // ENet/LAN host, or no lobby -> nothing to update
    const u32 connected = Net::getConnectedCount();       // ACTIVE slots incl. host (and both couch slots)
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%u", connected);
    Steam::setLobbyData("players", buf);                  // authoritative roster count the browser displays
    std::snprintf(buf, sizeof(buf), "%u", connected < MAX_PLAYERS ? (MAX_PLAYERS - connected) : 0u);
    Steam::setLobbyData("slots_free", buf);               // discovery metadata (kept for the list filter)
    // Progress metadata the public browser shows so a game is actually identifiable at a glance
    // (a floor-30 Hell run is a very different invitation from a fresh floor-1 Normal one). Republished
    // here on every roster change AND on floor descent, so a browsed row never shows a stale floor.
    std::snprintf(buf, sizeof(buf), "%u", m_level.currentFloor);
    Steam::setLobbyData("floor", buf);
    std::snprintf(buf, sizeof(buf), "%u", static_cast<u32>(m_difficulty));
    Steam::setLobbyData("difficulty", buf);
    Steam::setLobbyJoinable(connected < MAX_PLAYERS);     // full -> invites/join stop working until a slot frees
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
        // Pump Steam callbacks every frame, unconditionally — lobby invites / join requests arrive
        // even while sitting at the menu. No-op when Steam isn't built/initialized.
        Steam::runCallbacks();
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

        // Poll input once per rendered frame — decoupled from physics tick rate. Real frame time is
        // passed in (not FIXED_DT) because this runs once per RENDER frame: it clocks the menu
        // hold-to-repeat, which must advance in wall time regardless of how many substeps follow.
        Input::update(static_cast<f32>(frameTime));
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

        // Auto-screenshot (CLI --screenshot-interval): once IN_GAME, flag a capture every
        // m_shotInterval seconds. Set before render() so it is serviced (and saved) this frame.
        if (m_shotInterval > 0.0 && m_gameState == GameState::IN_GAME) {
            m_shotTimer += frameTime;
            if (m_shotTimer >= m_shotInterval) { m_shotTimer = 0.0; m_screenshotPending = true; }
        }

        render(static_cast<f32>(m_accumulator / FIXED_DT));
        m_frameCount++;

        // Record frame time for profiler
        profilerRecordFrame(frameTime * 1000.0);

        m_statsTimer += frameTime;
        if (m_statsTimer >= 1.0) {
            if (m_gameState == GameState::IN_GAME) logStats();
            checkAchievements();   // 1 Hz poll — catches every equip path incl. loaded saves
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
    X(m_glovesPassive,    m_glovesPassives) \
    X(m_inventoryOpen,        m_inventoryOpenArr)      \
    X(m_characterScreenOpen,  m_characterScreenOpenArr) \
    X(m_inspectYaw,           m_inspectYawArr)          \
    X(m_hitMarkerTimer,       m_hitMarkerTimers)        \
    X(m_potionCooldown,   m_potionCooldowns)\
    X(m_potionLastActivationTick, m_potionLastActivationTicks) \
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
    // Identity stamp for combat callbacks (dodge-through riposte/adrenaline): which
    // inventory/skill-state slot this Player is. Host lanes: lane == net slot. On a CLIENT
    // it's the local lane (0) — its callbacks are prediction against local state anyway.
    m_localPlayer.netSlot = idx;
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
    m_localPlayers[1].invulnTimer = 2.0f;                  // brief spawn protection for P1
    m_players[1].spawnPosition   = m_localPlayers[1].position;
}

// Begin a 2-player couch game once the menu has prepared both lanes. Player 2 was already loaded
// (Continue) or freshly equipped (New); here we prep a fresh Player 1 lane if needed, flip on
// split-screen, and start the world on Player 1's floor with lanes already populated — so a mixed
// New/Continue couch keeps each hero's gear instead of the NEW_GAME wipe erasing the loaded one.
void Engine::startCouchGame() {
    if (!m_menu.p1Continue) equipFreshLane(0); // a continued P1 was already loaded by loadGame
    m_splitPlayerCount = 2;
    Input::setSplitScreen(true);
    // Online couch co-op runs the split-screen lane loop UNDER a net role; the flag opts this
    // session out of the dispatch's force-to-1 guard (set before startGame so the first IN_GAME
    // frame already sees it). For a purely local couch game (m_netRole==NONE) it stays false.
    m_netCouch = (m_netRole != NetRole::NONE);
    startGame(m_menu.p1Continue ? GameStart::CONTINUE : GameStart::NEW_GAME, /*lanesPrepared=*/true);
    positionLocalPlayersAtSpawn();             // self-guards to split-screen

    if (m_netRole == NetRole::SERVER) {
        // Host-couch: seat the second local player at net slot 1 so it rides every snapshot to
        // remote clients (Net::hostServer already reserved slot 1 so no joiner overwrites it). The
        // NetPlayer is initialized from the lane's prepared state; thereafter the per-frame split
        // loop's syncLocalPlayerToNetPlayer keeps position/health/etc. current.
        NetPlayer& np = m_players[1];
        np.active = true;
        np.slotIndex = 1;
        np.playerClass = m_playerClasses[1];
        np.position = m_localPlayers[1].position;
        np.spawnPosition = m_localPlayers[1].position;
        np.maxHealth = m_localPlayers[1].maxHealth;
        np.health = m_localPlayers[1].health;
        np.moveSpeed = m_localPlayers[1].moveSpeed;
        np.weaponState.currentWeapon = 0;
        np.isDead = false;
        np.invulnTimer = 2.0f;
        Server::resetInputBuffer(1);
        resetFireDedup(1);
    }

    m_menu.subState = 0;
    m_menu.msg = nullptr;
}

// Online couch co-op JOIN. Both lanes have characters (couch lobby); prep a fresh P1 lane if needed,
// advertise both classes + localCount=2, and connect. updateLobby finalizes on SV_JOIN_ACCEPT
// (records both slots, flips on split-screen + m_netCouch, starts the game). m_netRole is CLIENT.
void Engine::beginCouchJoin() {
    if (!m_menu.p1Continue) equipFreshLane(0); // a continued P1 was already loaded by loadGame
    m_splitPlayerCount = 2;
    Input::setSplitScreen(true);
    // Tell updateLobby this is a 2-local-player join (it keys split-screen + m_netCouch on this flag
    // when SV_JOIN_ACCEPT arrives). Without it the accept handler treats us as single and drops P2.
    m_menu.couchJoin = true;
    Net::setLocalPlayerClasses(static_cast<u8>(m_playerClasses[0]),
                               static_cast<u8>(m_playerClasses[1]), 2);
    // Route the couch join through Steam relay when the join came from an invite/browse (m_steamJoinHost
    // set by beginSteamJoin), else ENet direct-IP. One-shot: clear the stashed SteamID like the
    // single-player join site so a later ENet couch join can't accidentally reuse a stale host.
    const u64 steamHost = m_steamJoinHost;
    m_steamJoinHost = 0;
    const bool connected = steamHost ? Net::connectToSteamHost(steamHost)
                                     : Net::connectToServer(m_menu.connectAddress);
    if (connected) {
        m_gameState = GameState::CONNECTING;
        m_connectingElapsed = 0.0f;
        m_menu.subState = 0;
        if (steamHost) LOG_INFO("Couch-joining Steam host %llu (2 local players)...",
                                (unsigned long long)steamHost);
        else           LOG_INFO("Couch-joining %s (2 local players)...", m_menu.connectAddress);
    } else {
        // Connection setup failed — drop back to the couch start-mode screen.
        m_netRole = NetRole::NONE;
        m_menu.couchJoin = false;
        m_splitPlayerCount = 2;          // keep the couch setup so they can retry/host
        m_menu.subState = 13;
    }
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
// path) never duplicate it. A field with no NetPlayer home stays at the Player default-zero
// and does NOT persist across the call — that is exactly how the whole Wanderer kit silently
// no-opped for guests until the 2026-07-16 parity port gave deflect/Death's-Dance/adrenaline
// their NetPlayer homes. If a new mechanic reads a Player field for remotes, mirror it here
// (BOTH directions) or it does nothing in co-op.

// Seed `v` from `np` with every field the AI/projectile/skill paths read. Position/yaw/pitch
// are included because skills (PhaseDash, ShadowStrike/Step) read the CURRENT transform to
// compute their destination; the AI/projectile path reads position too. lifesaverArmed (TA-1)
// is mirrored so the one-shot near-death i-frame stays one-shot for remotes.
// `slot` stamps the view's identity (combat callbacks need the right inventory + kill credit);
// `currentFloor` derives the Wanderer adrenaline gates the local player computes per-tick.
static void seedRemoteView(const NetPlayer& np, Player& v, u8 slot, u32 currentFloor) {
    v = Player{};                                        // default-zero Wanderer-only fields
    v.netSlot         = slot;
    if (np.playerClass == PlayerClass::WANDERER) {
        // Mirror the local player's per-tick gates (engine_update_player.cpp) — seedRemoteView
        // starts from Player{}, so without these a remote Wanderer could never gain a stack.
        v.adrenalineUnlocked  = true;
        v.adrenalineUpgraded  = (currentFloor >= 30);
        v.adrenalineMaxStacks = (currentFloor >= 5) ? 5 : 3;
    }
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
    v.graceInvuln     = np.graceInvuln;    // mirror grace tag so the 85%-HP clear persists across frames
    // M-3/M-4: ring-passive state on a remote — Soul Harvest stacks drive the gun-damage /
    // move-speed bonus the AI/combat code reads from the Player view; smokeTimer drives the
    // Phase Strike stealth check in enemy_ai_states.cpp. Without mirroring these, M8 credits
    // stacks/timer onto the NetPlayer but the remote view zero-defaults them and nothing reads.
    v.soulHarvestStacks  = np.soulHarvestStacks;
    v.soulHarvestTimer   = np.soulHarvestTimer;
    v.smokeTimer         = np.smokeTimer;
    v.secondWindCooldown = np.secondWindCooldown;
    // Legendary armor/shield passives (2026-07-16): the perfect-block callback and projectile
    // parry read these off the view; bloodNovaCooldown round-trips so a remote Aegis of Blood
    // can't re-detonate every block (the old reason the callback was pinned to the local player).
    v.offhandSkill      = np.offhandSkill;
    v.chargeStacks      = np.chargeStacks;
    v.chargeTimer       = np.chargeTimer;
    v.hemoTickTimer     = np.hemoTickTimer;
    v.bloodNovaCooldown = np.bloodNovaCooldown;
    // Wanderer death-preamble state (next-batch): mirror so a remote Wanderer's Shadow Dance
    // and mark-prey stacks credit-and-read through the view consistently.
    v.shadowDanceTimer   = np.shadowDanceTimer;
    v.markTimer          = np.markTimer;
    v.markSpeedStacks    = np.markSpeedStacks;
    // Overdrive (Mech Overdrive / War Cry +30% speed): without the mirror a remote's cast set
    // the timer on the throwaway view and writeBack had nothing to persist it into.
    v.overdriveTimer     = np.overdriveTimer;
    // Wanderer kit (Deflect / Death's Dance / Adrenaline) — the absorb pool accumulates on the
    // view during the AI/projectile pass, so it MUST round-trip through NetPlayer every frame
    // or a remote's absorbed damage evaporates before the burst.
    v.deflectTimer       = np.deflectTimer;
    v.deflectAbsorbed    = np.deflectAbsorbed;
    v.deflectHitCount    = np.deflectHitCount;
    v.deflectSpeedTimer  = np.deflectSpeedTimer;
    v.deathsDanceTimer   = np.deathsDanceTimer;
    v.dodgeState.counterStacks = np.counterStacks;
    for (u32 cs = 0; cs < 5; cs++) v.dodgeState.counterTimers[cs] = np.counterTimers[cs];
    // Roll state, READ-ONLY mirror (np.rollTimer is owned by the input replay; writeBack must
    // not touch it): Combat::applyDamageToPlayer's dodge-through detection keys on
    // dodgeState.rolling — without this line a remote's mid-roll hits never counted as a
    // dodge-through, which is why the whole Adrenaline/riposte kit was dead for guests.
    v.dodgeState.rollTimer = np.rollTimer;
    v.dodgeState.rolling   = (np.rollTimer > 0.0f);
    // Shrine buff. MUST be mirrored: seedRemoteView begins with `v = Player{}`, so any field not
    // copied here is silently zeroed on this remote's view every single frame.
    v.shrineBuff         = np.shrineBuff;
    v.shrineBuffValue    = np.shrineBuffValue;
    v.shrineBuffTimer    = np.shrineBuffTimer;
    v.shrineHealthBonus  = np.shrineHealthBonus;
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
    // The only channel by which the server learns a REMOTE took a hit this tick: enemy AI /
    // projectiles damage the throwaway view, not the NetPlayer. serverNetPost reads this to fire
    // the Blood Nova armor retaliation, then clears it.
    np.lastDamageTaken  = v.lastDamageTaken;
    np.lastDamageAttackerIdx = v.lastDamageAttackerIdx; // who hit them — aims Static Charge/Thunderwall
    np.lifesaverArmed   = v.lifesaverArmed;   // TA-1: persist consume/re-arm across frames
    np.graceInvuln      = v.graceInvuln;      // persist grace tag (set when the lifesaver fires on the view)
    // Persist ring-passive state mutations across frames (e.g., a stack decay that some skill
    // path applied to the view) — mirrors the seed list above.
    np.soulHarvestStacks  = v.soulHarvestStacks;
    np.soulHarvestTimer   = v.soulHarvestTimer;
    np.smokeTimer         = v.smokeTimer;
    np.secondWindCooldown = v.secondWindCooldown;
    // Legendary armor/shield passives — the other half of the round-trip (see seedRemoteView).
    np.chargeStacks       = v.chargeStacks;
    np.chargeTimer        = v.chargeTimer;
    np.hemoTickTimer      = v.hemoTickTimer;
    np.bloodNovaCooldown  = v.bloodNovaCooldown;
    // Wanderer death-preamble state (next-batch) — same persistence pattern.
    np.shadowDanceTimer   = v.shadowDanceTimer;
    np.markTimer          = v.markTimer;
    np.markSpeedStacks    = v.markSpeedStacks;
    np.overdriveTimer     = v.overdriveTimer;   // Mech Overdrive / War Cry — see seedRemoteView
    // Wanderer kit — the other half of the round-trip (see seedRemoteView).
    np.deflectTimer       = v.deflectTimer;
    np.deflectAbsorbed    = v.deflectAbsorbed;
    np.deflectHitCount    = v.deflectHitCount;
    np.deflectSpeedTimer  = v.deflectSpeedTimer;
    np.deathsDanceTimer   = v.deathsDanceTimer;
    np.counterStacks      = v.dodgeState.counterStacks;
    for (u32 cs = 0; cs < 5; cs++) np.counterTimers[cs] = v.dodgeState.counterTimers[cs];
    // Shrine buff — the other half of the mirror (see seedRemoteView).
    np.shrineBuff         = v.shrineBuff;
    np.shrineBuffValue    = v.shrineBuffValue;
    np.shrineBuffTimer    = v.shrineBuffTimer;
    np.shrineHealthBonus  = v.shrineHealthBonus;
    for (u32 ms = 0; ms < 20; ms++) np.markSpeedTimers[ms] = v.markSpeedTimers[ms];
}

// Array form (AI + projectile pass): build a view of every ACTIVE, non-dead REMOTE NetPlayer
// (skip the local host slot, whose damage flows through m_localPlayer in gameUpdate). `views[k]`
// is the view for `ptrs[k]`; `slots[k]` records its NetPlayer index for applyRemotePlayerViews.
u32 Engine::buildRemotePlayerViews(Player* views, Player** ptrs, u8* slots) {
    u32 count = 0;
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        // Skip ALL host-local lanes (slots 0..m_splitPlayerCount-1) — their damage flows through the
        // local m_localPlayers[] path in tickSharedSystems, not these remote views. Using the lane
        // count (not m_localPlayerIndex) is required: this runs in tickSharedSystems where
        // m_localPlayerIndex is the LAST swapped lane (count-1), so a `== m_localPlayerIndex` test
        // would mis-skip in host-couch (skip slot 1, then double-target the host at slot 0).
        if (i < m_splitPlayerCount) continue;
        const NetPlayer& np = m_players[i];
        // Only LIVE remotes are targetable. Check health<=0 too, not just isDead: np.isDead is set
        // in serverNetPost (AFTER the AI/projectile pass that consumes these views), so on the death
        // frame isDead still reads false — the health guard excludes the corpse the same frame so
        // enemies don't get a one-frame window to attack a just-killed remote.
        if (!np.active || np.isDead || np.health <= 0.0f) continue;
        slots[count] = static_cast<u8>(i);
        seedRemoteView(np, views[count], static_cast<u8>(i), m_level.currentFloor);
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
    seedRemoteView(m_players[slot], out, slot, m_level.currentFloor);
}

// TA-3 single-slot form: write a remote's skill-mutated view back into its NetPlayer.
void Engine::applyRemotePlayerView(const Player& v, u8 slot) {
    writeBackRemoteView(v, m_players[slot]);
}


// ---------------------------------------------------------------------------
// Steam achievements — 1 Hz poll from run()'s stats block.
// Polling (vs event hooks on every equip path) is deliberate: equips happen via A-button,
// double-click, drag, quickbar, auto-equip-on-pickup AND save-loading — one lazy check
// catches them all and can never rot when a new equip path is added. Any active LOCAL lane
// counts (couch P2 shares P1's Steam account anyway); Steam::unlockAchievement is a no-op
// on itch builds, so this costs a 7-slot scan per second and nothing else.
// ---------------------------------------------------------------------------
void Engine::checkAchievements() {
    if (m_achFullyEquipped || m_gameState != GameState::IN_GAME) return;
    for (u32 lane = 0; lane < m_splitPlayerCount; lane++) {
        const PlayerInventory& inv = m_inventories[m_netRole == NetRole::CLIENT
                                                   ? m_localPlayerIndex : lane];
        bool full = true;
        for (u32 s = 0; s < static_cast<u32>(ItemSlot::COUNT); s++) {
            if (isItemEmpty(inv.equipped[s])) { full = false; break; }
        }
        if (full) {
            Steam::unlockAchievement("ACH_FULLY_EQUIPPED");
            m_achFullyEquipped = true;
            return;
        }
    }
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
        // SERVER: show the authoritative server tick. CLIENT: show the client-local tick
        // (m_clientTick — M1.8); m_serverTick on a client is a leftover counter not
        // meaningful for diagnostics (use m_clockSync.serverTickEst for clock-sync status).
        const u32 displayTick = (m_netRole == NetRole::SERVER) ? m_serverTick : m_clientTick;
        LOG_INFO("FPS: %u | Frame: %.2f ms | Draw: %u | Ent: %u | Players: %u | Tick: %u | %s",
                 m_frameCount, avgFrameTime,
                 Renderer::getDrawCallCount(),
                 EntitySystem::activeCount(m_entities),
                 Net::getConnectedCount(),
                 displayTick,
                 m_netRole == NetRole::SERVER ? "SERVER" : "CLIENT");
    }
}
