#pragma once

#include "core/types.h"
#include "core/math.h"
#include "net/net.h"
#include "net/net_player.h"
#include "net/snapshot.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "game/weapon.h"
#include "world/level_grid.h"

static constexpr u32 SNAP_BUFFER_SIZE = 32;
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

    // Reconcile: when a new snapshot arrives, compare server position for
    // local player against our prediction. If error > threshold, correct.
    // Returns true if a correction was applied.
    bool reconcile(NetPlayer& localPlayer, const LevelGrid& grid, f32 dt);

    // Interpolate remote players between snapshots.
    // Fills outPositions/outYaws/outPitches for all MAX_PLAYERS slots.
    void interpolateRemotePlayers(u8 localSlot,
                                   Vec3* outPositions, f32* outYaws, f32* outPitches,
                                   bool* outActive, f32* outHealth, f32* outMaxHealth);

    // Interpolate entities from snapshots into a render-only pool.
    void interpolateEntities(EntityPool& renderEntities);

    // Interpolate projectiles from snapshots into a render-only pool.
    void interpolateProjectiles(ProjectilePool& renderProjectiles);

    // Store a prediction entry after local simulation
    void storePrediction(const NetInput& input, const NetPlayer& predicted);

    // Get local player index
    u8 getLocalPlayerIndex();
}
