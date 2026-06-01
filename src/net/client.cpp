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

// Rolling send window: holds the last INPUT_WINDOW_SIZE inputs in oldest→newest order.
// Every CL_INPUT packet carries the full window so a single dropped UDP packet no longer
// loses an input — the next packet still carries it. Three consecutive losses are required
// to actually drop an input.
static NetInput s_sendWindow[INPUT_WINDOW_SIZE] = {};
static u32      s_sendWindowCount = 0;

// Set by Client::init (every client floor descend re-enters startGame -> Client::init,
// engine_startgame.cpp). It forces receiveSnapshot to accept the FIRST snapshot after a
// descend unconditionally, closing the reset-gap straddle window: if a late prior-floor
// snapshot lands first it would become "newest", and a short prior floor (tick < RESET_GAP)
// would then trap the real new-floor snapshot (tick ~0) inside RESET_GAP and discard it as
// stale. Accepting the first arrival regardless makes the post-descend newest deterministic.
static bool     s_acceptNextSnapshot = false;

// D7.3 — Client-side baseline snapshot for delta decoding. Updated after each accepted
// snapshot so the server's next delta can diff against our most recently applied state.
// s_hasBaseline starts false and is cleared on init (floor change); the first snapshot
// after init is always full (server's sendSnapshotFullToSlot gates on no-baseline).
static WorldSnapshot s_baselineSnap;
static bool          s_hasBaseline = false;

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
    f32 t;                  // 0 = at older snapshot, 1 = at newer
    f32 extrapolateAhead;   // seconds past `newer` to project via wire velocity (0 if pair brackets renderTime)
};
// Cap forward extrapolation at ~120 ms — Phase 4 widened from 50 ms. The prior 50 ms
// cap meant a single dropped snapshot already freezes remote players at their last
// known pose until the next snapshot arrives, producing a brief teleport when it
// finally lands. 120 ms covers up to three back-to-back dropped 30 Hz snapshots
// smoothly via velocity-only projection. Beyond that we still hold at the newest
// — a stalled network can't drift entities across the world. The trade-off:
// projection error grows linearly with extrapolation time, so a remote that just
// stopped (decel from 6 m/s) overshoots by up to ~70 cm before the next snapshot
// corrects. That's preferable to a stutter on most paths.
static constexpr f32 MAX_EXTRAPOLATE_SEC = 0.12f;
static bool computeInterpPair(InterpPair& out) {
    out.extrapolateAhead = 0.0f;
    if (s_snapCount < 2) return false;
    f64 renderTime = Clock::getElapsedSeconds() - static_cast<f64>(INTERP_DELAY_SEC);

    // Newest snapshot's arrival time. If renderTime is past it, we're in the extrapolation
    // window (no future snap to bracket against). Set t=1 and emit an extrapolateAhead so
    // callers can project `position + velocity * extrapolateAhead` — covers a single dropped
    // snapshot smoothly instead of freezing on the newest until the next arrives.
    const f64 tNewest = getSnapTime(0);
    if (renderTime > tNewest) {
        out.older = getSnapshot(0);
        out.newer = getSnapshot(0);
        out.t     = 1.0f;
        f32 ahead = static_cast<f32>(renderTime - tNewest);
        if (ahead > MAX_EXTRAPOLATE_SEC) ahead = MAX_EXTRAPOLATE_SEC;
        out.extrapolateAhead = ahead;
        return true;
    }

    // Snapshots run newest (ago=0) to oldest with strictly decreasing arrival times. Take
    // the newest adjacent pair whose older bound is at or before renderTime: that pair
    // straddles renderTime.
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
    // Reset the send window so stale prior-floor inputs don't bleed into the new floor's
    // first packets (the server's input ring buffer also resets in Server::init).
    s_sendWindowCount = 0;
    s_acceptNextSnapshot = true; // first post-(re)init snapshot is accepted unconditionally (TA-6)
    // D7.3 — Clear the delta baseline so the first snapshot on the new floor is always
    // treated as a full (no stale prior-floor baseline confusing the decoder).
    s_hasBaseline = false;
    LOG_INFO("Client: initialized (local player=%u)", localPlayerIndex);
}

void Client::captureAndSendInput(const Player& player, u32 clientTick, u8 weaponId,
                                 u8 skillSlot, u8 extFlagsClearMask) {
    s_latestInput = PlayerController::captureLocalInput(player, clientTick, weaponId);
    // captureLocalInput defaults skillSlot to 0 ("set by engine before sending"); stamp the
    // engine's selected class-skill slot here so the server activates the chosen skill.
    s_latestInput.skillSlot = skillSlot;
    // R9 client-side cooldown gate: if the engine determined our local cooldown isn't
    // ready, clear the corresponding INPUT_EX_* bit so we don't even ask the server.
    // Server still gates persistently as a backstop, but this is the "snappy" path —
    // suppress spam at source.
    s_latestInput.extFlags &= static_cast<u8>(~extFlagsClearMask);
    s_hasInput = true;

    // Shift oldest out and append this input at the back (oldest→newest order).
    if (s_sendWindowCount < INPUT_WINDOW_SIZE) {
        s_sendWindow[s_sendWindowCount++] = s_latestInput;
    } else {
        for (u32 i = 0; i < INPUT_WINDOW_SIZE - 1; i++) {
            s_sendWindow[i] = s_sendWindow[i + 1];
        }
        s_sendWindow[INPUT_WINDOW_SIZE - 1] = s_latestInput;
    }

    // Serialize the full window into a local buffer, then wrap it in a packet header and
    // send. Wire layout (M2.2, PROTOCOL_VERSION 2):
    //   PacketHeader(4) + windowPayload: u8 windowCount + u8[3] reserved + N×14 B inputs
    //   Max total = 4 + 4 + 4*14 = 64 B per CL_INPUT.
    u8 payload[256];
    u32 payloadSize = serializeInputWindow(payload, sizeof(payload),
                                           s_sendWindow, s_sendWindowCount);
    PacketWriter w;
    w.writeU8(static_cast<u8>(NetPacketType::CL_INPUT));
    w.writeU8(0);
    w.writeU16(0);
    for (u32 i = 0; i < payloadSize; i++) w.writeU8(payload[i]);

    Net::sendToServer(w.data, w.cursor, false);
}

const NetInput* Client::getLatestInput() {
    return s_hasInput ? &s_latestInput : nullptr;
}

void Client::receiveSnapshot(const u8* data, u32 size, ClockSync& cs) {
    static WorldSnapshot snap;
    snap.serverTick = 0; snap.playerCount = 0; snap.entityCount = 0; snap.projectileCount = 0;

    // Route on the packet-header flags byte (data[1], bit 0 = isFullSnapshot).
    // Earlier builds peeked an offset inside the snapshot payload to find this bit,
    // but the full and delta snapshot wire layouts disagree on where that in-payload
    // byte sits (offset 13 for full, 24 for delta — see snapshot.cpp:374-394 vs
    // server.cpp:148-173 + snapshot.cpp:842-847). No single offset works for both,
    // so the bit now lives in the packet header which is identical-shape across
    // both paths. Falling back to "treat as full" if the packet is somehow shorter
    // than the 4-byte header lets the deserializer log a more useful error.
    bool isFullSnap = (size >= 2) && ((data[1] & 0x01) != 0);

    bool decoded = false;
    if (!isFullSnap && s_hasBaseline && size > 4) {
        // Delta path: strip the 4-byte packet header — deserializeDelta starts at serverTick.
        decoded = Snapshot::deserializeDelta(snap, data + 4, size - 4, s_baselineSnap);
        if (!decoded) {
            LOG_WARN("[D7.3] delta snap rx: deserializeDelta FAILED size=%u", size);
            return;
        }
    } else {
        // Full path (isFullSnapshot=1 OR no baseline yet on the client).
        decoded = Snapshot::deserialize(snap, data, size);
    }

    if (decoded) {
        // [AUDIT-P1] Diagnostic: what did the client actually decode? Compare against the
        // server's [AUDIT-P1] snap tx line — same tick should carry the same counts (modulo
        // priority drop). Throttled to every 5th rx (~4 Hz) so the log is readable.
        static u32 s_snapRxLogCounter = 0;
        if ((s_snapRxLogCounter++ % 5) == 0) {
            LOG_INFO("[AUDIT-P1] snap rx tick=%u players=%u ents=%u projs=%u items=%u size=%u",
                     snap.serverTick, snap.playerCount, snap.entityCount,
                     snap.projectileCount, snap.worldItemCount, size);
        }
        // M1.6: refine the client-side server-tick estimate using this snapshot's tick.
        // The P controller (SNAP_GAIN=0.1) smooths steady-state jitter; deltas >6 ticks
        // (e.g. host floor reset where serverTick jumps back to 0) snap rather than smooth.
        ClockSyncOps::onSnapshotReceived(cs, snap.serverTick, Clock::getElapsedSeconds());
        // Throttled ~1 Hz diagnostic so convergence is eyeball-readable during manual smoke tests.
        static u32 s_clockSyncLogCounter = 0;
        if ((s_clockSyncLogCounter++ % 30) == 0) {
            LOG_INFO("net: clock-sync serverTickEst=%.1f wireServerTick=%u oneWayTripMs=%.1f",
                     cs.serverTickEst, snap.serverTick, cs.oneWayTripMs);
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
            //
            // TODO(M3): the descent reset semantics here predate ClockSync. With ClockSync's
            // LARGE_DELTA snap (>6 ticks), the explicit m_serverTick reset may be redundant.
            // Revisit when client-side prediction lands and the ring-buffer model changes.
            // The s_acceptNextSnapshot path (TA-6, above) already handles the primary case;
            // this RESET_GAP remains as a backstop for in-session resets not through init.
            constexpr u32 RESET_GAP = 600; // 10 s at NET_TICK_RATE (60 Hz)
            if (newest->serverTick - snap.serverTick <= RESET_GAP) return; // genuine stale reorder
            s_snapHead = 0;
            s_snapCount = 0; // clear prior-floor snapshots; this becomes the new newest
        }
        // D7.3 — Update the client-side delta baseline with the just-accepted snapshot.
        // Done BEFORE pushSnapshot so that if the ring wraps, the baseline still holds
        // the most recently accepted tick (which the server's next delta will reference).
        s_baselineSnap = snap;
        s_hasBaseline  = true;
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
        // Phase 1.2 — Reconcile clip + reloading conservatively. Direct overwrite of
        // currentClip from each snapshot would flicker the HUD under fast fire: a snapshot
        // built before the server processed the client's CL_FIRE_WEAPON still carries the
        // old (higher) clip → reconcile snaps the count UP → the next snapshot snaps it
        // back DOWN once the fire lands. The local fire path predicts clip decrements
        // (handleWeaponFire ws.currentClip--), so the local value is "ahead of" the
        // server until lastProcessedInputTick catches up.
        //
        // Adopt server clip when there's a state change we don't predict perfectly:
        //   • server is reloading (reloadTimer is server-driven during the refill);
        //   • local was reloading but server is not (server's reload finished or never
        //     started — adopt server's view either way);
        //   • server clip jumped UP without a reload flag (legendary refill, weapon
        //     swap on server, missed snapshot covering a reload completion).
        // Otherwise, take MIN(local, server.clip) so the HUD never displays MORE ammo
        // than was available after our most recent local fire.
        bool serverReloading = (serverState->animFlags & (1 << 1)) != 0;
        if (serverReloading || np.weaponState.reloading ||
            serverState->currentClip > np.weaponState.currentClip) {
            np.weaponState.currentClip = serverState->currentClip;
        }
        // else: serverState.currentClip <= local clip and no reload — local is "ahead"
        // of server in fire processing; keep local (don't snap HUD upward).
        np.weaponState.reloading = serverReloading;
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
                                       u8* outAnimFlags, u8* outWeaponMeshId,
                                       u8* outPlayerClass)
{
    // Clear all slots
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        outActive[i] = false;
        outHealth[i] = 0.0f;
        outMaxHealth[i] = 100.0f;
        if (outAnimFlags) outAnimFlags[i] = 0;
        if (outWeaponMeshId) outWeaponMeshId[i] = 0;
        // Default to WARRIOR (0) — keeps the renderer safe pre-first-snap and matches
        // the onPlayerJoin out-of-range fallback used server-side.
        if (outPlayerClass) outPlayerClass[i] = 0;
    }

    // Interpolate at a delayed render time (computeInterpPair) for smooth, jitter-buffered
    // remote motion: snapB = newer endpoint we render toward, snapA = older endpoint we lerp
    // from. If the render time can't be bracketed (too few snapshots), render the newest
    // directly (snapA = null, handled by the single-snapshot path below).
    InterpPair ip;
    const WorldSnapshot* snapB;
    const WorldSnapshot* snapA;
    f32 t = 1.0f;
    f32 extrapolateAhead = 0.0f;
    if (computeInterpPair(ip)) {
        snapB = ip.newer; snapA = ip.older; t = ip.t;
        extrapolateAhead = ip.extrapolateAhead;
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
            if (outPlayerClass) outPlayerClass[slot] = sp.playerClass;
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
        // Forward extrapolation when renderTime is past the newest snapshot — project the
        // remote's position using snapB's wire velocity. Bounded by MAX_EXTRAPOLATE_SEC.
        if (extrapolateAhead > 0.0f) {
            Vec3 vel{Quantize::unpackVel(spB.velX), 0.0f, Quantize::unpackVel(spB.velZ)};
            outPositions[slot] = outPositions[slot] + vel * extrapolateAhead;
        }
        // Absolute HP from the wired maxHealth (R7-4) — falls back to 100 pre-first-snap.
        outMaxHealth[slot] = (spB.maxHealth > 0) ? static_cast<f32>(spB.maxHealth) : 100.0f;
        outHealth[slot] = (spB.health / 255.0f) * outMaxHealth[slot];
        if (outAnimFlags) outAnimFlags[slot] = spB.animFlags; // latest snapshot's anim state
        if (outWeaponMeshId) outWeaponMeshId[slot] = spB.weaponMeshId;
        // PlayerClass is a static identity, not a lerp-able value — take it straight from
        // the newer snapshot. (Index is already clamped to PlayerClass::CLASS_COUNT in deserialize.)
        if (outPlayerClass) outPlayerClass[slot] = spB.playerClass;
    }
}

void Client::interpolateEntities(EntityPool& renderEntities, f32 dt) {
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
    f32 extrapolateAhead = 0.0f;
    if (computeInterpPair(ip)) {
        snapB = ip.newer; snapA = ip.older; t = ip.t;
        extrapolateAhead = ip.extrapolateAhead;
    } else {
        snapB = getSnapshot(0); snapA = nullptr;
    }
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
            // Find in previous snapshot. Slot-recycle guard: only accept the match if the
            // entity TYPE also matches — if an enemy died and a new one of a different type
            // spawned into the same poolIndex within an interp window, the unrelated old
            // position would lerp through dead space (visible as a cross-room teleport).
            // On a mismatch we leave posA == posB so the lerp degenerates to "snap to posB".
            Vec3 posA = posB;
            f32 yawA = yawB;
            for (u32 j = 0; j < snapA->entityCount; j++) {
                const SnapEntity& seA = snapA->entities[j];
                if (seA.poolIndex == idx && seA.enemyTypeId == seB.enemyTypeId) {
                    posA.x = Quantize::unpackPos(seA.posX);
                    posA.y = Quantize::unpackPos(seA.posY);
                    posA.z = Quantize::unpackPos(seA.posZ);
                    yawA = Quantize::unpackAngle(seA.yaw);
                    break;
                }
            }
            e.position = posA + (posB - posA) * t;
            e.yaw = lerpAngle(yawA, yawB, t);
        } else {
            e.position = posB;
            e.yaw = yawB;
        }
        // Forward extrapolation when renderTime is past the newest snapshot (single-drop
        // recovery). Position only — yaw has no angular velocity on the wire. Bounded by
        // MAX_EXTRAPOLATE_SEC in computeInterpPair, so overshoot is at most velocity × 50 ms.
        if (extrapolateAhead > 0.0f) {
            e.position = e.position + e.velocity * extrapolateAhead;
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

        // Death timer state machine. Server initializes to 1.0 in Combat::killEntity
        // and ticks it down each frame; the renderer maps deathTimer to scaleY for the
        // squash-into-the-ground collapse (engine_render_entities.cpp:83). deathTimer
        // is NOT on the wire — the previous code unconditionally set it to 0.5 every
        // snapshot tick, freezing the entity halfway through the collapse forever.
        // Mirror the server's lifecycle locally: initialize to 1.0 on the first
        // snapshot we see ENT_DEAD (sentinel: deathTimer was 0 in the alive branch),
        // then tick down at dt. The alive branch clamps to 0 so slot recycle into a
        // new dead entity re-triggers a clean collapse.
        if (e.flags & ENT_DEAD) {
            if (e.deathTimer <= 0.0f) e.deathTimer = 1.0f;
            else                      e.deathTimer = fmaxf(0.01f, e.deathTimer - dt);
        } else {
            e.deathTimer = 0.0f;
        }

        // Attack swing countdown — drives the lunge / weapon-swing pose in renderEntities.
        // Taken from snapB (newest snap) only — no lerp. The animation is brief (0.3 s)
        // and the 30 Hz snapshot rate gives ~33 ms steps, sub-perceptible at that duration.
        // Without this the renderer reads 0 every frame and never shows an attack pose
        // (N4 gated off the local ghost AI that used to tick the timer down).
        e.attackAnimT = seB.attackAnimQ * (1.0f / 255.0f);

        // Procedural animation phase. `animTimer` is a continuous timer the server ticks
        // every frame in EnemyAI::update (enemy_ai.cpp:267), feeding sinf(animTimer * X)
        // for walk bob, arm sway, spin, pulse, glow throughout engine_render_entities.cpp.
        // It is NOT on the wire (saves a byte per entity per snapshot). Tick it locally
        // here so the field actually advances on CLIENT — otherwise every animation that
        // reads animTimer is frozen at sinf(0) = 0 (sliding enemies, motionless bosses).
        // Per-entity phase drift between host and client is irrelevant for procedural
        // animation; we only care that the phase keeps moving forward.
        e.animTimer += dt;
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
    f32 extrapolateAhead = 0.0f;
    if (computeInterpPair(ip)) {
        snapB = ip.newer; snapA = ip.older; t = ip.t;
        extrapolateAhead = ip.extrapolateAhead;
    } else {
        snapB = getSnapshot(0); snapA = nullptr;
    }
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
        // V2 fire prediction: copy the firing client's tick low bits into the projectile
        // so the match-and-despawn pass in clientNetPost can identify the local predicted
        // ghost this snapshot projectile supersedes. We store only the low 16 bits since
        // that's all the wire carries; the despawn pass also masks the ghost's clientTick
        // to 16 bits for comparison.
        p.clientTick = static_cast<u32>(spB.clientTickLow);
        p.predicted  = false;  // snapshot projectiles are authoritative, never predicted
        // D3.1 — Decode server-authoritative damage for use by D3.2's predicted HP decrement
        // on incoming-projectile impact. Inverse of the pack step: × 0.5f restores the original
        // f32 damage with ≤ 0.25 dmg quantization error, acceptable for a pre-snapshot prediction.
        p.damage = spB.expectedDamageQ * 0.5f;

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

        // Forward extrapolation past the newest snapshot — projectiles benefit a lot from
        // this since they fly fast and 50 ms of extrapolation covers a 1-snap gap cleanly.
        // Bounded by MAX_EXTRAPOLATE_SEC in computeInterpPair.
        if (extrapolateAhead > 0.0f) {
            p.position = p.position + p.velocity * extrapolateAhead;
        }

        renderProjectiles.activeCount++;
    }
}

void Client::mirrorWorldItems(WorldItemPool& outItems, const ItemDef* itemDefs, u32 itemDefCount,
                              f32 dt) {
    // (D8) itemDefs is no longer needed — rolled stats (damage, affixes, itemLevel,
    // bonusHealth) ride on the snapshot wire directly so the mirror can reconstruct a
    // fully-populated ItemInstance without consulting the local ItemDef table. Params
    // kept for API stability with the caller in engine_net.cpp.
    (void)itemDefs; (void)itemDefCount;
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
        // D8 — rolled stats from the snapshot. Required so the D4.2 client-side pickup
        // prediction (engine_update.cpp:1148 — `Inventory::addToBackpack(..., wi.item)`)
        // copies authoritative damage/affixes/itemLevel/bonusHealth into the bag instead
        // of the zero-initialised ItemInstance defaults, which is what was showing up as
        // "Damage: 0" in the inventory HUD after a multiplayer client pickup.
        wi.item.damage      = sw.damage;
        wi.item.bonusHealth = sw.bonusHealth;
        wi.item.itemLevel   = sw.itemLevel;
        u8 ac = sw.affixCount;
        if (ac > MAX_AFFIXES_PER_ITEM) ac = MAX_AFFIXES_PER_ITEM;
        wi.item.affixCount  = ac;
        for (u32 a = 0; a < ac; a++) wi.item.affixes[a] = sw.affixes[a];
        for (u32 a = ac; a < MAX_AFFIXES_PER_ITEM; a++) wi.item.affixes[a] = Affix{};
        // (Audit-B) Pickup ownership window is now on the wire — read it instead of
        // hardcoding FFA. Drives the CV-1 client-side pickup gate
        // (engine_update.cpp:853: w.ownerSlot != activeNetSlot() && w.exclusiveTimer > 0)
        // so non-owners can't grab a freshly-credited drop until the timer expires.
        wi.ownerSlot      = sw.ownerSlot;
        wi.exclusiveTimer = sw.exclusiveTimerQ * 0.04f;
        if (!sameItem) wi.bobTimer = 0.0f; // fresh drop in this slot — restart animation
        // bobTimer is incremented every frame on the server in WorldItemSystem::update
        // and consumed by the world-item renderer for the gentle vertical bob and slow
        // spin (engine_render_world.cpp). It's NOT on the wire — tick it locally here
        // so dropped items animate on the client instead of sitting frozen on the floor.
        wi.bobTimer += dt;
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
