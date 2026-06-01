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

    // Clear one slot's input ring buffer (call on join/leave). Without this, a client that
    // rejoins a slot mid-floor is frozen: the buffer's monotonic-tick guard rejects the
    // rejoiner's fresh low ticks until they climb past the departed client's stale ticks.
    void resetInputBuffer(u8 playerSlot);

    // Build the current snapshot from game state and broadcast a full copy to all
    // connected clients (unreliable fragment channel). Used for singleplayer.
    void sendSnapshot(u32 serverTick,
                      const NetPlayer* players,
                      const EntityPool& entities,
                      const ProjectilePool& projectiles,
                      const WorldItemPool& worldItems);

    // D7.3 — Build the snapshot WITHOUT broadcasting. The engine then calls
    // sendSnapshotFullToSlot / sendSnapshotDeltaToSlot per active remote slot.
    // Sets getLastSnapshot() so per-slot callers can read the built state.
    void buildSnapshotOnly(u32 serverTick,
                           const NetPlayer* players,
                           const EntityPool& entities,
                           const ProjectilePool& projectiles,
                           const WorldItemPool& worldItems);

    // D7.3 — Send a full snapshot to a single client slot (used when the client
    // has no accepted baseline yet, e.g. first snapshot or baseline mismatch).
    void sendSnapshotFullToSlot(u8 slot);

    // D7.3 — Send a delta snapshot to a single client slot encoded against the
    // given baseline. Falls back to a full send if serialization fails.
    void sendSnapshotDeltaToSlot(u8 slot, const WorldSnapshot& baseline);

    // D7.2 — Return the snapshot that was built and sent on the most recent
    // sendSnapshot call.  The server calls this once per snapshot tick to copy
    // the result into m_baselineSnap[i] for each active remote client.
    // Returns nullptr if no snapshot has been sent yet this session.
    const WorldSnapshot* getLastSnapshot();

    u32 getLevelSeed();
    u8  getLevelFloor();
    u8  getLevelDifficulty();

    // Update the authoritative level metadata WITHOUT touching the input buffers (M11).
    // Call at descent commit so a joiner arriving during the FLOOR_TRANSITION window gets the
    // NEW floor/seed in SV_JOIN_ACCEPT instead of the old one. The full Server::init still runs
    // at transition end via startGame(DESCEND); buffer clear stays there.
    void updateLevel(u32 levelSeed, u8 levelFloor, u8 difficulty);
}
