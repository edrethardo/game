#pragma once

#include "core/types.h"
#include "net/net.h"
#include "net/net_player.h"
#include "net/snapshot.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "game/weapon.h"
#include "world/level_grid.h"

namespace Server {
    void init(NetPlayer* players, u32 levelSeed);

    // Process incoming input packet from a client.
    void receiveInput(u8 playerSlot, const u8* data, u32 size);

    // Get the input buffer for a player slot.
    InputRingBuffer& getInputBuffer(u8 playerSlot);

    // Build and broadcast a snapshot to all clients.
    void sendSnapshot(u32 serverTick,
                      const NetPlayer* players,
                      const EntityPool& entities,
                      const ProjectilePool& projectiles);

    u32 getLevelSeed();
}
