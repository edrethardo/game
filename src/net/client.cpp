#include "net/client.h"
#include "net/packet.h"
#include "net/interp_delay.h" // adaptive render-delay helpers (jitter-driven)
#include "game/player.h"
#include "world/collision.h"
#include "platform/clock.h"   // wall-clock for time-based snapshot interpolation
#include "core/log.h"

#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------
static constexpr u32 MAX_LOCAL = 2; // local lanes per client (online couch co-op); == Engine::MAX_LOCAL_PLAYERS
static u8  s_localPlayerIndex = 0;   // lane 0's net slot (kept for getLocalPlayerIndex back-compat)

// Snapshot ring buffer
static WorldSnapshot s_snapshots[SNAP_BUFFER_SIZE];
static f64           s_snapRecvTime[SNAP_BUFFER_SIZE] = {}; // wall-clock arrival time per slot
static u32           s_snapHead  = 0;
static u32           s_snapCount = 0;

// Adaptive interpolation delay (jitter buffer width). s_interpDelaySec starts at the fixed
// base and widens with snapshot-arrival jitter so a late snapshot doesn't freeze remotes;
// see interp_delay.h. Snapshots broadcast every tick (60 Hz) → nominal arrival interval 1/60.
static constexpr f32 SNAP_NOMINAL_INTERVAL_SEC = 1.0f / 60.0f;
// Cap so remotes don't lag absurdly — and the same number the server clamps a reported delay
// to (LagComp::sanitize), so a jittered client can never claim a rewind the server won't honor.
static constexpr f32 MAX_INTERP_DELAY_SEC      = LagComp::MAX_INTERP_DELAY_MS / 1000.0f;
static f32           s_arrivalJitter           = 0.0f;
static f32           s_interpDelaySec          = INTERP_DELAY_SEC;

// Latest input captured this frame — PER LANE (online couch co-op sends one stream per local player).
static NetInput s_latestInput[MAX_LOCAL] = {};
static bool     s_hasInput[MAX_LOCAL]    = {};

// Rolling send window PER LANE: holds the last INPUT_WINDOW_SIZE inputs in oldest→newest order.
// Every CL_INPUT packet carries the full window so a single dropped UDP packet no longer
// loses an input — the next packet still carries it. Three consecutive losses are required
// to actually drop an input.
static NetInput s_sendWindow[MAX_LOCAL][INPUT_WINDOW_SIZE] = {};
static u32      s_sendWindowCount[MAX_LOCAL] = {};

// Set by Client::init (every client floor descend re-enters startGame -> Client::init,
// engine_startgame.cpp). It forces receiveSnapshot to accept the FIRST snapshot after a
// descend unconditionally, closing the reset-gap straddle window: if a late prior-floor
// snapshot lands first it would become "newest", and a short prior floor (tick < RESET_GAP)
// would then trap the real new-floor snapshot (tick ~0) inside RESET_GAP and discard it as
// stale. Accepting the first arrival regardless makes the post-descend newest deterministic.
static bool     s_acceptNextSnapshot = false;

// D7.3v2 — ack-based delta decoding. The server names the baseline TICK inside every delta
// (the snapshot the client last ACKED via NetInput.ackedSnapshotTick), and the client finds
// that snapshot in its own decoded ring. This replaced the "baseline = whatever I decoded
// last" scheme, which required the server's single stored baseline to match EXACTLY — a
// condition that a >1-tick RTT makes permanently false, which is why delta compression had
// never engaged for a real client: the ack was also never stamped (always 0), so every
// snapshot ever sent in production was a full one.
static u32 s_lastDecodedTick = 0;   // newest successfully decoded+applied snapshot tick (0 = none)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static const WorldSnapshot* getSnapshot(u32 ago) {
    if (ago >= s_snapCount) return nullptr;
    u32 idx = (s_snapHead + SNAP_BUFFER_SIZE - 1 - ago) % SNAP_BUFFER_SIZE;
    return &s_snapshots[idx];
}

// Find a decoded snapshot by its serverTick (linear over the ring — SNAP_BUFFER_SIZE is small).
// Used to locate the baseline a delta names; nullptr = it aged out and the delta is undecodable.
static const WorldSnapshot* getSnapshotByTick(u32 tick) {
    for (u32 ago = 0; ago < s_snapCount; ago++) {
        const WorldSnapshot* sn = getSnapshot(ago);
        if (sn && sn->serverTick == tick) return sn;
    }
    return nullptr;
}

static void pushSnapshot(const WorldSnapshot& snap) {
    const f64 now = Clock::getElapsedSeconds();
    // Update arrival jitter from the gap since the previous snapshot, then re-derive the
    // adaptive render delay. Skip on the very first snapshot (no previous arrival).
    if (s_snapCount >= 1) {
        const u32 prevIdx = (s_snapHead + SNAP_BUFFER_SIZE - 1) % SNAP_BUFFER_SIZE;
        const f32 delta = static_cast<f32>(now - s_snapRecvTime[prevIdx]);
        s_arrivalJitter = updateArrivalJitter(s_arrivalJitter, delta, SNAP_NOMINAL_INTERVAL_SEC);
        s_interpDelaySec = computeInterpDelay(s_interpDelaySec, s_arrivalJitter,
                                              INTERP_DELAY_SEC, MAX_INTERP_DELAY_SEC);
    }
    s_snapshots[s_snapHead] = snap;
    s_snapRecvTime[s_snapHead] = now;
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
    // Adaptive delay: widens with arrival jitter (interp_delay.h) so a late snapshot is
    // covered by the buffer instead of forcing extrapolation; falls back to INTERP_DELAY_SEC
    // on a calm link. Updated per arrival in pushSnapshot.
    f64 renderTime = Clock::getElapsedSeconds() - static_cast<f64>(s_interpDelaySec);

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
    s_arrivalJitter  = 0.0f;            // reset adaptive-delay state on (re)init / floor change
    s_interpDelaySec = INTERP_DELAY_SEC;
    // Reset every lane's input stream so stale prior-floor inputs don't bleed into the new floor's
    // first packets (the server's input ring buffer also resets in Server::init).
    for (u32 l = 0; l < MAX_LOCAL; l++) { s_hasInput[l] = false; s_sendWindowCount[l] = 0; }
    s_acceptNextSnapshot = true; // first post-(re)init snapshot is accepted unconditionally (TA-6)
    // D7.3 — Clear the delta baseline so the first snapshot on the new floor is always
    // treated as a full (no stale prior-floor baseline confusing the decoder).
    s_lastDecodedTick = 0;
    LOG_INFO("Client: initialized (local player=%u)", localPlayerIndex);
}

void Client::captureAndSendInput(const Player& player, u32 clientTick, u8 weaponId,
                                 u8 skillSlot, u8 extFlagsClearMask, bool freezeMovement,
                                 u8 laneId, u8 targetSlot) {
    if (laneId >= MAX_LOCAL) laneId = 0;
    NetInput& latest = s_latestInput[laneId];
    latest = PlayerController::captureLocalInput(player, clientTick, weaponId);
    // captureLocalInput defaults skillSlot to 0 ("set by engine before sending"); stamp the
    // engine's selected class-skill slot here so the server activates the chosen skill.
    latest.skillSlot = skillSlot;
    // Freeze: blocking UI open (inventory / pause menu) → don't drive the server from held
    // movement keys. The local sim already isn't moving the player; zero the wire movement so
    // the server-side NetPlayer stays put too (yaw/pitch kept so facing doesn't snap).
    if (freezeMovement) latest.moveFlags = 0;
    // R9 client-side cooldown gate: if the engine determined our local cooldown isn't
    // ready, clear the corresponding INPUT_EX_* bit so we don't even ask the server.
    // Server still gates persistently as a backstop, but this is the "snappy" path —
    // suppress spam at source.
    latest.extFlags &= static_cast<u8>(~extFlagsClearMask);
    // Tell the server which interp delay we used THIS tick. Our local moveAndSlide collided
    // against m_renderInterp.entities sampled at (now - s_interpDelaySec), and s_interpDelaySec
    // widens with jitter — so the server can only reproduce our collision result if it rewinds
    // enemies by this exact amount. Stamping it per-input (not per-session) matters: the delay
    // can change between two inputs in the same send window. See net/lag_comp.h.
    latest.interpDelayMs = LagComp::toWireMs(s_interpDelaySec);
    // Ack the newest snapshot we decoded, low 16 bits (the server reconstructs the full u32
    // against its current tick). THIS was the missing line that kept delta compression dead:
    // NetInput zero-inits, nothing ever stamped the field, so the server saw ack=0 forever and
    // sent full snapshots to every client at 60 Hz for the feature's whole shipped life.
    latest.ackedSnapshotTick = static_cast<u16>(s_lastDecodedTick & 0xFFFF);
    s_hasInput[laneId] = true;

    // Shift oldest out and append this input at the back of THIS lane's window (oldest→newest).
    NetInput* window = s_sendWindow[laneId];
    u32&      wcount = s_sendWindowCount[laneId];
    if (wcount < INPUT_WINDOW_SIZE) {
        window[wcount++] = latest;
    } else {
        for (u32 i = 0; i < INPUT_WINDOW_SIZE - 1; i++) window[i] = window[i + 1];
        window[INPUT_WINDOW_SIZE - 1] = latest;
    }

    // Serialize the full window, stamping `targetSlot` into the window header (byte 1) so the
    // server routes this input to the right one of this peer's local players (couch co-op).
    // Wire: PacketHeader(4) + payload[ windowCount, targetSlot, 0, 0, N×14 B inputs ].
    u8 payload[256];
    u32 payloadSize = serializeInputWindow(payload, sizeof(payload), window, wcount, targetSlot);
    PacketWriter w;
    w.writeU8(static_cast<u8>(NetPacketType::CL_INPUT));
    w.writeU8(0);
    w.writeU16(0);
    for (u32 i = 0; i < payloadSize; i++) w.writeU8(payload[i]);

    Net::sendToServer(w.data, w.cursor, false);
}

const NetInput* Client::getLatestInput(u8 laneId) {
    if (laneId >= MAX_LOCAL) laneId = 0;
    return s_hasInput[laneId] ? &s_latestInput[laneId] : nullptr;
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
    if (!isFullSnap && size > 4 + 25) {
        // Delta path. The wire names its baseline tick (payload offset 21: after serverTick(4) +
        // per-slot acks(16) + isFull(1)); decode is only possible against exactly that snapshot.
        // A miss (aged out of our ring after heavy loss) is not an error — drop the delta and
        // wait: our ack stops advancing, so the server keeps delta-ing against ever-older
        // baselines until ITS history misses too and it falls back to a full. Self-healing.
        u32 wireBaseTick;
        std::memcpy(&wireBaseTick, data + 4 + 21, 4);
        const WorldSnapshot* base = getSnapshotByTick(wireBaseTick);
        if (!base) {
            LOG_WARN("[D7.3] delta names baseline tick %u — not in ring, dropping (awaiting full)",
                     wireBaseTick);
            return;
        }
        decoded = Snapshot::deserializeDelta(snap, data + 4, size - 4, *base);
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
        if ((s_snapRxLogCounter++ % 1800) == 0) {
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
        if ((s_clockSyncLogCounter++ % 1800) == 0) {   // ~once/30s — was ~1 Hz, drowned the log
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
        s_lastDecodedTick = snap.serverTick;   // what we'll ack on the next input
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
bool Client::reconcile(NetPlayer& np, Player& lp, u8 localSlot) {
    const WorldSnapshot* snap = getSnapshot(0);
    if (!snap) return false; // no snapshot yet — keep pure local state

    // Find this lane's player in the snapshot (localSlot = this local lane's net slot — couch co-op
    // reconciles each lane against its own slot).
    const SnapPlayer* serverState = nullptr;
    for (u32 i = 0; i < snap->playerCount; i++) {
        if (snap->players[i].slotIndex == localSlot) {
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
        // Ammo + reload are CLIENT-AUTHORITATIVE. The client's own handleWeaponFire owns
        // currentClip / reloading / reloadTimer (predict the decrement on fire, run the reload
        // timer locally, trigger the throwaway) — the player holding the weapon needs it instant
        // and correct. We deliberately DON'T adopt the server's clip here anymore: the old
        // conservative adopt raised the client's just-decremented clip back to the server's
        // RTT/2-stale (higher) value nearly every snapshot, so the local decrements never stuck,
        // the clip never reached 0, and "the client never had to reload". The server also no
        // longer gates the client's fire on its own reload state (engine_combat.cpp), so it can't
        // drop an authoritative shot at a reload boundary. Nothing overwrites the local clip now,
        // so there's also no HUD flicker (the symptom the old conservative path guarded against).
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
        // CC (PvP): adopt the authoritative stun (same 25/s quantization as the timers above). The
        // client can't self-stun, so the server is the sole source — direct assign. This flows to
        // m_localPlayer.stunTimer via syncNetPlayerToLocalPlayer (beside freezeTimer), and the input
        // path locks the same frame → the stun feels instant, not a round-trip late (RL-feel).
        np.stunTimer   = serverState->stunTimerQ / 25.0f;
        // slowTimer has only a status FLAG on the wire (bit4); keep a short slow alive
        // while the flag is set so the speed debuff is visible client-side.
        if (serverState->statusFlags & (1 << 4)) {
            if (np.slowTimer < 0.1f) np.slowTimer = 0.2f;
        } else {
            np.slowTimer = 0.0f;
        }
    }

    // Loose-threshold safety snap for genuine TELEPORTS only (respawn, floor door).
    //
    // This comparison is time-skewed by construction: lp.position is the client's CURRENT
    // predicted position, serverPos is the server's ACKED (~RTT-old) state, so their diff is
    // dominated by legitimate in-flight movement — about speed x RTT, which at run speed
    // (~6.7 m/s) crosses 1 m from ~150 ms of effective RTT. The old 1 m threshold therefore
    // false-fired on perfectly healthy fast-moving clients (measured: 8 spurious hard snaps in
    // a 90 s soak at 15% loss + 100 ms RTT — one rubber-band every 11 seconds from nothing).
    //
    // Everything below 5 m is now owned by the time-ALIGNED reconcile (prediction ring @ acked
    // tick + rollback-replay in engine_net.cpp), which corrects real errors — including
    // knockback and anti-cheat clamps — without eating in-flight inputs. 5 m of in-flight
    // movement would need ~750 ms of RTT; every real teleport (respawn, floor door) is tens of
    // metres. Matches the replay path's own teleport guard, which resets the ring at the same 5 m.
    Vec3 serverPos{
        Quantize::unpackPos(serverState->posX),
        Quantize::unpackPos(serverState->posY),
        Quantize::unpackPos(serverState->posZ),
    };
    Vec3 diff = lp.position - serverPos;
    if (lengthSq(diff) > 5.0f * 5.0f) {
        lp.position = serverPos;
        lp.velocity = {0.0f, 0.0f, 0.0f}; // teleport zeros momentum
        np.position = serverPos;          // keep NetPlayer mirror in sync for snapshot ack
        np.velocity = {0.0f, 0.0f, 0.0f};
        LOG_INFO("Client: reconcile teleport snap (server-driven, dist=%.2f m)",
                 sqrtf(lengthSq(diff)));
    }
    return true;
}

void Client::interpolateRemotePlayers(u8 localSlot0, u8 localSlot1,
                                       Vec3* outPositions, f32* outYaws,
                                       bool* outActive, f32* outHealth, f32* outMaxHealth,
                                       u8* outAnimFlags, u8* outWeaponMeshId,
                                       u8* outPlayerClass, u8 (*outArmorMeshId)[4],
                                       u8* outDodgeFlags)
{
    // localSlot0/localSlot1 are this client's local lanes' net slots (couch co-op has two; the 2nd
    // is 0xFF for a single client). Both are skipped — local players render from their own predicted
    // m_localPlayers[], not from the delayed interp snapshot.
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
        if (outArmorMeshId) for (int k = 0; k < 4; ++k) outArmorMeshId[i][k] = 0;
        if (outDodgeFlags) outDodgeFlags[i] = 0;
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
            if (slot == localSlot0 || slot == localSlot1) continue;
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
            if (outArmorMeshId) for (int k = 0; k < 4; ++k) outArmorMeshId[slot][k] = sp.armorMeshId[k];
            if (outPlayerClass) outPlayerClass[slot] = sp.playerClass;
            // Roll bit is a boolean state (not lerp-able) — take it straight from the snapshot so
            // the remote body tumbles for the roll's duration. Only dodgeFlags bit0 is read here.
            if (outDodgeFlags) outDodgeFlags[slot] = sp.dodgeFlags;
        }
        return;
    }

    // For each player in snapB (newer endpoint), find its match in snapA (older) and lerp
    for (u32 i = 0; i < snapB->playerCount; i++) {
        const SnapPlayer& spB = snapB->players[i];
        u8 slot = spB.slotIndex;
        if (slot >= MAX_PLAYERS) continue; // guard malformed/corrupt snapshot -> OOB write
        if (slot == localSlot0 || slot == localSlot1) continue;
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
        // Armor mesh ids are identity fields (not lerp-able) — take from the newer snapshot.
        if (outArmorMeshId) for (int k = 0; k < 4; ++k) outArmorMeshId[slot][k] = spB.armorMeshId[k];
        // PlayerClass is a static identity, not a lerp-able value — take it straight from
        // the newer snapshot. (Index is already clamped to PlayerClass::CLASS_COUNT in deserialize.)
        if (outPlayerClass) outPlayerClass[slot] = spB.playerClass;
        // Roll bit is a boolean state (not lerp-able) — take it from the newer snapshot so the
        // remote body tumbles for the roll's duration. Only dodgeFlags bit0 is read by the renderer.
        if (outDodgeFlags) outDodgeFlags[slot] = spB.dodgeFlags;
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
        // Slot-recycle speech guard: speechText/speechTimer are CLIENT-planted (SV_EVENT::SPEECH
        // parks the line on this interp slot) and are NOT snapshot fields, so they survive this
        // rebuild — which is correct while the slot still holds the same monster, and wrong the
        // moment the server recycles the pool index for a different one (the new monster would
        // inherit the old one's bubble: "old and wrong speech bubbles" on guests). The pool
        // persists across frames, so e.* still holds LAST frame's identity here — compare it to
        // the incoming snapshot identity and drop the bubble on any change.
        if (e.enemyType != static_cast<EnemyType>(seB.enemyTypeId) ||
            e.enemyDefIdx != seB.enemyDefIdx || e.meshId != seB.meshId) {
            e.speechText  = nullptr;
            e.speechTimer = 0.0f;
        }
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
        // R9: mirror the boss-status bits so floorBossAlive() on the client (reading
        // this render pool) can identify the milestone boss and drive the portal-locked
        // / portal-unlocked color in engine_render_effects.cpp.
        e.isBoss       = (seB.bossStatus & (1u << 4)) != 0;
        e.minionShield = (seB.bossStatus & 0x01) != 0;
        e.bossPhase    = static_cast<u8>((seB.bossStatus >> 1) & 0x07);
        // Champion affixes. The ENT_CHAMPION bit already arrived inside `flags` (copied verbatim),
        // but the renderer needs the MASK to pick the tint — Champion::tintFor is pure, so host and
        // guest derive the same colour from the same byte and cannot disagree.
        e.champAffixes = seB.champAffixes;
        e.champNameIdx = seB.champNameIdx;
        e.enemyDefIdx  = seB.enemyDefIdx;
        // Boss identity: lets the nameplate resolve the real boss name from the local BossDef
        // table (Entity.nameTag is a host-side pointer and can't replicate).
        e.bossDefIdx   = seB.bossDefIdx;

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

        // Enemy HP as a RATIO carrier: health/maxHealth == healthPct/255 exactly, both freshly
        // written from THIS snapshot every frame (the old stale-slot hazard came from reusing a
        // previous occupant's maxHealth — a constant denominator can't go stale). Absolute enemy
        // HP still isn't wired and these are NOT authoritative stats (N5): they exist for the
        // render-side consumers that read a fraction or an alive-check from this pool — the
        // target bar (name + health, engine_hud.cpp), the goblin portal effect and the ambient
        // monster-cry picker. All three were dead on guests while these were zeroed: the bar
        // early-returns on maxHealth<=0, the other two skip health<=0.
        e.maxHealth = 255.0f;
        e.health    = static_cast<f32>(seB.healthPct);
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
        // R11: drive the visual rotation parameter on the client. The renderer's
        // staff / wand / thrown-weapon / orb paths all use t = p.lifetime as a
        // continuously-varying angle source (rotateY(t * 15.0f), etc.). On the host,
        // p.lifetime is the projectile's TTL — it decreases each frame as the
        // projectile ages, which is "monotonic" enough to drive rotation. On the
        // client, interpolateProjectiles never touched p.lifetime, so it stayed at
        // its default 0 → every projectile rendered at identity rotation (no spin).
        // Use absolute elapsed time here: any monotonic value advances rotation
        // uniformly, and Clock::getElapsedSeconds() is already used in this file
        // for snapshot timing. All projectiles in the same frame share a value,
        // which is fine visually since they're at different positions.
        p.lifetime   = static_cast<f32>(Clock::getElapsedSeconds());
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

f32 Client::getInterpDelaySec() {
    // Live adaptive delay (widens with arrival jitter in pushSnapshot). See the header
    // comment — the [NET-GRAPH] shaky-FOV diagnostic reads this to correlate camera shake
    // with the client/server obstacle-sampling-time mismatch under jitter.
    return s_interpDelaySec;
}
