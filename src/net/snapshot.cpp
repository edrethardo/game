#include "net/snapshot.h"
#include "net/packet.h"

#include <algorithm>

// Squared distance from `p` to the nearest active player. Used to order snapshot
// records nearest-the-action-first so that, if the serializer has to priority-drop
// records to fit the wire budget, it keeps the ones most relevant to the players.
static f32 nearestPlayerDistSq(Vec3 p, const NetPlayer* players) {
    f32 best = 1e30f;
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].active) continue;
        Vec3 d = players[i].position - p;
        f32 dsq = d.x * d.x + d.y * d.y + d.z * d.z;
        if (dsq < best) best = dsq;
    }
    return best;
}

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

    // Entities (only active ones). entKeys[i] = squared distance of entity i to the
    // nearest player, so we can sort nearest-first below for priority-safe dropping.
    static f32 entKeys[MAX_ENTITIES];
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        const Entity& e = entities.entities[i];
        if (!(e.flags & ENT_ACTIVE)) continue;

        u32 idx = snap.entityCount++;
        SnapEntity& se = snap.entities[idx];
        entKeys[idx] = nearestPlayerDistSq(e.position, players);
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
        // Pack boss invuln/shield state: bit0=minionShield, bits1-3=bossPhase (0-4).
        se.bossStatus    = (e.minionShield ? 0x01 : 0x00)
                         | static_cast<u8>((e.bossPhase & 0x07) << 1);
    }

    // Projectiles (only active ones). projKeys mirrors entKeys for distance ordering.
    static f32 projKeys[MAX_PROJECTILES];
    for (u32 i = 0; i < MAX_PROJECTILES; i++) {
        const Projectile& p = projectiles.projectiles[i];
        if (!p.active) continue;
        if (snap.projectileCount >= MAX_PROJECTILES) break;

        u32 idx = snap.projectileCount++;
        SnapProjectile& sp = snap.projectiles[idx];
        projKeys[idx] = nearestPlayerDistSq(p.position, players);
        sp.poolIndex = static_cast<u16>(i);
        u8 flags = 1; // active
        if (p.fromPlayer) flags |= (1 << 1);
        if (p.isCrit)     flags |= (1 << 2);
        sp.flags = flags;

        // Visual fields so skill/boss projectiles render correctly on clients
        // (without these every projectile fell through to the default energy bolt).
        sp.projFlags = p.projFlags;
        sp.meshId    = p.meshId;
        // radius: 0-2.55 m in 0.01 m steps (covers orbs up to ~2.5 m; clamp the rest).
        f32 rq = p.radius * 100.0f;
        if (rq < 0.0f)   rq = 0.0f;
        if (rq > 255.0f) rq = 255.0f;
        sp.radiusQ = static_cast<u8>(rq);

        sp.posX = Quantize::packPos(p.position.x);
        sp.posY = Quantize::packPos(p.position.y);
        sp.posZ = Quantize::packPos(p.position.z);
        sp.velX = Quantize::packVel(p.velocity.x);
        sp.velY = Quantize::packVel(p.velocity.y);
        sp.velZ = Quantize::packVel(p.velocity.z);
    }

    // Reorder entities/projectiles nearest-player-first. If serialize() later has
    // to cap a record list to fit the wire budget, it truncates the TAIL — which is
    // now the farthest-from-any-player records, the least visible to clients. Sorts
    // an index permutation (cheap) then applies it in place to the snap arrays.
    static u16 order[MAX_PROJECTILES];  // reused for entities (count <= MAX_ENTITIES)

    if (snap.entityCount > 1) {
        for (u16 i = 0; i < snap.entityCount; i++) order[i] = i;
        std::sort(order, order + snap.entityCount,
                  [](u16 a, u16 b) { return entKeys[a] < entKeys[b]; });
        static SnapEntity entTmp[MAX_ENTITIES]; // static scratch — no large stack frame
        for (u16 i = 0; i < snap.entityCount; i++) entTmp[i] = snap.entities[order[i]];
        for (u16 i = 0; i < snap.entityCount; i++) snap.entities[i] = entTmp[i];
    }

    if (snap.projectileCount > 1) {
        for (u16 i = 0; i < snap.projectileCount; i++) order[i] = i;
        std::sort(order, order + snap.projectileCount,
                  [](u16 a, u16 b) { return projKeys[a] < projKeys[b]; });
        // Permute the heap-backed projectile array in place via a scratch copy.
        // (One static scratch buffer, no per-call heap — matches the pool convention.)
        static SnapProjectile projTmp[MAX_PROJECTILES];
        for (u16 i = 0; i < snap.projectileCount; i++) projTmp[i] = snap.projectiles[order[i]];
        for (u16 i = 0; i < snap.projectileCount; i++) snap.projectiles[i] = projTmp[i];
    }
}

// Per-record wire sizes (bytes actually emitted by the loops below — NOT sizeof
// the structs, which include alignment padding that is never serialized).
static constexpr u32 SNAP_PLAYER_WIRE     = 29;
// Entity: 1+1+1+1 + 6(pos) + 2(yaw) + 4(vel) + 1(stun)+1(freeze)+1(limb)+1(bossStatus) = 20.
static constexpr u32 SNAP_ENTITY_WIRE     = 20;
// Projectile: 2(idx) + 1(flags)+1(projFlags)+1(meshId)+1(radiusQ) + 6(pos) + 6(vel) = 18.
static constexpr u32 SNAP_PROJECTILE_WIRE = 18;
// Fixed prefix: 4 B packet header + 8 B snapshot header + MAX_PLAYERS u32 input ticks.
static constexpr u32 SNAP_FIXED_BYTES     = 4 + 8 + MAX_PLAYERS * 4;

// Overflow-safe serializer.
//
// The old version wrote the count header FIRST and then streamed records into a
// fixed PacketWriter whose writes silently no-op'd once full — so a full buffer
// dropped trailing records while the header still claimed the full counts. The
// client then read those phantom records as all-zero (poolIndex 0, pos ~ -128 m),
// teleporting entities to the world edge and stomping slot 0.
//
// Fix: pre-compute how many records of each type fit in `maxSize` under a fixed
// priority — players first, then entities, then projectiles (lowest value) — and
// write the header with those ACTUAL counts. The packet is therefore always
// internally consistent: declared counts can never exceed the bytes present.
// `buildFromState` already orders entities/projectiles nearest-player-first, so a
// capped list drops the farthest (least visible) records and keeps the relevant ones.
u32 Snapshot::serialize(const WorldSnapshot& snap, u8* outData, u32 maxSize) {
    // Manual bounds-checked writer over the caller's buffer. We don't use
    // PacketWriter here because its inline data[MAX_PACKET_SIZE] (4 KB) is both too
    // small for full snapshots and would bloat the stack if grown — snapshots ride
    // the dedicated MAX_SNAPSHOT_SIZE buffer instead.
    u32 cursor = 0;
    auto w8  = [&](u8 v)  { outData[cursor++] = v; };
    auto w16 = [&](u16 v) { std::memcpy(outData + cursor, &v, 2); cursor += 2; };
    auto w32 = [&](u32 v) { std::memcpy(outData + cursor, &v, 4); cursor += 4; };

    if (maxSize < SNAP_FIXED_BYTES) return 0; // can't even fit the header

    // Decide final counts up front so the header is always truthful. Players are
    // never dropped (at most MAX_PLAYERS, ~116 B); entities and then projectiles
    // absorb any shortfall in that priority order.
    u32 budget = maxSize - SNAP_FIXED_BYTES;

    u8  playerCount = snap.playerCount;
    u32 playerBytes = playerCount * SNAP_PLAYER_WIRE;
    if (playerBytes > budget) {                 // pathological tiny buffer only
        playerCount = static_cast<u8>(budget / SNAP_PLAYER_WIRE);
        playerBytes = playerCount * SNAP_PLAYER_WIRE;
    }
    budget -= playerBytes;

    u8  entityCount = snap.entityCount;
    u32 entityBytes = entityCount * SNAP_ENTITY_WIRE;
    if (entityBytes > budget) {
        entityCount = static_cast<u8>(budget / SNAP_ENTITY_WIRE);
        entityBytes = entityCount * SNAP_ENTITY_WIRE;
    }
    budget -= entityBytes;

    u16 projectileCount = snap.projectileCount;
    if (projectileCount * SNAP_PROJECTILE_WIRE > budget)
        projectileCount = static_cast<u16>(budget / SNAP_PROJECTILE_WIRE);

    // Header
    w8(static_cast<u8>(NetPacketType::SV_SNAPSHOT));
    w8(0);  // flags
    w16(0); // seq (filled by caller)
    // Snapshot header — ACTUAL counts that will be written below.
    w32(snap.serverTick);
    w8(playerCount);
    w8(entityCount);
    w16(projectileCount);

    // Last input ticks per player
    for (u32 i = 0; i < MAX_PLAYERS; i++)
        w32(snap.lastInputTick[i]);

    // Players
    for (u32 i = 0; i < playerCount; i++) {
        const SnapPlayer& sp = snap.players[i];
        w8(sp.slotIndex);
        w8(sp.flags);
        w8(sp.weaponId);
        w8(sp.health);
        w16(sp.posX);
        w16(sp.posY);
        w16(sp.posZ);
        w16(sp.velX);
        w16(sp.velZ);
        w16(sp.yaw);
        w16(sp.pitch);
        w16(sp.lockIndex);
        w8(sp.currentClip);
        w8(sp.statusFlags);
        w8(sp.invulnTimer);
        w8(sp.poisonTimer);
        w8(sp.burnTimer);
        w8(sp.freezeTimer);
        w8(sp.animFlags);
        w8(sp.weaponMeshId);
        w8(sp.dodgeFlags);  // Wanderer dodge state (bit0=rolling, bits1-3=counterStacks)
    }

    // Entities
    for (u32 i = 0; i < entityCount; i++) {
        const SnapEntity& se = snap.entities[i];
        w8(se.poolIndex);
        w8(se.flags);
        w8(se.aiState);
        w8(se.healthPct);
        w16(se.posX);
        w16(se.posY);
        w16(se.posZ);
        w16(se.yaw);
        w16(se.velX);
        w16(se.velZ);
        w8(se.stunTimer);
        w8(se.freezeTimer);
        w8(se.bossLimbConfig);
        w8(se.bossStatus);
    }

    // Projectiles
    for (u32 i = 0; i < projectileCount; i++) {
        const SnapProjectile& sp = snap.projectiles[i];
        w16(sp.poolIndex);
        w8(sp.flags);
        w8(sp.projFlags);
        w8(sp.meshId);
        w8(sp.radiusQ);
        w16(sp.posX);
        w16(sp.posY);
        w16(sp.posZ);
        w16(sp.velX);
        w16(sp.velY);
        w16(sp.velZ);
    }

    // cursor == SNAP_FIXED_BYTES + playerBytes + entityBytes +
    //           projectileCount*SNAP_PROJECTILE_WIRE, always <= maxSize by the caps above.
    return cursor;
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
        se.bossStatus    = r.readU8();
    }

    for (u32 i = 0; i < snap.projectileCount; i++) {
        SnapProjectile& sp = snap.projectiles[i];
        sp.poolIndex = r.readU16();
        sp.flags     = r.readU8();
        sp.projFlags = r.readU8();
        sp.meshId    = r.readU8();
        sp.radiusQ   = r.readU8();
        sp.posX      = r.readU16();
        sp.posY      = r.readU16();
        sp.posZ      = r.readU16();
        sp.velX      = r.readU16();
        sp.velY      = r.readU16();
        sp.velZ      = r.readU16();
    }

    return r.cursor <= r.size;
}
