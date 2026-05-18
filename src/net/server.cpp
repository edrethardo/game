#include "net/server.h"
#include "net/packet.h"
#include "core/log.h"

#include <cstring>

static InputRingBuffer s_inputBuffers[MAX_PLAYERS];
static u32             s_levelSeed = 0;

void Server::init(NetPlayer* players, u32 levelSeed) {
    s_levelSeed = levelSeed;
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        s_inputBuffers[i] = InputRingBuffer{};
    }
    LOG_INFO("Server: initialized (seed=%u)", levelSeed);
}

void Server::receiveInput(u8 playerSlot, const u8* data, u32 size) {
    if (playerSlot >= MAX_PLAYERS) return;

    // Skip packet header (4 bytes)
    if (size < sizeof(PacketHeader) + 12) return; // 10 base + 2 extended
    PacketReader r;
    r.data = data;
    r.size = size;
    r.cursor = sizeof(PacketHeader);

    NetInput input;
    input.tick        = r.readU32();
    input.moveFlags   = r.readU8();
    input.weaponId    = r.readU8();
    input.mouseDeltaX = r.readS16();
    input.mouseDeltaY = r.readS16();
    input.extFlags    = r.readU8();
    input.skillSlot   = r.readU8();

    // Validate input to prevent server crash from malicious clients
    if (input.weaponId >= MAX_WEAPON_DEFS) input.weaponId = 0;
    input.moveFlags &= 0x7F;
    input.extFlags &= 0x7F; // allow bits 0-6 (includes INPUT_EX_RESPAWN)
    if (input.skillSlot > 3) input.skillSlot = 0;

    s_inputBuffers[playerSlot].push(input);
}

InputRingBuffer& Server::getInputBuffer(u8 playerSlot) {
    return s_inputBuffers[playerSlot];
}

void Server::sendSnapshot(u32 serverTick,
                           const NetPlayer* players,
                           const EntityPool& entities,
                           const ProjectilePool& projectiles)
{
    static WorldSnapshot snap;
    snap.serverTick = 0; snap.playerCount = 0; snap.entityCount = 0; snap.projectileCount = 0;
    Snapshot::buildFromState(snap, serverTick, players, entities, projectiles);

    u8 buf[MAX_PACKET_SIZE];
    u32 size = Snapshot::serialize(snap, buf, MAX_PACKET_SIZE);
    if (size > 0) {
        Net::broadcastUnreliable(buf, size);
    }
}

u32 Server::getLevelSeed() {
    return s_levelSeed;
}
