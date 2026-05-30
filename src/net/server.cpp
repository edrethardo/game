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

    // Wire layout (PROTOCOL_VERSION 2):
    //   header(4) + tick(4) + moveFlags(1) + weaponId(1) + yawQ(2) + pitchQ(2)
    //   + posXQ(2) + posYQ(2) + posZQ(2) + extFlags(1) + skillSlot(1) = 22 B.
    // Min size guard reflects the new layout (was 16 B before — a v1 client would
    // be rejected here too, but the PROTOCOL_VERSION check in CL_JOIN_REQUEST already
    // catches that with a clean SV_JOIN_REJECT before any CL_INPUT arrives).
    if (size < sizeof(PacketHeader) + 18) return;
    PacketReader r;
    r.data = data;
    r.size = size;
    r.cursor = sizeof(PacketHeader);

    NetInput input;
    input.tick      = r.readU32();
    input.moveFlags = r.readU8();
    input.weaponId  = r.readU8();
    input.yawQ      = r.readU16();
    input.pitchQ    = r.readU16();
    input.posXQ     = r.readU16();
    input.posYQ     = r.readU16();
    input.posZQ     = r.readU16();
    input.extFlags  = r.readU8();
    input.skillSlot = r.readU8();

    // Validate input to prevent server crash from malicious clients
    if (input.weaponId >= MAX_WEAPON_DEFS) input.weaponId = 0;
    input.moveFlags &= 0x7F;
    // Keep all defined INPUT_EX_* flags (bits 0-7). Previously masked 0x7F, which
    // silently stripped INPUT_EX_DODGE (bit7) for remote clients — so a remote
    // Wanderer's dodge never granted server-authoritative i-frames (player.cpp:266).
    input.extFlags &= (INPUT_EX_POTION | INPUT_EX_RELOAD | INPUT_EX_SKILL | INPUT_EX_BOOT_SKILL
                       | INPUT_EX_HELM_SKILL | INPUT_EX_INVENTORY | INPUT_EX_DODGE);
    if (input.skillSlot > 3) input.skillSlot = 0;

    s_inputBuffers[playerSlot].push(input);
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

void Server::sendSnapshot(u32 serverTick,
                           const NetPlayer* players,
                           const EntityPool& entities,
                           const ProjectilePool& projectiles,
                           const WorldItemPool& worldItems)
{
    static WorldSnapshot snap;
    snap.serverTick = 0; snap.playerCount = 0; snap.entityCount = 0;
    snap.worldItemCount = 0; snap.projectileCount = 0;
    Snapshot::buildFromState(snap, serverTick, players, entities, projectiles, worldItems);

    // Static scratch (server-only, single-threaded send path) — keeps the larger
    // snapshot buffer off the stack and out of the per-frame heap. serialize()
    // bounds every write by MAX_SNAPSHOT_SIZE and emits truthful counts, so the
    // packet is always internally consistent even if it has to priority-drop.
    static u8 buf[MAX_SNAPSHOT_SIZE];
    u32 size = Snapshot::serialize(snap, buf, MAX_SNAPSHOT_SIZE);
    if (size > 0) {
        // Snapshots may exceed one MTU; broadcastSnapshot fragments them unreliably.
        Net::broadcastSnapshot(buf, size);
    }
}

u32 Server::getLevelSeed() {
    return s_levelSeed;
}

u8 Server::getLevelFloor()      { return s_levelFloor; }
u8 Server::getLevelDifficulty() { return s_levelDifficulty; }
