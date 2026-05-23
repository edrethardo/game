#include "net/client.h"
#include "net/packet.h"
#include "game/player.h"
#include "world/collision.h"
#include "platform/clock.h"   // wall-clock for time-based snapshot interpolation
#include "core/log.h"

#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------
static u8  s_localPlayerIndex = 0;

// Snapshot ring buffer
static WorldSnapshot s_snapshots[SNAP_BUFFER_SIZE];
static f64           s_snapRecvTime[SNAP_BUFFER_SIZE] = {}; // wall-clock arrival time per slot
static u32           s_snapHead  = 0;
static u32           s_snapCount = 0;

// Prediction history ring buffer
static PredictionEntry s_predictions[PREDICTION_HISTORY_SIZE];
static u32             s_predHead  = 0;
static u32             s_predCount = 0;

// Latest input captured this frame
static NetInput s_latestInput = {};
static bool     s_hasInput    = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static const WorldSnapshot* getSnapshot(u32 ago) {
    if (ago >= s_snapCount) return nullptr;
    u32 idx = (s_snapHead + SNAP_BUFFER_SIZE - 1 - ago) % SNAP_BUFFER_SIZE;
    return &s_snapshots[idx];
}

static void pushSnapshot(const WorldSnapshot& snap) {
    s_snapshots[s_snapHead] = snap;
    s_snapRecvTime[s_snapHead] = Clock::getElapsedSeconds();
    s_snapHead = (s_snapHead + 1) % SNAP_BUFFER_SIZE;
    if (s_snapCount < SNAP_BUFFER_SIZE) s_snapCount++;
}

// Wall-clock arrival time of the snapshot `ago` steps back (0 = newest).
static f64 getSnapTime(u32 ago) {
    if (ago >= s_snapCount) return 0.0;
    u32 idx = (s_snapHead + SNAP_BUFFER_SIZE - 1 - ago) % SNAP_BUFFER_SIZE;
    return s_snapRecvTime[idx];
}

// Time-based interpolation factor between the two newest snapshots. Sweeps 0->1
// across the real inter-snapshot interval as wall-clock advances, so remote
// entities move smoothly every frame instead of being pinned at a fixed blend
// (the old hardcoded 0.7) and jumping when the next snapshot arrived. Clamped to
// [0,1] (no extrapolation); returns 1.0 (render newest) when interpolation isn't
// possible.
static f32 computeInterpAlpha() {
    if (s_snapCount < 2) return 1.0f;
    f64 tNewer = getSnapTime(0);
    f64 tOlder = getSnapTime(1);
    f64 interval = tNewer - tOlder;
    if (interval <= 1e-6) return 1.0f;
    f64 a = (Clock::getElapsedSeconds() - tNewer) / interval;
    if (a < 0.0) a = 0.0; else if (a > 1.0) a = 1.0;
    return static_cast<f32>(a);
}

static void pushPrediction(const PredictionEntry& entry) {
    s_predictions[s_predHead] = entry;
    s_predHead = (s_predHead + 1) % PREDICTION_HISTORY_SIZE;
    if (s_predCount < PREDICTION_HISTORY_SIZE) s_predCount++;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void Client::init(u8 localPlayerIndex) {
    s_localPlayerIndex = localPlayerIndex;
    s_snapHead = 0;
    s_snapCount = 0;
    s_predHead = 0;
    s_predCount = 0;
    s_hasInput = false;
    LOG_INFO("Client: initialized (local player=%u)", localPlayerIndex);
}

void Client::captureAndSendInput(u32 clientTick, u8 weaponId) {
    s_latestInput = PlayerController::captureLocalInput(clientTick, weaponId);
    s_hasInput = true;

    // Serialize and send to server (including extended flags)
    u8 buf[sizeof(PacketHeader) + 12];
    PacketWriter w;
    w.writeU8(static_cast<u8>(NetPacketType::CL_INPUT));
    w.writeU8(0);
    w.writeU16(0);
    w.writeU32(s_latestInput.tick);
    w.writeU8(s_latestInput.moveFlags);
    w.writeU8(s_latestInput.weaponId);
    w.writeS16(s_latestInput.mouseDeltaX);
    w.writeS16(s_latestInput.mouseDeltaY);
    w.writeU8(s_latestInput.extFlags);
    w.writeU8(s_latestInput.skillSlot);

    Net::sendToServer(w.data, w.cursor, false);
}

const NetInput* Client::getLatestInput() {
    return s_hasInput ? &s_latestInput : nullptr;
}

void Client::receiveSnapshot(const u8* data, u32 size) {
    static WorldSnapshot snap;
    snap.serverTick = 0; snap.playerCount = 0; snap.entityCount = 0; snap.projectileCount = 0;
    if (Snapshot::deserialize(snap, data, size)) {
        pushSnapshot(snap);
    }
}

const WorldSnapshot* Client::getLatestSnapshot() {
    return getSnapshot(0);
}

bool Client::reconcile(NetPlayer& localPlayer, const LevelGrid& grid, f32 dt) {
    const WorldSnapshot* snap = getSnapshot(0);
    if (!snap) return false;

    // Find our player in the snapshot
    const SnapPlayer* serverState = nullptr;
    for (u32 i = 0; i < snap->playerCount; i++) {
        if (snap->players[i].slotIndex == s_localPlayerIndex) {
            serverState = &snap->players[i];
            break;
        }
    }
    if (!serverState) return false;

    // Decode server position
    Vec3 serverPos;
    serverPos.x = Quantize::unpackPos(serverState->posX);
    serverPos.y = Quantize::unpackPos(serverState->posY);
    serverPos.z = Quantize::unpackPos(serverState->posZ);

    // Compare with our predicted position
    Vec3 diff = localPlayer.position - serverPos;
    f32 error = length(diff);

    if (error < 0.01f) {
        // Within tolerance — no correction needed
        return false;
    }

    if (error > 1.0f) {
        // Hard snap — something went very wrong
        localPlayer.position = serverPos;
        localPlayer.velocity.x = Quantize::unpackVel(serverState->velX);
        localPlayer.velocity.z = Quantize::unpackVel(serverState->velZ);
        localPlayer.yaw   = Quantize::unpackAngle(serverState->yaw);
        localPlayer.pitch = Quantize::unpackAngle(serverState->pitch);
        LOG_WARN("Client: hard snap correction (error=%.2f)", error);
        return true;
    }

    // Smooth blend toward server position
    f32 blendFactor = 0.15f;
    localPlayer.position = localPlayer.position + (serverPos - localPlayer.position) * blendFactor;
    return true;
}

void Client::storePrediction(const NetInput& input, const NetPlayer& predicted) {
    PredictionEntry entry;
    entry.input = input;
    entry.predictedPosition = predicted.position;
    entry.predictedVelocity = predicted.velocity;
    entry.predictedYaw      = predicted.yaw;
    entry.predictedPitch    = predicted.pitch;
    entry.predictedOnGround = predicted.onGround;
    pushPrediction(entry);
}

void Client::interpolateRemotePlayers(u8 localSlot,
                                       Vec3* outPositions, f32* outYaws, f32* outPitches,
                                       bool* outActive, f32* outHealth, f32* outMaxHealth,
                                       u8* outAnimFlags)
{
    // Clear all slots
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        outActive[i] = false;
        outHealth[i] = 0.0f;
        outMaxHealth[i] = 100.0f;
        if (outAnimFlags) outAnimFlags[i] = 0;
    }

    // Need at least 2 snapshots for interpolation
    const WorldSnapshot* snapB = getSnapshot(0); // newest
    const WorldSnapshot* snapA = getSnapshot(1); // second newest
    if (!snapB) return;

    if (!snapA) {
        // Only one snapshot — use it directly
        for (u32 i = 0; i < snapB->playerCount; i++) {
            const SnapPlayer& sp = snapB->players[i];
            u8 slot = sp.slotIndex;
            if (slot == localSlot) continue;
            if (!(sp.flags & 1)) continue;

            outActive[slot] = true;
            outPositions[slot].x = Quantize::unpackPos(sp.posX);
            outPositions[slot].y = Quantize::unpackPos(sp.posY);
            outPositions[slot].z = Quantize::unpackPos(sp.posZ);
            outYaws[slot]   = Quantize::unpackAngle(sp.yaw);
            outPitches[slot] = Quantize::unpackAngle(sp.pitch);
            outHealth[slot] = (sp.health / 255.0f) * 100.0f;
            outMaxHealth[slot] = 100.0f;
            if (outAnimFlags) outAnimFlags[slot] = sp.animFlags;
        }
        return;
    }

    // Time-based interpolation factor (see computeInterpAlpha): sweeps the blend
    // from the older snapshot toward the newer one across the real inter-snapshot
    // interval, instead of the old fixed 0.7 that froze motion between snapshots.
    f32 t = computeInterpAlpha();

    // For each player in snapB, find corresponding entry in snapA and lerp
    for (u32 i = 0; i < snapB->playerCount; i++) {
        const SnapPlayer& spB = snapB->players[i];
        u8 slot = spB.slotIndex;
        if (slot == localSlot) continue;
        if (!(spB.flags & 1)) continue;

        Vec3 posB;
        posB.x = Quantize::unpackPos(spB.posX);
        posB.y = Quantize::unpackPos(spB.posY);
        posB.z = Quantize::unpackPos(spB.posZ);
        f32 yawB   = Quantize::unpackAngle(spB.yaw);
        f32 pitchB = Quantize::unpackAngle(spB.pitch);

        // Find in snapA
        Vec3 posA = posB;
        f32 yawA = yawB;
        f32 pitchA = pitchB;
        for (u32 j = 0; j < snapA->playerCount; j++) {
            if (snapA->players[j].slotIndex == slot) {
                posA.x = Quantize::unpackPos(snapA->players[j].posX);
                posA.y = Quantize::unpackPos(snapA->players[j].posY);
                posA.z = Quantize::unpackPos(snapA->players[j].posZ);
                yawA   = Quantize::unpackAngle(snapA->players[j].yaw);
                pitchA = Quantize::unpackAngle(snapA->players[j].pitch);
                break;
            }
        }

        outActive[slot] = true;
        outPositions[slot] = posA + (posB - posA) * t;
        outYaws[slot]   = yawA + (yawB - yawA) * t;
        outPitches[slot] = pitchA + (pitchB - pitchA) * t;
        outHealth[slot] = (spB.health / 255.0f) * 100.0f;
        outMaxHealth[slot] = 100.0f;
        if (outAnimFlags) outAnimFlags[slot] = spB.animFlags; // latest snapshot's anim state
    }
}

void Client::interpolateEntities(EntityPool& renderEntities) {
    // Clear all
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        renderEntities.entities[i].flags = 0;
    }

    const WorldSnapshot* snapB = getSnapshot(0);
    if (!snapB) return;

    const WorldSnapshot* snapA = getSnapshot(1);
    f32 t = snapA ? computeInterpAlpha() : 1.0f;

    for (u32 i = 0; i < snapB->entityCount; i++) {
        const SnapEntity& seB = snapB->entities[i];
        u8 idx = seB.poolIndex;
        if (idx >= MAX_ENTITIES) continue;

        Entity& e = renderEntities.entities[idx];
        e.flags   = seB.flags;
        e.aiState = static_cast<AIState>(seB.aiState);

        Vec3 posB;
        posB.x = Quantize::unpackPos(seB.posX);
        posB.y = Quantize::unpackPos(seB.posY);
        posB.z = Quantize::unpackPos(seB.posZ);
        f32 yawB = Quantize::unpackAngle(seB.yaw);

        if (snapA && t < 1.0f) {
            // Find in previous snapshot
            Vec3 posA = posB;
            f32 yawA = yawB;
            for (u32 j = 0; j < snapA->entityCount; j++) {
                if (snapA->entities[j].poolIndex == idx) {
                    posA.x = Quantize::unpackPos(snapA->entities[j].posX);
                    posA.y = Quantize::unpackPos(snapA->entities[j].posY);
                    posA.z = Quantize::unpackPos(snapA->entities[j].posZ);
                    yawA = Quantize::unpackAngle(snapA->entities[j].yaw);
                    break;
                }
            }
            e.position = posA + (posB - posA) * t;
            e.yaw = yawA + (yawB - yawA) * t;
        } else {
            e.position = posB;
            e.yaw = yawB;
        }

        // Restore visual properties from snapshot
        e.health    = (seB.healthPct / 255.0f) * e.maxHealth;
        e.halfExtents = (e.flags & ENT_FLYING)
            ? Vec3{0.3f, 0.3f, 0.3f}
            : Vec3{0.4f, 0.5f, 0.4f};

        // Restore new snapshot fields
        e.stunTimer      = seB.stunTimer / 25.0f;
        e.freezeTimer    = seB.freezeTimer / 25.0f;
        e.bossLimbConfig = seB.bossLimbConfig;

        // Flash timer and death timer are cosmetic — approximate
        if (e.flags & ENT_DEAD) {
            e.deathTimer = 0.5f;
        }
    }
}

void Client::interpolateProjectiles(ProjectilePool& renderProjectiles) {
    // Clear all
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        renderProjectiles.projectiles[i].active = false;
    }
    renderProjectiles.activeCount = 0;

    const WorldSnapshot* snapB = getSnapshot(0);
    if (!snapB) return;

    const WorldSnapshot* snapA = getSnapshot(1);
    f32 t = snapA ? computeInterpAlpha() : 1.0f;

    for (u32 i = 0; i < snapB->projectileCount; i++) {
        const SnapProjectile& spB = snapB->projectiles[i];
        u16 idx = spB.poolIndex;
        if (idx >= MAX_PROJECTILES) continue;

        Projectile& p = renderProjectiles.projectiles[idx];
        p.active     = true;
        p.fromPlayer = (spB.flags & (1 << 1)) != 0;
        p.radius     = 0.15f;

        Vec3 posB;
        posB.x = Quantize::unpackPos(spB.posX);
        posB.y = Quantize::unpackPos(spB.posY);
        posB.z = Quantize::unpackPos(spB.posZ);

        if (snapA && t < 1.0f) {
            Vec3 posA = posB;
            for (u32 j = 0; j < snapA->projectileCount; j++) {
                if (snapA->projectiles[j].poolIndex == idx) {
                    posA.x = Quantize::unpackPos(snapA->projectiles[j].posX);
                    posA.y = Quantize::unpackPos(snapA->projectiles[j].posY);
                    posA.z = Quantize::unpackPos(snapA->projectiles[j].posZ);
                    break;
                }
            }
            p.position = posA + (posB - posA) * t;
        } else {
            p.position = posB;
        }

        p.velocity.x = Quantize::unpackVel(spB.velX);
        p.velocity.y = Quantize::unpackVel(spB.velY);
        p.velocity.z = Quantize::unpackVel(spB.velZ);

        renderProjectiles.activeCount++;
    }
}

u8 Client::getLocalPlayerIndex() {
    return s_localPlayerIndex;
}
