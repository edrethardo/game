#include "net/snapshot.h"
#include "net/packet.h"
#include "core/assert.h"  // ENGINE_ASSERT (debug-only) for the serialize cursor guard

#include <algorithm>
#include <cstring>  // std::memcmp for slot equality helpers

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
        snap.lastProcessedInputTick[i] = np.lastProcessedInputTick;

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
        // PlayerClass on the wire so clients pick the correct per-class mesh for remotes.
        // Clamped on read in deserialize() against PlayerClass::CLASS_COUNT.
        sp.playerClass = static_cast<u8>(np.playerClass);
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
        // Attack swing countdown: pack [0, 1.0s] into u8 (1/255s ≈ 3.9 ms per step).
        // Set to 0.3 by AI when an attack fires; decays to 0 in EnemyAI::update. Without this
        // on the wire, clients never see attack lunges/swings (N4 gated off the local ghost AI).
        f32 aq = e.attackAnimT * 255.0f;
        if (aq < 0.0f)   aq = 0.0f;
        if (aq > 255.0f) aq = 255.0f;
        se.attackAnimQ = static_cast<u8>(aq);
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
        // V2 prediction match key: low 16 bits of the firing client's tick at fire moment.
        // Carries through CL_FIRE_WEAPON → projectile.clientTick → wire → client interp,
        // where it matches a locally-spawned predicted ghost so the ghost despawns when
        // the authoritative projectile lands. 0 for host/NPC/skill-spawned projectiles.
        sp.clientTickLow = static_cast<u16>(p.clientTick & 0xFFFF);
        // D3.1 — Pack the authoritative damage so the client can predict HP decrements on
        // incoming-projectile impact (D3.2). Multiply by 2 → 0.5-step increments in [0,127.5].
        // Clamp to u8 max so values above 127.5 (rare, e.g. crit-boosted boss shots) saturate.
        {
            f32 dq = p.damage * 2.0f;
            if (dq > 255.0f) dq = 255.0f;
            sp.expectedDamageQ = static_cast<u8>(dq);
        }
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
        // D8 — rolled stats so client::mirrorWorldItems can populate the predicted
        // ItemInstance with real values instead of zero-initialised defaults.
        sw.damage      = wi.item.damage;
        sw.bonusHealth = wi.item.bonusHealth;
        sw.itemLevel   = wi.item.itemLevel;
        u8 ac = wi.item.affixCount;
        if (ac > MAX_AFFIXES_PER_ITEM) ac = MAX_AFFIXES_PER_ITEM;
        sw.affixCount = ac;
        for (u32 a = 0; a < ac; a++) sw.affixes[a] = wi.item.affixes[a];
        // Tail-init unused slots so the memcmp-based delta check sees a stable layout
        // even after the slot is reused for a different item with fewer affixes.
        for (u32 a = ac; a < MAX_AFFIXES_PER_ITEM; a++) sw.affixes[a] = Affix{};
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
//       + 1(animFlags)+1(weaponMeshId)+1(dodgeFlags)+1(playerClass) = 30.
//       (playerClass added so remote players render with the correct per-class mesh on clients.)
static constexpr u32 SNAP_PLAYER_WIRE     = 30;
// Entity: 1+1+1+1 + 6(pos) + 2(yaw) + 4(vel) + 1(stun)+1(freeze)+1(limb)+1(bossStatus)
//       + 1(meshId)+1(materialId)+1(enemyType)+1(weaponMeshId) + 3(halfExtentsQ)
//       + 1(attackAnimQ) = 28. (attackAnimQ added so clients see enemy attack animations
//       after N4 gated off the local ghost AI that used to tick the timer.)
static constexpr u32 SNAP_ENTITY_WIRE     = 28;
// Projectile: 2(idx) + 1(flags)+1(projFlags)+1(meshId)+1(radiusQ) + 6(pos) + 6(vel) + 1(ownerSlot)
//           + 2(clientTickLow) + 1(expectedDamageQ) = 22. (expectedDamageQ added in D3.1 so
//           clients can predict local HP decrement on incoming-projectile impact in D3.2.)
static constexpr u32 SNAP_PROJECTILE_WIRE = 22;
// World item is variable-length: fixed prefix + variable affix tail.
// Fixed prefix: 1(slotIndex) + 1(rarity) + 2(defId) + 4(uid) + 6(pos)
//             + 1(ownerSlot) + 1(exclusiveTimerQ)
//             + 4(damage) + 4(bonusHealth) + 1(itemLevel) + 1(affixCount) = 26 B.
// Variable tail: affixCount × {1(type) + 4(value)} = 0..20 B (MAX_AFFIXES_PER_ITEM = 4).
// Budget gating uses MAX so the priority-clamp never overcommits the buffer; the
// required-bytes check at deserialize-time uses MIN so a packet with no-affix items
// (the common case for low-tier loot) isn't falsely rejected.
static constexpr u32 SNAP_WORLDITEM_WIRE_MIN = 26;
static constexpr u32 SNAP_WORLDITEM_WIRE_MAX = 26 + MAX_AFFIXES_PER_ITEM * 5;  // = 46
// Legacy alias — the delta skip-past path used to advance by a fixed record size;
// see the variable-size discard read in deserializeDelta for the replacement.
// Fixed prefix: 4 B packet header + snapshot header + 1 B isFullSnapshot flag +
// MAX_PLAYERS u32 lastProcessedInputTick.
// Snapshot header is 9 B: serverTick(4) + playerCount(1) + entityCount(1) +
// worldItemCount(1) + projectileCount(2).  isFullSnapshot adds 1 B (D7.2).
// lastProcessedInputTick adds MAX_PLAYERS*4 = 16 B.
// (Total fixed prefix = 4 + 9 + 1 + 16 = 30 B per snapshot.)
static constexpr u32 SNAP_FIXED_BYTES     = 4 + 9 + 1 + MAX_PLAYERS * 4;

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
u32 Snapshot::serialize(const WorldSnapshot& snap, u8* outData, u32 maxSize,
                        u8 isFullSnapshot) {
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
    // players/entities/projectiles. (In practice 32*46=1472 B always fits the 8 KB budget
    // even when every item rolls the max affix count.) We gate on the WORST-case wire
    // size per item because we don't yet know each item's affixCount at clamp time and
    // we'd rather drop a couple of items than overrun the buffer.
    u8  worldItemCount = snap.worldItemCount;
    if (worldItemCount * SNAP_WORLDITEM_WIRE_MAX > budget)
        worldItemCount = static_cast<u8>(budget / SNAP_WORLDITEM_WIRE_MAX);

    // Header
    w8(static_cast<u8>(NetPacketType::SV_SNAPSHOT));
    // Flags byte: bit 0 = isFullSnapshot. The client peeks data[1] to route full vs
    // delta in Client::receiveSnapshot — encoding the bit in the header avoids the
    // layout mismatch where the in-payload isFullSnapshot byte sits at different
    // offsets in full (13) vs delta (24) snapshots, with no single offset working
    // for both. Other bits of this byte are reserved (must remain 0).
    w8(isFullSnapshot ? 0x01 : 0x00);
    // Header seq is unused for snapshots: the client orders/dedupes by the snapshot's
    // own monotonic serverTick (see Client::receiveSnapshot), so we leave it zero.
    w16(0); // seq (unused — ordering is by serverTick)
    // Snapshot header — ACTUAL counts that will be written below.
    w32(snap.serverTick);
    w8(playerCount);
    w8(entityCount);
    w8(worldItemCount);
    w16(projectileCount);
    // Redundant in-payload isFullSnapshot byte (the authoritative copy is now in
    // header byte 1). Kept for now so deserialize() at line 510 continues to consume
    // the same number of bytes; removing it would require coordinated wire-format
    // changes on both sides. Safe to drop in a future cleanup.
    w8(isFullSnapshot);

    // Per-slot ACK of newest input applied by the server (M3 uses for replay reconciliation)
    for (u32 i = 0; i < MAX_PLAYERS; i++)
        w32(snap.lastProcessedInputTick[i]);

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
        w8(sp.playerClass); // PlayerClass cast to u8 — drives per-class mesh on the client
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
        w8(se.attackAnimQ);      // attack swing countdown (0-1.0s in 1/255s steps)
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
        w8(sp.ownerSlot);      // (Audit-A) firing slot for kill-credit on clients
        w16(sp.clientTickLow); // V2 fire prediction match key
        w8(sp.expectedDamageQ); // D3.1 — damage × 2, decoded as × 0.5 by client (D3.2 HP predict)
    }

    // f32 emitter — same shape as w32 but writes the IEEE 754 bit pattern of an f32.
    auto wF32 = [&](f32 v) { if (cursor + 4 > maxSize) return; std::memcpy(outData + cursor, &v, 4); cursor += 4; };

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
        // D8 — rolled stats so the client's pickup-prediction mirror (Client::mirrorWorldItems)
        // sees authoritative damage/affixes/itemLevel/bonusHealth instead of zero-initialised
        // ItemInstance defaults. Without this the inventory UI shows "Damage: 0" after a
        // multiplayer client pickup.
        wF32(sw.damage);
        wF32(sw.bonusHealth);
        w8(sw.itemLevel);
        u8 affixCount = sw.affixCount;
        if (affixCount > MAX_AFFIXES_PER_ITEM) affixCount = MAX_AFFIXES_PER_ITEM;
        w8(affixCount);
        for (u32 a = 0; a < affixCount; a++) {
            w8(static_cast<u8>(sw.affixes[a].type));
            wF32(sw.affixes[a].value);
        }
    }

    // cursor == SNAP_FIXED_BYTES + playerBytes + entityBytes +
    //           projectileCount*SNAP_PROJECTILE_WIRE +
    //           Σ over included items of (26 + affixCount_i × 5),
    //           always <= maxSize by the caps above (world-item clamp uses
    //           SNAP_WORLDITEM_WIRE_MAX so the worst-case per-item size is reserved).
    //           The assert flags (in debug) a future record field added without
    //           bumping its SNAP_*_WIRE; the clamp keeps a release build from ever
    //           reporting a length past the caller's buffer.
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
    // D7.2 — Read delta flag.  1 = full snapshot; 0 = delta (D7.3 decoding path).
    // For v1 the server always writes 1, so the delta branch is never taken; we read
    // the byte here to keep the wire format in sync even when talking to a v1 server.
    u8 isFullSnap = r.readU8();
    (void)isFullSnap; // D7.3 will gate the bitmask + per-slot logic on this flag

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
    // World items are variable-length: use the MINIMUM (no-affix) size as the lower
    // bound — items that actually carry affixes will tail-extend the packet beyond this,
    // and the per-field readU8/readF32 guards stop a truncated packet from reading past
    // the buffer (PacketReader::hasData returns false → fields stay zero, post-loop
    // `r.cursor <= r.size` check catches under-read).
    u32 requiredBytes = SNAP_FIXED_BYTES
                      + static_cast<u32>(snap.playerCount)     * SNAP_PLAYER_WIRE
                      + static_cast<u32>(snap.entityCount)     * SNAP_ENTITY_WIRE
                      + static_cast<u32>(snap.projectileCount) * SNAP_PROJECTILE_WIRE
                      + static_cast<u32>(snap.worldItemCount)  * SNAP_WORLDITEM_WIRE_MIN;
    if (size < requiredBytes) return false;

    for (u32 i = 0; i < MAX_PLAYERS; i++)
        snap.lastProcessedInputTick[i] = r.readU32();

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
        sp.playerClass  = r.readU8();  // PlayerClass cast to u8 — validated on read in net code
        // Defensive clamp: a malformed/older-protocol packet would otherwise index kClassDefs
        // out of bounds when the renderer (or anyone else) looks up cd.meshName below. Map
        // anything past CLASS_COUNT to WARRIOR (0), matching onPlayerJoin's fallback.
        if (sp.playerClass >= static_cast<u8>(PlayerClass::CLASS_COUNT))
            sp.playerClass = static_cast<u8>(PlayerClass::WARRIOR);
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
        se.attackAnimQ   = r.readU8();   // attack swing countdown (1/255s steps)
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
        sp.ownerSlot     = r.readU8();   // (Audit-A) firing slot for kill-credit on clients
        sp.clientTickLow = r.readU16();  // V2 fire prediction match key
        sp.expectedDamageQ = r.readU8(); // D3.1 — decode: proj.damage = expectedDamageQ * 0.5f
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
        // D8 — rolled stats (matches writer in serialize()).
        sw.damage      = r.readF32();
        sw.bonusHealth = r.readF32();
        sw.itemLevel   = r.readU8();
        u8 affixCount  = r.readU8();
        // Defensive clamp — a byzantine / corrupt packet can't index past the affix array.
        if (affixCount > MAX_AFFIXES_PER_ITEM) affixCount = MAX_AFFIXES_PER_ITEM;
        sw.affixCount = affixCount;
        for (u32 a = 0; a < affixCount; a++) {
            sw.affixes[a].type  = static_cast<AffixType>(r.readU8());
            sw.affixes[a].value = r.readF32();
        }
        // Tail-init the unused affix slots so the memcmp-based worldItemSlotsEqual
        // doesn't observe stale data from the prior occupant of this slot.
        for (u32 a = affixCount; a < MAX_AFFIXES_PER_ITEM; a++) {
            sw.affixes[a] = Affix{};
        }
    }

    return r.cursor <= r.size;
}

// ---------------------------------------------------------------------------
// D7.1 — Per-slot equality helpers
//
// All snapshot structs are zero-initialised (WorldSnapshot's default constructor
// calls the default constructors of its members, which value-initialise the pod
// arrays to 0 via {}), so padding bytes are deterministically zero and memcmp is
// safe.  If a struct ever gains a non-pod member with opaque padding, switch to
// field-by-field comparison here.
//
// Out-of-range slots return true ("equal") so callers can treat them as "no change"
// without needing a separate bounds check.
// ---------------------------------------------------------------------------

bool Snapshot::playerSlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot) {
    if (slot >= MAX_PLAYERS) return true;
    return std::memcmp(&a.players[slot], &b.players[slot], sizeof(SnapPlayer)) == 0;
}

bool Snapshot::entitySlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot) {
    if (slot >= MAX_ENTITIES) return true;
    return std::memcmp(&a.entities[slot], &b.entities[slot], sizeof(SnapEntity)) == 0;
}

bool Snapshot::projectileSlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot) {
    if (slot >= static_cast<u32>(MAX_PROJECTILES)) return true;
    return std::memcmp(&a.projectiles[slot], &b.projectiles[slot], sizeof(SnapProjectile)) == 0;
}

bool Snapshot::worldItemSlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot) {
    if (slot >= MAX_WORLD_ITEMS) return true;
    return std::memcmp(&a.worldItems[slot], &b.worldItems[slot], sizeof(SnapWorldItem)) == 0;
}

// ---------------------------------------------------------------------------
// D7.3.1 — Pool-index lookup helpers
//
// Linear scan over the count-bounded dense arrays. Counts are small in practice
// (≤4 players, ≤64 entities, ≤64 projectiles, ≤32 world items) so the
// O(n) cost is cheaper than a hash table or sorted binary search for our sizes.
// ---------------------------------------------------------------------------

const SnapPlayer* Snapshot::findPlayerByPoolIndex(const WorldSnapshot& s, u8 slotIndex) {
    for (u8 i = 0; i < s.playerCount; i++) {
        if (s.players[i].slotIndex == slotIndex) return &s.players[i];
    }
    return nullptr;
}

const SnapEntity* Snapshot::findEntityByPoolIndex(const WorldSnapshot& s, u8 poolIndex) {
    for (u8 i = 0; i < s.entityCount; i++) {
        if (s.entities[i].poolIndex == poolIndex) return &s.entities[i];
    }
    return nullptr;
}

const SnapProjectile* Snapshot::findProjectileByPoolIndex(const WorldSnapshot& s, u16 poolIndex) {
    for (u16 i = 0; i < s.projectileCount; i++) {
        if (s.projectiles[i].poolIndex == poolIndex) return &s.projectiles[i];
    }
    return nullptr;
}

const SnapWorldItem* Snapshot::findWorldItemByPoolIndex(const WorldSnapshot& s, u8 slotIndex) {
    for (u8 i = 0; i < s.worldItemCount; i++) {
        if (s.worldItems[i].slotIndex == slotIndex) return &s.worldItems[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// D7.3.1 — 64-bit bitmask over u8[8]
//
// Bit N occupies byte[N/8] at position (N%8). Out-of-range (N >= 64) is
// defensively ignored rather than silently corrupting adjacent memory.
// ---------------------------------------------------------------------------

void Snapshot::setBit64(u8* mask, u32 bit) {
    if (bit >= 64) return;
    mask[bit / 8] |= static_cast<u8>(1u << (bit % 8));
}

bool Snapshot::getBit64(const u8* mask, u32 bit) {
    if (bit >= 64) return false;
    return (mask[bit / 8] & (1u << (bit % 8))) != 0;
}

// ---------------------------------------------------------------------------
// D7.3.2 — Delta encoding helpers (per-record write/read)
//
// Extracted from serialize()/deserialize() so both the full path and the delta
// path emit identical per-record wire bytes. The full serialize() is unchanged
// and continues to own its own inline loops for performance (no extra call per
// record in the hot broadcast path). These helpers are used only by the delta
// paths below.
// ---------------------------------------------------------------------------

// Write one SnapPlayer to the caller's bounds-checked buffer.
// Byte layout MUST match the full serializer's player loop above (SNAP_PLAYER_WIRE = 30).
static void writeSnapPlayer(u8* buf, u32 maxSize, u32& cursor, const SnapPlayer& sp) {
    auto w8  = [&](u8 v)  { if (cursor + 1 <= maxSize) buf[cursor++] = v; };
    auto w16 = [&](u16 v) { if (cursor + 2 <= maxSize) { std::memcpy(buf + cursor, &v, 2); cursor += 2; } };
    w8(sp.slotIndex); w8(sp.flags); w8(sp.weaponId); w8(sp.health);
    w16(sp.maxHealth);
    w16(sp.posX); w16(sp.posY); w16(sp.posZ);
    w16(sp.velX); w16(sp.velZ);
    w16(sp.yaw); w16(sp.pitch);
    w8(sp.currentClip); w8(sp.statusFlags);
    w8(sp.invulnTimer); w8(sp.poisonTimer); w8(sp.burnTimer); w8(sp.freezeTimer);
    w8(sp.animFlags); w8(sp.weaponMeshId); w8(sp.dodgeFlags); w8(sp.playerClass);
}

// Write one SnapEntity. MUST match SNAP_ENTITY_WIRE = 28.
static void writeSnapEntity(u8* buf, u32 maxSize, u32& cursor, const SnapEntity& se) {
    auto w8  = [&](u8 v)  { if (cursor + 1 <= maxSize) buf[cursor++] = v; };
    auto w16 = [&](u16 v) { if (cursor + 2 <= maxSize) { std::memcpy(buf + cursor, &v, 2); cursor += 2; } };
    w8(se.poolIndex); w8(se.flags); w8(se.aiState); w8(se.healthPct);
    w16(se.posX); w16(se.posY); w16(se.posZ);
    w16(se.yaw); w16(se.velX); w16(se.velZ);
    w8(se.stunTimer); w8(se.freezeTimer); w8(se.bossLimbConfig); w8(se.bossStatus);
    w8(se.meshId); w8(se.materialId); w8(se.enemyTypeId); w8(se.weaponMeshId);
    w8(se.halfExtentsXQ); w8(se.halfExtentsYQ); w8(se.halfExtentsZQ);
    w8(se.attackAnimQ);
}

// Write one SnapProjectile. MUST match SNAP_PROJECTILE_WIRE = 22.
static void writeSnapProjectile(u8* buf, u32 maxSize, u32& cursor, const SnapProjectile& sp) {
    auto w8  = [&](u8 v)  { if (cursor + 1 <= maxSize) buf[cursor++] = v; };
    auto w16 = [&](u16 v) { if (cursor + 2 <= maxSize) { std::memcpy(buf + cursor, &v, 2); cursor += 2; } };
    w16(sp.poolIndex); w8(sp.flags); w8(sp.projFlags); w8(sp.meshId); w8(sp.radiusQ);
    w16(sp.posX); w16(sp.posY); w16(sp.posZ);
    w16(sp.velX); w16(sp.velY); w16(sp.velZ);
    w8(sp.ownerSlot); w16(sp.clientTickLow); w8(sp.expectedDamageQ);
}

// Write one SnapWorldItem. MUST match the inline serialize() loop byte-for-byte and
// the readSnapWorldItem reader below. Variable-length: 26 B fixed + affixCount*5 B tail.
static void writeSnapWorldItem(u8* buf, u32 maxSize, u32& cursor, const SnapWorldItem& sw) {
    auto w8  = [&](u8 v)  { if (cursor + 1 <= maxSize) buf[cursor++] = v; };
    auto w16 = [&](u16 v) { if (cursor + 2 <= maxSize) { std::memcpy(buf + cursor, &v, 2); cursor += 2; } };
    auto w32 = [&](u32 v) { if (cursor + 4 <= maxSize) { std::memcpy(buf + cursor, &v, 4); cursor += 4; } };
    auto wF32 = [&](f32 v) { if (cursor + 4 <= maxSize) { std::memcpy(buf + cursor, &v, 4); cursor += 4; } };
    w8(sw.slotIndex); w8(sw.rarity); w16(sw.defId); w32(sw.uid);
    w16(sw.posX); w16(sw.posY); w16(sw.posZ);
    w8(sw.ownerSlot); w8(sw.exclusiveTimerQ);
    // D8 — rolled stats for the client's pickup-prediction mirror.
    wF32(sw.damage);
    wF32(sw.bonusHealth);
    w8(sw.itemLevel);
    u8 affixCount = sw.affixCount;
    if (affixCount > MAX_AFFIXES_PER_ITEM) affixCount = MAX_AFFIXES_PER_ITEM;
    w8(affixCount);
    for (u32 a = 0; a < affixCount; a++) {
        w8(static_cast<u8>(sw.affixes[a].type));
        wF32(sw.affixes[a].value);
    }
}

// Read one SnapPlayer from a PacketReader. MUST match writeSnapPlayer byte-for-byte.
static void readSnapPlayer(PacketReader& r, SnapPlayer& sp) {
    sp.slotIndex = r.readU8(); sp.flags = r.readU8(); sp.weaponId = r.readU8(); sp.health = r.readU8();
    sp.maxHealth = r.readU16();
    sp.posX = r.readU16(); sp.posY = r.readU16(); sp.posZ = r.readU16();
    sp.velX = r.readU16(); sp.velZ = r.readU16();
    sp.yaw = r.readU16(); sp.pitch = r.readU16();
    sp.currentClip = r.readU8(); sp.statusFlags = r.readU8();
    sp.invulnTimer = r.readU8(); sp.poisonTimer = r.readU8();
    sp.burnTimer = r.readU8(); sp.freezeTimer = r.readU8();
    sp.animFlags = r.readU8(); sp.weaponMeshId = r.readU8();
    sp.dodgeFlags = r.readU8(); sp.playerClass = r.readU8();
}

// Read one SnapEntity from a PacketReader. MUST match writeSnapEntity.
static void readSnapEntity(PacketReader& r, SnapEntity& se) {
    se.poolIndex = r.readU8(); se.flags = r.readU8(); se.aiState = r.readU8(); se.healthPct = r.readU8();
    se.posX = r.readU16(); se.posY = r.readU16(); se.posZ = r.readU16();
    se.yaw = r.readU16(); se.velX = r.readU16(); se.velZ = r.readU16();
    se.stunTimer = r.readU8(); se.freezeTimer = r.readU8();
    se.bossLimbConfig = r.readU8(); se.bossStatus = r.readU8();
    se.meshId = r.readU8(); se.materialId = r.readU8();
    se.enemyTypeId = r.readU8(); se.weaponMeshId = r.readU8();
    se.halfExtentsXQ = r.readU8(); se.halfExtentsYQ = r.readU8(); se.halfExtentsZQ = r.readU8();
    se.attackAnimQ = r.readU8();
}

// Read one SnapProjectile from a PacketReader. MUST match writeSnapProjectile.
static void readSnapProjectile(PacketReader& r, SnapProjectile& sp) {
    sp.poolIndex = r.readU16(); sp.flags = r.readU8(); sp.projFlags = r.readU8();
    sp.meshId = r.readU8(); sp.radiusQ = r.readU8();
    sp.posX = r.readU16(); sp.posY = r.readU16(); sp.posZ = r.readU16();
    sp.velX = r.readU16(); sp.velY = r.readU16(); sp.velZ = r.readU16();
    sp.ownerSlot = r.readU8(); sp.clientTickLow = r.readU16(); sp.expectedDamageQ = r.readU8();
}

// Read one SnapWorldItem from a PacketReader. MUST match writeSnapWorldItem.
static void readSnapWorldItem(PacketReader& r, SnapWorldItem& sw) {
    sw.slotIndex = r.readU8(); sw.rarity = r.readU8();
    sw.defId = r.readU16(); sw.uid = r.readU32();
    sw.posX = r.readU16(); sw.posY = r.readU16(); sw.posZ = r.readU16();
    sw.ownerSlot = r.readU8(); sw.exclusiveTimerQ = r.readU8();
    // D8 — rolled stats (matches writer).
    sw.damage      = r.readF32();
    sw.bonusHealth = r.readF32();
    sw.itemLevel   = r.readU8();
    u8 affixCount  = r.readU8();
    if (affixCount > MAX_AFFIXES_PER_ITEM) affixCount = MAX_AFFIXES_PER_ITEM;
    sw.affixCount = affixCount;
    for (u32 a = 0; a < affixCount; a++) {
        sw.affixes[a].type  = static_cast<AffixType>(r.readU8());
        sw.affixes[a].value = r.readF32();
    }
    // Tail-init unused slots so worldItemSlotsEqual's memcmp is deterministic.
    for (u32 a = affixCount; a < MAX_AFFIXES_PER_ITEM; a++) {
        sw.affixes[a] = Affix{};
    }
}

// ---------------------------------------------------------------------------
// D7.3.2 — Delta serialize
//
// Wire layout (no leading packet header — caller adds one if needed):
//   u32 serverTick
//   u32 lastProcessedInputTick[MAX_PLAYERS]
//   u8  isFullSnapshot = 0
//   u8  unchangedPlayersMask       (bits 0..MAX_PLAYERS-1)
//   u8[8] unchangedEntitiesMask    (64 bits, bit N = SnapEntity.poolIndex N unchanged)
//   u8[8] unchangedProjectilesMask (64 bits, poolIndex 0..63 only)
//   u8[8] unchangedWorldItemsMask  (64 bits, bit N = SnapWorldItem.slotIndex N unchanged)
//   u8  changedPlayerCount  + N × SNAP_PLAYER_WIRE
//   u8  changedEntityCount  + N × SNAP_ENTITY_WIRE
//   u16 changedProjectileCount + N × SNAP_PROJECTILE_WIRE
//   u8  changedWorldItemCount  + N × <variable> (26 B fixed + affixCount×5 B tail)
//
// The receiver starts with all baseline slots whose unchanged bit is set, then
// appends the changed records from the wire — producing a fully-populated
// WorldSnapshot without re-sending every static field each tick.
// ---------------------------------------------------------------------------

u32 Snapshot::serializeDelta(u8* outBuf, u32 outCap,
                              const WorldSnapshot& current, const WorldSnapshot& baseline) {
    u32 cursor = 0;

    // Bounds-checked inline write helpers (same pattern as serialize()).
    auto w8  = [&](u8 v)  { if (cursor + 1 <= outCap) outBuf[cursor++] = v; };
    auto w16 = [&](u16 v) { if (cursor + 2 <= outCap) { std::memcpy(outBuf + cursor, &v, 2); cursor += 2; } };
    auto w32 = [&](u32 v) { if (cursor + 4 <= outCap) { std::memcpy(outBuf + cursor, &v, 4); cursor += 4; } };

    // Header: tick + per-slot input acks + full/delta flag.
    w32(current.serverTick);
    for (u32 i = 0; i < MAX_PLAYERS; i++) w32(current.lastProcessedInputTick[i]);
    w8(0);  // isFullSnapshot = 0

    // --- Compute unchanged bitmasks by comparing against baseline via poolIndex ---

    u8 unchangedPlayers = 0;
    for (u8 i = 0; i < current.playerCount; i++) {
        u8 slot = current.players[i].slotIndex;
        if (slot >= MAX_PLAYERS) continue; // guard malformed slot
        const SnapPlayer* base = findPlayerByPoolIndex(baseline, slot);
        if (base && std::memcmp(base, &current.players[i], sizeof(SnapPlayer)) == 0) {
            unchangedPlayers |= static_cast<u8>(1u << slot);
        }
    }

    u8 unchangedEntities[8] = {};
    for (u8 i = 0; i < current.entityCount; i++) {
        u8 pi = current.entities[i].poolIndex;
        const SnapEntity* base = findEntityByPoolIndex(baseline, pi);
        if (base && std::memcmp(base, &current.entities[i], sizeof(SnapEntity)) == 0) {
            setBit64(unchangedEntities, pi);
        }
    }

    u8 unchangedProjectiles[8] = {};
    for (u16 i = 0; i < current.projectileCount; i++) {
        u16 pi = current.projectiles[i].poolIndex;
        if (pi >= 64) continue; // bitmask covers poolIndex 0..63; higher always included
        const SnapProjectile* base = findProjectileByPoolIndex(baseline, pi);
        if (base && std::memcmp(base, &current.projectiles[i], sizeof(SnapProjectile)) == 0) {
            setBit64(unchangedProjectiles, pi);
        }
    }

    u8 unchangedWorldItems[8] = {};
    for (u8 i = 0; i < current.worldItemCount; i++) {
        u8 si = current.worldItems[i].slotIndex;
        const SnapWorldItem* base = findWorldItemByPoolIndex(baseline, si);
        if (base && std::memcmp(base, &current.worldItems[i], sizeof(SnapWorldItem)) == 0) {
            setBit64(unchangedWorldItems, si);
        }
    }

    // Write masks to the wire.
    w8(unchangedPlayers);
    for (u32 i = 0; i < 8; i++) w8(unchangedEntities[i]);
    for (u32 i = 0; i < 8; i++) w8(unchangedProjectiles[i]);
    for (u32 i = 0; i < 8; i++) w8(unchangedWorldItems[i]);

    // --- Changed players ---
    u8 changedPlayerCount = 0;
    for (u8 i = 0; i < current.playerCount; i++) {
        u8 slot = current.players[i].slotIndex;
        if (slot < MAX_PLAYERS && (unchangedPlayers & (1u << slot))) continue;
        changedPlayerCount++;
    }
    w8(changedPlayerCount);
    for (u8 i = 0; i < current.playerCount; i++) {
        u8 slot = current.players[i].slotIndex;
        if (slot < MAX_PLAYERS && (unchangedPlayers & (1u << slot))) continue;
        writeSnapPlayer(outBuf, outCap, cursor, current.players[i]);
    }

    // --- Changed entities ---
    u8 changedEntityCount = 0;
    for (u8 i = 0; i < current.entityCount; i++) {
        u8 pi = current.entities[i].poolIndex;
        if (getBit64(unchangedEntities, pi)) continue;
        changedEntityCount++;
    }
    w8(changedEntityCount);
    for (u8 i = 0; i < current.entityCount; i++) {
        u8 pi = current.entities[i].poolIndex;
        if (getBit64(unchangedEntities, pi)) continue;
        writeSnapEntity(outBuf, outCap, cursor, current.entities[i]);
    }

    // --- Changed projectiles ---
    u16 changedProjectileCount = 0;
    for (u16 i = 0; i < current.projectileCount; i++) {
        u16 pi = current.projectiles[i].poolIndex;
        if (pi < 64 && getBit64(unchangedProjectiles, pi)) continue;
        changedProjectileCount++;
    }
    w16(changedProjectileCount);
    for (u16 i = 0; i < current.projectileCount; i++) {
        u16 pi = current.projectiles[i].poolIndex;
        if (pi < 64 && getBit64(unchangedProjectiles, pi)) continue;
        writeSnapProjectile(outBuf, outCap, cursor, current.projectiles[i]);
    }

    // --- Changed world items ---
    u8 changedWorldItemCount = 0;
    for (u8 i = 0; i < current.worldItemCount; i++) {
        u8 si = current.worldItems[i].slotIndex;
        if (getBit64(unchangedWorldItems, si)) continue;
        changedWorldItemCount++;
    }
    w8(changedWorldItemCount);
    for (u8 i = 0; i < current.worldItemCount; i++) {
        u8 si = current.worldItems[i].slotIndex;
        if (getBit64(unchangedWorldItems, si)) continue;
        writeSnapWorldItem(outBuf, outCap, cursor, current.worldItems[i]);
    }

    return cursor;
}

// ---------------------------------------------------------------------------
// D7.3.2 — Delta deserialize
//
// Reconstructs `out` by:
//   1. Seeding `out` with all baseline slots whose unchanged bit is set.
//   2. Appending changed records read from the wire payload.
// The result is a fully populated WorldSnapshot with the server tick updated.
// Returns false if the buffer is truncated or the isFullSnapshot byte is != 0
// (caller must dispatch to deserialize() for full snapshots).
// ---------------------------------------------------------------------------

bool Snapshot::deserializeDelta(WorldSnapshot& out, const u8* buf, u32 size,
                                  const WorldSnapshot& baseline) {
    PacketReader r;
    r.data   = buf;
    r.size   = size;
    r.cursor = 0;

    out.serverTick = r.readU32();
    for (u32 i = 0; i < MAX_PLAYERS; i++) out.lastProcessedInputTick[i] = r.readU32();

    u8 isFull = r.readU8();
    if (isFull != 0) return false; // caller should use deserialize() for full snapshots

    // Read unchanged bitmasks from the wire.
    u8 unchangedPlayers = r.readU8();
    u8 unchangedEntities[8];   for (u32 i = 0; i < 8; i++) unchangedEntities[i]    = r.readU8();
    u8 unchangedProjectiles[8];for (u32 i = 0; i < 8; i++) unchangedProjectiles[i] = r.readU8();
    u8 unchangedWorldItems[8]; for (u32 i = 0; i < 8; i++) unchangedWorldItems[i]  = r.readU8();

    // --- Seed out with unchanged baseline slots ---

    out.playerCount = 0;
    for (u8 i = 0; i < baseline.playerCount; i++) {
        u8 slot = baseline.players[i].slotIndex;
        if (slot < MAX_PLAYERS && (unchangedPlayers & (1u << slot))) {
            if (out.playerCount < MAX_PLAYERS)
                out.players[out.playerCount++] = baseline.players[i];
        }
    }

    out.entityCount = 0;
    for (u8 i = 0; i < baseline.entityCount; i++) {
        u8 pi = baseline.entities[i].poolIndex;
        if (getBit64(unchangedEntities, pi)) {
            if (out.entityCount < MAX_ENTITIES)
                out.entities[out.entityCount++] = baseline.entities[i];
        }
    }

    out.projectileCount = 0;
    for (u16 i = 0; i < baseline.projectileCount; i++) {
        u16 pi = baseline.projectiles[i].poolIndex;
        // Only baseline projectiles with poolIndex < 64 can have their unchanged bit set.
        if (pi < 64 && getBit64(unchangedProjectiles, pi)) {
            if (out.projectileCount < MAX_PROJECTILES)
                out.projectiles[out.projectileCount++] = baseline.projectiles[i];
        }
        // Projectiles with poolIndex >= 64 are always re-sent in the changed payload.
    }

    out.worldItemCount = 0;
    for (u8 i = 0; i < baseline.worldItemCount; i++) {
        u8 si = baseline.worldItems[i].slotIndex;
        if (getBit64(unchangedWorldItems, si)) {
            if (out.worldItemCount < MAX_WORLD_ITEMS)
                out.worldItems[out.worldItemCount++] = baseline.worldItems[i];
        }
    }

    // --- Append changed records from the wire payload ---

    u8 changedPlayers = r.readU8();
    if (changedPlayers > MAX_PLAYERS) return false; // guard against corrupt packet
    for (u8 i = 0; i < changedPlayers; i++) {
        if (out.playerCount >= MAX_PLAYERS) { r.cursor += SNAP_PLAYER_WIRE; continue; }
        SnapPlayer& sp = out.players[out.playerCount++];
        readSnapPlayer(r, sp);
        // Clamp playerClass for safety (mirrors the full deserializer's guard).
        if (sp.playerClass >= static_cast<u8>(PlayerClass::CLASS_COUNT))
            sp.playerClass = static_cast<u8>(PlayerClass::WARRIOR);
    }

    u8 changedEntities = r.readU8();
    if (changedEntities > MAX_ENTITIES) return false;
    for (u8 i = 0; i < changedEntities; i++) {
        if (out.entityCount >= MAX_ENTITIES) { r.cursor += SNAP_ENTITY_WIRE; continue; }
        readSnapEntity(r, out.entities[out.entityCount++]);
    }

    u16 changedProjectiles = r.readU16();
    if (changedProjectiles > MAX_PROJECTILES) return false;
    for (u16 i = 0; i < changedProjectiles; i++) {
        if (out.projectileCount >= MAX_PROJECTILES) { r.cursor += SNAP_PROJECTILE_WIRE; continue; }
        readSnapProjectile(r, out.projectiles[out.projectileCount++]);
    }

    u8 changedWorldItems = r.readU8();
    if (changedWorldItems > MAX_WORLD_ITEMS) return false;
    for (u8 i = 0; i < changedWorldItems; i++) {
        // World-item records are variable-length (the affix tail). We can't bump
        // r.cursor by a constant SNAP_WORLDITEM_WIRE to skip a record once our local
        // output array is full — read into a discard instead so the cursor advances by
        // the actual wire size.
        if (out.worldItemCount >= MAX_WORLD_ITEMS) { SnapWorldItem discard{}; readSnapWorldItem(r, discard); continue; }
        readSnapWorldItem(r, out.worldItems[out.worldItemCount++]);
    }

    return r.cursor <= r.size;
}
