#pragma once

#include "core/types.h"
#include "net/net_metrics.h"   // NetCounters / NetMetrics / NetMetricsOps (pure, header-only)

static constexpr u32 MAX_PLAYERS       = 4;
// net_metrics.h keeps its own copy of the slot count to stay dependency-free (no include
// cycle); pin it to the real value so any drift is a compile error, not a silent mismatch.
static_assert(NET_METRICS_SLOTS == MAX_PLAYERS,
              "NET_METRICS_SLOTS (net_metrics.h) must match MAX_PLAYERS");
static constexpr u16 DEFAULT_PORT      = 7777;
static constexpr u32 NET_TICK_RATE     = 60;
// 60 Hz snapshots (up from 30 Hz) reduce inter-snap gap from 33 ms to 16.6 ms — one snapshot
// per tick. The client's enemy interp blends maximally-fresh samples and the jitter buffer
// needs only ~2 × 16.6 ms = 33 ms of delay (INTERP_DELAY_SEC in client.h). Bandwidth roughly
// doubles vs 30 Hz (~24 KB/s steady state) which remains well below the 8 KB / packet ceiling.
// Delta compression (M11+) will keep wire size in check as the rate stays at 60 Hz.
static constexpr u32 SNAPSHOT_RATE     = 60;
static constexpr u32 TICKS_PER_SNAP    = NET_TICK_RATE / SNAPSHOT_RATE; // 1

// Bumped to 2 with the absolute-aim NetInput change: the wire layout changed from
// 14 bytes (header(4) + tick(4) + flags(2) + dx/dy(4)) to 18 bytes. M2 restructured
// CL_INPUT further: posXQ/Y/Z removed (-6 B), ackedSnapshotTick added (+2 B), net
// 18 B total. A v1 host paired with a v2 client (or vice versa) would silently mis-parse;
// the PROTOCOL_VERSION check in CL_JOIN_REQUEST handling produces a clean SV_JOIN_REJECT.
// Bumped to 3 when SnapEntity grew from 27 -> 28 B (added attackAnimQ so clients see
// enemy attack swing/lunge animations — N4 gated off the local ghost AI that used to
// drive them locally). A v2 client reading a v3 snapshot would misalign all entity
// fields after the new byte; clean reject instead.
// Bumped to 4 with client-side weapon fire prediction: new CL_FIRE_WEAPON packet
// carries the client's claimed origin + yaw/pitch + clientTick. The server no longer
// fires from per-tick polling of NetInput's FIRE bit (which used np.yaw — a drain-
// derived value that could be stale by seconds under UDP loss / queue lag). A v3
// client wouldn't send CL_FIRE_WEAPON, so on v4 servers it would never fire — clean
// reject is safer than silent broken combat.
// Bumped to 5 when SnapProjectile grew 19 -> 21 B (added clientTickLow u16 so the
// firing client can match snapshot projectiles to its locally-predicted ghosts and
// despawn them on the authoritative arrival — V2 of fire prediction, "projectile
// leaves the wand at click time"). A v4 client reading a v5 snapshot would misalign
// every projectile field after the new bytes; clean reject instead.
// Bumped to 7 for client-authoritative ammo + client-predicted meteors. Two behavioural breaks,
// neither of which misaligns a field — so a version check is the ONLY thing that catches them:
//   • Ammo/reload is now the CLIENT's (Client::reconcile no longer adopts the server clip, and the
//     server no longer gates a client's fire on its own reload). A v6 client on a v7 server would
//     still let the server's stale clip overwrite its own — reviving the "the client never has to
//     reload" bug — while the v7 server no longer gates its fire. Silently inconsistent ammo.
//   • New CL_METEOR (0x0B) + SV_EVENT::METEOR (0x05): each player predicts their own proc meteors
//     and reports them. A v6 client never sends CL_METEOR, so on a v7 server its meteor procs would
//     do no damage at all, and it would ignore the relay for everyone else's.
// Clean SV_JOIN_REJECT beats silently broken combat (same reasoning as the v4 bump above).
// v8: CL_INPUT grew a byte — NetInput.interpDelayMs (14 -> 15 B per input). A v7 host would
// mis-parse a v8 input window (every field after skillSlot shifts), so this MUST reject.
// v9: SV_EVENT gained NOVA_FX (0x06). Additive (an older client would just ignore the unknown
// event byte), but the Blood Nova armor aura is invisible without it, so pair the two builds.
static constexpr u32 PROTOCOL_VERSION  = 9; // v9: SV_EVENT NOVA_FX (replicated nova rings)
                                            // (v6: online couch co-op — join carries localCount+
                                            // class2, accept carries slot2, CL_INPUT/CL_FIRE
                                            // carry targetSlot)

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
    // Client requests the host trigger a floor descent at the portal (reliable; no payload).
    // Server re-validates proximity + boss-dead gate, then runs the same descent flow as the
    // host pressing E and broadcasts SV_LEVEL_SEED so every client transitions in lockstep.
    CL_REQUEST_DESCEND = 0x07,
    // Client weapon-fire request (RELIABLE — M10.1). Replaces the prior FIRE-bit-
    // in-NetInput trigger: the server fires from the claimed origin/yaw/pitch in this
    // packet rather than from the drain-derived np.yaw (which could lag under UDP loss).
    // Payload:
    //   u32 clientTick + u16 posXQ/YQ/ZQ + u16 yawQ + u16 pitchQ = 14 B (header + 14 = 18 B).
    // Sent reliably on every local fire trigger (mouse click edge OR continuous tick when
    // LMB held and cooldown expired). The server's per-(slot, clientTick) dedup ring is
    // retained as a cheap guard against rapid-fire scenarios where a delayed earlier
    // reliable arrives after a subsequent fire has already processed. The server also
    // validates cooldown + clamps origin to within ~1 m of np.position before firing
    // authoritatively.
    CL_FIRE_WEAPON    = 0x08,
    // Client → server full inventory push. Sent after SV_JOIN_ACCEPT when the joining client
    // opted to "Continue" a saved character from the menu, AND again on every subsequent local
    // equip/unequip (see the `// R7` call sites in engine_inventory.cpp + engine_combat.cpp) —
    // the server fires a client's weapon from its OWN copy of that client's inventory
    // (handleWeaponFireForPlayer), so it must be told when the client swaps gear or the damage
    // math keeps using the old item. The server's
    // onPlayerJoin grants a deterministic class-starting kit by default; if this packet
    // arrives, the server replaces the kit with the client's saved equipped / backpack /
    // quickbar / skill state. Wire payload is a single binary blob mirroring the
    // engine_persist.cpp per-player layout (PlayerInventory + QuickbarState + SkillState
    // + class + active class skill + 4 class SkillStates + hp + maxHp); see
    // Engine::sendInventorySync / Engine::onInventorySync. Reliable on channel 0.
    CL_INVENTORY_SYNC = 0x09,
    SV_INVENTORY_SYNC = 0x16,  // server sends full inventory to client (reserved, unused)

    // Clock-sync handshake (M1). Client stamps its wall-clock ms and the server echoes it
    // back alongside its own tick + wall-clock so the client can compute RTT and clock
    // offset without trusting the server's absolute time. Sent/received on channel 1
    // (unreliable) at ~1 Hz from the client; server replies immediately.
    CL_TIME_PING      = 0x0A,  // 4-byte payload: u32 clientTimeMs (echoed by SV_TIME_PONG)
    // Client-predicted weapon on-hit PROC meteor, communicated to the server (reliable).
    // Each player PREDICTS THEIR OWN meteors: the proc roll rides a local `std::rand()` the server
    // cannot reproduce (host/client RNG streams diverge), so the CLIENT owns the roll — it spawns
    // its predicted meteor immediately for an instant telegraph, then tells the server here. The
    // server spawns the single AUTHORITATIVE (damaging) meteor from this message and relays
    // SV_EVENT::METEOR to the OTHER clients only — never back to the caster, which already has its
    // prediction (that would double-spawn). Payload after the 4-byte header:
    //   posX, posY, posZ (f32×3 = 12 B) + radius (4 B) + delay (4 B) + damage (4 B) = 24 B.
    // Damage is clamped server-side against the METEOR_STRIKE SkillDef.
    CL_METEOR         = 0x0B,
    SV_TIME_PONG      = 0x17,  // 12-byte payload: u32 clientTimeMs + u32 serverTick + u32 serverTimeMs

    // M10.2 — Server → Client: the server confirmed a remote player's fire hit an entity.
    // Sent reliably to the firing client only so it can ack the PendingHitRing entry and
    // resolve the predicted hit-marker. Payload (8 B):
    //   u32 clientTick + u16 targetEntityIdx + u16 reserved.
    SV_DAMAGE_DONE    = 0x18,

    // M10.3 — Server → Client: the server applied projectile damage to a player.
    // Sent reliably to the victim client so it can ack the PendingDamageRing entry.
    // Payload (10 B):
    //   u32 projectileSrcKey + f32 damage + u16 reserved.
    // Key encoding mirrors the client: (ownerSlot << 24) | (proj.clientTick & 0xFFFFFF).
    SV_DAMAGE_TO_ME   = 0x19,

    // D1.1 — Server → Client: a kill was confirmed server-side.
    // Sent reliably to ALL clients so they can drive kill-feed, sounds, and
    // future on-kill UI effects. Payload (6 B):
    //   u8  killerSlot   — net slot of the killing player (0xFF = environmental)
    //   u8  victimType   — 0 = entity (NPC), 1 = player
    //   u16 victimIdx    — entity pool index (victimType=0) or net slot (victimType=1)
    //   u8  weaponMeshId — mesh ID of the killing weapon (0 = unknown/unarmed)
    //   u8  isCrit       — 1 if the killing blow was a crit, 0 otherwise
    SV_KILL           = 0x1A,

    // D1.2 — Server → Client: result of a CL_PICKUP_ITEM request.
    // Sent reliably to the requesting client only. Payload (6 B):
    //   u8  accept    — 1 = accepted (item is in inventory), 0 = rejected
    //   u8  reserved  — padding
    //   u32 itemUid   — uid of the world item that was requested
    SV_PICKUP_RESULT  = 0x1B,

    // D1.3 — Server → Client: a new world item spawned (loot drop / boss drop).
    // Broadcast reliably to ALL clients so client-side loot UIs / minimaps can
    // react immediately rather than waiting for the next snapshot window. Payload (12 B):
    //   u32 uid       — world item uid (matches SnapWorldItem.uid for dedup)
    //   u16 posXQ     — packed X position via Quantize::packPos
    //   u16 posYQ     — packed Y position via Quantize::packPos
    //   u16 posZQ     — packed Z position via Quantize::packPos
    //   u16 itemDefId — ItemDef index (for icon/rarity preview before full snapshot)
    SV_LOOT_SPAWN     = 0x1C,

    // Server → Client: energy ("mana") the server computed on a guest's behalf that the guest
    // could NOT predict (projectile manasteal, mana-on-kill — both resolved host-side). Sent
    // reliably to that guest only; it adds the amount to its own energy pool (clamped to max).
    // Coalesced server-side to <=1 packet per guest per tick. Payload (4 B): f32 amount.
    SV_ENERGY_GAIN    = 0x1D,
};

// Sub-types for SV_EVENT packets
enum struct NetEventType : u8 {
    HITSCAN_IMPACT  = 0x01,  // remote player hitscan hit: position + hitEntity flag
    // Server-authoritative class-skill cooldown reset (Wanderer Exploit Weakness on
    // marked-enemy kill, etc.). Payload: skillId (u16 LE). The host can't write the
    // client's m_classSkillStates directly, so it ships this targeted reliable event
    // and the client zeros the matching slot's cooldownTimer on receipt.
    SKILL_CD_RESET  = 0x02,
    // Floating damage-number replication. The host's Combat::applyDamage spawns these
    // locally for HUD display, but with the N4 ghost-sim gated off on CLIENT, the
    // client never runs that path and would see no damage numbers. Payload:
    //   posX, posY, posZ (f32×3 = 12 B) + amount (f32 = 4 B) + flags (u8 = 1 B,
    //   bit0=isHeal, bit1=isCrit). Phase 2.1: now reliable so packet loss doesn't
    //   silently drop the user-facing "I hit them" feedback.
    DAMAGE_NUMBER   = 0x03,
    // Projectile AoE splash impact (fireball/molotov/explosive-round/freeze-trap). The splash
    // VFX callback fires inside ProjectileSystem::update, which is gated off on CLIENT (N4
    // ghost-sim removal) — so the host must signal it or guests see no splash. Payload:
    //   posX, posY, posZ (f32×3 = 12 B) + radius (f32 = 4 B). The client re-runs the same
    //   floor-snap + FX spawn via Engine::spawnSplashFX. Mirrors HITSCAN_IMPACT; reliable
    //   (splashes are infrequent and a missed explosion is jarring).
    PROJECTILE_SPLASH = 0x04,
    // A meteor belonging to SOMEONE ELSE, relayed so this client can see it.
    // Every player PREDICTS THEIR OWN meteors — skill casts (deterministic) and weapon on-hit procs
    // (the roll is a local std::rand() nobody else can reproduce, so the FIRING player owns it and
    // reports the result via CL_METEOR). This event carries the meteors a client did NOT cast: the
    // host's own, and other guests' (the server relays those via broadcastReliableExcept, skipping
    // the caster — echoing one back to the player who predicted it would double-spawn).
    // The receiver spawns a visual-only PendingMeteor; its client-side meteor tick animates and
    // detonates it and applies NO damage — damage is always the server's.
    // Payload: posX, posY, posZ (f32×3 = 12 B) + radius (f32 = 4 B) + delay (f32 = 4 B) = 20 B.
    // Reliable — a missing meteor telegraph is jarring, and they're infrequent.
    METEOR            = 0x05,
    // An expanding nova ring belonging to ANY player, replicated so everyone sees it.
    // Needed because the Blood Nova armor aura is server-authoritative (it fires on taking
    // damage, and only the server knows a guest was hit — SV_DAMAGE_TO_ME covers projectile
    // hits only, not melee), so without this the guest wearing the armor would watch enemies
    // die and 5% of its health vanish with no ring to explain why.
    // Payload: posX, posY, posZ (f32×3 = 12 B) + radius (f32 = 4 B) + r,g,b (f32×3 = 12 B) = 28 B.
    // Purely cosmetic; reliable, since novas are infrequent and a missing one is confusing.
    NOVA_FX           = 0x06,
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
    // Opaque transport handle. ENet: (u64)(uintptr_t)ENetPeer*. Steam relay: the peer's SteamID.
    // 0 = none. The net layer never dereferences it directly — asEnet() casts back for ENet.
    u64       peer        = 0;
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
    // useUpnp=true (default) attempts a UPnP IGD port mapping so friends on other
    // networks can join without manual router config. Pass false for a strictly
    // LAN-only host — the engine skips the SSDP discovery (saving the ~1 s
    // blocking wait) and never asks the router to open a port. Both modes are
    // identical on the wire; the only difference is whether the WAN gets a
    // mapping. Switch builds always behave like useUpnp=false (the wrapper is
    // a stub that returns false instantly).
    // localPlayerCount reserves that many host-owned slots starting at 0 (slot 0 = host, slot 1 =
    // a couch partner for online couch co-op) so findFreeSlot hands remote peers slots beyond them.
    bool hostServer(u16 port = DEFAULT_PORT, bool useUpnp = true, u8 localPlayerCount = 1);

    // Steam relay variants (P1). Same listen-server / drop-in model as hostServer/connectToServer,
    // but the transport is Steam Networking Messages (SDR relay) addressed by SteamID — no IP/port,
    // no UPnP, NAT-traversal for free. Only meaningful when built with USE_STEAM and Steam::isAvailable();
    // otherwise they return false and the caller falls back to the ENet path. hostServerSteam reserves
    // localPlayerCount host slots exactly like hostServer. connectToSteamHost joins the host identified
    // by its SteamID (obtained from a lobby / invite). Set the local class(es) first, as with ENet.
    bool hostServerSteam(u8 localPlayerCount = 1);
    bool connectToSteamHost(u64 hostSteamId);

    // Lobby/HUD accessors for the UPnP IGD result of the last hostServer call.
    // getExternalIp() returns the WAN-side IP the router reported when UPnP
    // succeeded, or "" if it failed / wasn't tried. getUpnpError() returns the
    // short failure cause ("no IGD found", "UPnP unsupported on this platform"
    // on Switch, etc.), or "" if the mapping succeeded. Used by the LOBBY_HOST
    // render path so the host can tell friends what IP to type without leaving
    // the lobby.
    const char* getExternalIp();
    const char* getUpnpError();

    // Set the chosen PlayerClass (cast to u8) to advertise in CL_JOIN_REQUEST.
    // Call before connectToServer() so the join request carries the right class.
    void setLocalPlayerClass(u8 classId);
    // Online couch co-op: advertise BOTH local players' classes + how many local players are joining
    // on this one connection (1 or 2). The server allocates that many slots. Call before connect.
    void setLocalPlayerClasses(u8 class0, u8 class1, u8 localCount);

    // Connect to a remote server.
    bool connectToServer(const char* address, u16 port = DEFAULT_PORT);

    void disconnect();

    // Pump network events. Call once per frame before game update.
    void poll();

    // Send to a specific player slot (server only)
    void sendReliable(u8 playerSlot, const u8* data, u32 size);
    void sendUnreliable(u8 playerSlot, const u8* data, u32 size);
    // D7.3 — Send a snapshot (potentially large, fragmentable) to one client slot only.
    // Uses ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT so the payload may exceed one MTU.
    // Mirrors broadcastSnapshot but targets a single peer.
    void sendSnapshotToSlot(u8 playerSlot, const u8* data, u32 size);

    // Broadcast to all connected clients (server only)
    void broadcastReliable(const u8* data, u32 size);
    // Reliable broadcast to every ACTIVE client EXCEPT one slot. Used to relay a CLIENT-ORIGINATED
    // event (a predicted proc meteor) onward to the other players: the originating client already
    // spawned its own prediction, so echoing it back would double-spawn.
    void broadcastReliableExcept(u8 exceptSlot, const u8* data, u32 size);
    void broadcastUnreliable(const u8* data, u32 size);
    // Broadcast a snapshot: unreliable but fragmentable. Unlike broadcastUnreliable
    // (ENET_PACKET_FLAG_UNSEQUENCED), payloads larger than the MTU are split into
    // MTU-sized fragments delivered unreliably (no retransmit latency) instead of
    // being silently downgraded to reliable. A lost fragment just drops that one
    // snapshot; the next arrives 16.6 ms later. Use for the 60 Hz world snapshot.
    void broadcastSnapshot(const u8* data, u32 size);

    // Send to server (client only)
    void sendToServer(const u8* data, u32 size, bool reliable);

    // Query
    NetRole getRole();
    u8      getLocalPlayerIndex();
    u8      getLocalPlayerIndex2();     // couch client: the 2nd local player's assigned slot (0xFF if none)
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

    // Net-metrics for the F9 net-graph overlay and the 1 Hz [NET-GRAPH] log. The send/recv
    // paths accumulate raw byte/packet/snapshot counters on the net thread (no atomics —
    // single-threaded). tickMetricsWindow() folds them into per-slot NetMetrics once per
    // second and zeroes the counters; call it from the existing 1 Hz gate.
    void       noteSnapshotKind(u8 playerSlot, bool isFull); // server: tally full vs delta snapshot sends
    void       tickMetricsWindow(f32 elapsedSec);            // compute per-slot metrics + reset counters
    NetMetrics getMetrics();                                 // local view (client: server stream as slot 0)
    NetMetrics getMetricsForSlot(u8 playerSlot);             // one remote client's stream (server read-off)

    // Callback types — set before poll()
    using OnSnapshotFn   = void(*)(const u8* data, u32 size);
    using OnInputFn      = void(*)(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_PICKUP_ITEM handler — slot = requesting client, payload carries the uid.
    using OnPickupFn     = void(*)(u8 playerSlot, const u8* data, u32 size);
    // R11: client drops an inventory item. Payload carries the slot kind, slot index,
    // drop position, and the full ItemInstance. Server removes from inventory + spawns
    // world item — see handleDropRequest. Mirrors the OnPickupFn shape.
    using OnDropItemFn   = void(*)(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_METEOR handler — slot = the client that PREDICTED this proc meteor; payload
    // carries pos + radius + delay + damage. Server spawns the authoritative meteor and relays it
    // to the other clients. See CL_METEOR.
    using OnMeteorFn     = void(*)(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_RESPAWN handler — slot = requesting (dead) client; no payload.
    using OnRespawnFn    = void(*)(u8 playerSlot);
    // Server-side CL_REQUEST_DESCEND handler — slot = requesting client; no payload.
    // Server re-validates proximity to the door + boss-dead gate, then runs the same
    // descent flow as the host pressing E.
    using OnDescendRequestFn = void(*)(u8 playerSlot);
    // Server-side CL_FIRE_WEAPON handler — slot = firing client. Payload is the
    // raw CL_FIRE_WEAPON packet starting at the header; the engine unpacks the
    // claimed origin / yaw / pitch / clientTick and fires authoritatively from those.
    using OnFireWeaponFn = void(*)(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_INVENTORY_SYNC handler. slot = sending client; payload is the raw
    // packet starting at the header. Engine deserializes into m_inventories[slot] /
    // m_quickbars[slot] / m_skillStates[slot] etc., overriding the starting kit from
    // onPlayerJoin.
    using OnInventorySyncFn = void(*)(u8 playerSlot, const u8* data, u32 size);
    // Server-side CL_TIME_PING handler (M1.4). slot = pinging client; payload is the raw
    // packet starting at the header. Engine reads clientTimeMs, stamps serverTick +
    // serverTimeMs, and sends SV_TIME_PONG back on the unreliable channel.
    using OnTimePingFn = void(*)(u8 playerSlot, const u8* data, u32 size);
    // Client-side SV_TIME_PONG handler (M1.5). Payload is the raw SV_TIME_PONG packet
    // starting at the header. Engine strips the 4-byte header and passes the 12-byte
    // body to Client::handleTimePong which feeds ClockSyncOps::onPongReceived.
    using OnTimePongFn = void(*)(const u8* data, u32 size);
    // Client-side SV_DAMAGE_DONE handler (M10.2). Called when the server confirms a
    // remote player's fire successfully hit an entity. The client uses this to ack the
    // matching PendingHitRing entry so predicted hit-marker state is cleaned up.
    using OnDamageDoneFn = void(*)(u32 clientTick, u16 targetEntityIdx);
    // Client-side SV_DAMAGE_TO_ME handler (M10.3). Called when the server confirms a
    // projectile hit the local player. The client uses this to ack the matching
    // PendingDamageRing entry.
    using OnDamageToMeFn = void(*)(u32 projectileSrcKey, f32 damage);
    // Client-side SV_KILL handler (D1.1). Called when the server broadcasts a confirmed
    // kill event. killerSlot=0xFF means environmental (no specific player credited).
    // victimType: 0=entity, 1=player. weaponMeshId and isCrit available for FX.
    using OnKillFn = void(*)(u8 killerSlot, u8 victimType, u16 victimIdx,
                             u8 weaponMeshId, u8 isCrit);
    // Client-side SV_PICKUP_RESULT handler (D1.2). Called when the server acknowledges
    // or rejects a CL_PICKUP_ITEM request. accept=1 → confirmed, accept=0 → rejected.
    using OnPickupResultFn = void(*)(u8 accept, u32 itemUid);
    // Client-side SV_LOOT_SPAWN handler (D1.3). Called when a new world item is spawned
    // server-side. v1 client is log-only; future UIs can pin map icons immediately.
    using OnLootSpawnFn = void(*)(u32 uid, f32 posX, f32 posY, f32 posZ, u16 itemDefId);
    // Client-side SV_ENERGY_GAIN handler. Called when the server grants the local player energy
    // it computed authoritatively (projectile manasteal / mana-on-kill). The client adds `amount`
    // to its own pool, clamped to maxEnergy.
    using OnEnergyGainFn = void(*)(f32 amount);
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
    void setOnMeteor(OnMeteorFn fn);            // client-predicted proc meteor → authoritative spawn
    void setOnDropItem(OnDropItemFn fn);        // R11
    void setOnRespawn(OnRespawnFn fn);
    void setOnDescendRequest(OnDescendRequestFn fn);
    void setOnFireWeapon(OnFireWeaponFn fn);
    void setOnInventorySync(OnInventorySyncFn fn);
    void setOnTimePing(OnTimePingFn fn);
    void setOnTimePong(OnTimePongFn fn);
    void setOnDamageDone(OnDamageDoneFn fn);    // M10.2
    void setOnDamageToMe(OnDamageToMeFn fn);    // M10.3
    void setOnKill(OnKillFn fn);                // D1.1
    void setOnPickupResult(OnPickupResultFn fn);// D1.2
    void setOnLootSpawn(OnLootSpawnFn fn);      // D1.3
    void setOnEnergyGain(OnEnergyGainFn fn);
    void setOnEvent(OnEventFn fn);
    void setOnPlayerJoin(OnPlayerJoinFn fn);
    void setOnPlayerLeft(OnPlayerLeftFn fn);
    void setOnLevelSeed(OnLevelSeedFn fn);

    // Server-only: broadcast a mid-run floor descent to all clients (SV_LEVEL_SEED).
    // Carries the NEW floor + difficulty + per-run seed so clients regenerate the
    // identical next dungeon. The run seed is unchanged across floors but is included
    // for robustness (a client that missed SV_JOIN_ACCEPT still self-corrects).
    void broadcastLevelSeed(u8 floor, u8 difficulty, u32 seed);

    // M14: fake packet-loss injection for stress testing.
    // 0 = off (default), 1-100 = drop percentage. Applied in sendToServer (CLIENT) and
    // broadcastSnapshot / sendReliable / sendUnreliable (SERVER) at the send callsites.
    void setFakeLossPct(u8 pct);
    u8   getFakeLossPct();

    // D5: fake-latency cvar. The engine calls setFakeLatencyMs(m_netFakeLatencyMs) at
    // the top of serverNetPre / clientNetPre (just before pumpDelayQueue) so the net
    // layer sees the current value without holding a pointer to Engine.
    void setFakeLatencyMs(u32 ms);
    u32  getFakeLatencyMs();

    // D5: fake-latency delay queue for manual smoke testing.
    // When Engine::m_netFakeLatencyMs > 0 (read via s_engineForNet), every outgoing
    // packet is enqueued with a deliverAt timestamp instead of being sent immediately.
    // pumpDelayQueue() is called at the top of serverNetPre + clientNetPre each frame
    // to release any packets whose deliverAt has passed.
    void pumpDelayQueue();
}
