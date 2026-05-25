#pragma once

#include "core/types.h"
#include "core/math.h"
#include "net/net.h"
#include "net/net_player.h"
#include "net/snapshot.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "game/item.h"        // WorldItemPool / ItemDef for world-item mirroring
#include "game/weapon.h"
#include "world/level_grid.h"
#include "world/collision.h"  // CollisionObstacle for the reconcile replay step

static constexpr u32 SNAP_BUFFER_SIZE = 4;  // 4 snapshots × ~66KB = 264KB (was 32 × 66KB = 2.1MB)
static constexpr f32 INTERP_DELAY_SEC = 0.1f;  // 100ms interpolation delay
static constexpr u32 PREDICTION_HISTORY_SIZE = 128;

struct PredictionEntry {
    NetInput input;
    Vec3     predictedPosition;
    Vec3     predictedVelocity;
    f32      predictedYaw;
    f32      predictedPitch;
    bool     predictedOnGround;
};

namespace Client {
    void init(u8 localPlayerIndex);

    // Capture local input, pack into NetInput, send to server.
    // Also stores in prediction history.
    void captureAndSendInput(u32 clientTick, u8 weaponId);

    // Get the latest captured input (for local prediction in engine)
    const NetInput* getLatestInput();

    // Receive and store a snapshot from the server.
    void receiveSnapshot(const u8* data, u32 size);

    // Get the latest received snapshot (may be null if none yet)
    const WorldSnapshot* getLatestSnapshot();

    // Reconcile: compare the server's authoritative position for the local player
    // (from the newest snapshot, at the server-acked input tick) against the prediction
    // we stored for that same tick. If they diverge, snap to the server state and replay
    // every later buffered input to recover the corrected current position (snap-and-replay).
    // `obstacles`/`obstacleCount` must be the SAME entity-obstacle list used for prediction
    // this tick so the replayed collision matches the prediction step 1:1.
    // Returns true if a correction was applied.
    bool reconcile(NetPlayer& localPlayer, const LevelGrid& grid, f32 dt,
                   const CollisionObstacle* obstacles, u32 obstacleCount);

    // Interpolate remote players between snapshots.
    // Fills outPositions/outYaws/outPitches for all MAX_PLAYERS slots.
    void interpolateRemotePlayers(u8 localSlot,
                                   Vec3* outPositions, f32* outYaws, f32* outPitches,
                                   bool* outActive, f32* outHealth, f32* outMaxHealth,
                                   u8* outAnimFlags = nullptr);

    // Interpolate entities from snapshots into a render-only pool.
    void interpolateEntities(EntityPool& renderEntities);

    // Interpolate projectiles from snapshots into a render-only pool.
    void interpolateProjectiles(ProjectilePool& renderProjectiles);

    // Mirror the server-authoritative world-item list (loot drops) from the newest
    // snapshot into the client's local pool. Items are static so a direct copy is fine
    // (no interpolation). The renderer and pickup-aim code read m_worldItems directly,
    // so populating it here makes loot appear/disappear in lockstep with the server.
    void mirrorWorldItems(WorldItemPool& outItems, const ItemDef* itemDefs, u32 itemDefCount);

    // Store a prediction entry after local simulation
    void storePrediction(const NetInput& input, const NetPlayer& predicted);

    // Get local player index
    u8 getLocalPlayerIndex();
}
