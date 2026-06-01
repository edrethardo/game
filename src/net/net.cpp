#include "net/net.h"
#include "net/server.h"
#include "net/packet.h"  // Quantize::unpackPos for SV_LOOT_SPAWN decode (D1.3)
#include "net/upnp.h"    // Upnp::tryAddPortMapping for auto port-forward on Host Game
#include "core/log.h"
#include "platform/clock.h"  // Clock::getElapsedSeconds() for delay timestamps (D5)

#ifdef __SWITCH__
#include <sys/select.h> // needed before enet on Switch (fd_set)
#endif
#include <enet/enet.h>
#include <cstring>
#include <cstdlib>  // rand()

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------
static NetRole          s_role = NetRole::NONE;
static ENetHost*        s_enetHost = nullptr;         // server or client host
static ENetPeer*        s_serverPeer = nullptr;   // client's peer to server
static bool             s_joinFailed = false;     // client: set on SV_JOIN_REJECT or pre-accept disconnect
static NetPlayerSlot    s_slots[MAX_PLAYERS];
static u8               s_localPlayerIndex = 0;
static u8               s_localPlayerClass = 0; // chosen PlayerClass to send in CL_JOIN_REQUEST
static u16              s_seq = 0;
static bool             s_initialized = false;
// Dungeon sync received from the server in SV_JOIN_ACCEPT (client side)
static u32              s_serverLevelSeed = 0;
static u8               s_serverFloor = 1;
static u8               s_serverDifficulty = 0;

// Callbacks
static Net::OnSnapshotFn   s_onSnapshot   = nullptr;
static Net::OnInputFn      s_onInput      = nullptr;
static Net::OnPickupFn     s_onPickup     = nullptr;
static Net::OnRespawnFn    s_onRespawn    = nullptr;
static Net::OnDescendRequestFn s_onDescendRequest = nullptr;
static Net::OnFireWeaponFn s_onFireWeapon = nullptr;
static Net::OnInventorySyncFn s_onInventorySync = nullptr;
static Net::OnTimePingFn   s_onTimePing   = nullptr;
static Net::OnTimePongFn   s_onTimePong   = nullptr;  // client-side SV_TIME_PONG decoder (M1.5)
static Net::OnDamageDoneFn   s_onDamageDone   = nullptr;  // client-side SV_DAMAGE_DONE (M10.2)
static Net::OnDamageToMeFn   s_onDamageToMe   = nullptr;  // client-side SV_DAMAGE_TO_ME (M10.3)
static Net::OnKillFn         s_onKill         = nullptr;  // client-side SV_KILL (D1.1)
static Net::OnPickupResultFn s_onPickupResult = nullptr;  // client-side SV_PICKUP_RESULT (D1.2)
static Net::OnLootSpawnFn    s_onLootSpawn    = nullptr;  // client-side SV_LOOT_SPAWN (D1.3)
static Net::OnEventFn        s_onEvent        = nullptr;
static Net::OnPlayerJoinFn s_onPlayerJoin = nullptr;
static Net::OnPlayerLeftFn s_onPlayerLeft = nullptr;
static Net::OnLevelSeedFn  s_onLevelSeed  = nullptr;

// Channels: 0 = reliable ordered, 1 = unreliable sequenced
static constexpr u32 NUM_CHANNELS = 2;

// M14: fake-loss cvar — set via Net::setFakeLossPct(). Applied at all outgoing send
// callsites so both CLIENT→SERVER and SERVER→CLIENT traffic is stressed uniformly.
static u8 s_fakeLossPct = 0;

// D5: fake-latency cvar — set via Net::setFakeLatencyMs(). When > 0, every outgoing
// packet is enqueued with a deliverAt timestamp instead of being sent immediately.
// Net::pumpDelayQueue() (called at the top of serverNetPre + clientNetPre) drains
// any entries whose deliverAt <= now, simulating the requested one-way delay.
static u32 s_fakeLatencyMs = 0;

// Target type for a queued delayed packet — tells the pump which send helper to invoke.
enum struct DelayTarget : u8 {
    PEER_R,        // sendReliable to a specific player slot
    PEER_U,        // sendUnreliable to a specific player slot
    BROADCAST_R,   // broadcastReliable
    BROADCAST_U,   // broadcastUnreliable
    BROADCAST_SNAP,// broadcastSnapshot
    TO_SERVER_R,   // sendToServer (reliable)
    TO_SERVER_U,   // sendToServer (unreliable)
};

// Maximum queued delayed packets. At 60 Hz with up to 200 ms simulated latency we
// can accumulate at most ~12 packets per sender — 256 slots is generous for 4 players.
static constexpr u32 DELAY_QUEUE_SIZE = 256;

// Maximum payload size we copy into the queue. Snapshots can be up to 8 KB; use
// MAX_SNAPSHOT_SIZE from packet.h so we never truncate.
static constexpr u32 DELAY_MAX_PAYLOAD = 8192;

struct DelayEntry {
    bool        active      = false;
    DelayTarget target;
    u8          playerSlot; // used only for PEER target
    f64         deliverAtSec;
    // Payload copy — avoids lifetime issues with the caller's stack buffer.
    u8          payload[DELAY_MAX_PAYLOAD];
    u32         payloadSize;
};

static DelayEntry s_delayQueue[DELAY_QUEUE_SIZE];

// Find a free slot in the queue; returns DELAY_QUEUE_SIZE on overflow (packet dropped).
static u32 findFreeDelaySlot() {
    for (u32 i = 0; i < DELAY_QUEUE_SIZE; i++) {
        if (!s_delayQueue[i].active) return i;
    }
    return DELAY_QUEUE_SIZE;
}

// Enqueue a packet for delayed delivery. payloadSize must be <= DELAY_MAX_PAYLOAD.
static void enqueueDelayed(DelayTarget target, u8 playerSlot,
                            const u8* data, u32 size) {
    if (size > DELAY_MAX_PAYLOAD) {
        // Oversized — deliver immediately rather than silently drop.
        LOG_WARN("Net delay queue: oversized packet (%u B), sending immediately", size);
        // Caller is responsible for immediate send in this case.
        return;
    }
    u32 slot = findFreeDelaySlot();
    if (slot == DELAY_QUEUE_SIZE) {
        LOG_WARN("Net delay queue: full (%u slots), dropping packet", DELAY_QUEUE_SIZE);
        return;
    }
    DelayEntry& e = s_delayQueue[slot];
    e.active       = true;
    e.target       = target;
    e.playerSlot   = playerSlot;
    e.deliverAtSec = Clock::getElapsedSeconds() + s_fakeLatencyMs * 0.001;
    e.payloadSize  = size;
    std::memcpy(e.payload, data, size);
}

// Returns true if this packet should be dropped (used at all send sites).
// Only active when s_fakeLossPct > 0 to keep the fast path branchless.
static inline bool shouldDropPacket() {
    return s_fakeLossPct > 0 && (rand() % 100) < static_cast<int>(s_fakeLossPct);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static u8 findFreeSlot() {
    for (u8 i = 1; i < MAX_PLAYERS; i++) { // slot 0 = host
        if (s_slots[i].state == SlotState::EMPTY)
            return i;
    }
    return 0xFF;
}

static u8 findSlotByPeer(ENetPeer* peer) {
    for (u8 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].peer == peer)
            return i;
    }
    return 0xFF;
}

static void resetSlots() {
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        s_slots[i] = NetPlayerSlot{};
    }
}

// ---------------------------------------------------------------------------
// Server packet handlers
// ---------------------------------------------------------------------------
static void serverHandlePacket(u8 slot, const u8* data, u32 size) {
    if (size < sizeof(PacketHeader)) return;
    const PacketHeader* hdr = reinterpret_cast<const PacketHeader*>(data);

    switch (hdr->type) {
    case NetPacketType::CL_JOIN_REQUEST: {
        if (size < sizeof(PacketHeader) + 4) break; // header(4) + version(4) minimum
        u32 version;
        std::memcpy(&version, data + sizeof(PacketHeader), 4);
        // Chosen class is an optional 9th byte; 0xFF if absent (the join callback
        // validates and falls back to Warrior for out-of-range/missing values).
        u8 chosenClass = 0xFF;
        if (size >= sizeof(PacketHeader) + 5) chosenClass = data[sizeof(PacketHeader) + 4];
        if (version != PROTOCOL_VERSION) {
            // Send reject
            PacketHeader reject;
            reject.type = NetPacketType::SV_JOIN_REJECT;
            reject.flags = 0;
            reject.seq = s_seq++;
            Net::sendReliable(slot, reinterpret_cast<const u8*>(&reject), sizeof(reject));
            LOG_WARN("Net: rejected player (version mismatch: %u vs %u)", version, PROTOCOL_VERSION);
            break;
        }
        // Accept
        s_slots[slot].state = SlotState::ACTIVE;
        s_slots[slot].connectTimeMs = 0; // joined in time — clear the CONNECTING deadline (N12)
        LOG_INFO("Net: player %u connected (class=%u)", slot, chosenClass);

        // Send accept packet
        u8 buf[12];
        PacketHeader* acc = reinterpret_cast<PacketHeader*>(buf);
        acc->type = NetPacketType::SV_JOIN_ACCEPT;
        acc->flags = 0;
        acc->seq = s_seq++;
        buf[4] = slot;                         // playerIndex
        buf[5] = Net::getConnectedCount();     // playerCount
        buf[6] = Server::getLevelFloor();      // current floor (client generates this floor)
        buf[7] = Server::getLevelDifficulty(); // difficulty tier (folds into the dungeon seed)
        // Send the per-run dungeon seed so the client generates the identical dungeon.
        u32 seed = Server::getLevelSeed();
        std::memcpy(buf + 8, &seed, 4);
        Net::sendReliable(slot, buf, 12);

        if (s_onPlayerJoin) s_onPlayerJoin(slot, chosenClass);
    } break;

    case NetPacketType::CL_INPUT: {
        if (s_onInput) s_onInput(slot, data, size);
    } break;

    case NetPacketType::CL_PICKUP_ITEM: {
        // Client requests pickup of a world item by uid. The engine handler validates
        // proximity/ownership server-side and removes the item (propagates via snapshot).
        if (s_onPickup) s_onPickup(slot, data, size);
    } break;

    case NetPacketType::CL_RESPAWN: {
        // Dead client requests respawn. The engine handler respawns that slot's NetPlayer
        // (idempotent) and the revival propagates back via the next snapshot. Sent reliably
        // so it can't be lost like the old INPUT_EX_RESPAWN-through-the-input-buffer hack.
        if (s_onRespawn) s_onRespawn(slot);
    } break;

    case NetPacketType::CL_REQUEST_DESCEND: {
        // Client at the portal asks the host to trigger the floor descent. The engine handler
        // re-validates proximity to m_level.floorDoorPos + the boss-dead gate (anti-cheat /
        // race-safety) and only then runs triggerFloorDescent(), which broadcasts SV_LEVEL_SEED
        // so the requesting client (and any other clients) transition in lockstep.
        if (s_onDescendRequest) s_onDescendRequest(slot);
    } break;

    case NetPacketType::CL_FIRE_WEAPON: {
        // Client wants to fire its weapon from a specific origin + aim. Engine validates
        // (cooldown gate, origin clamp) and queues a pending fire for the next per-tick
        // handleWeaponFireForPlayer pass to consume — replaces the old FIRE-bit-driven path
        // that fired from drain-derived np.yaw (which could be stale by seconds under UDP
        // loss / queue lag). Hand the raw bytes to the engine so it can unpack the payload
        // itself; minimum size = 4(header) + 4(tick) + 6(pos) + 4(yaw+pitch) = 18 B.
        if (size < sizeof(PacketHeader) + 14) break;
        if (s_onFireWeapon) s_onFireWeapon(slot, data, size);
    } break;

    case NetPacketType::CL_INVENTORY_SYNC: {
        // Client is pushing its saved inventory (Continue-join path). Engine deserializes
        // and replaces the auto-granted starting kit. Size validation is delegated to the
        // handler since the payload shape is engine-side.
        if (s_onInventorySync) s_onInventorySync(slot, data, size);
    } break;

    case NetPacketType::CL_TIME_PING: {
        // Clock-sync handshake (M1.4). Engine handler reads clientTimeMs, stamps
        // serverTick + serverTimeMs, and sends SV_TIME_PONG back on the unreliable
        // channel. Minimum payload: 4B header + 4B clientTimeMs = 8 B.
        if (size < sizeof(PacketHeader) + 4) break;
        if (s_onTimePing) s_onTimePing(slot, data, size);
    } break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Client packet handlers
// ---------------------------------------------------------------------------
static void clientHandlePacket(const u8* data, u32 size) {
    if (size < sizeof(PacketHeader)) return;
    const PacketHeader* hdr = reinterpret_cast<const PacketHeader*>(data);

    switch (hdr->type) {
    case NetPacketType::SV_JOIN_ACCEPT: {
        if (size < 12) break;
        // Validate the assigned slot before storing — a malicious or buggy server
        // sending a bogus byte here would otherwise be used unchecked across the
        // client (m_clientNetSlot → activeNetSlot() → m_players[]/m_inventories[]/
        // m_renderInterp.* every frame, OOB-write all over). Slot 0 is the host's,
        // so a remote slot must be in [1, MAX_PLAYERS). Reject otherwise — the lobby
        // bails on s_joinFailed (M10).
        u8 assignedSlot = data[4];
        if (assignedSlot == 0 || assignedSlot >= MAX_PLAYERS) {
            LOG_WARN("Net: server-assigned slot %u out of range — rejecting join",
                     assignedSlot);
            s_joinFailed = true;
            break;
        }
        s_localPlayerIndex = assignedSlot;
        s_serverFloor      = data[6];
        s_serverDifficulty = data[7];
        std::memcpy(&s_serverLevelSeed, data + 8, 4); // per-run dungeon seed
        LOG_INFO("Net: joined as player %u (floor=%u seed=%u)",
                 s_localPlayerIndex, s_serverFloor, s_serverLevelSeed);
    } break;

    case NetPacketType::SV_JOIN_REJECT: {
        LOG_WARN("Net: join rejected by server");
        s_joinFailed = true; // lobby polls this to bail out of CONNECTING (M10)
    } break;

    case NetPacketType::SV_SNAPSHOT: {
        // [AUDIT-P2] Disambiguate sub-cause D: SV_SNAPSHOT arrived at clientHandlePacket but
        // the callback pointer is null — the registration was clobbered or never set in the
        // first place. Log warns ONCE then sets a sticky bit to avoid log spam (one snapshot
        // every 50 ms = 1200/min worth of warn lines would drown everything else).
        if (!s_onSnapshot) {
            static bool s_warnedNullSnapCb = false;
            if (!s_warnedNullSnapCb) {
                LOG_WARN("[AUDIT-P2] SV_SNAPSHOT received but s_onSnapshot==null — callback not registered");
                s_warnedNullSnapCb = true;
            }
        } else {
            s_onSnapshot(data, size);
        }
    } break;

    case NetPacketType::SV_EVENT: {
        if (s_onEvent) s_onEvent(data, size);
    } break;

    case NetPacketType::SV_PLAYER_LEFT: {
        if (size >= sizeof(PacketHeader) + 1) {
            u8 slot = data[sizeof(PacketHeader)];
            LOG_INFO("Net: player %u left", slot);
            if (s_onPlayerLeft) s_onPlayerLeft(slot);
        }
    } break;

    case NetPacketType::SV_LEVEL_SEED: {
        // Mid-run floor descent. Layout mirrors SV_JOIN_ACCEPT's tail:
        //   [4]=floor [5]=difficulty [6..7]=reserved [8..11]=run seed (u32 LE).
        if (size < 12) break;
        u8  floor      = data[4];
        u8  difficulty = data[5];
        u32 seed;
        std::memcpy(&seed, data + 8, 4);
        // Keep the client-side join getters coherent in case of a later late-join path.
        s_serverFloor      = floor;
        s_serverDifficulty = difficulty;
        s_serverLevelSeed  = seed;
        LOG_INFO("Net: server descended to floor %u (diff=%u seed=%u)",
                 floor, difficulty, seed);
        if (s_onLevelSeed) s_onLevelSeed(floor, difficulty, seed);
    } break;

    case NetPacketType::SV_TIME_PONG: {
        // Clock-sync pong (M1.5). Payload: 4B header + 12B body (clientTimeMs +
        // serverTick + serverTimeMs). Engine handler strips the header and passes
        // the 12-byte body to Client::handleTimePong → ClockSyncOps::onPongReceived.
        if (size < sizeof(PacketHeader) + 12) break;
        if (s_onTimePong) s_onTimePong(data, size);
    } break;

    case NetPacketType::SV_DAMAGE_DONE: {
        // M10.2 — Server confirms a remote player's fire hit an entity. Unpack the
        // firing client's tick + entity index and forward to the engine's ack handler.
        // Payload: 4B header + u32 clientTick(4) + u16 targetIdx(2) + u16 reserved(2).
        if (size < sizeof(PacketHeader) + 8) break;
        u32 clientTick;
        u16 targetIdx;
        std::memcpy(&clientTick, data + sizeof(PacketHeader),     4);
        std::memcpy(&targetIdx,  data + sizeof(PacketHeader) + 4, 2);
        if (s_onDamageDone) s_onDamageDone(clientTick, targetIdx);
    } break;

    case NetPacketType::SV_DAMAGE_TO_ME: {
        // M10.3 — Server confirms a projectile hit the local player. Unpack the
        // projectile key + damage and forward to the engine's ack handler.
        // Payload: 4B header + u32 key(4) + f32 damage(4) + u16 reserved(2) = 14B total.
        if (size < sizeof(PacketHeader) + 10) break;
        u32 key;
        f32 damage;
        std::memcpy(&key,    data + sizeof(PacketHeader),     4);
        std::memcpy(&damage, data + sizeof(PacketHeader) + 4, 4);
        if (s_onDamageToMe) s_onDamageToMe(key, damage);
    } break;

    case NetPacketType::SV_KILL: {
        // D1.1 — Server broadcast: a kill was confirmed. Payload (6 B):
        //   u8 killerSlot + u8 victimType + u16 victimIdx + u8 weaponMeshId + u8 isCrit.
        if (size < sizeof(PacketHeader) + 6) break;
        const u8* p = data + sizeof(PacketHeader);
        u8  killerSlot   = p[0];
        u8  victimType   = p[1];
        u16 victimIdx;
        std::memcpy(&victimIdx, p + 2, 2);
        u8 weaponMeshId  = p[4];
        u8 isCrit        = p[5];
        if (s_onKill) s_onKill(killerSlot, victimType, victimIdx, weaponMeshId, isCrit);
    } break;

    case NetPacketType::SV_PICKUP_RESULT: {
        // D1.2 — Server response to CL_PICKUP_ITEM. Payload (6 B):
        //   u8 accept + u8 reserved + u32 itemUid.
        if (size < sizeof(PacketHeader) + 6) break;
        const u8* p = data + sizeof(PacketHeader);
        u8 accept = p[0];
        // p[1] = reserved (skip)
        u32 uid;
        std::memcpy(&uid, p + 2, 4);
        if (s_onPickupResult) s_onPickupResult(accept, uid);
    } break;

    case NetPacketType::SV_LOOT_SPAWN: {
        // D1.3 — Server broadcast: a world item spawned. Payload (12 B):
        //   u32 uid + u16 posXQ + u16 posYQ + u16 posZQ + u16 itemDefId.
        if (size < sizeof(PacketHeader) + 12) break;
        const u8* p = data + sizeof(PacketHeader);
        u32 uid;
        std::memcpy(&uid, p, 4);
        u16 posXQ, posYQ, posZQ, defId;
        std::memcpy(&posXQ, p + 4, 2);
        std::memcpy(&posYQ, p + 6, 2);
        std::memcpy(&posZQ, p + 8, 2);
        std::memcpy(&defId, p + 10, 2);
        if (s_onLootSpawn) {
            f32 x = Quantize::unpackPos(posXQ);
            f32 y = Quantize::unpackPos(posYQ);
            f32 z = Quantize::unpackPos(posZQ);
            s_onLootSpawn(uid, x, y, z, defId);
        }
    } break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool Net::init() {
    if (s_initialized) return true;
    if (enet_initialize() != 0) {
        LOG_ERROR("Net: enet_initialize failed");
        return false;
    }
    s_initialized = true;
    resetSlots();
    s_role = NetRole::NONE;
    s_enetHost = nullptr;
    s_serverPeer = nullptr;
    s_localPlayerIndex = 0;
    s_seq = 0;
    LOG_INFO("Net: initialized");
    return true;
}

void Net::shutdown() {
    disconnect();
    if (s_initialized) {
        enet_deinitialize();
        s_initialized = false;
    }
    LOG_INFO("Net: shut down");
}

bool Net::hostServer(u16 port) {
    if (!s_initialized) return false;
    if (s_enetHost) return false;

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;

    // MAX_PLAYERS - 1 peers (slot 0 is the local host)
    s_enetHost = enet_host_create(&address, MAX_PLAYERS - 1, NUM_CHANNELS, 0, 0);
    if (!s_enetHost) {
        LOG_ERROR("Net: failed to create server on port %u", port);
        return false;
    }

    s_role = NetRole::SERVER;
    s_localPlayerIndex = 0;
    resetSlots();
    s_slots[0].state = SlotState::ACTIVE;
    s_slots[0].playerIndex = 0;

    LOG_INFO("Net: hosting on port %u", port);

    // Best-effort UPnP IGD port mapping so friends on other networks can join
    // without the host manually configuring their router. Blocking for up to
    // ~1 s (SSDP discovery); fine on a one-time menu action. Failure is non-
    // fatal — the host stays LAN-reachable either way, and the lobby UI reads
    // Upnp::lastError() to tell the user. On Switch the wrapper is a stub
    // that returns false instantly with "UPnP unsupported on this platform".
    {
        char extIp[64] = {0};
        char errMsg[128] = {0};
        if (Upnp::tryAddPortMapping(port, /*discoveryTimeoutMs=*/1000, extIp, errMsg)) {
            LOG_INFO("Net: UPnP mapped UDP %u — external IP %s", port, extIp);
        } else {
            LOG_INFO("Net: UPnP not available (%s) — host is LAN-only unless port-forwarded", errMsg);
        }
    }
    return true;
}

const char* Net::getExternalIp() { return Upnp::currentExternalIp(); }
const char* Net::getUpnpError()  { return Upnp::lastError(); }

void Net::setLocalPlayerClass(u8 classId) {
    s_localPlayerClass = classId;
}

bool Net::connectToServer(const char* address, u16 port) {
    if (!s_initialized) return false;
    if (s_enetHost) return false;

    s_enetHost = enet_host_create(nullptr, 1, NUM_CHANNELS, 0, 0);
    if (!s_enetHost) {
        LOG_ERROR("Net: failed to create client host");
        return false;
    }

    ENetAddress addr;
    enet_address_set_host(&addr, address);
    addr.port = port;

    s_serverPeer = enet_host_connect(s_enetHost, &addr, NUM_CHANNELS, 0);
    if (!s_serverPeer) {
        LOG_ERROR("Net: failed to initiate connection to %s:%u", address, port);
        enet_host_destroy(s_enetHost);
        s_enetHost = nullptr;
        return false;
    }

    s_role = NetRole::CLIENT;
    s_joinFailed = false; // fresh attempt
    resetSlots();
    LOG_INFO("Net: connecting to %s:%u...", address, port);
    return true;
}

void Net::disconnect() {
    if (s_role == NetRole::CLIENT && s_serverPeer) {
        enet_peer_disconnect(s_serverPeer, 0);
        // Flush disconnect
        ENetEvent event;
        while (enet_host_service(s_enetHost, &event, 200) > 0) {
            if (event.type == ENET_EVENT_TYPE_DISCONNECT) break;
            if (event.type == ENET_EVENT_TYPE_RECEIVE)
                enet_packet_destroy(event.packet);
        }
        s_serverPeer = nullptr;
    }

    if (s_enetHost) {
        enet_host_destroy(s_enetHost);
        s_enetHost = nullptr;
    }

    // Release the UPnP port-mapping if we held one (hostServer paired with this
    // on success). No-op for clients and on Switch. Done before resetting slots
    // because the wrapper logs to LOG_WARN on failure and we want that visible
    // in the disconnect line.
    Upnp::removePortMapping();

    resetSlots();
    s_role = NetRole::NONE;
    s_localPlayerIndex = 0;
    // Hygiene (CV-3): clear server-level statics so a stale value can't be read by a future
    // lobby probe between connect attempts. They're refreshed by the next SV_JOIN_ACCEPT.
    s_serverLevelSeed  = 0;
    s_serverFloor      = 1;
    s_serverDifficulty = 0;
}

void Net::poll() {
    if (!s_enetHost) return;

    ENetEvent event;
    while (enet_host_service(s_enetHost, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            if (s_role == NetRole::SERVER) {
                u8 slot = findFreeSlot();
                if (slot == 0xFF) {
                    // Server full
                    enet_peer_disconnect(event.peer, 0);
                    LOG_WARN("Net: server full, rejecting connection");
                    break;
                }
                s_slots[slot].state = SlotState::CONNECTING;
                s_slots[slot].playerIndex = slot;
                s_slots[slot].peer = event.peer;
                s_slots[slot].connectTimeMs = enet_time_get(); // start the join deadline (N12)
                event.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(slot));
                // Tighten ENet's own keep-alive timeout (default ~30 s) so a peer that
                // silently dies after joining is also reaped quickly. The CONNECTING
                // sweep above handles the never-sent-JOIN case independently.
                enet_peer_timeout(event.peer, 0, 4000, 8000);
                LOG_INFO("Net: peer connecting to slot %u", slot);
            } else {
                // Client connected to server
                LOG_INFO("Net: connected to server");
                // Send join request: header(4) + version(4) + chosenClass(1) = 9 bytes.
                // The class byte (added after the seed/floor/difficulty SV_JOIN_ACCEPT
                // work) tells the server which class to set up for this slot so a joiner
                // isn't forced to Warrior. Older servers that only read 8 bytes simply
                // ignore the trailing byte; the size guard below accepts >= 8.
                u8 buf[sizeof(PacketHeader) + 5];
                PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
                hdr->type = NetPacketType::CL_JOIN_REQUEST;
                hdr->flags = 0;
                hdr->seq = s_seq++;
                u32 version = PROTOCOL_VERSION;
                std::memcpy(buf + sizeof(PacketHeader), &version, 4);
                buf[sizeof(PacketHeader) + 4] = s_localPlayerClass; // chosen PlayerClass
                sendToServer(buf, sizeof(buf), true);
            }
        } break;

        case ENET_EVENT_TYPE_RECEIVE: {
            const u8* data = event.packet->data;
            u32 size = static_cast<u32>(event.packet->dataLength);

            if (s_role == NetRole::SERVER) {
                u8 slot = static_cast<u8>(reinterpret_cast<uintptr_t>(event.peer->data));
                serverHandlePacket(slot, data, size);
                // Update RTT
                if (slot < MAX_PLAYERS)
                    s_slots[slot].rttMs = event.peer->roundTripTime;
            } else {
                // [AUDIT-P2] Disambiguate sub-causes C / D / E: log every packet the CLIENT
                // actually receives from ENet (before dispatch). If we see NO RX lines at all,
                // either Net::poll isn't running (E) or the broadcast isn't reaching us (C).
                // If we see RX with type=SV_SNAPSHOT (NetPacketType numeric value) but no
                // matching [AUDIT-P1] snap rx, sub-cause D (callback null). Throttled every
                // 5th packet so steady-state spam stays readable; the FIRST few packets are
                // always logged so we see the join handshake.
                static u32 s_pollRxLogCounter = 0;
                u8 pktType = (size >= 1) ? data[0] : 0xFF;
                if (s_pollRxLogCounter < 4 || (s_pollRxLogCounter % 5) == 0) {
                    LOG_INFO("[AUDIT-P2] poll RX type=%u size=%u", pktType, size);
                }
                s_pollRxLogCounter++;
                clientHandlePacket(data, size);
            }

            enet_packet_destroy(event.packet);
        } break;

        case ENET_EVENT_TYPE_DISCONNECT: {
            if (s_role == NetRole::SERVER) {
                u8 slot = findSlotByPeer(event.peer);
                if (slot != 0xFF) {
                    LOG_INFO("Net: player %u disconnected", slot);
                    s_slots[slot].state = SlotState::DISCONNECTED;
                    s_slots[slot].peer = nullptr;

                    // Broadcast player left
                    u8 buf[sizeof(PacketHeader) + 1];
                    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
                    hdr->type = NetPacketType::SV_PLAYER_LEFT;
                    hdr->flags = 0;
                    hdr->seq = s_seq++;
                    buf[sizeof(PacketHeader)] = slot;
                    broadcastReliable(buf, sizeof(buf));

                    if (s_onPlayerLeft) s_onPlayerLeft(slot);

                    // Free slot for reuse
                    s_slots[slot].state = SlotState::EMPTY;
                    s_slots[slot].playerIndex = 0xFF;
                }
            } else {
                LOG_INFO("Net: disconnected from server");
                s_serverPeer = nullptr;
                s_joinFailed = true; // covers server-full reject (peer dropped) + any pre-accept drop (M10)
            }
        } break;

        case ENET_EVENT_TYPE_NONE:
            break;
        }
    }

    // N12: evict peers stuck in CONNECTING (handshake done, no CL_JOIN_REQUEST) so an
    // idle/abandoned connection can't hold a slot until ENet's own long timeout. A
    // real joiner flips to ACTIVE (clearing connectTimeMs) well within the window.
    if (s_role == NetRole::SERVER) {
        u32 nowMs = enet_time_get();
        for (u32 i = 1; i < MAX_PLAYERS; i++) { // slot 0 = host, never CONNECTING
            if (s_slots[i].state != SlotState::CONNECTING) continue;
            if (nowMs - s_slots[i].connectTimeMs < CONNECTING_TIMEOUT_MS) continue;
            LOG_WARN("Net: dropping slot %u — no join request within %u ms", i, CONNECTING_TIMEOUT_MS);
            if (s_slots[i].peer)
                enet_peer_disconnect_now(static_cast<ENetPeer*>(s_slots[i].peer), 0);
            s_slots[i] = NetPlayerSlot{}; // back to EMPTY so findFreeSlot reuses it
        }
    }
}

// ---------------------------------------------------------------------------
// D5: sendImmediate_* helpers — the actual ENet send. Used both by the public
// API (when fake-latency is off) and by pumpDelayQueue() (when delivering a
// queued packet). Role guards are deliberately omitted here because the pump
// already validated the role at enqueue time.
// ---------------------------------------------------------------------------

// ENet only takes ownership of a packet on successful enet_peer_send (return >= 0).
// On failure (mid-handshake peer, disconnected slot) the packet leaks unless we
// destroy it explicitly — same defensive pattern used in broadcastSnapshot below.
static void sendImmediate_Reliable(u8 playerSlot, const u8* data, u32 size) {
    if (playerSlot >= MAX_PLAYERS || !s_slots[playerSlot].peer) return;
    ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(static_cast<ENetPeer*>(s_slots[playerSlot].peer), 0, pkt) < 0)
        enet_packet_destroy(pkt);
}

static void sendImmediate_Unreliable(u8 playerSlot, const u8* data, u32 size) {
    if (playerSlot >= MAX_PLAYERS || !s_slots[playerSlot].peer) return;
    ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNSEQUENCED);
    if (enet_peer_send(static_cast<ENetPeer*>(s_slots[playerSlot].peer), 1, pkt) < 0)
        enet_packet_destroy(pkt);
}

static void sendImmediate_BroadcastReliable(const u8* data, u32 size) {
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].state == SlotState::ACTIVE && s_slots[i].peer) {
            ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
            if (enet_peer_send(static_cast<ENetPeer*>(s_slots[i].peer), 0, pkt) < 0)
                enet_packet_destroy(pkt);
        }
    }
}

static void sendImmediate_BroadcastUnreliable(const u8* data, u32 size) {
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].state == SlotState::ACTIVE && s_slots[i].peer) {
            ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNSEQUENCED);
            if (enet_peer_send(static_cast<ENetPeer*>(s_slots[i].peer), 1, pkt) < 0)
                enet_packet_destroy(pkt);
        }
    }
}

static void sendImmediate_BroadcastSnapshot(const u8* data, u32 size) {
    // UNRELIABLE_FRAGMENT: ENet fragments payloads over the MTU into MTU-sized
    // pieces sent unreliably (peer.c chooses SEND_UNRELIABLE_FRAGMENT, not the
    // reliable fragment path an UNSEQUENCED oversize packet would fall back to).
    // Sub-MTU snapshots are sent as a single unreliable datagram. On send failure
    // (enet_peer_send returns < 0, e.g. peer mid-handshake) we must destroy the
    // packet ourselves; ENet only takes ownership on success.
    u32 activePeers = 0;  // [AUDIT-P2] count for the diagnostic LOG below
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].state == SlotState::ACTIVE && s_slots[i].peer) {
            activePeers++;
            ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
            if (enet_peer_send(static_cast<ENetPeer*>(s_slots[i].peer), 1, pkt) < 0)
                enet_packet_destroy(pkt);
        }
    }
    // [AUDIT-P2] Disambiguate sub-cause B: if active_peers=0 here while the server is
    // publishing snapshots, the joiner connected (ENET_EVENT_TYPE_CONNECT fired) but
    // never advanced to ACTIVE — almost always a version-mismatch reject or a stuck
    // CL_JOIN_REQUEST. Throttled every 5th broadcast (~4 Hz) to match snap tx cadence.
    static u32 s_bcastLogCounter = 0;
    if ((s_bcastLogCounter++ % 5) == 0) {
        LOG_INFO("[AUDIT-P2] broadcastSnapshot size=%u active_peers=%u",
                 size, activePeers);
    }
}

static void sendImmediate_ToServer(const u8* data, u32 size, bool reliable) {
    if (!s_serverPeer) return;
    u32 flags   = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    u8  channel = reliable ? 0 : 1;
    ENetPacket* pkt = enet_packet_create(data, size, flags);
    if (enet_peer_send(s_serverPeer, channel, pkt) < 0)
        enet_packet_destroy(pkt);
}

// ---------------------------------------------------------------------------
// D5: pumpDelayQueue — called once per frame at the top of serverNetPre and
// clientNetPre. Scans the queue and delivers any entries whose deliverAt time
// has elapsed, then compacts the queue by clearing their active flag.
// ---------------------------------------------------------------------------
void Net::pumpDelayQueue() {
    const f64 now = Clock::getElapsedSeconds();
    for (u32 i = 0; i < DELAY_QUEUE_SIZE; i++) {
        DelayEntry& e = s_delayQueue[i];
        if (!e.active) continue;
        if (now < e.deliverAtSec) continue;
        // Deadline reached — deliver via the matching immediate helper.
        switch (e.target) {
        case DelayTarget::PEER_R:
            sendImmediate_Reliable(e.playerSlot, e.payload, e.payloadSize);
            break;
        case DelayTarget::PEER_U:
            sendImmediate_Unreliable(e.playerSlot, e.payload, e.payloadSize);
            break;
        case DelayTarget::BROADCAST_R:
            sendImmediate_BroadcastReliable(e.payload, e.payloadSize);
            break;
        case DelayTarget::BROADCAST_U:
            sendImmediate_BroadcastUnreliable(e.payload, e.payloadSize);
            break;
        case DelayTarget::BROADCAST_SNAP:
            sendImmediate_BroadcastSnapshot(e.payload, e.payloadSize);
            break;
        case DelayTarget::TO_SERVER_R:
            sendImmediate_ToServer(e.payload, e.payloadSize, /*reliable=*/true);
            break;
        case DelayTarget::TO_SERVER_U:
            sendImmediate_ToServer(e.payload, e.payloadSize, /*reliable=*/false);
            break;
        }
        e.active = false; // free slot
    }
}

// ---------------------------------------------------------------------------
// Public send API — delegates to sendImmediate_* directly when fake-latency is
// off (fast path), or enqueues for delayed delivery when s_fakeLatencyMs > 0.
// ---------------------------------------------------------------------------

void Net::sendReliable(u8 playerSlot, const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    if (playerSlot >= MAX_PLAYERS || !s_slots[playerSlot].peer) return;
    // M14: fake-loss injection on SERVER→CLIENT direction.
    if (shouldDropPacket()) return;
    // D5: enqueue if fake latency is enabled, otherwise send immediately.
    if (s_fakeLatencyMs > 0) {
        enqueueDelayed(DelayTarget::PEER_R, playerSlot, data, size);
        return;
    }
    sendImmediate_Reliable(playerSlot, data, size);
}

void Net::sendUnreliable(u8 playerSlot, const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    if (playerSlot >= MAX_PLAYERS || !s_slots[playerSlot].peer) return;
    // M14: fake-loss injection on SERVER→CLIENT direction.
    if (shouldDropPacket()) return;
    // D5: enqueue if fake latency is enabled, otherwise send immediately.
    if (s_fakeLatencyMs > 0) {
        enqueueDelayed(DelayTarget::PEER_U, playerSlot, data, size);
        return;
    }
    sendImmediate_Unreliable(playerSlot, data, size);
}

void Net::broadcastReliable(const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    // D5: enqueue if fake latency is enabled.
    if (s_fakeLatencyMs > 0) {
        enqueueDelayed(DelayTarget::BROADCAST_R, 0, data, size);
        return;
    }
    sendImmediate_BroadcastReliable(data, size);
}

void Net::broadcastUnreliable(const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    // D5: enqueue if fake latency is enabled.
    if (s_fakeLatencyMs > 0) {
        enqueueDelayed(DelayTarget::BROADCAST_U, 0, data, size);
        return;
    }
    sendImmediate_BroadcastUnreliable(data, size);
}

void Net::broadcastSnapshot(const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    // D5: enqueue if fake latency is enabled. Snapshots are the primary payload we want
    // to delay — they carry the authoritative world state the client needs to see late.
    if (s_fakeLatencyMs > 0) {
        enqueueDelayed(DelayTarget::BROADCAST_SNAP, 0, data, size);
        return;
    }
    sendImmediate_BroadcastSnapshot(data, size);
}

// D7.3 — Per-slot snapshot send using ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT so that
// large payloads (e.g. a full snapshot for a newly-joined client) are fragmented
// across MTU-sized datagrams, matching the behaviour of broadcastSnapshot. Delta
// snapshots in steady state will be well under one MTU (a few changed records) but
// we use the same flag for safety. On send failure (peer mid-handshake or gone) the
// packet is destroyed immediately — ENet only takes ownership on success.
void Net::sendSnapshotToSlot(u8 playerSlot, const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    if (playerSlot >= MAX_PLAYERS || !s_slots[playerSlot].peer) return;
    if (shouldDropPacket()) return; // M14: fake-loss injection
    if (s_fakeLatencyMs > 0) {
        // Reuse the BROADCAST_SNAP delay target — it calls sendImmediate_BroadcastSnapshot
        // which iterates all slots. That's wasteful for a per-slot send, but the delay
        // queue is a debug/smoke-test facility and the overhead doesn't matter there.
        // A clean fix would add a PEER_SNAP target; deferred as not worth the complexity.
        enqueueDelayed(DelayTarget::BROADCAST_SNAP, playerSlot, data, size);
        return;
    }
    ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
    if (enet_peer_send(static_cast<ENetPeer*>(s_slots[playerSlot].peer), 1, pkt) < 0)
        enet_packet_destroy(pkt);
}

void Net::broadcastLevelSeed(u8 floor, u8 difficulty, u32 seed) {
    if (s_role != NetRole::SERVER) return;
    // 12-byte packet, same layout/size as SV_JOIN_ACCEPT so the wire stays uniform:
    //   [0..3]=header [4]=floor [5]=difficulty [6..7]=reserved [8..11]=seed (u32 LE).
    u8 buf[12];
    PacketHeader* h = reinterpret_cast<PacketHeader*>(buf);
    h->type  = NetPacketType::SV_LEVEL_SEED;
    h->flags = 0;
    h->seq   = s_seq++;
    buf[4] = floor;
    buf[5] = difficulty;
    buf[6] = 0;
    buf[7] = 0;
    std::memcpy(buf + 8, &seed, 4);
    broadcastReliable(buf, sizeof(buf)); // reliable: a missed descent would hard-desync
}

void Net::sendToServer(const u8* data, u32 size, bool reliable) {
    if (s_role != NetRole::CLIENT || !s_serverPeer) return;
    // M14: fake-loss injection — silently drop this outbound packet at the configured rate.
    if (shouldDropPacket()) return;
    // D5: enqueue if fake latency is enabled; choose the matching target enum.
    if (s_fakeLatencyMs > 0) {
        DelayTarget t = reliable ? DelayTarget::TO_SERVER_R : DelayTarget::TO_SERVER_U;
        enqueueDelayed(t, 0, data, size);
        return;
    }
    sendImmediate_ToServer(data, size, reliable);
}

NetRole Net::getRole()              { return s_role; }
u8      Net::getLocalPlayerIndex()  { return s_localPlayerIndex; }
bool    Net::joinFailed()           { return s_joinFailed; }
u32     Net::getServerLevelSeed()       { return s_serverLevelSeed; }
u8      Net::getServerLevelFloor()      { return s_serverFloor; }
u8      Net::getServerLevelDifficulty() { return s_serverDifficulty; }
bool    Net::isConnected()          { return s_enetHost != nullptr; }

const NetPlayerSlot* Net::getSlots() { return s_slots; }

u32 Net::getConnectedCount() {
    u32 count = 0;
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].state == SlotState::ACTIVE) count++;
    }
    return count;
}

NetStats Net::getStats(u8 playerSlot) {
    NetStats stats = {};
    if (s_role == NetRole::SERVER && playerSlot < MAX_PLAYERS && s_slots[playerSlot].peer) {
        ENetPeer* p = static_cast<ENetPeer*>(s_slots[playerSlot].peer);
        stats.rttMs = static_cast<f32>(p->roundTripTime);
        stats.packetLoss = p->packetLoss / 65536.0f;
    } else if (s_role == NetRole::CLIENT && s_serverPeer) {
        stats.rttMs = static_cast<f32>(s_serverPeer->roundTripTime);
        stats.packetLoss = s_serverPeer->packetLoss / 65536.0f;
    }
    return stats;
}

void Net::setOnSnapshot(OnSnapshotFn fn)   { s_onSnapshot = fn; }
void Net::setOnInput(OnInputFn fn)         { s_onInput = fn; }
void Net::setOnPickup(OnPickupFn fn)       { s_onPickup = fn; }
void Net::setOnRespawn(OnRespawnFn fn)     { s_onRespawn = fn; }
void Net::setOnDescendRequest(OnDescendRequestFn fn) { s_onDescendRequest = fn; }
void Net::setOnFireWeapon(OnFireWeaponFn fn) { s_onFireWeapon = fn; }
void Net::setOnInventorySync(OnInventorySyncFn fn) { s_onInventorySync = fn; }
void Net::setOnTimePing(OnTimePingFn fn)   { s_onTimePing = fn; }
void Net::setOnTimePong(OnTimePongFn fn)   { s_onTimePong = fn; }
void Net::setOnDamageDone(OnDamageDoneFn fn)     { s_onDamageDone   = fn; }  // M10.2
void Net::setOnDamageToMe(OnDamageToMeFn fn)     { s_onDamageToMe   = fn; }  // M10.3
void Net::setOnKill(OnKillFn fn)                 { s_onKill         = fn; }  // D1.1
void Net::setOnPickupResult(OnPickupResultFn fn) { s_onPickupResult = fn; }  // D1.2
void Net::setOnLootSpawn(OnLootSpawnFn fn)       { s_onLootSpawn    = fn; }  // D1.3
void Net::setOnEvent(OnEventFn fn)               { s_onEvent        = fn; }
void Net::setOnPlayerJoin(OnPlayerJoinFn fn) { s_onPlayerJoin = fn; }
void Net::setOnPlayerLeft(OnPlayerLeftFn fn) { s_onPlayerLeft = fn; }
void Net::setOnLevelSeed(OnLevelSeedFn fn)   { s_onLevelSeed = fn; }

// M14: fake-loss cvar accessors
void Net::setFakeLossPct(u8 pct) { s_fakeLossPct = pct; }
u8   Net::getFakeLossPct()       { return s_fakeLossPct; }

// D5: fake-latency cvar accessors — set by the engine each frame so that
// pumpDelayQueue can deliver queued packets at the right wall-clock time.
void Net::setFakeLatencyMs(u32 ms) { s_fakeLatencyMs = ms; }
u32  Net::getFakeLatencyMs()       { return s_fakeLatencyMs; }
