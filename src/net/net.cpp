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

#ifdef USE_STEAM
#include "platform/steam.h"                    // Steam::isAvailable (gate callback registration)
#include "steam/steam_api.h"
#include "steam/isteamnetworkingmessages.h"    // ISteamNetworkingMessages — relay (SDR) P2P transport
#include "steam/isteamnetworkingutils.h"
#endif

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------
static NetRole          s_role = NetRole::NONE;
static ENetHost*        s_enetHost = nullptr;         // server or client host
// Client's handle to the server: an ENet ENetPeer* (as uintptr) or, on Steam, the host's SteamID. 0=none.
static u64              s_serverPeer = 0;

// Active byte-transport for this session. ENet (LAN / direct IP) is the default; STEAM is set for relay
// sessions started via hostServerSteam / connectToSteamHost, and reset to ENET in disconnect().
enum class Transport : u8 { ENET, STEAM };
static Transport        s_transport = Transport::ENET;

// The slot/serverPeer fields hold an opaque u64. For ENet sessions it's an ENetPeer* — these cast back.
static inline ENetPeer* asEnet(u64 h) { return reinterpret_cast<ENetPeer*>(static_cast<uintptr_t>(h)); }
static inline u64 enetHandle(ENetPeer* p) { return static_cast<u64>(reinterpret_cast<uintptr_t>(p)); }
static bool             s_joinFailed = false;     // client: set on SV_JOIN_REJECT or pre-accept disconnect
static NetPlayerSlot    s_slots[MAX_PLAYERS];
static u8               s_localPlayerIndex = 0;
static u8               s_localPlayerIndex2 = 0xFF; // couch client: 2nd local player's slot (0xFF=none)
static u8               s_localPlayerClass = 0; // chosen PlayerClass to send in CL_JOIN_REQUEST
static u8               s_localPlayerClass2 = 0xFF; // couch: 2nd local player's class (0xFF=none)
static u8               s_localJoinCount   = 1;     // couch: # local players this connection joins with (1 or 2)
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
static Net::OnMeteorFn     s_onMeteor     = nullptr;   // client-predicted proc meteor → authoritative
static Net::OnDropItemFn   s_onDropItem   = nullptr;   // R11
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
static Net::OnEnergyGainFn   s_onEnergyGain   = nullptr;  // client-side SV_ENERGY_GAIN
static Net::OnEventFn        s_onEvent        = nullptr;
static Net::OnPlayerJoinFn s_onPlayerJoin = nullptr;
static Net::OnPlayerLeftFn s_onPlayerLeft = nullptr;
static Net::OnLevelSeedFn  s_onLevelSeed  = nullptr;

// Channels: 0 = reliable ordered, 1 = unreliable sequenced
static constexpr u32 NUM_CHANNELS = 2;
static_assert(NET_METRICS_CHANNELS == NUM_CHANNELS,
              "NET_METRICS_CHANNELS (net_metrics.h) must match NUM_CHANNELS");

// Net-metrics accumulators (F9 net-graph / 1 Hz [NET-GRAPH] log). Touched only on the net
// thread — Net::poll() inbound and the sendImmediate_* outbound path, both main-thread — so
// plain u32, no atomics. s_counters accumulates the live 1 s window; tickMetricsWindow folds
// it into s_lastMetrics[] and zeroes it. See net_metrics.h for the math + rationale.
static NetCounters s_counters;
static NetMetrics  s_lastMetrics[MAX_PLAYERS];

// Count one outbound datagram's payload. Called at the sendImmediate_* layer (after the
// fake-loss drop guard at the public layer) so each real datagram is counted once, with
// broadcasts naturally multiplied per peer. ch: 0 = reliable, 1 = unreliable.
static inline void countOut(u8 slot, u8 ch, u32 size) {
    if (slot >= MAX_PLAYERS || ch >= NET_METRICS_CHANNELS) return;
    s_counters.bytesOut[slot][ch] += size;
    s_counters.pktsOut[slot][ch]++;
}

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

#ifdef USE_STEAM
// --- Steam relay transport (ISteamNetworkingMessages / SDR) --------------------------------------
static SteamNetworkingIdentity steamIdentOf(u64 sid) {
    SteamNetworkingIdentity id; id.SetSteamID64(sid); return id;
}
static u8 findSlotBySteamId(u64 sid) {
    for (u8 i = 0; i < MAX_PLAYERS; i++) if (s_slots[i].peer == sid) return i;
    return 0xFF;
}
// Set by the session-failed callback, drained in pollSteam. One pending id suffices for <=4 players.
static u64 s_steamFailedPeer = 0;

// Persistent object holding the Steam Networking Messages callbacks (STEAM_CALLBACK auto-registers
// via `this`). Constructed in Net::init when Steam is available.
struct SteamNetCallbacks {
    STEAM_CALLBACK(SteamNetCallbacks, onSessionRequest, SteamNetworkingMessagesSessionRequest_t);
    STEAM_CALLBACK(SteamNetCallbacks, onSessionFailed,  SteamNetworkingMessagesSessionFailed_t);
};
inline void SteamNetCallbacks::onSessionRequest(SteamNetworkingMessagesSessionRequest_t* e) {
    // Accept every incoming P2P session; game-level validation happens at CL_JOIN_REQUEST (protocol
    // version + server-full). Only the listen-server ever receives these.
    if (SteamNetworkingMessages())
        SteamNetworkingMessages()->AcceptSessionWithUser(e->m_identityRemote);
}
inline void SteamNetCallbacks::onSessionFailed(SteamNetworkingMessagesSessionFailed_t* e) {
    s_steamFailedPeer = e->m_info.m_identityRemote.GetSteamID64();
}
static SteamNetCallbacks* s_steamNetCb = nullptr;

static void steamSend(u64 sid, u8 channel, const u8* data, u32 size, bool reliable) {
    if (!sid || !SteamNetworkingMessages()) return;
    // Unreliable game traffic (snapshots/inputs at 60 Hz) wants NoNagle so it isn't batched/delayed.
    // Unreliable still fragments+reassembles large payloads (whole-message-drop on loss) like ENet's
    // UNRELIABLE_FRAGMENT, so 8 KB snapshots are fine.
    int flags = reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoNagle;
    SteamNetworkingMessages()->SendMessageToUser(steamIdentOf(sid), data, size, flags, channel);
}
#endif  // USE_STEAM

static u8 findSlotByPeer(ENetPeer* peer) {
    u64 h = enetHandle(peer);
    for (u8 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].peer == h)
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
        // Couch co-op (v6): optional localCount(1) + class2(1) — two local players on one connection.
        u8 localCount = 1, chosenClass2 = 0xFF;
        if (size >= sizeof(PacketHeader) + 7) {
            localCount   = data[sizeof(PacketHeader) + 5];
            chosenClass2 = data[sizeof(PacketHeader) + 6];
        }
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

        // Couch joiner: allocate a 2nd slot sharing this peer (best-effort — if the session is full,
        // drop the partner and join as single). CL_INPUT/CL_FIRE route to it by absolute target slot
        // with a same-peer ownership check, so the two slots needn't be consecutive.
        u8 slot2 = 0xFF;
        if (localCount >= 2) {
            slot2 = findFreeSlot();
            if (slot2 != 0xFF) {
                s_slots[slot2].state        = SlotState::ACTIVE;
                s_slots[slot2].playerIndex  = slot2;
                s_slots[slot2].peer         = s_slots[slot].peer;
                s_slots[slot2].connectTimeMs = 0;
            } else {
                LOG_WARN("Net: couch joiner %u — no free slot for partner, joining as single", slot);
            }
        }
        LOG_INFO("Net: player %u connected (class=%u, localCount=%u, partnerSlot=%u)",
                 slot, chosenClass, localCount, slot2);

        // Send accept packet (13 B = legacy 12 + slot2)
        u8 buf[13];
        PacketHeader* acc = reinterpret_cast<PacketHeader*>(buf);
        acc->type = NetPacketType::SV_JOIN_ACCEPT;
        acc->flags = 0;
        acc->seq = s_seq++;
        buf[4] = slot;                         // playerIndex
        buf[5] = Net::getConnectedCount();     // playerCount (both couch slots are ACTIVE now)
        buf[6] = Server::getLevelFloor();      // current floor (client generates this floor)
        buf[7] = Server::getLevelDifficulty(); // difficulty tier (folds into the dungeon seed)
        // Send the per-run dungeon seed so the client generates the identical dungeon.
        u32 seed = Server::getLevelSeed();
        std::memcpy(buf + 8, &seed, 4);
        buf[12] = slot2;                       // 2nd local slot (0xFF if single)
        Net::sendReliable(slot, buf, 13);

        if (s_onPlayerJoin) {
            s_onPlayerJoin(slot, chosenClass);
            if (slot2 != 0xFF) s_onPlayerJoin(slot2, chosenClass2);
        }
    } break;

    case NetPacketType::CL_INPUT: {
        // Couch co-op: the input-window header's reserved byte 1 carries the ABSOLUTE target net
        // slot (which of this peer's local players this input is for). Route there iff the SAME peer
        // owns it (a client can't write another peer's slot). Single clients stamp their own slot.
        u8 targetSlot = slot;
        if (size >= sizeof(PacketHeader) + 2) {
            u8 ts = data[sizeof(PacketHeader) + 1];
            if (ts < MAX_PLAYERS && s_slots[ts].peer != 0 && s_slots[ts].peer == s_slots[slot].peer)
                targetSlot = ts;
        }
        if (s_onInput) s_onInput(targetSlot, data, size);
    } break;

    case NetPacketType::CL_PICKUP_ITEM: {
        // Client requests pickup of a world item by uid. The engine handler validates
        // proximity/ownership server-side and removes the item (propagates via snapshot).
        if (s_onPickup) s_onPickup(slot, data, size);
    } break;

    case NetPacketType::CL_METEOR: {
        // Client PREDICTED a weapon on-hit proc meteor (it owns the roll — see CL_METEOR) and is
        // telling us where it landed. The engine handler validates + spawns the single authoritative
        // (damaging) meteor and relays it to the OTHER clients.
        if (s_onMeteor) s_onMeteor(slot, data, size);
    } break;

    case NetPacketType::CL_DROP_ITEM: {
        // R11: client drops an item from inventory. Engine handler removes the slot
        // server-side and spawns the world item at the client-reported drop position.
        // Without this the client's predicted drop is silently undone the next time
        // mirrorWorldItems / inventory state syncs from the server.
        if (s_onDropItem) s_onDropItem(slot, data, size);
    } break;

    case NetPacketType::CL_RESPAWN: {
        // Dead client requests respawn. The engine handler respawns that slot's NetPlayer
        // (idempotent) and the revival propagates back via the next snapshot. Sent reliably
        // so it can't be lost like the old INPUT_EX_RESPAWN-through-the-input-buffer hack.
        // Online couch co-op: an optional trailing byte names WHICH of this peer's local players
        // is respawning (a couch client owns two slots; a header-only packet = the peer's primary).
        // Route iff the same peer owns the target slot, so P2's respawn revives P2, not P1.
        u8 respawnSlot = slot;
        if (size >= sizeof(PacketHeader) + 1) {
            u8 ts = data[sizeof(PacketHeader)];
            if (ts < MAX_PLAYERS && s_slots[ts].peer != 0 && s_slots[ts].peer == s_slots[slot].peer)
                respawnSlot = ts;
        }
        if (s_onRespawn) s_onRespawn(respawnSlot);
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
        // Couch co-op: optional trailing byte = absolute target net slot (which local player fired).
        // Route iff the same peer owns it; single clients omit it (or stamp their own slot).
        u8 fireSlot = slot;
        if (size >= sizeof(PacketHeader) + 15) {
            u8 ts = data[sizeof(PacketHeader) + 14];
            if (ts < MAX_PLAYERS && s_slots[ts].peer != 0 && s_slots[ts].peer == s_slots[slot].peer)
                fireSlot = ts;
        }
        if (s_onFireWeapon) s_onFireWeapon(fireSlot, data, size);
    } break;

    case NetPacketType::CL_INVENTORY_SYNC: {
        // Client is pushing one local player's inventory. Engine deserializes and replaces the
        // auto-granted starting kit. Size validation is delegated to the handler. Online couch
        // co-op: the LAST byte is the target net slot (which of this peer's local players) — read it
        // here (the body offset is engine-side, but the slot is always the trailing byte) and route
        // iff the same peer owns it; otherwise fall back to the peer's primary slot.
        u8 invSlot = slot;
        if (size >= 1) {
            u8 ts = data[size - 1];
            if (ts < MAX_PLAYERS && s_slots[ts].peer != 0 && s_slots[ts].peer == s_slots[slot].peer)
                invSlot = ts;
        }
        if (s_onInventorySync) s_onInventorySync(invSlot, data, size);
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
        // Couch co-op (v6): 13th byte = 2nd local player's slot (0xFF if we joined single, or an
        // older 12-byte accept). Validate the same way as the primary before storing.
        s_localPlayerIndex2 = 0xFF;
        if (size >= 13) {
            u8 s2 = data[12];
            if (s2 != 0xFF && s2 >= 1 && s2 < MAX_PLAYERS && s2 != assignedSlot) s_localPlayerIndex2 = s2;
        }
        LOG_INFO("Net: joined as player %u (+partner %u, floor=%u seed=%u)",
                 s_localPlayerIndex, s_localPlayerIndex2, s_serverFloor, s_serverLevelSeed);
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

    case NetPacketType::SV_ENERGY_GAIN: {
        // Server granted the local player energy it computed authoritatively (projectile
        // manasteal / mana-on-kill). Payload: 4B header + f32 amount.
        if (size < sizeof(PacketHeader) + 4) break;
        f32 amount;
        std::memcpy(&amount, data + sizeof(PacketHeader), 4);
        if (s_onEnergyGain) s_onEnergyGain(amount);
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
    s_serverPeer = 0;
    s_localPlayerIndex = 0;
    s_seq = 0;
#ifdef USE_STEAM
    // Register the Steam Networking Messages callbacks (session request/failed) once, so incoming relay
    // sessions are accepted even before a session starts. Requires Steam::init() (ran before Net::init).
    if (Steam::isAvailable() && !s_steamNetCb) s_steamNetCb = new SteamNetCallbacks();
#endif
    LOG_INFO("Net: initialized");
    return true;
}

void Net::shutdown() {
    disconnect();
#ifdef USE_STEAM
    if (s_steamNetCb) { delete s_steamNetCb; s_steamNetCb = nullptr; }
#endif
    if (s_initialized) {
        enet_deinitialize();
        s_initialized = false;
    }
    LOG_INFO("Net: shut down");
}

bool Net::hostServer(u16 port, bool useUpnp, u8 localPlayerCount) {
    if (!s_initialized) return false;
    if (s_enetHost) return false;

    // Host-owned local slots: slot 0 (host) plus, for online couch co-op, slot 1 (the host's
    // split-screen partner). Clamp to [1, MAX_PLAYERS-1] so at least one remote peer can still join.
    u8 localCount = localPlayerCount < 1 ? 1
                  : (localPlayerCount > MAX_PLAYERS - 1 ? static_cast<u8>(MAX_PLAYERS - 1) : localPlayerCount);

    ENetAddress address;
    // R12: zero the address struct so the new family + host6 fields default to
    // AF_INET/0. ENET_HOST_ANY=0 plus family=AF_INET tells the patched ENet to
    // bind to in6addr_any (dual-stack wildcard) — same socket carries both v4
    // and v6 peers via IPv4-mapped IPv6.
    std::memset(&address, 0, sizeof(address));
    address.host = ENET_HOST_ANY;
    address.port = port;

    // Remaining slots (MAX_PLAYERS - localCount) are the max concurrent remote peers.
    s_enetHost = enet_host_create(&address, MAX_PLAYERS - localCount, NUM_CHANNELS, 0, 0);
    if (!s_enetHost) {
        LOG_ERROR("Net: failed to create server on port %u", port);
        return false;
    }

    s_role = NetRole::SERVER;
    s_localPlayerIndex = 0;
    resetSlots();
    // Reserve the host's local slots (ACTIVE, no peer — never networked) so findFreeSlot() hands
    // remotes slots >= localCount and a couch partner at slot 1 isn't overwritten by a joiner.
    for (u8 i = 0; i < localCount; i++) {
        s_slots[i].state = SlotState::ACTIVE;
        s_slots[i].playerIndex = i;
        s_slots[i].peer = 0;
    }

    LOG_INFO("Net: hosting on port %u", port);

    // Best-effort UPnP IGD port mapping so friends on other networks can join
    // without the host manually configuring their router. Blocking for up to
    // ~1 s (SSDP discovery); fine on a one-time menu action. Failure is non-
    // fatal — the host stays LAN-reachable either way, and the lobby UI reads
    // Upnp::lastError() to tell the user. On Switch the wrapper is a stub
    // that returns false instantly with "UPnP unsupported on this platform".
    //
    // useUpnp=false (LAN-only host) skips the SSDP discovery entirely — saves
    // the ~1 s blocking wait at host time and avoids creating any router-level
    // mapping that would outlive the session.
    if (useUpnp) {
        char extIp[64] = {0};
        char errMsg[128] = {0};
        if (Upnp::tryAddPortMapping(port, /*discoveryTimeoutMs=*/1000, extIp, errMsg)) {
            LOG_INFO("Net: UPnP mapped UDP %u — external IP %s", port, extIp);
        } else {
            LOG_INFO("Net: UPnP not available (%s) — host is LAN-only unless port-forwarded", errMsg);
        }
    } else {
        LOG_INFO("Net: hosting LAN-only on port %u (UPnP skipped by user choice)", port);
    }
    return true;
}

const char* Net::getExternalIp() { return Upnp::currentExternalIp(); }
const char* Net::getUpnpError()  { return Upnp::lastError(); }

bool Net::hostServerSteam(u8 localPlayerCount) {
#ifdef USE_STEAM
    if (!s_initialized || s_enetHost) return false;
    if (!Steam::isAvailable()) { LOG_WARN("Net: hostServerSteam — Steam unavailable"); return false; }
    u8 localCount = localPlayerCount < 1 ? 1
                  : (localPlayerCount > MAX_PLAYERS - 1 ? static_cast<u8>(MAX_PLAYERS - 1) : localPlayerCount);
    s_transport = Transport::STEAM;   // relay transport (no ENet host, no UPnP)
    s_role = NetRole::SERVER;
    s_localPlayerIndex = 0;
    resetSlots();
    for (u8 i = 0; i < localCount; i++) {   // reserve host-local slots exactly as hostServer does
        s_slots[i].state = SlotState::ACTIVE;
        s_slots[i].playerIndex = i;
        s_slots[i].peer = 0;
    }
    // The relay "listens" implicitly (InitRelayNetworkAccess ran in Steam::init). Joiners surface via
    // the auto-accepted SteamNetworkingMessagesSessionRequest_t callback, then their CL_JOIN_REQUEST.
    LOG_INFO("Net: hosting via Steam relay (SteamID %llu, %u local slot(s))",
             (unsigned long long)Steam::localSteamId(), localCount);
    return true;
#else
    (void)localPlayerCount; return false;
#endif
}

bool Net::connectToSteamHost(u64 hostSteamId) {
#ifdef USE_STEAM
    if (!s_initialized || s_enetHost) return false;
    if (!Steam::isAvailable() || hostSteamId == 0) { LOG_WARN("Net: connectToSteamHost — Steam off / bad id"); return false; }
    s_transport = Transport::STEAM;
    s_role = NetRole::CLIENT;
    s_joinFailed = false;
    resetSlots();
    s_serverPeer = hostSteamId;
    // Steam relay needs no handshake — the first SendMessageToUser opens the session. Send CL_JOIN_REQUEST
    // immediately, mirroring the ENet CONNECT branch (header + version + class1 + localCount + class2 = 11 B).
    u8 buf[sizeof(PacketHeader) + 7];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type = NetPacketType::CL_JOIN_REQUEST; hdr->flags = 0; hdr->seq = s_seq++;
    u32 version = PROTOCOL_VERSION;
    std::memcpy(buf + sizeof(PacketHeader), &version, 4);
    buf[sizeof(PacketHeader) + 4] = s_localPlayerClass;
    buf[sizeof(PacketHeader) + 5] = s_localJoinCount;
    buf[sizeof(PacketHeader) + 6] = s_localPlayerClass2;
    sendToServer(buf, sizeof(buf), true);
    LOG_INFO("Net: connecting via Steam relay to host %llu...", (unsigned long long)hostSteamId);
    return true;
#else
    (void)hostSteamId; return false;
#endif
}

void Net::setLocalPlayerClass(u8 classId) {
    s_localPlayerClass = classId;
    s_localPlayerClass2 = 0xFF;
    s_localJoinCount    = 1;
}

void Net::setLocalPlayerClasses(u8 class0, u8 class1, u8 localCount) {
    s_localPlayerClass  = class0;
    s_localPlayerClass2 = (localCount >= 2) ? class1 : 0xFF;
    s_localJoinCount    = (localCount >= 2) ? 2 : 1;
}

bool Net::connectToServer(const char* address, u16 port) {
    if (!s_initialized) return false;
    if (s_enetHost) return false;

    s_enetHost = enet_host_create(nullptr, 1, NUM_CHANNELS, 0, 0);
    if (!s_enetHost) {
        LOG_ERROR("Net: failed to create client host");
        return false;
    }

    // R12: strip URL-style brackets around IPv6 literals before handing the
    // address to enet_address_set_host. The lobby ships port separately so
    // we don't need to parse `:port` out — just the bare host inside `[]`.
    // Examples accepted: "192.168.1.1" / "[::1]" / "[2001:db8::1]" / "host.example".
    const char* host = address;
    char unbracketed[64];
    if (address && address[0] == '[') {
        const char* end = std::strchr(address, ']');
        if (end) {
            size_t len = static_cast<size_t>(end - (address + 1));
            if (len < sizeof(unbracketed)) {
                std::memcpy(unbracketed, address + 1, len);
                unbracketed[len] = '\0';
                host = unbracketed;
            }
        }
    }

    ENetAddress addr;
    std::memset(&addr, 0, sizeof(addr));   // R12: zero family + host6 before set_host populates them
    enet_address_set_host(&addr, host);
    addr.port = port;

    ENetPeer* sp = enet_host_connect(s_enetHost, &addr, NUM_CHANNELS, 0);
    if (!sp) {
        LOG_ERROR("Net: failed to initiate connection to %s:%u", address, port);
        enet_host_destroy(s_enetHost);
        s_enetHost = nullptr;
        return false;
    }
    s_serverPeer = enetHandle(sp);
    s_transport  = Transport::ENET;   // direct-IP join uses ENet

    s_role = NetRole::CLIENT;
    s_joinFailed = false; // fresh attempt
    resetSlots();
    LOG_INFO("Net: connecting to %s:%u...", address, port);
    return true;
}

void Net::disconnect() {
    if (s_role == NetRole::CLIENT && s_serverPeer && s_transport == Transport::ENET) {
        enet_peer_disconnect(asEnet(s_serverPeer), 0);
        // Flush disconnect
        ENetEvent event;
        while (enet_host_service(s_enetHost, &event, 200) > 0) {
            if (event.type == ENET_EVENT_TYPE_DISCONNECT) break;
            if (event.type == ENET_EVENT_TYPE_RECEIVE)
                enet_packet_destroy(event.packet);
        }
        s_serverPeer = 0;
    }

#ifdef USE_STEAM
    // Steam relay: close every open P2P session (host peers or the client's host) so the relay tears
    // them down promptly instead of relying on inactivity timeout.
    if (s_transport == Transport::STEAM && SteamNetworkingMessages()) {
        for (u8 i = 0; i < MAX_PLAYERS; i++)
            if (s_slots[i].peer) SteamNetworkingMessages()->CloseSessionWithUser(steamIdentOf(s_slots[i].peer));
        if (s_serverPeer) SteamNetworkingMessages()->CloseSessionWithUser(steamIdentOf(s_serverPeer));
        s_serverPeer = 0;
    }
#endif

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
    s_transport = Transport::ENET;   // next session defaults back to ENet until a Steam entry point sets it
    s_localPlayerIndex = 0;
    // Hygiene (CV-3): clear server-level statics so a stale value can't be read by a future
    // lobby probe between connect attempts. They're refreshed by the next SV_JOIN_ACCEPT.
    s_serverLevelSeed  = 0;
    s_serverFloor      = 1;
    s_serverDifficulty = 0;
}

// Server-side: fully release a slot a peer owned — mark DISCONNECTED, tell everyone (SV_PLAYER_LEFT +
// onPlayerLeft), then free it for reuse. Shared by the ENet disconnect event and the Steam
// session-failed path so both transports behave identically. An online-couch client owns two slots;
// the caller invokes this once per owned slot.
static void serverDropSlot(u8 slot) {
    LOG_INFO("Net: player %u disconnected", slot);
    s_slots[slot].state = SlotState::DISCONNECTED;
    s_slots[slot].peer  = 0;
    u8 buf[sizeof(PacketHeader) + 1];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type = NetPacketType::SV_PLAYER_LEFT; hdr->flags = 0; hdr->seq = s_seq++;
    buf[sizeof(PacketHeader)] = slot;
    Net::broadcastReliable(buf, sizeof(buf));
    if (s_onPlayerLeft) s_onPlayerLeft(slot);
    s_slots[slot].state = SlotState::EMPTY;
    s_slots[slot].playerIndex = 0xFF;
}

// N12: evict peers stuck in CONNECTING (handshake done, no CL_JOIN_REQUEST) so an abandoned connection
// can't hold a slot until the transport's own long timeout. Transport-agnostic (ENet clock is always
// available since enet_initialize ran in Net::init).
static void sweepConnecting() {
    if (s_role != NetRole::SERVER) return;
    u32 nowMs = enet_time_get();
    for (u32 i = 1; i < MAX_PLAYERS; i++) {  // slot 0 = host, never CONNECTING
        if (s_slots[i].state != SlotState::CONNECTING) continue;
        if (nowMs - s_slots[i].connectTimeMs < CONNECTING_TIMEOUT_MS) continue;
        LOG_WARN("Net: dropping slot %u — no join request within %u ms", i, CONNECTING_TIMEOUT_MS);
        if (s_slots[i].peer) {
#ifdef USE_STEAM
            if (s_transport == Transport::STEAM) {
                if (SteamNetworkingMessages())
                    SteamNetworkingMessages()->CloseSessionWithUser(steamIdentOf(s_slots[i].peer));
            } else
#endif
            enet_peer_disconnect_now(asEnet(s_slots[i].peer), 0);
        }
        s_slots[i] = NetPlayerSlot{};
    }
}

#ifdef USE_STEAM
// Steam relay poll: drain queued session failures + inbound messages on both channels, mapping
// SteamID -> slot and dispatching through the SAME packet handlers the ENet path uses.
static void pollSteam() {
    ISteamNetworkingMessages* msgs = SteamNetworkingMessages();
    if (!msgs) return;

    if (s_steamFailedPeer) {
        u64 failed = s_steamFailedPeer; s_steamFailedPeer = 0;
        if (s_role == NetRole::SERVER) {
            for (u8 slot = 0; slot < MAX_PLAYERS; slot++)
                if (s_slots[slot].peer == failed) serverDropSlot(slot);
        } else if (s_serverPeer == failed) {
            LOG_INFO("Net: Steam session to host failed");
            s_serverPeer = 0; s_joinFailed = true;
        }
    }

    SteamNetworkingMessage_t* inbox[32];
    for (u32 ch = 0; ch < NUM_CHANNELS; ch++) {
        int n = msgs->ReceiveMessagesOnChannel(static_cast<int>(ch), inbox, 32);
        for (int i = 0; i < n; i++) {
            SteamNetworkingMessage_t* m = inbox[i];
            u64 sid = m->m_identityPeer.GetSteamID64();
            const u8* data = static_cast<const u8*>(m->m_pData);
            u32 size = static_cast<u32>(m->m_cbSize);

            u8 inCh = (ch < NET_METRICS_CHANNELS) ? static_cast<u8>(ch) : 1;
            s_counters.bytesIn[inCh] += size; s_counters.pktsIn[inCh]++;
            if (size >= 1 && data[0] == static_cast<u8>(NetPacketType::SV_SNAPSHOT)) s_counters.snapsIn++;

            if (s_role == NetRole::SERVER) {
                u8 slot = findSlotBySteamId(sid);
                if (slot == 0xFF) {
                    // First contact from a new SteamID — mirror the ENet CONNECT seat.
                    slot = findFreeSlot();
                    if (slot == 0xFF) { m->Release(); continue; } // server full: ignore; client times out
                    s_slots[slot].state = SlotState::CONNECTING;
                    s_slots[slot].playerIndex = slot;
                    s_slots[slot].peer = sid;
                    s_slots[slot].connectTimeMs = enet_time_get();
                    LOG_INFO("Net: Steam peer connecting to slot %u", slot);
                }
                serverHandlePacket(slot, data, size);
            } else {
                clientHandlePacket(data, size);
            }
            m->Release();
        }
    }
    sweepConnecting();
}
#endif  // USE_STEAM

void Net::poll() {
#ifdef USE_STEAM
    if (s_transport == Transport::STEAM) { pollSteam(); return; }
#endif
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
                s_slots[slot].peer = enetHandle(event.peer);
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
                // v6: header(4) + version(4) + class1(1) + localCount(1) + class2(1) = 11 B.
                // For a single joiner localCount=1 and class2=0xFF (the server ignores the partner).
                u8 buf[sizeof(PacketHeader) + 7];
                PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
                hdr->type = NetPacketType::CL_JOIN_REQUEST;
                hdr->flags = 0;
                hdr->seq = s_seq++;
                u32 version = PROTOCOL_VERSION;
                std::memcpy(buf + sizeof(PacketHeader), &version, 4);
                buf[sizeof(PacketHeader) + 4] = s_localPlayerClass;  // P1 chosen PlayerClass
                buf[sizeof(PacketHeader) + 5] = s_localJoinCount;     // 1 or 2 (online couch co-op)
                buf[sizeof(PacketHeader) + 6] = s_localPlayerClass2;  // P2 class (0xFF if single)
                sendToServer(buf, sizeof(buf), true);
            }
        } break;

        case ENET_EVENT_TYPE_RECEIVE: {
            const u8* data = event.packet->data;
            u32 size = static_cast<u32>(event.packet->dataLength);

            // Net-metrics: count inbound payload by channel (ch0=reliable, ch1=unreliable).
            // Single hook covers client RX (snapshots/events) and server RX (input). snapsIn
            // is meaningful on the client; the server simply never receives SV_SNAPSHOT.
            u8 inCh = (event.channelID < NET_METRICS_CHANNELS) ? static_cast<u8>(event.channelID) : 1;
            s_counters.bytesIn[inCh] += size;
            s_counters.pktsIn[inCh]++;
            if (size >= 1 && data[0] == static_cast<u8>(NetPacketType::SV_SNAPSHOT))
                s_counters.snapsIn++;

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
                // Free EVERY slot this peer owns — an online-couch client owns two. Each gets its
                // own SV_PLAYER_LEFT + onPlayerLeft so all clients clear both. (peer is nulled per
                // slot AFTER its match test, so the second couch slot still matches on a later pass.)
                for (u8 slot = 0; slot < MAX_PLAYERS; slot++) {
                    if (s_slots[slot].peer != enetHandle(event.peer)) continue;
                    serverDropSlot(slot);
                }
            } else {
                LOG_INFO("Net: disconnected from server");
                s_serverPeer = 0;
                s_joinFailed = true; // covers server-full reject (peer dropped) + any pre-accept drop (M10)
            }
        } break;

        case ENET_EVENT_TYPE_NONE:
            break;
        }
    }

    // N12: evict peers stuck in CONNECTING (see sweepConnecting — transport-agnostic).
    sweepConnecting();
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
    countOut(playerSlot, 0, size); // ch0 = reliable
#ifdef USE_STEAM
    if (s_transport == Transport::STEAM) { steamSend(s_slots[playerSlot].peer, 0, data, size, true); return; }
#endif
    ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(asEnet(s_slots[playerSlot].peer), 0, pkt) < 0)
        enet_packet_destroy(pkt);
}

static void sendImmediate_Unreliable(u8 playerSlot, const u8* data, u32 size) {
    if (playerSlot >= MAX_PLAYERS || !s_slots[playerSlot].peer) return;
    countOut(playerSlot, 1, size); // ch1 = unreliable
#ifdef USE_STEAM
    if (s_transport == Transport::STEAM) { steamSend(s_slots[playerSlot].peer, 1, data, size, false); return; }
#endif
    ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNSEQUENCED);
    if (enet_peer_send(asEnet(s_slots[playerSlot].peer), 1, pkt) < 0)
        enet_packet_destroy(pkt);
}

static void sendImmediate_BroadcastReliable(const u8* data, u32 size) {
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].state == SlotState::ACTIVE && s_slots[i].peer) {
            countOut(static_cast<u8>(i), 0, size); // per-peer fan-out
#ifdef USE_STEAM
            if (s_transport == Transport::STEAM) { steamSend(s_slots[i].peer, 0, data, size, true); continue; }
#endif
            ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
            if (enet_peer_send(asEnet(s_slots[i].peer), 0, pkt) < 0)
                enet_packet_destroy(pkt);
        }
    }
}

static void sendImmediate_BroadcastUnreliable(const u8* data, u32 size) {
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (s_slots[i].state == SlotState::ACTIVE && s_slots[i].peer) {
            countOut(static_cast<u8>(i), 1, size); // per-peer fan-out
#ifdef USE_STEAM
            if (s_transport == Transport::STEAM) { steamSend(s_slots[i].peer, 1, data, size, false); continue; }
#endif
            ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNSEQUENCED);
            if (enet_peer_send(asEnet(s_slots[i].peer), 1, pkt) < 0)
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
            countOut(static_cast<u8>(i), 1, size);   // ch1, per-peer fan-out
            s_counters.snapsOut[i]++;                // one snapshot send to this slot
#ifdef USE_STEAM
            // Steam messages fragment/reassemble large payloads natively — a plain unreliable send.
            if (s_transport == Transport::STEAM) { steamSend(s_slots[i].peer, 1, data, size, false); continue; }
#endif
            ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
            if (enet_peer_send(asEnet(s_slots[i].peer), 1, pkt) < 0)
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
    countOut(0, channel, size); // client has a single server peer — attribute to slot 0
#ifdef USE_STEAM
    if (s_transport == Transport::STEAM) { steamSend(s_serverPeer, channel, data, size, reliable); return; }
#endif
    ENetPacket* pkt = enet_packet_create(data, size, flags);
    if (enet_peer_send(asEnet(s_serverPeer), channel, pkt) < 0)
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
    // NO fake-loss here. Dropping a reliable packet BEFORE ENet simulates a network that cannot
    // exist: real loss on the reliable channel means retransmit-and-delay, never permanent loss.
    // The pre-ENet drop permanently vanished CL_FIRE_WEAPON / pickup / damage events, so the
    // adversity harness was testing failure modes the game can never actually experience while
    // NOT testing the retransmit latency it can. Loss on unreliable sends + the latency knob
    // together cover the realistic envelope.
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

// Reliable broadcast to every ACTIVE client EXCEPT one slot — used to relay a client-originated
// predicted proc meteor onward to the other players without echoing it back to the caster (which
// already spawned its own prediction; an echo would double-spawn it). Sends immediately: unlike
// broadcastReliable this bypasses the D5 fake-latency queue, whose DelayTarget enum has no
// per-slot-exclusion variant. That queue is a debug-only harness, so the omission is acceptable.
void Net::broadcastReliableExcept(u8 exceptSlot, const u8* data, u32 size) {
    if (s_role != NetRole::SERVER) return;
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (i == exceptSlot) continue;
        if (s_slots[i].state != SlotState::ACTIVE || !s_slots[i].peer) continue;
        countOut(static_cast<u8>(i), 0, size); // per-peer fan-out (mirrors sendImmediate_BroadcastReliable)
#ifdef USE_STEAM
        if (s_transport == Transport::STEAM) { steamSend(s_slots[i].peer, 0, data, size, true); continue; }
#endif
        ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
        if (enet_peer_send(asEnet(s_slots[i].peer), 0, pkt) < 0)
            enet_packet_destroy(pkt);
    }
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
    countOut(playerSlot, 1, size);            // ch1, per-slot snapshot send
    s_counters.snapsOut[playerSlot]++;
#ifdef USE_STEAM
    if (s_transport == Transport::STEAM) { steamSend(s_slots[playerSlot].peer, 1, data, size, false); return; }
#endif
    ENetPacket* pkt = enet_packet_create(data, size, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
    if (enet_peer_send(asEnet(s_slots[playerSlot].peer), 1, pkt) < 0)
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
    // M14: fake-loss injection — UNRELIABLE sends only (see sendReliable for why dropping a
    // reliable packet pre-ENet simulates an impossible network).
    if (!reliable && shouldDropPacket()) return;
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
u8      Net::getLocalPlayerIndex2() { return s_localPlayerIndex2; }
bool    Net::joinFailed()           { return s_joinFailed; }
u32     Net::getServerLevelSeed()       { return s_serverLevelSeed; }
u8      Net::getServerLevelFloor()      { return s_serverFloor; }
u8      Net::getServerLevelDifficulty() { return s_serverDifficulty; }
bool    Net::isConnected()          {
#ifdef USE_STEAM
    // Steam sessions never create an ENet host, so s_enetHost stays null — check the relay state
    // instead, or the client-loop's host-loss detector bounces every Steam session back to the menu.
    if (s_transport == Transport::STEAM)
        return s_role == NetRole::SERVER || (s_role == NetRole::CLIENT && s_serverPeer != 0);
#endif
    return s_enetHost != nullptr;
}

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
#ifdef USE_STEAM
    if (s_transport == Transport::STEAM) {
        // NEVER cast the SteamID handle to an ENetPeer* — read ping/quality from the relay session.
        u64 peer = (s_role == NetRole::SERVER && playerSlot < MAX_PLAYERS) ? s_slots[playerSlot].peer
                 : (s_role == NetRole::CLIENT ? s_serverPeer : 0);
        if (peer && SteamNetworkingMessages()) {
            SteamNetConnectionRealTimeStatus_t rt;
            std::memset(&rt, 0, sizeof(rt));
            if (SteamNetworkingMessages()->GetSessionConnectionInfo(steamIdentOf(peer), nullptr, &rt)
                    != k_ESteamNetworkingConnectionState_None) {
                stats.rttMs = static_cast<f32>(rt.m_nPing);
                if (rt.m_flConnectionQualityLocal >= 0.0f && rt.m_flConnectionQualityLocal <= 1.0f)
                    stats.packetLoss = 1.0f - rt.m_flConnectionQualityLocal;   // quality 1.0 = no loss
            }
        }
        return stats;
    }
#endif
    if (s_role == NetRole::SERVER && playerSlot < MAX_PLAYERS && s_slots[playerSlot].peer) {
        ENetPeer* p = asEnet(s_slots[playerSlot].peer);
        stats.rttMs = static_cast<f32>(p->roundTripTime);
        stats.packetLoss = p->packetLoss / 65536.0f;
    } else if (s_role == NetRole::CLIENT && s_serverPeer) {
        stats.rttMs = static_cast<f32>(asEnet(s_serverPeer)->roundTripTime);
        stats.packetLoss = asEnet(s_serverPeer)->packetLoss / 65536.0f;
    }
    return stats;
}

// Net-metrics accessors (F9 net-graph / 1 Hz [NET-GRAPH] log). See net_metrics.h.
void Net::noteSnapshotKind(u8 playerSlot, bool isFull) {
    if (playerSlot >= MAX_PLAYERS) return;
    if (isFull) s_counters.fullSnaps[playerSlot]++;
    else        s_counters.deltaSnaps[playerSlot]++;
}

void Net::tickMetricsWindow(f32 elapsedSec) {
    // Fold each slot's accumulated counters into per-second metrics, then reset the window.
    // packetLoss comes from the ENet peer (getStats). baselineAge is engine-derived state
    // (server tick vs the client's acked tick) that Net can't see, so it's left 0 here and
    // filled in by the display path (HUD/log) via NetMetricsOps::baselineAgeTicks().
    for (u32 slot = 0; slot < MAX_PLAYERS; slot++) {
        f32 loss = getStats(static_cast<u8>(slot)).packetLoss;
        s_lastMetrics[slot] = NetMetricsOps::compute(s_counters, static_cast<u8>(slot),
                                                     elapsedSec, loss, /*baselineAge=*/0);
    }
    s_counters = NetCounters{};
}

NetMetrics Net::getMetrics() {
    // Client: outbound is attributed to slot 0 (single server peer) and inbound is role-global,
    // so slot 0 is the local view. The server reads each remote client via getMetricsForSlot().
    return s_lastMetrics[0];
}

NetMetrics Net::getMetricsForSlot(u8 playerSlot) {
    if (playerSlot >= MAX_PLAYERS) return NetMetrics{};
    return s_lastMetrics[playerSlot];
}

void Net::setOnSnapshot(OnSnapshotFn fn)   { s_onSnapshot = fn; }
void Net::setOnInput(OnInputFn fn)         { s_onInput = fn; }
void Net::setOnPickup(OnPickupFn fn)       { s_onPickup = fn; }
void Net::setOnMeteor(OnMeteorFn fn)       { s_onMeteor = fn; }
void Net::setOnDropItem(OnDropItemFn fn)   { s_onDropItem = fn; }   // R11
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
void Net::setOnEnergyGain(OnEnergyGainFn fn)     { s_onEnergyGain   = fn; }
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
