#include "net/snapshot.h"
#include "net/packet.h"
#include "core/assert.h"  // ENGINE_ASSERT (debug-only) for the serialize cursor guard

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
                               const ProjectilePool& projectiles,
                               const WorldItemPool& worldItems)
{
    snap.serverTick = tick;
    snap.playerCount = 0;
    snap.entityCount = 0;
    snap.worldItemCount = 0;
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
        if (np.weaponState.reloading)   flags |= (1 << 3);
        if (np.blocking)                flags |= (1 << 4);
        sp.flags = flags;

        sp.weaponId = np.weaponState.currentWeapon;
        // Guard divide-by-zero (mirrors the entity path) and clamp the ratio so a
        // transient overheal or maxHealth==0 can't NaN or wrap the u8.
        f32 hpFrac = (np.maxHealth > 0.0f) ? (np.health / np.maxHealth) : 0.0f;
        if (hpFrac < 0.0f) hpFrac = 0.0f;
        if (hpFrac > 1.0f) hpFrac = 1.0f;
        // Round (+0.5f) to match maxHealth and Quantize::packFloat — the dequant side
        // divides by 255 so rounding here just centers the bucket (no symmetry change).
        sp.health   = static_cast<u8>(hpFrac * 255.0f + 0.5f);
        // Absolute max HP on the wire so the client reconstructs absolute health and tracks
        // per-floor growth (R7-4). Round + clamp into u16 (HP never realistically exceeds 65535).
        f32 mh = np.maxHealth;
        if (mh < 0.0f) mh = 0.0f;
        if (mh > 65535.0f) mh = 65535.0f;
        sp.maxHealth = static_cast<u16>(mh + 0.5f);

        sp.posX = Quantize::packPos(np.position.x);
        sp.posY = Quantize::packPos(np.position.y);
        sp.posZ = Quantize::packPos(np.position.z);
        sp.velX = Quantize::packVel(np.velocity.x);
        sp.velZ = Quantize::packVel(np.velocity.z);
        sp.yaw   = Quantize::packAngle(np.yaw);
        sp.pitch = Quantize::packAngle(np.pitch);

        // Status effects + clip (new fields)
        sp.currentClip = np.weaponState.currentClip;
        u8 sf = 0;
        if (np.invulnTimer > 0.0f) sf |= (1 << 0);
        if (np.poisonTimer > 0.0f) sf |= (1 << 1);
        if (np.burnTimer > 0.0f)   sf |= (1 << 2);
        if (np.freezeTimer > 0.0f) sf |= (1 << 3);
        if (np.slowTimer > 0.0f)   sf |= (1 << 4);
        sp.statusFlags  = sf;
        // Status timers pack at 25/s into a u8: 255/25 = 10.2 s is the max representable.
        // Clamp before the cast — a longer timer would overflow the u8 (float->u8 past 255
        // is UB) and wrap to a tiny value. quantTimer mirrors the radiusQ/maxHealth clamp style.
        auto quantTimer = [](f32 t) -> u8 {
            if (t < 0.0f)    t = 0.0f;
            if (t > 10.2f)   t = 10.2f;   // 10.2 s = u8 max at 25/s
            return static_cast<u8>(t * 25.0f);
        };
        sp.invulnTimer  = quantTimer(np.invulnTimer);  // 0-10.2s in 0.04s steps
        sp.poisonTimer  = quantTimer(np.poisonTimer);
        sp.burnTimer    = quantTimer(np.burnTimer);
        sp.freezeTimer  = quantTimer(np.freezeTimer);

        // Animation state for remote player rendering
        u8 anim = 0;
        if (np.weaponState.cooldownTimer > 0.0f) anim |= (1 << 0); // attacking/fired recently
        if (np.weaponState.reloading)             anim |= (1 << 1); // reload animation
        if (np.isDead)                             anim |= (1 << 2); // dead
        sp.animFlags = anim;
        sp.weaponMeshId = np.weaponState.weaponMeshId; // resolved MeshDef index (refreshed each snap)

        // Dodge state: the Wanderer roll (rolling flag + counterStacks) lives only on
        // the local-only Player.dodgeState (player.h) and is NOT mirrored to NetPlayer.
        // The server only tracks dodge i-frames as invulnTimer (player.cpp:266), which is
        // also set by respawn (engine_net.cpp:265) — so it can't be reused as a "rolling"
        // bit without animating a roll on every remote respawn. Until NetPlayer carries
        // real roll state, dodgeFlags stays 0 and remotes don't animate the roll.
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
        // Clamp to [0, 10.2 s] before the 25/s pack: 255/25 = 10.2 s is the u8 max; a longer
        // timer would overflow the u8 (float->u8 past 255 is UB) and wrap to a tiny value.
        auto quantTimer = [](f32 t) -> u8 {
            if (t < 0.0f)  t = 0.0f;
            if (t > 10.2f) t = 10.2f;
            return static_cast<u8>(t * 25.0f);
        };
        se.stunTimer     = quantTimer(e.stunTimer);
        se.freezeTimer   = quantTimer(e.freezeTimer);
        se.bossLimbConfig = e.bossLimbConfig;
        // Pack boss invuln/shield state: bit0=minionShield, bits1-3=bossPhase (0-4).
        se.bossStatus    = (e.minionShield ? 0x01 : 0x00)
                         | static_cast<u8>((e.bossPhase & 0x07) << 1);
        // Visual identity (authoritative — see SnapEntity comment).
        se.meshId       = e.meshId;
        se.materialId   = e.materialId;
        se.enemyTypeId  = static_cast<u8>(e.enemyType);
        se.weaponMeshId = e.weaponMeshId;
        // halfExtents quantized 0-2.55 m in 0.01 m steps. Clamp into range (any def
        // configured above 2.55 m would have hit the cap; today's max is 1.3 m).
        auto packExt = [](f32 v) -> u8 {
            f32 q = v * 100.0f;
            if (q < 0.0f) q = 0.0f;
            if (q > 255.0f) q = 255.0f;
            return static_cast<u8>(q);
        };
        se.halfExtentsXQ = packExt(e.halfExtents.x);
        se.halfExtentsYQ = packExt(e.halfExtents.y);
        se.halfExtentsZQ = packExt(e.halfExtents.z);
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
        // (Audit-A) Owner net slot so client-side projectile kills credit the right
        // player for loot exclusivity / ring on-kill passives. Without this, a remote's
        // molotov in flight gets ownerSlot=0xFF on every observer and credits no one.
        sp.ownerSlot = p.ownerSlot;
    }

    // World items (dropped loot). Server-authoritative (N5): every active world item is
    // mirrored by slotIndex so the client can copy it directly into its own pool. Globes
    // are included too — clients render them and the server auto-picks them up. These have
    // the LOWEST serialize priority (after projectiles), so a pathologically tiny budget
    // drops loot first; in practice 32 items * 16 B = 512 B always fits the 8 KB budget.
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        const WorldItem& wi = worldItems.items[i];
        if (!wi.active) continue;
        SnapWorldItem& sw = snap.worldItems[snap.worldItemCount++];
        sw.slotIndex = static_cast<u8>(i);
        sw.rarity    = static_cast<u8>(wi.item.rarity);
        sw.defId     = wi.item.defId;
        sw.uid       = wi.item.uid;
        sw.posX = Quantize::packPos(wi.position.x);
        sw.posY = Quantize::packPos(wi.position.y);
        sw.posZ = Quantize::packPos(wi.position.z);
        // (Audit-B) Pickup-ownership window so the client honors exclusivity instead of
        // treating every drop as instantly FFA. 0.04 s steps gives 10.2 s range, covers
        // the 3-5 s exclusivity windows used by drop code with margin.
        sw.ownerSlot = wi.ownerSlot;
        f32 et = wi.exclusiveTimer * 25.0f;  // 1/0.04 = 25
        if (et < 0.0f)   et = 0.0f;
        if (et > 255.0f) et = 255.0f;
        sw.exclusiveTimerQ = static_cast<u8>(et);
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
// Player: 1(slot)+1(flags)+1(weapon)+1(health)+2(maxHealth) + 6(pos)+4(vel)+2(yaw)+2(pitch)
//       + 1(clip)+1(statusFlags)+1(invuln)+1(poison)+1(burn)+1(freeze)
//       + 1(animFlags)+1(weaponMeshId)+1(dodgeFlags) = 29. (Was 31 before lockIndex was retired.)
static constexpr u32 SNAP_PLAYER_WIRE     = 29;
// Entity: 1+1+1+1 + 6(pos) + 2(yaw) + 4(vel) + 1(stun)+1(freeze)+1(limb)+1(bossStatus)
//       + 1(meshId)+1(materialId)+1(enemyType)+1(weaponMeshId) + 3(halfExtentsQ) = 27.
static constexpr u32 SNAP_ENTITY_WIRE     = 27;
// Projectile: 2(idx) + 1(flags)+1(projFlags)+1(meshId)+1(radiusQ) + 6(pos) + 6(vel) + 1(ownerSlot) = 19.
static constexpr u32 SNAP_PROJECTILE_WIRE = 19;
// World item: 1(slotIndex) + 1(rarity) + 2(defId) + 4(uid) + 6(pos) + 1(ownerSlot) + 1(exclusiveTimerQ) = 16.
static constexpr u32 SNAP_WORLDITEM_WIRE  = 16;
// Fixed prefix: 4 B packet header + snapshot header + MAX_PLAYERS u32 input ticks.
// Snapshot header is now 9 B: serverTick(4) + playerCount(1) + entityCount(1) +
// worldItemCount(1) + projectileCount(2). (Was 8 B before the world-item count field.)
static constexpr u32 SNAP_FIXED_BYTES     = 4 + 9 + MAX_PLAYERS * 4;

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
    // Defense-in-depth: the record counts below are pre-computed to fit `maxSize`, so these
    // never clip today. The per-write capacity check guards a FUTURE field added without
    // bumping the matching SNAP_*_WIRE — instead of scribbling past the buffer it silently
    // skips the write (the end-of-function guard then catches the truncation in debug).
    auto w8  = [&](u8 v)  { if (cursor + 1 > maxSize) return; outData[cursor++] = v; };
    auto w16 = [&](u16 v) { if (cursor + 2 > maxSize) return; std::memcpy(outData + cursor, &v, 2); cursor += 2; };
    auto w32 = [&](u32 v) { if (cursor + 4 > maxSize) return; std::memcpy(outData + cursor, &v, 4); cursor += 4; };

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
    u32 projectileBytes = projectileCount * SNAP_PROJECTILE_WIRE;
    if (projectileBytes > budget) {
        projectileCount = static_cast<u16>(budget / SNAP_PROJECTILE_WIRE);
        projectileBytes = projectileCount * SNAP_PROJECTILE_WIRE;
    }
    budget -= projectileBytes;

    // World items have the LOWEST priority — they absorb whatever budget remains after
    // players/entities/projectiles. (In practice 32*16=512 B always fits the 8 KB budget.)
    u8  worldItemCount = snap.worldItemCount;
    if (worldItemCount * SNAP_WORLDITEM_WIRE > budget)
        worldItemCount = static_cast<u8>(budget / SNAP_WORLDITEM_WIRE);

    // Header
    w8(static_cast<u8>(NetPacketType::SV_SNAPSHOT));
    w8(0);  // flags
    // Header seq is unused for snapshots: the client orders/dedupes by the snapshot's
    // own monotonic serverTick (see Client::receiveSnapshot), so we leave it zero.
    w16(0); // seq (unused — ordering is by serverTick)
    // Snapshot header — ACTUAL counts that will be written below.
    w32(snap.serverTick);
    w8(playerCount);
    w8(entityCount);
    w8(worldItemCount);
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
        w16(sp.maxHealth);
        w16(sp.posX);
        w16(sp.posY);
        w16(sp.posZ);
        w16(sp.velX);
        w16(sp.velZ);
        w16(sp.yaw);
        w16(sp.pitch);
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
        w8(se.meshId);
        w8(se.materialId);
        w8(se.enemyTypeId);
        w8(se.weaponMeshId);
        w8(se.halfExtentsXQ);    // (Audit P2 #4) per-entity collider extents
        w8(se.halfExtentsYQ);
        w8(se.halfExtentsZQ);
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
        w8(sp.ownerSlot);     // (Audit-A) firing slot for kill-credit on clients
    }

    // World items (lowest priority — written last so they drop first under a tight budget).
    for (u32 i = 0; i < worldItemCount; i++) {
        const SnapWorldItem& sw = snap.worldItems[i];
        w8(sw.slotIndex);
        w8(sw.rarity);
        w16(sw.defId);
        w32(sw.uid);
        w16(sw.posX);
        w16(sw.posY);
        w16(sw.posZ);
        w8(sw.ownerSlot);          // (Audit-B) pickup-window owner
        w8(sw.exclusiveTimerQ);    // (Audit-B) 0.04 s steps, 10.2 s range
    }

    // cursor == SNAP_FIXED_BYTES + playerBytes + entityBytes +
    //           projectileCount*SNAP_PROJECTILE_WIRE + worldItemCount*SNAP_WORLDITEM_WIRE,
    //           always <= maxSize by the caps above. The assert flags (in debug) a future
    //           record field added without bumping its SNAP_*_WIRE; the clamp keeps a release
    //           build from ever reporting a length past the caller's buffer.
    ENGINE_ASSERT(cursor <= maxSize, "snapshot serialize overran buffer — a SNAP_*_WIRE is stale");
    if (cursor > maxSize) cursor = maxSize;
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
    snap.worldItemCount  = r.readU8();
    snap.projectileCount = r.readU16();

    // Validate counts to prevent out-of-bounds on malformed packets
    if (snap.playerCount > MAX_PLAYERS) snap.playerCount = MAX_PLAYERS;
    if (snap.entityCount > MAX_ENTITIES) snap.entityCount = MAX_ENTITIES;
    if (snap.worldItemCount > MAX_WORLD_ITEMS) snap.worldItemCount = MAX_WORLD_ITEMS;
    if (snap.projectileCount > MAX_PROJECTILES)
        snap.projectileCount = MAX_PROJECTILES;

    // Reject packets too small for the counts they declare. Without this, a short/truncated
    // packet keeps reading zeros past the end (PacketReader returns 0 once exhausted), which
    // unpack to ~ -128 m positions on slot 0 and stomp it. SNAP_FIXED_BYTES covers the packet
    // header + snapshot header + the MAX_PLAYERS input ticks read in the loop below; the rest
    // is the (now-clamped) record payload. Bail with false so the caller drops the packet.
    u32 requiredBytes = SNAP_FIXED_BYTES
                      + static_cast<u32>(snap.playerCount)     * SNAP_PLAYER_WIRE
                      + static_cast<u32>(snap.entityCount)     * SNAP_ENTITY_WIRE
                      + static_cast<u32>(snap.projectileCount) * SNAP_PROJECTILE_WIRE
                      + static_cast<u32>(snap.worldItemCount)  * SNAP_WORLDITEM_WIRE;
    if (size < requiredBytes) return false;

    for (u32 i = 0; i < MAX_PLAYERS; i++)
        snap.lastInputTick[i] = r.readU32();

    for (u32 i = 0; i < snap.playerCount; i++) {
        SnapPlayer& sp = snap.players[i];
        sp.slotIndex = r.readU8();
        sp.flags     = r.readU8();
        sp.weaponId  = r.readU8();
        sp.health    = r.readU8();
        sp.maxHealth = r.readU16();
        sp.posX      = r.readU16();
        sp.posY      = r.readU16();
        sp.posZ      = r.readU16();
        sp.velX      = r.readU16();
        sp.velZ      = r.readU16();
        sp.yaw       = r.readU16();
        sp.pitch     = r.readU16();
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
        se.meshId        = r.readU8();
        se.materialId    = r.readU8();
        se.enemyTypeId   = r.readU8();
        se.weaponMeshId  = r.readU8();
        se.halfExtentsXQ = r.readU8();   // (Audit P2 #4)
        se.halfExtentsYQ = r.readU8();
        se.halfExtentsZQ = r.readU8();
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
        sp.ownerSlot = r.readU8();   // (Audit-A) firing slot for kill-credit on clients
    }

    for (u32 i = 0; i < snap.worldItemCount; i++) {
        SnapWorldItem& sw = snap.worldItems[i];
        sw.slotIndex = r.readU8();
        sw.rarity    = r.readU8();
        sw.defId     = r.readU16();
        sw.uid       = r.readU32();
        sw.posX      = r.readU16();
        sw.posY      = r.readU16();
        sw.posZ      = r.readU16();
        sw.ownerSlot       = r.readU8();   // (Audit-B)
        sw.exclusiveTimerQ = r.readU8();   // (Audit-B)
    }

    return r.cursor <= r.size;
}
