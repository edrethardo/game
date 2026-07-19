#pragma once

#include "core/types.h"
#include "core/math.h"
#include "net/net.h"
#include "net/clock_sync.h"
#include "net/lag_comp.h"     // canonical interp-delay bounds (shared with the server's rewind)
#include "net/net_player.h"
#include "net/snapshot.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "game/item.h"        // WorldItemPool / ItemDef for world-item mirroring
#include "game/weapon.h"
#include "world/level_grid.h"
struct Player;  // fwd-decl: captureAndSendInput + reconcile read/write its transform

// 32 deep × sizeof(WorldSnapshot)=8.2KB = ~264KB. Sized by its two consumers:
//   * the ADAPTIVE interp delay may widen to 150 ms under jitter (lag_comp.h) — the old depth of
//     4 held ~50 ms, so the bracketing pair had already been evicted exactly on the links that
//     needed the cushion, and interpolation degenerated to raw snapshot stepping;
//   * delta decoding needs the snapshot the server's delta NAMES as baseline — that's the one we
//     acked ~RTT ago, i.e. ~10 pushes deep at 166 ms, and arrival batching slides it deeper. A
//     16-deep ring measurably missed in bursts on a clean 100 ms link (ack stalls on a miss, so
//     one miss repeats until the server falls back to a full). 32 matches the server's
//     SNAP_HISTORY_DEPTH: both sides hold ~533 ms, so any baseline the server can name, the
//     client can still decode. (The old "~66KB per snapshot" here was stale by 8x — 8,248 B.)
static constexpr u32 SNAP_BUFFER_SIZE = 32;
// 33 ms interpolation delay paired with 60 Hz snapshots gives ~2 snapshots of cushion —
// snappy enough to feel responsive while still riding out a single dropped snapshot via
// extrapolation (computeInterpPair). Was 50 ms when snapshots were 30 Hz; reduced alongside
// the rate bump to net.h so the render stays 2 × interval behind the latest sample.
// Derived from the shared client/server contract in lag_comp.h rather than declared here:
// the server rewinds enemies by the delay the client reports, so the two must be the same
// number by construction, not by two constants that happen to agree today.
static constexpr f32 INTERP_DELAY_SEC = LagComp::DEFAULT_INTERP_DELAY_MS / 1000.0f;

namespace Client {
    void init(u8 localPlayerIndex);

    // Capture local input, pack into NetInput, send to server. Reads `player` for the
    // absolute yaw/pitch/position baseline (NetInput now carries absolute values, not
    // mouse deltas — see net_player.h NetInput comment). `skillSlot` is the client's
    // selected class-skill slot (0-3); stamped into the sent input so the server
    // activates the right skill. Does not mutate the player — PlayerController::update
    // applies the same frame's input to `player` later in the tick.
    //
    // R9: `extFlagsClearMask` clears those bits on the just-captured input's extFlags
    // before the packet is sent. The engine sets each bit when the corresponding local
    // cooldown isn't ready, so the client doesn't bother the server with a request it
    // knows would be denied. The server still gates authoritatively (engine_net.cpp's
    // remote-skill handlers run tryActivate against persistent SkillState), but this
    // trims the spam at the source — "the client knows when the skills have cooldown
    // best". Pass 0 to disable (e.g. host path, where remote inputs aren't sent).
    // `freezeMovement` zeroes the sent input's moveFlags (W/S/A/D/jump/fire/lock) so the
    // server holds the player still while a blocking UI is open (inventory / pause menu) —
    // the local sim already skips PlayerController::update, and without this the server would
    // still walk the player from any held key, causing a reconcile rubber-band. Aim
    // (yaw/pitch) is left intact so the body facing doesn't snap.
    // laneId selects which local player's input stream this is (online couch co-op); targetSlot is
    // the absolute net slot the server should route it to (stamped into the CL_INPUT window header).
    void captureAndSendInput(const Player& player, u32 clientTick, u8 weaponId,
                             u8 skillSlot, u8 extFlagsClearMask = 0,
                             bool freezeMovement = false,
                             u8 laneId = 0, u8 targetSlot = 0);

    // Get the latest captured input for a local lane (for local prediction in engine)
    const NetInput* getLatestInput(u8 laneId = 0);

    // Receive and store a snapshot from the server. Also feeds snap.serverTick into
    // ClockSyncOps::onSnapshotReceived (P controller, gain 0.1) so the clock estimate
    // is refined on every successful deserialize.
    void receiveSnapshot(const u8* data, u32 size, ClockSync& cs);

    // Get the latest received snapshot (may be null if none yet)
    const WorldSnapshot* getLatestSnapshot();

    // Reconcile: pull server-authoritative HP, status timers, death, ammo, and reload
    // state from the newest snapshot into `np` (NetPlayer mirror — flows back into the
    // snapshot chain) and `lp` (m_localPlayer — what the camera/HUD reads). A loose
    // 1 m safety snap also runs: if the server has teleported the player (knockback,
    // respawn, anti-cheat clamp), pull the local camera/position along; otherwise
    // position is purely client-driven by PlayerController::update. No prediction
    // history, no replay, no per-tick snapping — the trust-client position model makes
    // those unnecessary and they fought with PlayerController::update's mouse + WASD.
    // Returns true if any field was adopted from the snapshot.
    // localSlot = this lane's net slot (couch co-op reconciles each lane against its own slot).
    bool reconcile(NetPlayer& np, Player& lp, u8 localSlot);

    // Interpolate remote players between snapshots.
    // Fills outPositions/outYaws (+ active/health/anim/weapon/class) for all MAX_PLAYERS slots.
    // (Remote-player rendering doesn't use pitch — body is yaw-only — so it's omitted.)
    // outPlayerClass receives the SnapPlayer.playerClass byte; the renderer indexes kClassDefs.
    // localSlot0/localSlot1 are this client's local lanes' net slots (both skipped — drawn from
    // their own predicted state). localSlot1 = 0xFF for a single (non-couch) client.
    void interpolateRemotePlayers(u8 localSlot0, u8 localSlot1,
                                   Vec3* outPositions, f32* outYaws,
                                   bool* outActive, f32* outHealth, f32* outMaxHealth,
                                   u8* outAnimFlags = nullptr, u8* outWeaponMeshId = nullptr,
                                   u8* outPlayerClass = nullptr,
                                   u8 (*outArmorMeshId)[4] = nullptr,
                                   u8* outDodgeFlags = nullptr,  // bit0=rolling — drives remote roll-tumble render
                                   Vec3* outVelXZ = nullptr,     // wire velocity XZ — recovers the roll tumble direction
                                   bool* outOnGround = nullptr); // flags bit1=onGround — grounded gate for takeoff dust

    // Interpolate entities from snapshots into a render-only pool. `dt` is the
    // frame delta time; used to tick each entity's procedural `animTimer` locally
    // because that field isn't on the wire and the renderer drives nearly every
    // enemy animation (walk bob, arm sway, spin, pulse) off `sinf(animTimer * X)`.
    // Without the local tick, animTimer stays at 0.0f and every CLIENT-side enemy
    // animation is frozen.
    void interpolateEntities(EntityPool& renderEntities, f32 dt);

    // Interpolate projectiles from snapshots into a render-only pool.
    void interpolateProjectiles(ProjectilePool& renderProjectiles);

    // Mirror the server-authoritative world-item list (loot drops) from the newest
    // snapshot into the client's local pool. Items are static so a direct copy is fine
    // (no interpolation). The renderer and pickup-aim code read m_worldItems directly,
    // so populating it here makes loot appear/disappear in lockstep with the server.
    // `dt` is the frame delta — used to tick each item's procedural `bobTimer` locally,
    // because that field isn't on the wire and the renderer drives the bob/spin
    // animation off `sinf(bobTimer * X)` / `bobTimer * spinRate`. Without the local
    // tick, dropped items sit motionless on the floor on the client.
    void mirrorWorldItems(WorldItemPool& outItems, const ItemDef* itemDefs, u32 itemDefCount,
                          f32 dt);

    // Get local player index
    u8 getLocalPlayerIndex();

    // Current adaptive interpolation delay (seconds). Starts at INTERP_DELAY_SEC and widens
    // with snapshot-arrival jitter (see interp_delay.h). Exposed for the [NET-GRAPH]
    // shaky-FOV diagnostic: the client samples enemy collision obstacles at THIS delay, while
    // the server rewinds a FIXED 2 ticks — so a widened value means a bigger client/server
    // obstacle-time mismatch and thus more prediction divergence (the shake). Read-only.
    f32 getInterpDelaySec();

    // Decode a SV_TIME_PONG payload and feed it to ClockSync. Caller supplies the
    // wall-clock time of the pong arrival (Clock::getElapsedSeconds at decode time).
    // Short payloads are logged and ignored (cs unchanged).
    void handleTimePong(const u8* data, u32 size, ClockSync& cs, f64 pongRecvNowSec);
}
