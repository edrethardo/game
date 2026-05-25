#pragma once

#include "core/types.h"
#include "net/net.h"
#include "net/net_player.h"
#include "net/snapshot.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "game/weapon.h"
#include "game/item.h"        // WorldItemPool (world-item replication in snapshots)
#include "world/level_grid.h"

namespace Server {
    void init(NetPlayer* players, u32 levelSeed, u8 levelFloor, u8 difficulty);

    // Process incoming input packet from a client.
    void receiveInput(u8 playerSlot, const u8* data, u32 size);

    // Get the input buffer for a player slot.
    InputRingBuffer& getInputBuffer(u8 playerSlot);

    // Build and broadcast a snapshot to all clients.
    void sendSnapshot(u32 serverTick,
                      const NetPlayer* players,
                      const EntityPool& entities,
                      const ProjectilePool& projectiles,
                      const WorldItemPool& worldItems);

    u32 getLevelSeed();
    u8  getLevelFloor();
    u8  getLevelDifficulty();
}
