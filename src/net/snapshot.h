#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/entity.h"
#include "game/projectile.h"
#include "game/item.h"     // MAX_WORLD_ITEMS, WorldItemPool (world-item replication)
#include "net/net.h"
#include "net/net_player.h"

#include <cstring>  // std::memset in WorldSnapshot default constructor

// Bytes of the per-entity "unchanged" bitmask on the wire, one bit per pool slot. DERIVED from
// MAX_ENTITIES so the two can never drift (192 -> 24 B); a mask narrower than the pool would
// silently resend every high-index entity forever.
static constexpr u32 ENTITY_MASK_BYTES = (MAX_ENTITIES + 7) / 8;

// Quantized snapshot of one player (64 wire bytes — see SNAP_PLAYER_WIRE; was 30 pre-R17,
// +28 for the seven u32 lastActivationTick fields, +4 for armorMeshId[4], +2 for
// shrineTimerQ + reserved0 at the tail)
struct SnapPlayer {
    u8   slotIndex;     // 1
    u8   flags;         // 1: bit0=active, bit1=onGround, bit2=stunned (PvP action-lock; timer in stunTimerQ), bit3=reloading, bit4=blocking (bits5-7=Static Charge stacks; isDead rides animFlags bit2)
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
    // bits 5-6 now carry the shrine-buff TYPE (ShrineBuff::, 0-3) — see shrineTimerQ below.
    u8   statusFlags;   // 1: bit0=invuln, bit1=poisoned, bit2=burning, bit3=frozen, bit4=slowed,
                        //    bits5-7=shrineBuff type (0-7)
    u8   invulnTimer;   // 1: quantized 0-10s in 0.04s steps
    u8   poisonTimer;   // 1: quantized
    u8   burnTimer;     // 1: quantized
    u8   freezeTimer;   // 1: quantized
    // Animation state for remote player rendering
    u8   animFlags;     // 1: bit0=attacking (cooldown active), bit1=reloading, bit2=isDead
    u8   weaponMeshId;  // 1: mesh ID of equipped weapon (for third-person rendering)
    u8   armorMeshId[4]; // 4: helmet, chest, boots, gloves tier-mesh ids (0 = empty). 3rd-person rendering.
    // Wanderer dodge state for remote rendering: bit0=rolling, bits1-3=counterStacks (0-5)
    u8   dodgeFlags;    // 1
    // Visual identity: the chosen PlayerClass (cast to u8). The client's NetPlayer.playerClass
    // is only set on the local slot (CL_JOIN_REQUEST → server's onPlayerJoin), so without this
    // remote players render with the default WARRIOR mesh until the wire carries it. Sent every
    // snapshot rather than once-on-join so a late-joining observer or post-reconnect client
    // converges to the right mesh without a side-channel.
    u8   playerClass;   // 1: PlayerClass cast to u8 (0..CLASS_COUNT-1)
    // R17 — belt-and-suspenders cooldown sync. Authoritative lastActivationTick
    // (in the OWNING PLAYER's clientTick frame — i.e. each remote client's own
    // m_clientTick for its slot, m_serverTick for the host's slot). Client
    // adopts MAX(local, snapshot) in Client::reconcile so a benign client
    // over-prediction (server rejected for non-cooldown reasons → server didn't
    // bump its lastActivationTick) is never under-gated. Sentinel 0 = never
    // activated. 28 bytes per player per snapshot — at 60 Hz × 4 players,
    // ~6.7 KB/s overhead within the netcode budget.
    u32  classSkillLastActivationTick[4];  // 16
    u32  bootSkillLastActivationTick;      //  4
    u32  helmetSkillLastActivationTick;    //  4
    u32  potionLastActivationTick;         //  4
    // Shrine buff remaining, quantized to 0-51 s in 0.2 s steps. WITHOUT this a client would never
    // learn it had a shrine buff at all: the server grants it onto the authoritative NetPlayer, so
    // the client's local Player would keep predicting BASE speed while the server moved it faster —
    // rubber-banding it forward every tick. The type rides in statusFlags bits 5-6; the magnitude is
    // a constant per type (Shrine::bonusFor), so it needs no bytes.
    u8   shrineTimerQ;  // 1
    // Player-facing stun remaining (PvP action-lock), quantized 0-10s in 0.04s steps (same scale as
    // invulnTimer). Was `reserved0`. WITHOUT this a stunned CLIENT would never learn it is stunned:
    // the server locks the authoritative NetPlayer, but the client keeps predicting free movement and
    // rubber-bands every tick. The client adopts this (flags bit5 = stunned) so its own input-lock
    // engages the same frame — the RL-feel predictive+reconciled path. Constant per event + decaying,
    // so ack-driven delta compression sends it ~once per stun, not every tick.
    u8   stunTimerQ;    // 1 (was reserved0 — always 0)
};

// Quantized snapshot of one entity (32 bytes — see SNAP_ENTITY_WIRE)
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
    // 0-4 fits in 3 bits); bit4 = isBoss (R9: required so the client can tell which
    // interpolated entity is the milestone boss for the portal-locked check, since
    // Entity.isBoss is otherwise not shipped). Bits 5-7 reserved.
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
    // Champion affix bitmask (ChampAffix::, game/champion.h). The client derives the champion's
    // tint from this ALONE (Champion::tintFor is a pure function of the mask) — so the tell can
    // never depend on host-only state. Entity.hasAuraBuff is the cautionary tale: it drives a tint
    // and is NOT replicated, so that tell is invisible to every guest.
    // Constant for an entity's lifetime, and snapshots are delta-encoded, so it costs one byte once.
    // (ENT_CHAMPION itself rides in `flags`, which is already copied verbatim.)
    u8   champAffixes;  // 1
    // The rolled champion name INDEX (Champion::formatName rebuilds "Grimfang the Molten" from this
    // plus champAffixes — both pure, so host and guest always agree on what a champion is called).
    // This byte was previously an explicit pad: SnapEntity contains u16s and so aligns to 2, and the
    // pad is what keeps `sizeof == SNAP_ENTITY_WIRE` true — the canary that catches a field added to
    // the struct but forgotten in one of the FOUR (de)serializers. Giving the pad a job costs nothing
    // and needs no protocol bump. Constant per entity, so delta encoding sends it once.
    u8   champNameIdx;  // 1
    // Which authored monster this is (index into EnemyDefTable; 0xFF = not an enemy def). EnemyType
    // is only the RIG — 38 monsters share ~16 of them — so without this a guest (and the target bar)
    // could only ever say "Skeleton" for a Bone Archer, a Bone Mage and a Demon Caster alike.
    // Constant per entity, so delta encoding sends it once.
    u8   enemyDefIdx;   // 1
    // Which BossDef this boss is (index into Engine::m_bossDefs; 0xFF = not a boss). Entity.nameTag
    // is a host-side const char* into the def table and cannot replicate, so without this byte a
    // guest's nameplate could only ever say "Boss" — never "The Butcher". Constant per entity, so
    // delta encoding sends it once. (This byte was the struct's explicit alignment pad until it
    // earned a job, exactly like champNameIdx before it: SnapEntity contains u16s and so aligns
    // to 2 — the `sizeof(SnapEntity) == SNAP_ENTITY_WIRE` static_assert in snapshot.cpp is the
    // canary that catches a field added here but forgotten in one of the FOUR (de)serializers.)
    u8   bossDefIdx;    // 1
};

// Quantized snapshot of one projectile (22 bytes — see SNAP_PROJECTILE_WIRE)
struct SnapProjectile {
    u16  poolIndex;     // 2: u16 index into the projectile pool (1024 PC / 512 Switch)
    u8   flags;         // 1: bit0=active, bit1=fromPlayer, bit2=isCrit
    u8   projFlags;     // 1: PROJ_ORB/ORB_SHARD/GRAVITY/SPLASH/SPARK/VOID — drives skill VFX
    u8   meshId;        // 1: weapon mesh to render (0 = procedural energy bolt)
    u8   radiusQ;       // 1: radius quantized to 0-2.55 m in 0.01 m steps
    u16  posX, posY, posZ;  // 6
    u16  velX, velY, velZ;  // 6
    u8   ownerSlot;     // 1: net slot that fired this projectile (0xFF = unattributed)
    // Low 16 bits of the FIRING CLIENT's m_clientTick at fire moment (M1.8: was m_serverTick;
    // carried through CL_FIRE_WEAPON → server-stored on Projectile.clientTick → snapshotted
    // back). Used by the firing client to find and despawn its locally-predicted ghost
    // projectile when the authoritative one arrives. 16 bits suffice — a predicted ghost lives
    // < 0.5 s and the 16-bit tick window is ~18 min at 60 Hz, so collisions are practically
    // impossible. 0 = no prediction owner (host-fired, NPC projectile, skill orb) — skipped.
    u16  clientTickLow; // 2
    // D3.1 — Expected damage carried from server Projectile.damage so the client can predict
    // local HP decrement on incoming-projectile impact (D3.2). Quantized: multiply by 2 to pack,
    // multiply by 0.5 to unpack. Covers 0–127.5 dmg in 0.5-step increments; clamp 128+ to 255.
    // Most enemy hits are 5–30 dmg, so 0.5-step resolution is imperceptible. (1 byte vs 4 for f32.)
    u8   expectedDamageQ; // 1
};

// Quantized snapshot of one dropped world item. Loot is server-authoritative (N5):
// only the host/SP rolls drops; clients mirror this list into their local
// m_worldItems for rendering and pickup requests. The rolled stats (damage, affixes,
// itemLevel, bonusHealth) are on the wire so the client's snapshot-mirrored
// ItemInstance carries the real authoritative values — without them, the D4.2
// client-side pickup prediction adds a zero-damage / no-affix copy into the bag
// and the inventory UI shows "Damage: 0".
//
// Wire size: 16 B fixed prefix + 4 B damage + 4 B bonusHealth + 1 B itemLevel
//           + 1 B affixCount + N*5 B affixes = 26..46 B per item (N ∈ [0,4]).
struct SnapWorldItem {
    u8   slotIndex;     // 1: index into WorldItemPool (0..MAX_WORLD_ITEMS-1) — client mirrors directly
    u8   rarity;        // 1: Rarity enum value
    u16  defId;         // 2: item definition id (or GLOBE_* sentinel)
    u32  uid;           // 4: unique instance id — pickup requests reference this (full u32; no overflow)
    u16  posX, posY, posZ; // 6: position packed via Quantize::packPos
    u8   ownerSlot;     // 1: exclusive-pickup owner (0xFF = FFA) — drives client pickup gate (Audit-B)
    u8   exclusiveTimerQ; // 1: exclusive window quantized to 0-10.2 s in 0.04 s steps

    // Rolled-stat replication for D4.2 pickup prediction. Raw f32 — quantization
    // would invite per-affix tuning and silent rounding bugs, and the wire delta
    // (D7.3.2) zeros out unchanged world items by memcmp so a stable item costs
    // nothing per frame after first transmission.
    f32  damage;        // 4: ItemInstance.damage (post-roll, post-level-scale)
    f32  bonusHealth;   // 4: ItemInstance.bonusHealth (defensive items)
    u8   itemLevel;     // 1: ItemInstance.itemLevel (level-scaling tier, 0..255)
    u8   affixCount;    // 1: number of valid entries in affixes[] (clamped on read)
    // Full fixed-size affix array — only `affixCount` entries are on the wire, but the
    // unused tail is zero-initialised (WorldSnapshot ctor memsets worldItems[]), keeping
    // the memcmp-based slot-equality check (worldItemSlotsEqual) deterministic.
    Affix affixes[MAX_AFFIXES_PER_ITEM];
};

// Full world snapshot
struct WorldSnapshot {
    u32  serverTick       = 0;
    u32  lastProcessedInputTick[MAX_PLAYERS] = {};  // per-slot ACK of newest input the
                                                    // server has applied. Clients read
                                                    // this in M3 to replay only inputs
                                                    // newer than the ACK.
    u8   playerCount      = 0;
    u8   entityCount      = 0;
    u8   worldItemCount   = 0;  // dropped loot count (<= MAX_WORLD_ITEMS)
    u16  projectileCount  = 0;  // supports up to 4096

    SnapPlayer     players[MAX_PLAYERS];
    SnapEntity     entities[MAX_ENTITIES];
    SnapWorldItem  worldItems[MAX_WORLD_ITEMS];
    // Heap-allocated projectile array — supports full MAX_PROJECTILES without
    // bloating BSS (1024 × 18 bytes = 18KB per snapshot on PC; 512 × 18 = 9KB on Switch).
    SnapProjectile* projectiles = nullptr;

    WorldSnapshot() {
        // Zero-initialise all stack arrays so that memcmp-based slot equality helpers
        // (D7.1) produce deterministic results: uninitialized padding bytes in
        // independently-constructed snapshots can differ, causing spurious "changed"
        // readings.  The heap array uses the () value-init form for the same reason.
        std::memset(players,   0, sizeof(players));
        std::memset(entities,  0, sizeof(entities));
        std::memset(worldItems, 0, sizeof(worldItems));
        projectiles = new SnapProjectile[MAX_PROJECTILES]();
    }
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
        for (u32 i = 0; i < MAX_PLAYERS; i++) lastProcessedInputTick[i] = o.lastProcessedInputTick[i];
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
    // isFullSnapshot controls the wire header flag (D7.2). For v1 always pass 1.
    u32 serialize(const WorldSnapshot& snap, u8* outData, u32 maxSize,
                  u8 isFullSnapshot = 1);

    // Deserialize snapshot from packet bytes.
    bool deserialize(WorldSnapshot& snap, const u8* data, u32 size);

    // D7.3 — Delta encode/decode. serializeDelta encodes `current` as a delta against
    // `baseline` (wire: header + masks + changed-only records). Does NOT include a packet
    // header — the caller wraps in PacketWriter if needed; the returned buffer starts with
    // serverTick. Returns byte count written; 0 on buffer overflow.
    // deserializeDelta reconstructs `out` by merging baseline unchanged slots with the
    // changed-record payload. Returns false on a truncated or malformed buffer.
    // Wire format (after isFullSnapshot=0):
    //   4 B baselineTick — the serverTick of the snapshot this delta is encoded against
    //   1 B unchangedPlayersMask (bit-per-slot, slots 0-3)
    //  ENTITY_MASK_BYTES unchangedEntitiesMask  (bit N = poolIndex N unchanged — full MAX_ENTITIES)
    //   8 B unchangedProjectilesMask (64 bits, poolIndex < 64 only; higher always included)
    //   8 B unchangedWorldItemsMask  (64 bits, bit N = slotIndex N unchanged)
    //   then count-prefixed changed records per pool (same per-record layout as full serialize)
    u32  serializeDelta  (u8* outBuf, u32 outCap,
                          const WorldSnapshot& current, const WorldSnapshot& baseline);
    bool deserializeDelta(WorldSnapshot& out, const u8* buf, u32 size,
                          const WorldSnapshot& baseline);

    // D7.1 — Per-slot equality helpers used by delta-encoding to decide which slots
    // have changed since the last baseline snapshot.  Each compares the slot struct
    // fields byte-for-byte via memcmp (safe because all snapshot structs are
    // zero-initialised by WorldSnapshot's default constructor, so padding bytes
    // are deterministically 0).  Out-of-range indices return true ("equal") so
    // callers can treat them as "no change" without special-casing.
    bool playerSlotsEqual    (const WorldSnapshot& a, const WorldSnapshot& b, u32 slot);
    bool entitySlotsEqual    (const WorldSnapshot& a, const WorldSnapshot& b, u32 slot);
    bool projectileSlotsEqual(const WorldSnapshot& a, const WorldSnapshot& b, u32 slot);
    bool worldItemSlotsEqual (const WorldSnapshot& a, const WorldSnapshot& b, u32 slot);

    // D7.3.1 — Pool-index lookup helpers. Each scans the snapshot's count-bounded dense
    // array for the record whose identity field (slotIndex / poolIndex) matches the
    // caller's key. Returns a const pointer into the snapshot's array on hit, nullptr on
    // miss. Linear scan is safe because pools are small (≤4 players, ≤64 entities, etc.).
    const SnapPlayer*     findPlayerByPoolIndex   (const WorldSnapshot& s, u8  slotIndex);
    const SnapEntity*     findEntityByPoolIndex   (const WorldSnapshot& s, u8  poolIndex);
    const SnapProjectile* findProjectileByPoolIndex(const WorldSnapshot& s, u16 poolIndex);
    const SnapWorldItem*  findWorldItemByPoolIndex (const WorldSnapshot& s, u8  slotIndex);

    // D7.3.1 — 64-bit bitmask helpers over a u8[8] array.
    // bit values 0-63 map to byte[bit/8] bit(bit%8). Out-of-range (>=64) are no-ops/false.
    void setBit64(u8* mask, u32 bit);
    bool getBit64(const u8* mask, u32 bit);
    // Entity-mask twins. The width is DERIVED from MAX_ENTITIES, never hardcoded: a mask narrower
    // than the pool silently no-ops for the high slots (they can never be marked unchanged, so they
    // are resent every tick), which is exactly the bug the 64-bit mask had over a 128 pool. Bump
    // MAX_ENTITIES and this follows automatically — but it is WIRE layout, so bump PROTOCOL_VERSION.
    void setBitEnt(u8* mask, u32 bit);
    bool getBitEnt(const u8* mask, u32 bit);
}
