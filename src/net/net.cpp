#include "net/net.h"
#include "net/server.h"
#include "core/log.h"

#ifdef __SWITCH__
#include <sys/select.h> // needed before enet on Switch (fd_set)
#endif
#include <enet/enet.h>
#include <cstring>

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
static Net::OnEventFn      s_onEvent      = nullptr;
static Net::OnPlayerJoinFn s_onPlayerJoin = nullptr;
static Net::OnPlayerLeftFn s_onPlayerLeft = nullptr;
static Net::OnLevelSeedFn  s_onLevelSeed  = nullptr;

// Channels: 0 = reliable ordered, 1 = unreliable sequenced
static constexpr u32 NUM_CHANNELS = 2;

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
    return true;
}

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

// ENet only takes ownership of a packet on successful enet_peer_send (return >= 0).
// On failure (mid-handshake peer, disconnected slot) the packet leaks unless we
// destroy it explicitly — same defensive pattern used in broadcastSnapshot below.
void Net::sendReliable(u8 playerSlot, const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    if (playerSlot >= MAX_PLAYERS || !s_slots[playerSlot].peer) return;
    ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(static_cast<ENetPeer*>(s_slots[playerSlot].peer), 0, pkt) < 0)
        enet_packet_destroy(pkt);
}

void Net::sendUnreliable(u8 playerSlot, const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    if (playerSlot >= MAX_PLAYERS || !s_slots[playerSlot].peer) return;
    ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNSEQUENCED);
    if (enet_peer_send(static_cast<ENetPeer*>(s_slots[playerSlot].peer), 1, pkt) < 0)
        enet_packet_destroy(pkt);
}

void Net::broadcastReliable(const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    // Send only to active peers (not disconnected/empty slots)
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].state == SlotState::ACTIVE && s_slots[i].peer) {
            ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
            if (enet_peer_send(static_cast<ENetPeer*>(s_slots[i].peer), 0, pkt) < 0)
                enet_packet_destroy(pkt);
        }
    }
}

void Net::broadcastUnreliable(const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].state == SlotState::ACTIVE && s_slots[i].peer) {
            ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNSEQUENCED);
            if (enet_peer_send(static_cast<ENetPeer*>(s_slots[i].peer), 1, pkt) < 0)
                enet_packet_destroy(pkt);
        }
    }
}

void Net::broadcastSnapshot(const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
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
    u32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    u8 channel = reliable ? 0 : 1;
    ENetPacket* pkt = enet_packet_create(data, size, flags);
    if (enet_peer_send(s_serverPeer, channel, pkt) < 0)
        enet_packet_destroy(pkt);
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
void Net::setOnEvent(OnEventFn fn)         { s_onEvent = fn; }
void Net::setOnPlayerJoin(OnPlayerJoinFn fn) { s_onPlayerJoin = fn; }
void Net::setOnPlayerLeft(OnPlayerLeftFn fn) { s_onPlayerLeft = fn; }
void Net::setOnLevelSeed(OnLevelSeedFn fn)   { s_onLevelSeed = fn; }
