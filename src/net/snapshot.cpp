#include "net/snapshot.h"
#include "net/packet.h"

void Snapshot::buildFromState(WorldSnapshot& snap, u32 tick,
                               const NetPlayer* players,
                               const EntityPool& entities,
                               const ProjectilePool& projectiles)
{
    snap.serverTick = tick;
    snap.playerCount = 0;
    snap.entityCount = 0;
    snap.projectileCount = 0;

    // Players
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        const NetPlayer& np = players[i];
        snap.lastInputTick[i] = np.lastProcessedInputTick;

        if (!np.active) continue;

        SnapPlayer& sp = snap.players[snap.playerCount++];
        sp.slotIndex = static_cast<u8>(i);

        u8 flags = 0;
        flags |= 1;  // active
        if (np.onGround)                flags |= (1 << 1);
        if (np.lockActive)              flags |= (1 << 2);
        if (np.weaponState.reloading)   flags |= (1 << 3);
        if (np.blocking)                flags |= (1 << 4);
        sp.flags = flags;

        sp.weaponId = np.weaponState.currentWeapon;
        // Guard divide-by-zero (mirrors the entity path) and clamp the ratio so a
        // transient overheal or maxHealth==0 can't NaN or wrap the u8.
        f32 hpFrac = (np.maxHealth > 0.0f) ? (np.health / np.maxHealth) : 0.0f;
        if (hpFrac < 0.0f) hpFrac = 0.0f;
        if (hpFrac > 1.0f) hpFrac = 1.0f;
        sp.health   = static_cast<u8>(hpFrac * 255.0f);

        sp.posX = Quantize::packPos(np.position.x);
        sp.posY = Quantize::packPos(np.position.y);
        sp.posZ = Quantize::packPos(np.position.z);
        sp.velX = Quantize::packVel(np.velocity.x);
        sp.velZ = Quantize::packVel(np.velocity.z);
        sp.yaw   = Quantize::packAngle(np.yaw);
        sp.pitch = Quantize::packAngle(np.pitch);
        sp.lockIndex = np.lockIndex;

        // Status effects + clip (new fields)
        sp.currentClip = np.weaponState.currentClip;
        u8 sf = 0;
        if (np.invulnTimer > 0.0f) sf |= (1 << 0);
        if (np.poisonTimer > 0.0f) sf |= (1 << 1);
        if (np.burnTimer > 0.0f)   sf |= (1 << 2);
        if (np.freezeTimer > 0.0f) sf |= (1 << 3);
        if (np.slowTimer > 0.0f)   sf |= (1 << 4);
        sp.statusFlags  = sf;
        sp.invulnTimer  = static_cast<u8>(np.invulnTimer * 25.0f);  // 0-10s in 0.04s steps
        sp.poisonTimer  = static_cast<u8>(np.poisonTimer * 25.0f);
        sp.burnTimer    = static_cast<u8>(np.burnTimer * 25.0f);
        sp.freezeTimer  = static_cast<u8>(np.freezeTimer * 25.0f);

        // Animation state for remote player rendering
        u8 anim = 0;
        if (np.weaponState.cooldownTimer > 0.0f) anim |= (1 << 0); // attacking/fired recently
        if (np.weaponState.reloading)             anim |= (1 << 1); // reload animation
        if (np.isDead)                             anim |= (1 << 2); // dead
        sp.animFlags = anim;
        sp.weaponMeshId = np.weaponState.currentWeapon; // weapon index for mesh lookup

        // Dodge state: server doesn't track full DodgeState on NetPlayer yet,
        // so we zero-out; clients use local prediction for the rolling player.
        sp.dodgeFlags = 0;
    }

    // Entities (only active ones)
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& e = entities.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;

        SnapEntity& se = snap.entities[snap.entityCount++];
        se.poolIndex = static_cast<u8>(i);
        se.flags     = e.flags;
        se.aiState   = static_cast<u8>(e.aiState);
        se.healthPct = (e.maxHealth > 0.0f)
            ? static_cast<u8>((e.health / e.maxHealth) * 255.0f)
            : 0;

        se.posX = Quantize::packPos(e.position.x);
        se.posY = Quantize::packPos(e.position.y);
        se.posZ = Quantize::packPos(e.position.z);
        se.yaw  = Quantize::packAngle(e.yaw);
        se.velX = Quantize::packVel(e.velocity.x);
        se.velZ = Quantize::packVel(e.velocity.z);
        se.stunTimer     = static_cast<u8>(e.stunTimer * 25.0f);
        se.freezeTimer   = static_cast<u8>(e.freezeTimer * 25.0f);
        se.bossLimbConfig = e.bossLimbConfig;
        se.padding2      = 0;
    }

    // Projectiles (only active ones)
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        const Projectile& p = projectiles.projectiles[i];
        if (!p.active) continue;
        if (snap.projectileCount >= MAX_PROJECTILES) break;

        SnapProjectile& sp = snap.projectiles[snap.projectileCount++];
        sp.poolIndex = static_cast<u16>(i);
        u8 flags = 1; // active
        if (p.fromPlayer) flags |= (1 << 1);
        sp.flags = flags;

        sp.posX = Quantize::packPos(p.position.x);
        sp.posY = Quantize::packPos(p.position.y);
        sp.posZ = Quantize::packPos(p.position.z);
        sp.velX = Quantize::packVel(p.velocity.x);
        sp.velY = Quantize::packVel(p.velocity.y);
        sp.velZ = Quantize::packVel(p.velocity.z);
    }
}

u32 Snapshot::serialize(const WorldSnapshot& snap, u8* outData, u32 maxSize) {
    PacketWriter w;
    // Header
    w.writeU8(static_cast<u8>(NetPacketType::SV_SNAPSHOT));
    w.writeU8(0); // flags
    w.writeU16(0); // seq (filled by caller)
    // Snapshot header
    w.writeU32(snap.serverTick);
    w.writeU8(snap.playerCount);
    w.writeU8(snap.entityCount);
    w.writeU16(snap.projectileCount);

    // Last input ticks per player
    for (u32 i = 0; i < MAX_PLAYERS; i++)
        w.writeU32(snap.lastInputTick[i]);

    // Players
    for (u32 i = 0; i < snap.playerCount; i++) {
        const SnapPlayer& sp = snap.players[i];
        w.writeU8(sp.slotIndex);
        w.writeU8(sp.flags);
        w.writeU8(sp.weaponId);
        w.writeU8(sp.health);
        w.writeU16(sp.posX);
        w.writeU16(sp.posY);
        w.writeU16(sp.posZ);
        w.writeU16(sp.velX);
        w.writeU16(sp.velZ);
        w.writeU16(sp.yaw);
        w.writeU16(sp.pitch);
        w.writeU16(sp.lockIndex);
        w.writeU8(sp.currentClip);
        w.writeU8(sp.statusFlags);
        w.writeU8(sp.invulnTimer);
        w.writeU8(sp.poisonTimer);
        w.writeU8(sp.burnTimer);
        w.writeU8(sp.freezeTimer);
        w.writeU8(sp.animFlags);
        w.writeU8(sp.weaponMeshId);
        w.writeU8(sp.dodgeFlags);  // Wanderer dodge state (bit0=rolling, bits1-3=counterStacks)
    }

    // Entities
    for (u32 i = 0; i < snap.entityCount; i++) {
        const SnapEntity& se = snap.entities[i];
        w.writeU8(se.poolIndex);
        w.writeU8(se.flags);
        w.writeU8(se.aiState);
        w.writeU8(se.healthPct);
        w.writeU16(se.posX);
        w.writeU16(se.posY);
        w.writeU16(se.posZ);
        w.writeU16(se.yaw);
        w.writeU16(se.velX);
        w.writeU16(se.velZ);
        w.writeU8(se.stunTimer);
        w.writeU8(se.freezeTimer);
        w.writeU8(se.bossLimbConfig);
        w.writeU8(se.padding2);
    }

    // Projectiles
    for (u32 i = 0; i < snap.projectileCount; i++) {
        const SnapProjectile& sp = snap.projectiles[i];
        w.writeU16(sp.poolIndex);
        w.writeU8(sp.flags);
        w.writeU16(sp.posX);
        w.writeU16(sp.posY);
        w.writeU16(sp.posZ);
        w.writeU16(sp.velX);
        w.writeU16(sp.velY);
        w.writeU16(sp.velZ);
    }

    u32 written = w.cursor;
    if (written <= maxSize) {
        std::memcpy(outData, w.data, written);
    }
    return written;
}

bool Snapshot::deserialize(WorldSnapshot& snap, const u8* data, u32 size) {
    PacketReader r;
    r.data = data;
    r.size = size;
    r.cursor = 0;

    // Skip packet header (4 bytes)
    r.readU8(); r.readU8(); r.readU16();

    snap.serverTick      = r.readU32();
    snap.playerCount     = r.readU8();
    snap.entityCount     = r.readU8();
    snap.projectileCount = r.readU16();

    // Validate counts to prevent out-of-bounds on malformed packets
    if (snap.playerCount > MAX_PLAYERS) snap.playerCount = MAX_PLAYERS;
    if (snap.entityCount > MAX_ENTITIES) snap.entityCount = MAX_ENTITIES;
    if (snap.projectileCount > MAX_PROJECTILES)
        snap.projectileCount = MAX_PROJECTILES;

    for (u32 i = 0; i < MAX_PLAYERS; i++)
        snap.lastInputTick[i] = r.readU32();

    for (u32 i = 0; i < snap.playerCount; i++) {
        SnapPlayer& sp = snap.players[i];
        sp.slotIndex = r.readU8();
        sp.flags     = r.readU8();
        sp.weaponId  = r.readU8();
        sp.health    = r.readU8();
        sp.posX      = r.readU16();
        sp.posY      = r.readU16();
        sp.posZ      = r.readU16();
        sp.velX      = r.readU16();
        sp.velZ      = r.readU16();
        sp.yaw       = r.readU16();
        sp.pitch     = r.readU16();
        sp.lockIndex = r.readU16();
        sp.currentClip  = r.readU8();
        sp.statusFlags  = r.readU8();
        sp.invulnTimer  = r.readU8();
        sp.poisonTimer  = r.readU8();
        sp.burnTimer    = r.readU8();
        sp.freezeTimer  = r.readU8();
        sp.animFlags    = r.readU8();
        sp.weaponMeshId = r.readU8();
        sp.dodgeFlags   = r.readU8();  // bit0=rolling, bits1-3=counterStacks
    }

    for (u32 i = 0; i < snap.entityCount; i++) {
        SnapEntity& se = snap.entities[i];
        se.poolIndex = r.readU8();
        se.flags     = r.readU8();
        se.aiState   = r.readU8();
        se.healthPct = r.readU8();
        se.posX      = r.readU16();
        se.posY      = r.readU16();
        se.posZ      = r.readU16();
        se.yaw       = r.readU16();
        se.velX      = r.readU16();
        se.velZ      = r.readU16();
        se.stunTimer     = r.readU8();
        se.freezeTimer   = r.readU8();
        se.bossLimbConfig = r.readU8();
        se.padding2      = r.readU8();
    }

    for (u32 i = 0; i < snap.projectileCount; i++) {
        SnapProjectile& sp = snap.projectiles[i];
        sp.poolIndex = r.readU16();
        sp.flags     = r.readU8();
        sp.posX      = r.readU16();
        sp.posY      = r.readU16();
        sp.posZ      = r.readU16();
        sp.velX      = r.readU16();
        sp.velY      = r.readU16();
        sp.velZ      = r.readU16();
    }

    return r.cursor <= r.size;
}
