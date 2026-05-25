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

// Set by Client::init (every client floor descend re-enters startGame -> Client::init,
// engine_startgame.cpp). It forces receiveSnapshot to accept the FIRST snapshot after a
// descend unconditionally, closing the reset-gap straddle window: if a late prior-floor
// snapshot lands first it would become "newest", and a short prior floor (tick < RESET_GAP)
// would then trap the real new-floor snapshot (tick ~0) inside RESET_GAP and discard it as
// stale. Accepting the first arrival regardless makes the post-descend newest deterministic.
static bool     s_acceptNextSnapshot = false;

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
// Shortest-arc angle interpolation. A plain lerp of two angles in [-π,π] sweeps the
// long way around when they straddle the ±π seam, making remote players/entities spin.
static f32 lerpAngle(f32 a, f32 b, f32 t) {
    f32 d = b - a;
    while (d >  3.14159265f) d -= 6.28318531f;
    while (d < -3.14159265f) d += 6.28318531f;
    return a + d * t;
}

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
    s_acceptNextSnapshot = true; // first post-(re)init snapshot is accepted unconditionally (TA-6)
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
        // TA-6: right after a descend, accept whatever arrives first as the sole newest,
        // regardless of straddle ordering. Clear the (already-emptied-by-init) ring so this
        // becomes the unambiguous newest, then resume normal monotonic discard. The RESET_GAP
        // logic below stays as a backstop for in-session resets that don't go through init.
        if (s_acceptNextSnapshot) {
            s_acceptNextSnapshot = false;
            s_snapHead = 0;
            s_snapCount = 0;
            pushSnapshot(snap);
            return;
        }
        // Snapshots ride an UNRELIABLE_FRAGMENT channel, so a stale fragment can
        // arrive AFTER a newer one. The buffer is ordered by arrival, not by tick,
        // and the 2-snapshot wall-clock interpolation blends the two newest arrivals
        // — so a late older snapshot pushed as "newest" makes remotes jump backward.
        // Discard anything not strictly newer than the latest already accepted.
        // serverTick is a monotonic u32 from the server sim (no wrap in any realistic
        // session) WITHIN a floor, so a plain compare is correct for same-floor reordering.
        const WorldSnapshot* newest = getSnapshot(0);
        if (newest && snap.serverTick <= newest->serverTick) {
            // EXCEPTION: the host resets m_serverTick=0 when it descends, so new-floor
            // snapshots start near tick 0 — far below the previous floor's tick. A plain
            // monotonic discard would then reject every new-floor snapshot until the tick
            // climbed back, freezing remotes/entities/loot for minutes. If the incoming
            // tick is FAR below newest, treat it as a server reset (floor change): drop the
            // now-stale prior-floor buffer and accept the new-floor snapshot. The gap
            // (~10 s of ticks) is large enough that no legitimate same-floor reorder can
            // hit it (in-flight reordering spans at most a few snapshots).
            constexpr u32 RESET_GAP = 600; // 10 s at NET_TICK_RATE (60 Hz)
            if (newest->serverTick - snap.serverTick <= RESET_GAP) return; // genuine stale reorder
            s_snapHead = 0;
            s_snapCount = 0; // clear prior-floor snapshots; this becomes the new newest
        }
        pushSnapshot(snap);
    }
}

const WorldSnapshot* Client::getLatestSnapshot() {
    return getSnapshot(0);
}

// Find the prediction-history slot whose stored input tick equals `tick`.
// Returns the ring index, or -1 if that tick is no longer buffered (too old / lost).
static s32 findPredictionByTick(u32 tick) {
    for (u32 i = 0; i < s_predCount; i++) {
        u32 idx = (s_predHead + PREDICTION_HISTORY_SIZE - 1 - i) % PREDICTION_HISTORY_SIZE;
        if (s_predictions[idx].input.tick == tick) return static_cast<s32>(idx);
    }
    return -1;
}

bool Client::reconcile(NetPlayer& localPlayer, const LevelGrid& grid, f32 dt,
                       const CollisionObstacle* obstacles, u32 obstacleCount) {
    const WorldSnapshot* snap = getSnapshot(0);
    if (!snap) return false; // no snapshot yet — keep pure prediction

    // Find our own player in the snapshot.
    const SnapPlayer* serverState = nullptr;
    for (u32 i = 0; i < snap->playerCount; i++) {
        if (snap->players[i].slotIndex == s_localPlayerIndex) {
            serverState = &snap->players[i];
            break;
        }
    }
    if (!serverState) return false; // local player absent from snapshot — no-op

    // R7-4: adopt the server's AUTHORITATIVE health/maxHealth + status into the local
    // player BEFORE any position-reconcile branch returns. The wire carries health as a
    // 0-255 ratio of the (now also wired) absolute maxHealth, so reconstruct absolute HP =
    // ratio * maxHealth. This is the client's source of truth for the HUD HP bar and the
    // death check — the local ghost sim no longer dictates HP (the engine also save/restores
    // local HP around its ghost AI pass, option (a), so this value isn't fought between
    // snapshots). maxHealth==0 (pre-first-real-snapshot) is ignored so we don't zero HP.
    if (serverState->maxHealth > 0) {
        localPlayer.maxHealth = static_cast<f32>(serverState->maxHealth);
        localPlayer.health    = (serverState->health / 255.0f) * localPlayer.maxHealth;
        // Status timers (quantized 0.04 s steps; see buildFromState). The status path on the
        // client should reflect the server so HUD debuff icons / DoT match authoritative state.
        localPlayer.invulnTimer = serverState->invulnTimer / 25.0f;
        localPlayer.poisonTimer = serverState->poisonTimer / 25.0f;
        localPlayer.burnTimer   = serverState->burnTimer   / 25.0f;
        localPlayer.freezeTimer = serverState->freezeTimer / 25.0f;
        // slowTimer has only a status FLAG on the wire (bit4), not a quantized value; keep a
        // short slow alive while the flag is set so the speed debuff is visible client-side.
        if (serverState->statusFlags & (1 << 4)) {
            if (localPlayer.slowTimer < 0.1f) localPlayer.slowTimer = 0.2f;
        } else {
            localPlayer.slowTimer = 0.0f;
        }
    }

    // Acked tick: the highest client input tick the server has processed for our slot.
    // The server sets lastProcessedInputTick for every REMOTE slot each tick
    // (engine_net.cpp), and from the server's view a connected client IS a remote slot,
    // so lastInputTick[ourSlot] is populated in every snapshot (snapshot.cpp:33).
    u32 ackTick = snap->lastInputTick[s_localPlayerIndex];

    // Decode the server's authoritative state for our player.
    Vec3 serverPos;
    serverPos.x = Quantize::unpackPos(serverState->posX);
    serverPos.y = Quantize::unpackPos(serverState->posY);
    serverPos.z = Quantize::unpackPos(serverState->posZ);
    Vec3 serverVel{ Quantize::unpackVel(serverState->velX), 0.0f,
                    Quantize::unpackVel(serverState->velZ) };
    f32  serverYaw   = Quantize::unpackAngle(serverState->yaw);
    f32  serverPitch = Quantize::unpackAngle(serverState->pitch);
    bool serverGround = (serverState->flags & (1 << 1)) != 0;

    // Locate the prediction we made for the acked tick.
    s32 ackIdx = findPredictionByTick(ackTick);
    if (ackIdx < 0) {
        // Acked tick fell out of history (very high latency / tiny buffer). We can't verify
        // a specific prediction, so fall back to the old hard-snap to the authoritative state.
        Vec3 diff = localPlayer.position - serverPos;
        if (length(diff) <= 0.01f) return false; // already coincident — nothing to do
        localPlayer.position = serverPos;
        localPlayer.velocity.x = serverVel.x;
        localPlayer.velocity.z = serverVel.z;
        localPlayer.yaw   = serverYaw;
        localPlayer.pitch = serverPitch;
        localPlayer.onGround = serverGround;
        LOG_WARN("Client: reconcile fallback snap (acked tick %u not in history)", ackTick);
        return true;
    }

    // Compare the server position to what WE predicted at that same tick (not the live
    // position, which is several ticks ahead). If the prediction matched, the local player
    // is already correct — accept it and avoid touching position (no jitter).
    Vec3 predErr = s_predictions[ackIdx].predictedPosition - serverPos;
    if (length(predErr) < 0.05f) return false; // within tolerance — prediction was right

    // Diverged: reset the local player to the server's authoritative state at ackTick, then
    // REPLAY every buffered input newer than ackTick in ascending order, re-running the exact
    // prediction step (updateNetPlayerFromInput → moveAndSlide with the same obstacle list and
    // dt) so the corrected current position accounts for all inputs the server hasn't acked yet.
    localPlayer.position = serverPos;
    localPlayer.velocity.x = serverVel.x;
    localPlayer.velocity.z = serverVel.z;
    localPlayer.yaw   = serverYaw;
    localPlayer.pitch = serverPitch;
    localPlayer.onGround = serverGround;

    // Walk the history oldest→newest; replay only ticks strictly after the acked one.
    for (u32 i = 0; i < s_predCount; i++) {
        u32 idx = (s_predHead + PREDICTION_HISTORY_SIZE - s_predCount + i) % PREDICTION_HISTORY_SIZE;
        PredictionEntry& e = s_predictions[idx];
        if (e.input.tick <= ackTick) continue;

        // Re-apply movement from input (mirrors clientNetPre's prediction call exactly).
        PlayerController::updateNetPlayerFromInput(localPlayer, e.input, dt);

        // Re-run collision against the SAME obstacle list used for this tick's prediction.
        // (We don't re-fire weapons/skills here — those are one-shot, server-authoritative
        // actions; replaying their extFlags would double-trigger them. Only movement state
        // is re-simulated, which is all the local player predicts.)
        Player tempP;
        tempP.position = localPlayer.position;
        tempP.velocity = localPlayer.velocity;
        tempP.onGround = localPlayer.onGround;
        tempP.noclip   = localPlayer.noclip;
        Collision::moveAndSlide(tempP, grid, dt, obstacles, obstacleCount);
        localPlayer.position = tempP.position;
        localPlayer.velocity = tempP.velocity;
        localPlayer.onGround = tempP.onGround;

        // Keep the stored prediction consistent with the corrected base so a later reconcile
        // against an intermediate tick compares against the right (recomputed) value.
        e.predictedPosition = localPlayer.position;
        e.predictedVelocity = localPlayer.velocity;
        e.predictedYaw      = localPlayer.yaw;
        e.predictedPitch    = localPlayer.pitch;
        e.predictedOnGround = localPlayer.onGround;
    }
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
            if (slot >= MAX_PLAYERS) continue; // guard malformed/corrupt snapshot -> OOB write
            if (slot == localSlot) continue;
            if (!(sp.flags & 1)) continue;

            outActive[slot] = true;
            outPositions[slot].x = Quantize::unpackPos(sp.posX);
            outPositions[slot].y = Quantize::unpackPos(sp.posY);
            outPositions[slot].z = Quantize::unpackPos(sp.posZ);
            outYaws[slot]   = Quantize::unpackAngle(sp.yaw);
            outPitches[slot] = Quantize::unpackAngle(sp.pitch);
            // Absolute HP from the wired maxHealth (R7-4) — falls back to 100 pre-first-snap.
            outMaxHealth[slot] = (sp.maxHealth > 0) ? static_cast<f32>(sp.maxHealth) : 100.0f;
            outHealth[slot] = (sp.health / 255.0f) * outMaxHealth[slot];
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
        if (slot >= MAX_PLAYERS) continue; // guard malformed/corrupt snapshot -> OOB write
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
        outYaws[slot]   = lerpAngle(yawA, yawB, t);
        outPitches[slot] = lerpAngle(pitchA, pitchB, t);
        // Absolute HP from the wired maxHealth (R7-4) — falls back to 100 pre-first-snap.
        outMaxHealth[slot] = (spB.maxHealth > 0) ? static_cast<f32>(spB.maxHealth) : 100.0f;
        outHealth[slot] = (spB.health / 255.0f) * outMaxHealth[slot];
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
            e.yaw = lerpAngle(yawA, yawB, t);
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
        // Boss invuln/shield state so the boss renders as un-killable when it is
        // (bit0=minionShield, bits1-3=bossPhase). Mirrors the host pack in buildFromState.
        e.minionShield   = (seB.bossStatus & 0x01) != 0;
        e.bossPhase      = static_cast<u8>((seB.bossStatus >> 1) & 0x07);

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
        p.isCrit     = (spB.flags & (1 << 2)) != 0;
        // Visual fields recovered from the wire so skill/boss projectiles render with
        // their real look instead of the default energy bolt.
        p.projFlags  = spB.projFlags;
        p.meshId     = spB.meshId;
        p.radius     = (spB.radiusQ > 0) ? (spB.radiusQ / 100.0f) : 0.15f;

        // lightColor isn't on the wire (byte-frugal) — reconstruct a glow color from
        // projFlags using the host's conventions so lit projectiles still emit light.
        if (p.projFlags & (PROJ_ORB | PROJ_ORB_SHARD)) p.lightColor = {0.3f, 0.7f, 1.0f}; // cyan frost
        else if (p.projFlags & PROJ_SPARK)             p.lightColor = {0.4f, 0.6f, 1.0f}; // electric blue
        else if (p.projFlags & PROJ_VOID)              p.lightColor = {0.4f, 0.0f, 0.8f}; // void purple
        else if (p.meshId > 0)                         p.lightColor = {0.0f, 0.0f, 0.0f}; // mesh weapons: no glow
        else p.lightColor = p.fromPlayer ? Vec3{1.0f, 0.6f, 0.2f}  // warm player bolt
                                         : Vec3{0.6f, 0.1f, 0.9f}; // enemy purple bolt

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

void Client::mirrorWorldItems(WorldItemPool& outItems, const ItemDef* itemDefs, u32 itemDefCount) {
    (void)itemDefs; (void)itemDefCount; // reserved: client could resolve full stats later
    const WorldSnapshot* snap = getSnapshot(0); // newest — items are static, no lerp needed
    if (!snap) return;

    // Build a presence set from the snapshot keyed by pool slot, preserving the local
    // bobTimer for slots whose uid is unchanged so the bob/spin animation stays smooth
    // across snapshots (only reset it when a slot is newly occupied by a different item).
    bool present[MAX_WORLD_ITEMS] = {};
    u32  activeCount = 0;

    for (u32 i = 0; i < snap->worldItemCount; i++) {
        const SnapWorldItem& sw = snap->worldItems[i];
        u8 slot = sw.slotIndex;
        if (slot >= MAX_WORLD_ITEMS) continue; // guard malformed snapshot
        present[slot] = true;

        WorldItem& wi = outItems.items[slot];
        bool sameItem = wi.active && wi.item.uid == sw.uid;

        wi.active        = true;
        wi.item.defId    = sw.defId;
        wi.item.rarity   = static_cast<Rarity>(sw.rarity);
        wi.item.uid      = sw.uid;
        wi.position.x    = Quantize::unpackPos(sw.posX);
        wi.position.y    = Quantize::unpackPos(sw.posY);
        wi.position.z    = Quantize::unpackPos(sw.posZ);
        wi.ownerSlot     = 0xFF;          // ownership is resolved server-side
        wi.exclusiveTimer = 0.0f;
        if (!sameItem) wi.bobTimer = 0.0f; // fresh drop in this slot — restart animation
        activeCount++;
    }

    // Free any local slot the server no longer reports (picked up / despawned).
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        if (!present[i]) outItems.items[i].active = false;
    }
    outItems.activeCount = activeCount;
}

u8 Client::getLocalPlayerIndex() {
    return s_localPlayerIndex;
}
