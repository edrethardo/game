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
static NetPlayerSlot    s_slots[MAX_PLAYERS];
static u8               s_localPlayerIndex = 0;
static u16              s_seq = 0;
static bool             s_initialized = false;

// Callbacks
static Net::OnSnapshotFn   s_onSnapshot   = nullptr;
static Net::OnInputFn      s_onInput      = nullptr;
static Net::OnEventFn      s_onEvent      = nullptr;
static Net::OnPlayerJoinFn s_onPlayerJoin = nullptr;
static Net::OnPlayerLeftFn s_onPlayerLeft = nullptr;

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
        s_slots[i] = {};
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
        if (size < sizeof(PacketHeader) + 4) break;
        u32 version;
        std::memcpy(&version, data + sizeof(PacketHeader), 4);
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
        LOG_INFO("Net: player %u connected", slot);

        // Send accept packet
        u8 buf[12];
        PacketHeader* acc = reinterpret_cast<PacketHeader*>(buf);
        acc->type = NetPacketType::SV_JOIN_ACCEPT;
        acc->flags = 0;
        acc->seq = s_seq++;
        buf[4] = slot;                    // playerIndex
        buf[5] = Net::getConnectedCount(); // playerCount
        buf[6] = 0; buf[7] = 0;          // padding
        // Send the actual level seed so client generates the same dungeon
        u32 seed = Server::getLevelSeed();
        std::memcpy(buf + 8, &seed, 4);
        Net::sendReliable(slot, buf, 12);

        if (s_onPlayerJoin) s_onPlayerJoin(slot);
    } break;

    case NetPacketType::CL_INPUT: {
        if (s_onInput) s_onInput(slot, data, size);
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
        s_localPlayerIndex = data[4];
        LOG_INFO("Net: joined as player %u", s_localPlayerIndex);
    } break;

    case NetPacketType::SV_JOIN_REJECT: {
        LOG_WARN("Net: join rejected by server");
    } break;

    case NetPacketType::SV_SNAPSHOT: {
        if (s_onSnapshot) s_onSnapshot(data, size);
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
                event.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(slot));
                LOG_INFO("Net: peer connecting to slot %u", slot);
            } else {
                // Client connected to server
                LOG_INFO("Net: connected to server");
                // Send join request
                u8 buf[sizeof(PacketHeader) + 4];
                PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
                hdr->type = NetPacketType::CL_JOIN_REQUEST;
                hdr->flags = 0;
                hdr->seq = s_seq++;
                u32 version = PROTOCOL_VERSION;
                std::memcpy(buf + sizeof(PacketHeader), &version, 4);
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
            }
        } break;

        case ENET_EVENT_TYPE_NONE:
            break;
        }
    }
}

void Net::sendReliable(u8 playerSlot, const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    if (playerSlot >= MAX_PLAYERS || !s_slots[playerSlot].peer) return;
    ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(static_cast<ENetPeer*>(s_slots[playerSlot].peer), 0, pkt);
}

void Net::sendUnreliable(u8 playerSlot, const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    if (playerSlot >= MAX_PLAYERS || !s_slots[playerSlot].peer) return;
    ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNSEQUENCED);
    enet_peer_send(static_cast<ENetPeer*>(s_slots[playerSlot].peer), 1, pkt);
}

void Net::broadcastReliable(const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    // Send only to active peers (not disconnected/empty slots)
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].state == SlotState::ACTIVE && s_slots[i].peer) {
            ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
            enet_peer_send(static_cast<ENetPeer*>(s_slots[i].peer), 0, pkt);
        }
    }
}

void Net::broadcastUnreliable(const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].state == SlotState::ACTIVE && s_slots[i].peer) {
            ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNSEQUENCED);
            enet_peer_send(static_cast<ENetPeer*>(s_slots[i].peer), 1, pkt);
        }
    }
}

void Net::sendToServer(const u8* data, u32 size, bool reliable) {
    if (s_role != NetRole::CLIENT || !s_serverPeer) return;
    u32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    u8 channel = reliable ? 0 : 1;
    ENetPacket* pkt = enet_packet_create(data, size, flags);
    enet_peer_send(s_serverPeer, channel, pkt);
}

NetRole Net::getRole()              { return s_role; }
u8      Net::getLocalPlayerIndex()  { return s_localPlayerIndex; }
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
void Net::setOnEvent(OnEventFn fn)         { s_onEvent = fn; }
void Net::setOnPlayerJoin(OnPlayerJoinFn fn) { s_onPlayerJoin = fn; }
void Net::setOnPlayerLeft(OnPlayerLeftFn fn) { s_onPlayerLeft = fn; }
