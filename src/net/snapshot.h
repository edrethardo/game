#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "net/net.h"
#include "net/net_player.h"

// Quantized snapshot of one player (30 bytes)
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
    u8   padding2;      // 1: alignment
};

// Quantized snapshot of one projectile (14 bytes)
struct SnapProjectile {
    u8   poolIndex;     // 1
    u8   flags;         // 1: bit0=active, bit1=fromPlayer
    u16  posX, posY, posZ;  // 6
    u16  velX, velY, velZ;  // 6
};

// Full world snapshot
struct WorldSnapshot {
    u32  serverTick       = 0;
    u8   playerCount      = 0;
    u8   entityCount      = 0;
    u8   projectileCount  = 0;
    u8   padding          = 0;

    // Per-player last processed input tick (for client reconciliation)
    u32  lastInputTick[MAX_PLAYERS] = {};

    SnapPlayer     players[MAX_PLAYERS];
    SnapEntity     entities[MAX_ENTITIES];
    SnapProjectile projectiles[MAX_PROJECTILES];
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
