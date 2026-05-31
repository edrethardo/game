#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "game/item.h"     // MAX_WORLD_ITEMS, WorldItemPool (world-item replication)
#include "net/net.h"
#include "net/net_player.h"

// Quantized snapshot of one player (30 wire bytes — see SNAP_PLAYER_WIRE)
struct SnapPlayer {
    u8   slotIndex;     // 1
    u8   flags;         // 1: bit0=active, bit1=onGround, bit2=UNUSED, bit3=reloading, bit4=blocking (bits5-7 unused; isDead rides animFlags bit2)
    u8   weaponId;      // 1
    u8   health;        // 1: 0-255 ratio of maxHealth (client reconstructs absolute = ratio*maxHealth)
    u16  maxHealth;     // 2: absolute max HP (raw, rounded) — lets the client reconstruct absolute
                        //    HP and track per-floor growth (R7-4). Capped to u16 range on pack.
    u16  posX, posY, posZ;  // 6
    u16  velX, velZ;    // 4
    u16  yaw;           // 2
    u16  pitch;         // 2
    // Status/clip sync
    u8   currentClip;   // 1: rounds remaining
    u8   statusFlags;   // 1: bit0=invuln, bit1=poisoned, bit2=burning, bit3=frozen, bit4=slowed
    u8   invulnTimer;   // 1: quantized 0-10s in 0.04s steps
    u8   poisonTimer;   // 1: quantized
    u8   burnTimer;     // 1: quantized
    u8   freezeTimer;   // 1: quantized
    // Animation state for remote player rendering
    u8   animFlags;     // 1: bit0=attacking (cooldown active), bit1=reloading, bit2=isDead
    u8   weaponMeshId;  // 1: mesh ID of equipped weapon (for third-person rendering)
    // Wanderer dodge state for remote rendering: bit0=rolling, bits1-3=counterStacks (0-5)
    u8   dodgeFlags;    // 1
    // Visual identity: the chosen PlayerClass (cast to u8). The client's NetPlayer.playerClass
    // is only set on the local slot (CL_JOIN_REQUEST → server's onPlayerJoin), so without this
    // remote players render with the default WARRIOR mesh until the wire carries it. Sent every
    // snapshot rather than once-on-join so a late-joining observer or post-reconnect client
    // converges to the right mesh without a side-channel.
    u8   playerClass;   // 1: PlayerClass cast to u8 (0..CLASS_COUNT-1)
};

// Quantized snapshot of one entity (28 bytes — see SNAP_ENTITY_WIRE)
struct SnapEntity {
    u8   poolIndex;     // 1
    u8   flags;         // 1
    u8   aiState;       // 1
    u8   healthPct;     // 1: 0-255 = 0%-100%
    u16  posX, posY, posZ;  // 6
    u16  yaw;           // 2
    u16  velX, velZ;    // 4
    // New fields
    u8   stunTimer;     // 1: quantized 0-10s
    u8   freezeTimer;   // 1: quantized 0-10s
    u8   bossLimbConfig;// 1
    // Attack animation countdown — Entity.attackAnimT (0.3s pulse, drives ground/bat lunge +
    // skeleton/boss weapon swing in engine_render_entities.cpp). Quantized to 0-1.0s in
    // ~4 ms steps (1/255 s). On CLIENT the ghost AI is gated off after N4, so this is the
    // only path for the client to learn that an enemy swung — without it, attack
    // animations never play on the client (renderer reads attackAnimT == 0 every frame).
    u8   attackAnimQ;   // 1
    // Boss status (replaces the old alignment-only padding byte — wire size unchanged).
    // bit0 = minionShield (75% damage reduction active); bits1-3 = bossPhase (BossPhase::,
    // 0-4 fits in 3 bits). Lets clients render the invuln/sealed boss as un-killable.
    u8   bossStatus;    // 1
    // Visual identity — the client's local entity pool diverges from the server's (the client
    // predicts kills), so its poolIndex can hold a different enemy. Send identity authoritatively
    // so clients render the correct mesh/material/type instead of a default gray cube.
    u8   meshId;        // 1: index into the mesh registry
    u8   materialId;    // 1: index into MaterialSystem
    u8   enemyTypeId;   // 1: EnemyType (cast to u8) — drives mimic/material logic
    u8   weaponMeshId;  // 1: skeleton/boss weapon mesh (0 = none)
    // halfExtents quantized to 0-2.55 m in 0.01 m steps (3 B). Covers every enemy def's
    // collider, including the chonky GENERIC variants. Drives client-side collision +
    // visual scale; without this every non-boss entity uses a default ~0.4×0.5×0.4 box
    // and renders/collides wrong (audit P2 #4). Boss-floor patch in engine_net.cpp is
    // now redundant — left in as a no-op fallback for older snapshots.
    u8   halfExtentsXQ; // 1
    u8   halfExtentsYQ; // 1
    u8   halfExtentsZQ; // 1
};

// Quantized snapshot of one projectile (21 bytes — see SNAP_PROJECTILE_WIRE)
struct SnapProjectile {
    u16  poolIndex;     // 2: u16 index into the projectile pool (1024 PC / 512 Switch)
    u8   flags;         // 1: bit0=active, bit1=fromPlayer, bit2=isCrit
    u8   projFlags;     // 1: PROJ_ORB/ORB_SHARD/GRAVITY/SPLASH/SPARK/VOID — drives skill VFX
    u8   meshId;        // 1: weapon mesh to render (0 = procedural energy bolt)
    u8   radiusQ;       // 1: radius quantized to 0-2.55 m in 0.01 m steps
    u16  posX, posY, posZ;  // 6
    u16  velX, velY, velZ;  // 6
    u8   ownerSlot;     // 1: net slot that fired this projectile (0xFF = unattributed)
    // Low 16 bits of the FIRING CLIENT's m_serverTick at fire moment (carried through
    // CL_FIRE_WEAPON → server-stored on Projectile.clientTick → snapshotted back). Used by
    // the firing client to find and despawn its locally-predicted ghost projectile when the
    // authoritative one arrives. 16 bits suffice — a predicted ghost lives < 0.5 s and the
    // 16-bit tick window is ~18 min at 60 Hz, so collisions are practically impossible.
    // 0 = no prediction owner (host-fired, NPC projectile, skill orb) — match is skipped.
    u16  clientTickLow; // 2
};

// Quantized snapshot of one dropped world item (16 bytes). Loot is server-
// authoritative (N5): only the host/SP rolls drops; clients mirror this list into
// their local m_worldItems for rendering and pickup requests.
struct SnapWorldItem {
    u8   slotIndex;     // 1: index into WorldItemPool (0..MAX_WORLD_ITEMS-1) — client mirrors directly
    u8   rarity;        // 1: Rarity enum value
    u16  defId;         // 2: item definition id (or GLOBE_* sentinel)
    u32  uid;           // 4: unique instance id — pickup requests reference this (full u32; no overflow)
    u16  posX, posY, posZ; // 6: position packed via Quantize::packPos
    u8   ownerSlot;     // 1: exclusive-pickup owner (0xFF = FFA) — drives client pickup gate (Audit-B)
    u8   exclusiveTimerQ; // 1: exclusive window quantized to 0-10.2 s in 0.04 s steps
};

// Full world snapshot
struct WorldSnapshot {
    u32  serverTick       = 0;
    u8   playerCount      = 0;
    u8   entityCount      = 0;
    u8   worldItemCount   = 0;  // dropped loot count (<= MAX_WORLD_ITEMS)
    u16  projectileCount  = 0;  // supports up to 4096

    // Per-player last processed input tick (for client reconciliation)
    u32  lastInputTick[MAX_PLAYERS] = {};

    SnapPlayer     players[MAX_PLAYERS];
    SnapEntity     entities[MAX_ENTITIES];
    SnapWorldItem  worldItems[MAX_WORLD_ITEMS];
    // Heap-allocated projectile array — supports full MAX_PROJECTILES without
    // bloating BSS (1024 × 18 bytes = 18KB per snapshot on PC; 512 × 18 = 9KB on Switch).
    SnapProjectile* projectiles = nullptr;

    WorldSnapshot()  { projectiles = new SnapProjectile[MAX_PROJECTILES](); }
    ~WorldSnapshot() { delete[] projectiles; }

    // Copy/move support (snapshot ring buffer copies these)
    WorldSnapshot(const WorldSnapshot& o) {
        // Allocate our projectile array exactly once, then let operator= copy the
        // fields and projectile contents into it. (Previously this called *this = o
        // first — which itself allocated because projectiles was still null — then
        // allocated a SECOND array here, leaking ~64KB on every snapshot copy.)
        projectiles = new SnapProjectile[MAX_PROJECTILES]();
        *this = o;
    }
    WorldSnapshot& operator=(const WorldSnapshot& o) {
        if (this == &o) return *this;
        serverTick = o.serverTick;
        playerCount = o.playerCount;
        entityCount = o.entityCount;
        worldItemCount = o.worldItemCount;
        projectileCount = o.projectileCount;
        for (u32 i = 0; i < MAX_PLAYERS; i++) lastInputTick[i] = o.lastInputTick[i];
        for (u32 i = 0; i < MAX_PLAYERS; i++) players[i] = o.players[i];
        for (u32 i = 0; i < MAX_ENTITIES; i++) entities[i] = o.entities[i];
        for (u32 i = 0; i < o.worldItemCount; i++) worldItems[i] = o.worldItems[i];
        if (!projectiles) projectiles = new SnapProjectile[MAX_PROJECTILES];
        for (u16 i = 0; i < o.projectileCount; i++) projectiles[i] = o.projectiles[i];
        return *this;
    }
};

namespace Snapshot {
    // Server: build snapshot from current game state
    void buildFromState(WorldSnapshot& snap, u32 tick,
                        const NetPlayer* players,
                        const EntityPool& entities,
                        const ProjectilePool& projectiles,
                        const WorldItemPool& worldItems);

    // Serialize snapshot to packet bytes. Returns byte count written.
    u32 serialize(const WorldSnapshot& snap, u8* outData, u32 maxSize);

    // Deserialize snapshot from packet bytes.
    bool deserialize(WorldSnapshot& snap, const u8* data, u32 size);
}
