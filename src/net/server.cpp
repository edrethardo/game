#include "net/server.h"
#include "net/packet.h"
#include "core/log.h"

#include <cstring>

static InputRingBuffer s_inputBuffers[MAX_PLAYERS];
static u32             s_levelSeed = 0;
static u8              s_levelFloor = 1;      // current floor, sent to joiners in JOIN_ACCEPT
static u8              s_levelDifficulty = 0; // current difficulty tier, sent to joiners

void Server::init(NetPlayer* players, u32 levelSeed, u8 levelFloor, u8 difficulty) {
    s_levelSeed = levelSeed;
    s_levelFloor = levelFloor;
    s_levelDifficulty = difficulty;
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        s_inputBuffers[i] = InputRingBuffer{};
    }
    LOG_INFO("Server: initialized (seed=%u floor=%u diff=%u)", levelSeed, levelFloor, difficulty);
}

void Server::receiveInput(u8 playerSlot, const u8* data, u32 size) {
    if (playerSlot >= MAX_PLAYERS) return;

    // Wire layout: PacketHeader(4) + window payload.
    // Window payload: u8 windowCount + u8 targetSlot + u8[2] reserved + N×15 B inputs
    // (N ≤ INPUT_WINDOW_SIZE; 15 B = 14 + interpDelayMs — input_wire.cpp is the authority).
    // Min valid size = 4(header) + 4(window header) + 1×15(one input) = 23 B.
    // deserializeInputWindow validates the exact count vs. buffer size; the guard here
    // is just a cheap early-out for obviously-corrupt/truncated packets.
    if (size < sizeof(PacketHeader) + 4) return;

    NetInput batch[INPUT_WINDOW_SIZE];
    // Payload starts after the 4-byte PacketHeader
    u32 count = deserializeInputWindow(data + sizeof(PacketHeader),
                                       size - sizeof(PacketHeader),
                                       batch, INPUT_WINDOW_SIZE);
    if (count == 0) return;  // malformed or truncated — discard silently

    InputRingBuffer& buf = s_inputBuffers[playerSlot];
    for (u32 i = 0; i < count; i++) {
        NetInput& input = batch[i];
        // Validate each input to prevent server crash from malicious clients
        if (input.weaponId >= MAX_WEAPON_DEFS) input.weaponId = 0;
        input.moveFlags &= 0x7F;
        // Keep all defined INPUT_EX_* flags (bits 0-7). Previously masked 0x7F, which
        // silently stripped INPUT_EX_DODGE (bit7) for remote clients — so a remote
        // Wanderer's dodge never granted server-authoritative i-frames (player.cpp:266).
        input.extFlags &= (INPUT_EX_POTION | INPUT_EX_RELOAD | INPUT_EX_SKILL | INPUT_EX_BOOT_SKILL
                           | INPUT_EX_HELM_SKILL | INPUT_EX_INVENTORY | INPUT_EX_DODGE);
        if (input.skillSlot > 3) input.skillSlot = 0;
        // push() filters duplicates and stale ticks via monotonic clientTick check, so
        // re-sending earlier inputs in the window is safe even at high packet rates.
        buf.push(input);
    }
}

InputRingBuffer& Server::getInputBuffer(u8 playerSlot) {
    return s_inputBuffers[playerSlot];
}

void Server::resetInputBuffer(u8 playerSlot) {
    if (playerSlot < MAX_PLAYERS) s_inputBuffers[playerSlot] = InputRingBuffer{};
}

void Server::updateLevel(u32 levelSeed, u8 levelFloor, u8 difficulty) {
    s_levelSeed = levelSeed;
    s_levelFloor = levelFloor;
    s_levelDifficulty = difficulty;
}

// D7.2 — File-scope snapshot scratch promoted from the former static local inside
// sendSnapshot so that getLastSnapshot() can return a stable pointer to it.
// Single-threaded send path: populated by buildFromState, read by getLastSnapshot.
static WorldSnapshot s_lastSnap;
static bool          s_snapBuilt = false; // true after the first sendSnapshot call

// D7.3 — Shared snapshot build helper (used by both sendSnapshot and buildSnapshotOnly).
static void doBuildSnapshot(u32 serverTick,
                             const NetPlayer* players,
                             const EntityPool& entities,
                             const ProjectilePool& projectiles,
                             const WorldItemPool& worldItems) {
    s_lastSnap.serverTick = 0; s_lastSnap.playerCount = 0; s_lastSnap.entityCount = 0;
    s_lastSnap.worldItemCount = 0; s_lastSnap.projectileCount = 0;
    Snapshot::buildFromState(s_lastSnap, serverTick, players, entities, projectiles, worldItems);
    s_snapBuilt = true;
}

void Server::sendSnapshot(u32 serverTick,
                           const NetPlayer* players,
                           const EntityPool& entities,
                           const ProjectilePool& projectiles,
                           const WorldItemPool& worldItems)
{
    doBuildSnapshot(serverTick, players, entities, projectiles, worldItems);

    // Static scratch (server-only, single-threaded send path) — keeps the larger
    // snapshot buffer off the stack and out of the per-frame heap. serialize()
    // bounds every write by MAX_SNAPSHOT_SIZE and emits truthful counts, so the
    // packet is always internally consistent even if it has to priority-drop.
    static u8 buf[MAX_SNAPSHOT_SIZE];
    u32 size = Snapshot::serialize(s_lastSnap, buf, MAX_SNAPSHOT_SIZE);
    if (size > 0) {
        // Snapshots may exceed one MTU; broadcastSnapshot fragments them unreliably.
        Net::broadcastSnapshot(buf, size);
    }
}

// D7.3 — Build the snapshot from game state without broadcasting. Lets the engine
// call sendSnapshotFullToSlot / sendSnapshotDeltaToSlot per active remote slot after
// building, instead of broadcasting the same full snapshot to every client.
void Server::buildSnapshotOnly(u32 serverTick,
                                const NetPlayer* players,
                                const EntityPool& entities,
                                const ProjectilePool& projectiles,
                                const WorldItemPool& worldItems) {
    doBuildSnapshot(serverTick, players, entities, projectiles, worldItems);
}

// D7.2 — Expose the most recently built snapshot so the engine can store it
// as a per-client baseline immediately after Server::sendSnapshot returns.
// Returns nullptr before the first snapshot has been built (early in startup).
const WorldSnapshot* Server::getLastSnapshot() {
    return s_snapBuilt ? &s_lastSnap : nullptr;
}

WorldSnapshot* Server::getLastSnapshotMutable() {
    return s_snapBuilt ? &s_lastSnap : nullptr;
}

// D7.3 — Send the most recently built full snapshot to a single client slot.
// Used when a client has no accepted baseline (first snapshot after join, or
// after a baseline mismatch). s_lastSnap must have been built by sendSnapshot()
// before this call (guarded by s_snapBuilt).
void Server::sendSnapshotFullToSlot(u8 slot) {
    if (!s_snapBuilt) return;
    // Re-serialize with isFullSnapshot=1 into a per-slot scratch buffer.
    // Not sharing the broadcast buffer so the delay queue can hold independent copies.
    static u8 s_perSlotBuf[MAX_SNAPSHOT_SIZE];
    u32 size = Snapshot::serialize(s_lastSnap, s_perSlotBuf, MAX_SNAPSHOT_SIZE, /*isFullSnapshot=*/1);
    if (size > 0) {
        Net::sendSnapshotToSlot(slot, s_perSlotBuf, size);
        // No per-send logging here: this used to LOG_INFO on every send — 60 lines/s per client
        // in the hottest net path (and every send WAS full while the ack bug kept deltas dead).
        // Net::noteSnapshotKind owns full/delta accounting; the F9 overlay shows the ratio.
    }
}

// D7.3 — Send a delta snapshot to a single client slot, encoded against the given
// baseline. The delta payload contains only changed/new records; the client merges
// it with its locally-stored m_lastAppliedSnap to reconstruct the full snapshot.
// Falls back to a full send if delta serialization produces no output (shouldn't
// happen with a valid baseline, but guards against an empty-baseline edge case).
void Server::sendSnapshotDeltaToSlot(u8 slot, const WorldSnapshot& baseline) {
    if (!s_snapBuilt) return;
    // D7.3 wire format: serializeDelta produces a self-contained buffer starting
    // with serverTick (no outer packet header). The client peeks the isFullSnapshot
    // byte to route to deserializeDelta vs. deserialize.
    // We prepend the standard SV_SNAPSHOT packet header (4 bytes) so the client's
    // existing dispatch path (which routes on NetPacketType::SV_SNAPSHOT) still fires.
    static u8 s_deltaBuf[MAX_SNAPSHOT_SIZE];
    u32 cursor = 0;

    // 4-byte packet header (mirrors serialize()'s header: type + flags + seq).
    s_deltaBuf[cursor++] = static_cast<u8>(NetPacketType::SV_SNAPSHOT);
    // Flags byte: bit 0 = isFullSnapshot. Delta path leaves it cleared (matches the
    // existing in-payload isFullSnapshot=0 written by serializeDelta at +20 of its
    // own buffer). The client routes on data[1] alone — see Client::receiveSnapshot.
    s_deltaBuf[cursor++] = 0; // flags (bit 0 cleared = delta)
    s_deltaBuf[cursor++] = 0; // seq lo
    s_deltaBuf[cursor++] = 0; // seq hi

    u32 deltaSize = Snapshot::serializeDelta(s_deltaBuf + cursor, MAX_SNAPSHOT_SIZE - cursor,
                                              s_lastSnap, baseline);
    if (deltaSize == 0) {
        // Serialization failure (shouldn't happen but be safe) — fall back to full.
        sendSnapshotFullToSlot(slot);
        return;
    }
    cursor += deltaSize;

    Net::sendSnapshotToSlot(slot, s_deltaBuf, cursor);
}

u32 Server::getLevelSeed() {
    return s_levelSeed;
}

u8 Server::getLevelFloor()      { return s_levelFloor; }
u8 Server::getLevelDifficulty() { return s_levelDifficulty; }
