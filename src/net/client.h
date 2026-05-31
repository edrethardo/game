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
struct Player;  // fwd-decl: captureAndSendInput + reconcile read/write its transform

static constexpr u32 SNAP_BUFFER_SIZE = 4;  // 4 snapshots × ~66KB = 264KB (was 32 × 66KB = 2.1MB)
// 50 ms interpolation delay paired with 30 Hz snapshots gives ~1.5 snapshots of cushion —
// snappy enough to feel responsive against a player who now moves with no reconcile lag,
// while still riding out a single dropped snapshot via extrapolation (computeInterpPair).
// Was 100 ms when snapshots were 20 Hz; halved alongside the rate bump to net.h.
static constexpr f32 INTERP_DELAY_SEC = 0.05f;

namespace Client {
    void init(u8 localPlayerIndex);

    // Capture local input, pack into NetInput, send to server. Reads `player` for the
    // absolute yaw/pitch/position baseline (NetInput now carries absolute values, not
    // mouse deltas — see net_player.h NetInput comment). `skillSlot` is the client's
    // selected class-skill slot (0-3); stamped into the sent input so the server
    // activates the right skill. Does not mutate the player — PlayerController::update
    // applies the same frame's input to `player` later in the tick.
    void captureAndSendInput(const Player& player, u32 clientTick, u8 weaponId, u8 skillSlot);

    // Get the latest captured input (for local prediction in engine)
    const NetInput* getLatestInput();

    // Receive and store a snapshot from the server.
    void receiveSnapshot(const u8* data, u32 size);

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
    bool reconcile(NetPlayer& np, Player& lp);

    // Interpolate remote players between snapshots.
    // Fills outPositions/outYaws (+ active/health/anim/weapon/class) for all MAX_PLAYERS slots.
    // (Remote-player rendering doesn't use pitch — body is yaw-only — so it's omitted.)
    // outPlayerClass receives the SnapPlayer.playerClass byte; the renderer indexes kClassDefs.
    void interpolateRemotePlayers(u8 localSlot,
                                   Vec3* outPositions, f32* outYaws,
                                   bool* outActive, f32* outHealth, f32* outMaxHealth,
                                   u8* outAnimFlags = nullptr, u8* outWeaponMeshId = nullptr,
                                   u8* outPlayerClass = nullptr);

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
}
