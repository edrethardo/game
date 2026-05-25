#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "net/net.h"
#include "net/net_player.h"

// Quantized snapshot of one player (31 bytes)
struct SnapPlayer {
    u8   slotIndex;     // 1
    u8   flags;         // 1: bit0=active, bit1=onGround, bit2=lockActive, bit3=reloading, bit4=blocking, bit5=isDead
    u8   weaponId;      // 1
    u8   health;        // 1: 0-255 mapped to 0-maxHealth
    u16  posX, posY, posZ;  // 6
    u16  velX, velZ;    // 4
    u16  yaw;           // 2
    u16  pitch;         // 2
    u16  lockIndex;     // 2
    // Status/clip sync
    u8   currentClip;   // 1: rounds remaining
    u8   statusFlags;   // 1: bit0=invuln, bit1=poisoned, bit2=burning, bit3=frozen, bit4=slowed
    u8   invulnTimer;   // 1: quantized 0-10s in 0.04s steps
    u8   poisonTimer;   // 1: quantized
    u8   burnTimer;     // 1: quantized
    u8   freezeTimer;   // 1: quantized
    // Animation state for remote player rendering
    u8   animFlags;     // 1: bit0=melee swing, bit1=firing/recoil, bit2=skillActive
    u8   weaponMeshId;  // 1: mesh ID of equipped weapon (for third-person rendering)
    // Wanderer dodge state for remote rendering: bit0=rolling, bits1-3=counterStacks (0-5)
    u8   dodgeFlags;    // 1
};

// Quantized snapshot of one entity (20 bytes)
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
    // Boss status (replaces the old alignment-only padding byte — wire size unchanged).
    // bit0 = minionShield (75% damage reduction active); bits1-3 = bossPhase (BossPhase::,
    // 0-4 fits in 3 bits). Lets clients render the invuln/sealed boss as un-killable.
    u8   bossStatus;    // 1
};

// Quantized snapshot of one projectile (18 bytes)
struct SnapProjectile {
    u16  poolIndex;     // 2: u16 index into the projectile pool (1024 PC / 512 Switch)
    u8   flags;         // 1: bit0=active, bit1=fromPlayer, bit2=isCrit
    u8   projFlags;     // 1: PROJ_ORB/ORB_SHARD/GRAVITY/SPLASH/SPARK/VOID — drives skill VFX
    u8   meshId;        // 1: weapon mesh to render (0 = procedural energy bolt)
    u8   radiusQ;       // 1: radius quantized to 0-2.55 m in 0.01 m steps
    u16  posX, posY, posZ;  // 6
    u16  velX, velY, velZ;  // 6
};

// Full world snapshot
struct WorldSnapshot {
    u32  serverTick       = 0;
    u8   playerCount      = 0;
    u8   entityCount      = 0;
    u16  projectileCount  = 0;  // supports up to 4096

    // Per-player last processed input tick (for client reconciliation)
    u32  lastInputTick[MAX_PLAYERS] = {};

    SnapPlayer     players[MAX_PLAYERS];
    SnapEntity     entities[MAX_ENTITIES];
    // Heap-allocated projectile array — supports full MAX_PROJECTILES without
    // bloating BSS (1024 × 16 bytes = 16KB per snapshot on PC; 512 × 16 = 8KB on Switch).
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
        projectileCount = o.projectileCount;
        for (u32 i = 0; i < MAX_PLAYERS; i++) lastInputTick[i] = o.lastInputTick[i];
        for (u32 i = 0; i < MAX_PLAYERS; i++) players[i] = o.players[i];
        for (u32 i = 0; i < MAX_ENTITIES; i++) entities[i] = o.entities[i];
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
                        const ProjectilePool& projectiles);

    // Serialize snapshot to packet bytes. Returns byte count written.
    u32 serialize(const WorldSnapshot& snap, u8* outData, u32 maxSize);

    // Deserialize snapshot from packet bytes.
    bool deserialize(WorldSnapshot& snap, const u8* data, u32 size);
}
