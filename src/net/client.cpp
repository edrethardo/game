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

// Shortest-arc angle interpolation. A plain lerp of two angles in [-π,π] sweeps the
// long way around when they straddle the ±π seam, making remote players/entities spin.
static f32 lerpAngle(f32 a, f32 b, f32 t) {
    f32 d = b - a;
    while (d >  3.14159265f) d -= 6.28318531f;
    while (d < -3.14159265f) d += 6.28318531f;
    return a + d * t;
}

// Pick the two buffered snapshots that bracket the delayed render time
// (now - INTERP_DELAY_SEC) and the blend factor between them. Rendering remote state
// ~100 ms in the past turns the snapshot ring into a jitter buffer: a late or dropped
// 20 Hz snapshot is covered by interpolating between two already-received snapshots
// instead of freezing on the newest and snapping when the next arrives. (The old code
// blended "now vs newest-arrival", which had no delay buffer and ignored INTERP_DELAY_SEC
// entirely — remotes effectively rendered at the raw newest snapshot.) Returns false when
// the render time can't be bracketed (fewer than 2 snapshots, or it predates the oldest we
// hold) — callers then fall back to rendering the newest snapshot directly.
struct InterpPair {
    const WorldSnapshot* older;
    const WorldSnapshot* newer;
    f32 t; // 0 = at older snapshot, 1 = at newer
};
static bool computeInterpPair(InterpPair& out) {
    if (s_snapCount < 2) return false;
    f64 renderTime = Clock::getElapsedSeconds() - static_cast<f64>(INTERP_DELAY_SEC);
    // Snapshots run newest (ago=0) to oldest with strictly decreasing arrival times. Take
    // the newest adjacent pair whose older bound is at or before renderTime: that pair
    // straddles renderTime. If even the newest snapshot predates renderTime (we're starved),
    // the first pair matches with t clamped to 1, so we hold on the newest.
    for (u32 ago = 0; ago + 1 < s_snapCount; ago++) {
        f64 tNewer = getSnapTime(ago);
        f64 tOlder = getSnapTime(ago + 1);
        if (renderTime >= tOlder) {
            f64 interval = tNewer - tOlder;
            f64 a = (interval > 1e-6) ? (renderTime - tOlder) / interval : 1.0;
            if (a < 0.0) a = 0.0; else if (a > 1.0) a = 1.0;
            out.older = getSnapshot(ago + 1);
            out.newer = getSnapshot(ago);
            out.t     = static_cast<f32>(a);
            return true;
        }
    }
    // renderTime predates the oldest buffered snapshot (transient at session start). Holding
    // on the OLDEST is correct: rendering at t=0 of the oldest pair is the visible past;
    // falling forward to the newest would be a brief jump. Both endpoints = oldest, t = 0.
    const u32 oldestAgo = s_snapCount - 1;
    out.older = getSnapshot(oldestAgo);
    out.newer = getSnapshot(oldestAgo);
    out.t     = 0.0f;
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void Client::init(u8 localPlayerIndex) {
    s_localPlayerIndex = localPlayerIndex;
    s_snapHead = 0;
    s_snapCount = 0;
    s_hasInput = false;
    s_acceptNextSnapshot = true; // first post-(re)init snapshot is accepted unconditionally (TA-6)
    LOG_INFO("Client: initialized (local player=%u)", localPlayerIndex);
}

void Client::captureAndSendInput(const Player& player, u32 clientTick, u8 weaponId, u8 skillSlot) {
    s_latestInput = PlayerController::captureLocalInput(player, clientTick, weaponId);
    // captureLocalInput defaults skillSlot to 0 ("set by engine before sending"); stamp the
    // engine's selected class-skill slot here so the server activates the chosen skill.
    s_latestInput.skillSlot = skillSlot;
    s_hasInput = true;

    // Serialize and send to server. Wire layout (PROTOCOL_VERSION 2):
    //   header(4) + tick(4) + moveFlags(1) + weaponId(1) + yawQ(2) + pitchQ(2)
    //   + posXQ(2) + posYQ(2) + posZQ(2) + extFlags(1) + skillSlot(1) = 22 B
    PacketWriter w;
    w.writeU8(static_cast<u8>(NetPacketType::CL_INPUT));
    w.writeU8(0);
    w.writeU16(0);
    w.writeU32(s_latestInput.tick);
    w.writeU8(s_latestInput.moveFlags);
    w.writeU8(s_latestInput.weaponId);
    w.writeU16(s_latestInput.yawQ);
    w.writeU16(s_latestInput.pitchQ);
    w.writeU16(s_latestInput.posXQ);
    w.writeU16(s_latestInput.posYQ);
    w.writeU16(s_latestInput.posZQ);
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
        // [AUDIT-P1] Diagnostic: what did the client actually decode? Compare against the
        // server's [AUDIT-P1] snap tx line — same tick should carry the same counts (modulo
        // priority drop). Throttled to every 5th rx (~4 Hz) so the log is readable.
        static u32 s_snapRxLogCounter = 0;
        if ((s_snapRxLogCounter++ % 5) == 0) {
            LOG_INFO("[AUDIT-P1] snap rx tick=%u players=%u ents=%u projs=%u items=%u size=%u",
                     snap.serverTick, snap.playerCount, snap.entityCount,
                     snap.projectileCount, snap.worldItemCount, size);
        }
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
    } else {
        // [AUDIT-P1] Diagnostic: deserialize failed (almost always size < requiredBytes from a
        // truncated/lost UDP fragment). Today this is silently dropped; if the client is seeing
        // no enemies/projectiles, a steady stream of these fully explains it. EVERY failure is
        // logged (unthrottled) — they should be rare in practice; a high rate is a real problem.
        LOG_WARN("[AUDIT-P1] snap rx: deserialize FAILED size=%u", size);
    }
}

const WorldSnapshot* Client::getLatestSnapshot() {
    return getSnapshot(0);
}

// Reconcile snapshot's authoritative state into the local player. Under the trust-client
// position model the client's m_localPlayer is the source of truth for movement/aim, so
// reconcile is no longer a position corrector — it just pulls server-authoritative HP,
// status timers, death, ammo, and reload state into the local player. A loose-threshold
// safety snap (1 m) still triggers for legitimate teleports (knockback, respawn, anti-
// cheat clamp): below that, position/yaw/pitch are NOT touched, so the camera advances
// only via PlayerController::update with no double-application or rubberband.
//
// `np` is the local NetPlayer slot (gets HP/status updates flowing back into the snapshot
// chain). `lp` is m_localPlayer — updated for HP/status (mirrored fields) and the rare
// teleport snap so the camera follows server-driven position changes.
bool Client::reconcile(NetPlayer& np, Player& lp) {
    const WorldSnapshot* snap = getSnapshot(0);
    if (!snap) return false; // no snapshot yet — keep pure local state

    // Find our own player in the snapshot.
    const SnapPlayer* serverState = nullptr;
    for (u32 i = 0; i < snap->playerCount; i++) {
        if (snap->players[i].slotIndex == s_localPlayerIndex) {
            serverState = &snap->players[i];
            break;
        }
    }
    if (!serverState) return false; // local player absent from snapshot — no-op

    // R7-4: adopt the server's AUTHORITATIVE health/maxHealth + status. The wire carries
    // health as a 0-255 ratio of the (also wired) absolute maxHealth, so reconstruct
    // absolute HP = ratio * maxHealth. maxHealth==0 (pre-first-real-snapshot) is ignored.
    if (serverState->maxHealth > 0) {
        np.maxHealth = static_cast<f32>(serverState->maxHealth);
        np.health    = (serverState->health / 255.0f) * np.maxHealth;
        np.isDead    = (serverState->animFlags & (1 << 2)) != 0;
        np.weaponState.currentClip = serverState->currentClip;
        np.weaponState.reloading   = (serverState->animFlags & (1 << 1)) != 0;
        // Status timers (quantized 0.04 s steps; see buildFromState).
        // Take max of locally-predicted invuln vs server snapshot so a fresh dodge's
        // 0.3 s i-frame isn't briefly wiped between prediction and the next ack.
        {
            const f32 srvInvuln = serverState->invulnTimer / 25.0f;
            if (np.invulnTimer < srvInvuln) np.invulnTimer = srvInvuln;
        }
        np.poisonTimer = serverState->poisonTimer / 25.0f;
        np.burnTimer   = serverState->burnTimer   / 25.0f;
        np.freezeTimer = serverState->freezeTimer / 25.0f;
        // slowTimer has only a status FLAG on the wire (bit4); keep a short slow alive
        // while the flag is set so the speed debuff is visible client-side.
        if (serverState->statusFlags & (1 << 4)) {
            if (np.slowTimer < 0.1f) np.slowTimer = 0.2f;
        } else {
            np.slowTimer = 0.0f;
        }
    }

    // Loose-threshold safety snap: under normal play the client's m_localPlayer is the
    // truth and the server mirrors it exactly (within quantization), so position will
    // never diverge by more than a few millimetres. But legitimate server-driven moves
    // — knockback impulse, respawn teleport, anti-cheat clamp on a flagrant move — need
    // to pull the local camera along. 1 m threshold is far above any quantization or
    // tick-of-lag drift, so it only triggers on real teleports. No snap below threshold:
    // m_localPlayer keeps advancing smoothly from PlayerController::update.
    Vec3 serverPos{
        Quantize::unpackPos(serverState->posX),
        Quantize::unpackPos(serverState->posY),
        Quantize::unpackPos(serverState->posZ),
    };
    Vec3 diff = lp.position - serverPos;
    if (lengthSq(diff) > 1.0f * 1.0f) {
        lp.position = serverPos;
        lp.velocity = {0.0f, 0.0f, 0.0f}; // teleport zeros momentum
        np.position = serverPos;          // keep NetPlayer mirror in sync for snapshot ack
        np.velocity = {0.0f, 0.0f, 0.0f};
        LOG_INFO("Client: reconcile teleport snap (server-driven, dist=%.2f m)",
                 sqrtf(lengthSq(diff)));
    }
    return true;
}

void Client::interpolateRemotePlayers(u8 localSlot,
                                       Vec3* outPositions, f32* outYaws,
                                       bool* outActive, f32* outHealth, f32* outMaxHealth,
                                       u8* outAnimFlags, u8* outWeaponMeshId)
{
    // Clear all slots
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        outActive[i] = false;
        outHealth[i] = 0.0f;
        outMaxHealth[i] = 100.0f;
        if (outAnimFlags) outAnimFlags[i] = 0;
        if (outWeaponMeshId) outWeaponMeshId[i] = 0;
    }

    // Interpolate at a delayed render time (computeInterpPair) for smooth, jitter-buffered
    // remote motion: snapB = newer endpoint we render toward, snapA = older endpoint we lerp
    // from. If the render time can't be bracketed (too few snapshots), render the newest
    // directly (snapA = null, handled by the single-snapshot path below).
    InterpPair ip;
    const WorldSnapshot* snapB;
    const WorldSnapshot* snapA;
    f32 t = 1.0f;
    if (computeInterpPair(ip)) {
        snapB = ip.newer; snapA = ip.older; t = ip.t;
    } else {
        snapB = getSnapshot(0); snapA = nullptr;
    }
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
            // Absolute HP from the wired maxHealth (R7-4) — falls back to 100 pre-first-snap.
            outMaxHealth[slot] = (sp.maxHealth > 0) ? static_cast<f32>(sp.maxHealth) : 100.0f;
            outHealth[slot] = (sp.health / 255.0f) * outMaxHealth[slot];
            if (outAnimFlags) outAnimFlags[slot] = sp.animFlags;
            if (outWeaponMeshId) outWeaponMeshId[slot] = sp.weaponMeshId;
        }
        return;
    }

    // For each player in snapB (newer endpoint), find its match in snapA (older) and lerp
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

        // Find in snapA
        Vec3 posA = posB;
        f32 yawA = yawB;
        for (u32 j = 0; j < snapA->playerCount; j++) {
            if (snapA->players[j].slotIndex == slot) {
                posA.x = Quantize::unpackPos(snapA->players[j].posX);
                posA.y = Quantize::unpackPos(snapA->players[j].posY);
                posA.z = Quantize::unpackPos(snapA->players[j].posZ);
                yawA   = Quantize::unpackAngle(snapA->players[j].yaw);
                break;
            }
        }

        outActive[slot] = true;
        outPositions[slot] = posA + (posB - posA) * t;
        outYaws[slot]   = lerpAngle(yawA, yawB, t);
        // Absolute HP from the wired maxHealth (R7-4) — falls back to 100 pre-first-snap.
        outMaxHealth[slot] = (spB.maxHealth > 0) ? static_cast<f32>(spB.maxHealth) : 100.0f;
        outHealth[slot] = (spB.health / 255.0f) * outMaxHealth[slot];
        if (outAnimFlags) outAnimFlags[slot] = spB.animFlags; // latest snapshot's anim state
        if (outWeaponMeshId) outWeaponMeshId[slot] = spB.weaponMeshId;
    }
}

void Client::interpolateEntities(EntityPool& renderEntities) {
    // Clear all (flags=0 marks every slot inactive; activeList/activeCount rebuilt below)
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        renderEntities.entities[i].flags = 0;
    }
    renderEntities.activeCount = 0;

    // Delayed-render-time interpolation (see computeInterpPair); fall back to the newest
    // snapshot when the render time can't be bracketed (too few snapshots).
    InterpPair ip;
    const WorldSnapshot* snapB;
    const WorldSnapshot* snapA;
    f32 t = 1.0f;
    if (computeInterpPair(ip)) { snapB = ip.newer; snapA = ip.older; t = ip.t; }
    else { snapB = getSnapshot(0); snapA = nullptr; }
    if (!snapB) return;

    for (u32 i = 0; i < snapB->entityCount; i++) {
        const SnapEntity& seB = snapB->entities[i];
        u8 idx = seB.poolIndex;
        if (idx >= MAX_ENTITIES) continue;

        Entity& e = renderEntities.entities[idx];
        e.flags   = seB.flags;
        e.aiState = static_cast<AIState>(seB.aiState);
        // C1: rebuild the render pool's active list — every renderer iterates by activeList, so
        // without this the client drew zero enemies. Each snapshot entity is active by construction.
        renderEntities.activeList[renderEntities.activeCount++] = idx;
        // H2: authoritative visual identity (the client's local ghost pool diverges — it predicts
        // kills — so its slot can't be trusted for mesh/material/type) + velocity for walk anim.
        e.meshId       = seB.meshId;
        e.materialId   = seB.materialId;
        e.enemyType    = static_cast<EnemyType>(seB.enemyTypeId);
        e.weaponMeshId = seB.weaponMeshId;
        e.velocity     = { Quantize::unpackVel(seB.velX), 0.0f, Quantize::unpackVel(seB.velZ) };

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

        // Note: enemy absolute HP isn't wired (no enemy HP bar today). Reconstructing
        // health = healthPct*maxHealth used a stale `e.maxHealth` from a possibly-different
        // ghost entity at the same poolIndex. Zero both to make the "not authoritative on
        // client" contract explicit — renderers don't read these. (N5)
        e.maxHealth = 0.0f;
        e.health    = 0.0f;
        // (Audit P2 #4) Use the wire-quantized per-entity halfExtents so non-boss
        // entities with custom collider sizes (most GENERIC enemies — 13 variants in
        // enemies.json) render and collide at their real size on the client. Fall back
        // to the prior ENT_FLYING/default boxes only if the snapshot truncated the field
        // to zero (would only happen pre-batch on a server still running the old wire,
        // not a problem in practice but cheap to keep symmetric).
        if (seB.halfExtentsXQ || seB.halfExtentsYQ || seB.halfExtentsZQ) {
            e.halfExtents = {seB.halfExtentsXQ * 0.01f,
                             seB.halfExtentsYQ * 0.01f,
                             seB.halfExtentsZQ * 0.01f};
        } else {
            e.halfExtents = (e.flags & ENT_FLYING)
                ? Vec3{0.3f, 0.3f, 0.3f}
                : Vec3{0.4f, 0.5f, 0.4f};
        }

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

    // Delayed-render-time interpolation (see computeInterpPair); fall back to the newest
    // snapshot when the render time can't be bracketed (too few snapshots).
    InterpPair ip;
    const WorldSnapshot* snapB;
    const WorldSnapshot* snapA;
    f32 t = 1.0f;
    if (computeInterpPair(ip)) { snapB = ip.newer; snapA = ip.older; t = ip.t; }
    else { snapB = getSnapshot(0); snapA = nullptr; }
    if (!snapB) return;

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
        // (Audit-A) Owner slot so projectile-impact kills credit the firing player. The
        // CLIENT itself doesn't apply damage (server-authoritative) but `Combat` and skill
        // systems read this on the host when projectiles arrive via reconciliation paths,
        // and any other observer needs the right attribution for ring on-kill VFX/loot.
        p.ownerSlot  = spB.ownerSlot;

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
        // (Audit-B) Pickup ownership window is now on the wire — read it instead of
        // hardcoding FFA. Drives the CV-1 client-side pickup gate
        // (engine_update.cpp:853: w.ownerSlot != activeNetSlot() && w.exclusiveTimer > 0)
        // so non-owners can't grab a freshly-credited drop until the timer expires.
        wi.ownerSlot      = sw.ownerSlot;
        wi.exclusiveTimer = sw.exclusiveTimerQ * 0.04f;
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
