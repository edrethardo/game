#pragma once

#include "core/types.h"

static constexpr u32 MAX_PLAYERS       = 4;
static constexpr u16 DEFAULT_PORT      = 7777;
static constexpr u32 NET_TICK_RATE     = 60;
static constexpr u32 SNAPSHOT_RATE     = 20;
static constexpr u32 TICKS_PER_SNAP    = NET_TICK_RATE / SNAPSHOT_RATE; // 3

// Bumped to 2 with the absolute-aim + client-authoritative-position NetInput change:
// the wire layout grew from 14 bytes (header(4) + tick(4) + flags(2) + dx/dy(4)) to
// 20 bytes (header(4) + tick(4) + flags(2) + yawQ/pitchQ(4) + posXQ/Y/Z(6)). A v1
// host paired with a v2 client (or vice versa) would silently mis-parse; the version
// check in CL_JOIN_REQUEST handling now produces a clean SV_JOIN_REJECT instead.
static constexpr u32 PROTOCOL_VERSION  = 2;

// A peer that finishes the ENet handshake but never sends CL_JOIN_REQUEST is dropped
// after this many milliseconds so it can't hold a CONNECTING slot until ENet's own
// (much longer) timeout. A normal joiner sends JOIN_REQUEST immediately on connect,
// so this window only ever catches stalled/abandoned handshakes (N12).
static constexpr u32 CONNECTING_TIMEOUT_MS = 5000;

enum struct NetRole : u8 {
    NONE,       // offline / singleplayer
    SERVER,     // listen server (host plays + serves)
    CLIENT,     // remote client
};

enum struct NetPacketType : u8 {
    // Client -> Server
    CL_INPUT          = 0x01,
    CL_JOIN_REQUEST   = 0x02,

    // Server -> Client
    SV_JOIN_ACCEPT    = 0x10,
    SV_JOIN_REJECT    = 0x11,
    SV_SNAPSHOT       = 0x12,
    SV_EVENT          = 0x13,
    SV_PLAYER_LEFT    = 0x14,
    SV_LEVEL_SEED     = 0x15,  // mid-run floor descent: new floor + difficulty + run seed

    // Inventory packets
    CL_EQUIP_ITEM     = 0x03,  // client requests equip
    CL_DROP_ITEM      = 0x04,  // client drops item
    CL_PICKUP_ITEM    = 0x05,  // client picks up world item
    CL_RESPAWN        = 0x06,  // client requests respawn after death (reliable; no payload)
    SV_INVENTORY_SYNC = 0x16,  // server sends full inventory to client
};

// Sub-types for SV_EVENT packets
enum struct NetEventType : u8 {
    HITSCAN_IMPACT  = 0x01,  // remote player hitscan hit: position + hitEntity flag
    // Server-authoritative class-skill cooldown reset (Wanderer Exploit Weakness on
    // marked-enemy kill, etc.). Payload: skillId (u16 LE). The host can't write the
    // client's m_classSkillStates directly, so it ships this targeted reliable event
    // and the client zeros the matching slot's cooldownTimer on receipt.
    SKILL_CD_RESET  = 0x02,
};

// 4-byte packet header on every packet.
// Wire format is raw host-order bytes (memcpy of multi-byte fields), so it is
// little-endian and assumes PacketHeader has no padding — both are asserted below.
struct PacketHeader {
    NetPacketType type;
    u8            flags;
    // Reserved / currently unused — packet ordering is via the snapshot's serverTick
    // (SV_SNAPSHOT) and the input tick (CL_INPUT); the client writes a literal 0 here.
    // Kept on the wire to preserve the 4-byte header size (removing it is a format change).
    u16           seq;
};

// The wire format depends on a 4-byte, padding-free header laid out in little-endian
// host order. C++17 has no std::endian, so use the GCC/clang byte-order builtins
// (defined by gcc/clang/MinGW/devkitA64 — all of this project's compilers).
static_assert(sizeof(PacketHeader) == 4,
              "PacketHeader must be 4 bytes / no padding — wire layout depends on it");
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "netcode wire format is little-endian only");

struct NetStats {
    f32 rttMs;
    f32 packetLoss;
    u32 bytesSent;
    u32 bytesReceived;
};

enum struct SlotState : u8 {
    EMPTY,
    CONNECTING,
    ACTIVE,
    DISCONNECTED,
};

struct NetPlayerSlot {
    SlotState state       = SlotState::EMPTY;
    u8        playerIndex = 0xFF;
    void*     peer        = nullptr; // ENetPeer* (opaque)
    u32       lastInputTick = 0;
    f32       rttMs       = 0.0f;
    // enet_time_get() ms when the peer entered CONNECTING. Used to evict a peer that
    // completes the ENet handshake but never sends CL_JOIN_REQUEST (N12). 0 = not set.
    u32       connectTimeMs = 0;
};

namespace Net {
    bool init();
    void shutdown();

    // Host a listen server. Slot 0 is the local host player.
    bool hostServer(u16 port = DEFAULT_PORT);

    // Set the chosen PlayerClass (cast to u8) to advertise in CL_JOIN_REQUEST.
    // Call before connectToServer() so the join request carries the right class.
    void setLocalPlayerClass(u8 classId);

    // Connect to a remote server.
    bool connectToServer(const char* address, u16 port = DEFAULT_PORT);

    void disconnect();

    // Pump network events. Call once per frame before game update.
    void poll();

    // Send to a specific player slot (server only)
    void sendReliable(u8 playerSlot, const u8* data, u32 size);
    void sendUnreliable(u8 playerSlot, const u8* data, u32 size);

    // Broadcast to all connected clients (server only)
    void broadcastReliable(const u8* data, u32 size);
    void broadcastUnreliable(const u8* data, u32 size);
    // Broadcast a snapshot: unreliable but fragmentable. Unlike broadcastUnreliable
    // (ENET_PACKET_FLAG_UNSEQUENCED), payloads larger than the MTU are split into
    // MTU-sized fragments delivered unreliably (no retransmit latency) instead of
    // being silently downgraded to reliable. A lost fragment just drops that one
    // snapshot; the next arrives 50 ms later. Use for the 20 Hz world snapshot.
    void broadcastSnapshot(const u8* data, u32 size);

    // Send to server (client only)
    void sendToServer(const u8* data, u32 size, bool reliable);

    // Query
    NetRole getRole();
    u8      getLocalPlayerIndex();
    u32     getServerLevelSeed();       // per-run dungeon seed from SV_JOIN_ACCEPT (client)
    u8      getServerLevelFloor();      // host's current floor at join time
    u8      getServerLevelDifficulty(); // host's difficulty tier at join time
    bool    isConnected();
    // Client: true if the in-progress join failed (SV_JOIN_REJECT or pre-accept disconnect).
    // Cleared on the next connectToServer. Let the lobby bail out of CONNECTING (M10).
    bool    joinFailed();
    const NetPlayerSlot* getSlots(); // array of MAX_PLAYERS
    u32     getConnectedCount();
    NetStats getStats(u8 playerSlot);

    // Callback types — set before poll()
    using OnSnapshotFn   = void(*)(const u8* data, u32 size);
    using OnInputFn      = void(*)(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_PICKUP_ITEM handler — slot = requesting client, payload carries the uid.
    using OnPickupFn     = void(*)(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_RESPAWN handler — slot = requesting (dead) client; no payload.
    using OnRespawnFn    = void(*)(u8 playerSlot);
    using OnEventFn      = void(*)(const u8* data, u32 size);
    // classId is the joining client's chosen PlayerClass (validated by the callback;
    // 0xFF if the join request predates the class byte — treated as default Warrior).
    using OnPlayerJoinFn = void(*)(u8 playerSlot, u8 classId);
    using OnPlayerLeftFn = void(*)(u8 playerSlot);
    // Mid-run floor descent pushed by the server (SV_LEVEL_SEED). The client adopts
    // (floor, difficulty, seed) and regenerates the identical next dungeon. Distinct
    // from the join-time floor sync (SV_JOIN_ACCEPT), which is read via the getters.
    using OnLevelSeedFn  = void(*)(u8 floor, u8 difficulty, u32 seed);

    void setOnSnapshot(OnSnapshotFn fn);
    void setOnInput(OnInputFn fn);
    void setOnPickup(OnPickupFn fn);
    void setOnRespawn(OnRespawnFn fn);
    void setOnEvent(OnEventFn fn);
    void setOnPlayerJoin(OnPlayerJoinFn fn);
    void setOnPlayerLeft(OnPlayerLeftFn fn);
    void setOnLevelSeed(OnLevelSeedFn fn);

    // Server-only: broadcast a mid-run floor descent to all clients (SV_LEVEL_SEED).
    // Carries the NEW floor + difficulty + per-run seed so clients regenerate the
    // identical next dungeon. The run seed is unchanged across floors but is included
    // for robustness (a client that missed SV_JOIN_ACCEPT still self-corrects).
    void broadcastLevelSeed(u8 floor, u8 difficulty, u32 seed);
}
