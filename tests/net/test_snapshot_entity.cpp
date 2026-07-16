#include <doctest/doctest.h>
#include "net/snapshot.h"
#include "net/net.h"
#include "net/packet.h"  // MAX_SNAPSHOT_SIZE
#include "game/entity.h" // ENT_ACTIVE

// Wire-format regression test for the R9 isBoss bit. SnapEntity.bossStatus packs:
//   bit0 = minionShield
//   bits1-3 = bossPhase
//   bit4 = isBoss          ← shipped so the client's floorBossAlive() can identify
//                            the milestone boss in m_renderInterp.entities and drive
//                            the portal-locked / portal-unlocked color. Pre-R9 there
//                            was no path for the client to learn which entity was the
//                            boss, so the portal never turned green after a kill.
//   bits5-7 reserved.

TEST_CASE("SnapEntity.bossStatus: isBoss bit round-trips through full serialize") {
    WorldSnapshot snap{};
    snap.serverTick      = 1;
    snap.playerCount     = 0;
    snap.worldItemCount  = 0;
    snap.projectileCount = 0;
    snap.entityCount     = 1;

    SnapEntity& se = snap.entities[0];
    se.poolIndex = 0;
    se.flags     = static_cast<u8>(ENT_ACTIVE); // boss alive
    se.bossStatus = static_cast<u8>(1u << 4);   // R9 isBoss bit set, no shield, phase 0
    // Other fields zeroed — full-path serialize doesn't require them populated.

    u8 buf[MAX_SNAPSHOT_SIZE];
    u32 size = Snapshot::serialize(snap, buf, sizeof(buf), /*isFullSnapshot=*/1);
    REQUIRE(size > 0);

    WorldSnapshot out{};
    bool ok = Snapshot::deserialize(out, buf, size);
    REQUIRE(ok);
    REQUIRE(out.entityCount == 1);
    CHECK((out.entities[0].bossStatus & (1u << 4)) != 0); // isBoss survived the round trip
    CHECK((out.entities[0].bossStatus & 0x01) == 0);      // minionShield untouched
    CHECK(((out.entities[0].bossStatus >> 1) & 0x07) == 0); // bossPhase untouched
}

TEST_CASE("SnapEntity.bossStatus: non-boss entity leaves isBoss clear") {
    WorldSnapshot snap{};
    snap.serverTick      = 1;
    snap.entityCount     = 1;
    SnapEntity& se = snap.entities[0];
    se.poolIndex = 0;
    se.flags     = static_cast<u8>(ENT_ACTIVE);
    se.bossStatus = 0; // ordinary mob

    u8 buf[MAX_SNAPSHOT_SIZE];
    u32 size = Snapshot::serialize(snap, buf, sizeof(buf), /*isFullSnapshot=*/1);
    REQUIRE(size > 0);

    WorldSnapshot out{};
    REQUIRE(Snapshot::deserialize(out, buf, size));
    REQUIRE(out.entityCount == 1);
    CHECK((out.entities[0].bossStatus & (1u << 4)) == 0);
}

TEST_CASE("SnapEntity.bossStatus: isBoss survives alongside minionShield+bossPhase") {
    // Stress the bit layout — set every used field at once so a wrong shift count
    // would cross-contaminate them.
    WorldSnapshot snap{};
    snap.serverTick  = 1;
    snap.entityCount = 1;
    SnapEntity& se = snap.entities[0];
    se.poolIndex = 0;
    se.flags     = static_cast<u8>(ENT_ACTIVE);
    se.bossStatus = static_cast<u8>(0x01)                // minionShield
                  | static_cast<u8>((4u & 0x07) << 1)    // bossPhase = 4 (max value: bits 1-3 = 0b100)
                  | static_cast<u8>(1u << 4);            // isBoss

    u8 buf[MAX_SNAPSHOT_SIZE];
    u32 size = Snapshot::serialize(snap, buf, sizeof(buf), /*isFullSnapshot=*/1);
    REQUIRE(size > 0);

    WorldSnapshot out{};
    REQUIRE(Snapshot::deserialize(out, buf, size));
    REQUIRE(out.entityCount == 1);
    const u8 bs = out.entities[0].bossStatus;
    CHECK((bs & 0x01) == 0x01);            // minionShield preserved
    CHECK(((bs >> 1) & 0x07) == 4);        // bossPhase preserved
    CHECK((bs & (1u << 4)) != 0);          // isBoss preserved
}

// ---------------------------------------------------------------------------
// Netcode-audit 2026-07 — healthPct pack must be total.
//
// buildFromState's PLAYER path clamps the HP ratio to [0,1] before the u8 cast;
// the ENTITY path didn't. Every current mutation site happens to keep entity
// health inside [0, maxHealth] (killEntity clamps to 0 in the same tick, all
// heals clamp at max), so this is latent — but a float->u8 cast of an
// out-of-range value is UB, and the wire pack is the choke-point that must not
// depend on every present and future combat path remembering to clamp.
// ---------------------------------------------------------------------------

TEST_CASE("buildFromState: entity healthPct clamps negative and overheal health") {
    // Static: the pools are large (ProjectilePool alone is ~100 KB) — keep them off the stack.
    static NetPlayer      players[MAX_PLAYERS] = {};
    static EntityPool     entities;
    static ProjectilePool projectiles;
    static WorldItemPool  items;

    Entity& neg = entities.entities[0];
    neg.flags     = ENT_ACTIVE;
    neg.health    = -50.0f;    // hypothetical overkill that skipped the killEntity clamp
    neg.maxHealth = 100.0f;

    Entity& over = entities.entities[1];
    over.flags     = ENT_ACTIVE;
    over.health    = 150.0f;   // hypothetical unclamped overheal
    over.maxHealth = 100.0f;

    static WorldSnapshot snap;
    Snapshot::buildFromState(snap, 1, players, entities, projectiles, items);
    REQUIRE(snap.entityCount == 2);

    // buildFromState sorts entities nearest-player-first — look up by poolIndex, not position.
    const SnapEntity* e0 = Snapshot::findEntityByPoolIndex(snap, 0);
    const SnapEntity* e1 = Snapshot::findEntityByPoolIndex(snap, 1);
    REQUIRE(e0 != nullptr);
    REQUIRE(e1 != nullptr);
    CHECK(e0->healthPct == 0);     // negative HP floors at 0%
    CHECK(e1->healthPct == 255);   // overheal saturates at 100%
}

// v16: the old alignment pad became bossDefIdx — boss identity for guest nameplates.
// Entity.nameTag is a host-side pointer into the BossDef table and cannot replicate,
// so the def INDEX rides the wire and the client resolves the name from its own
// (identical) bosses.json. 0xFF = not a boss.
TEST_CASE("SnapEntity.bossDefIdx: boss identity round-trips through full serialize") {
    WorldSnapshot snap{};
    snap.serverTick  = 1;
    snap.entityCount = 2;

    SnapEntity& boss = snap.entities[0];
    boss.poolIndex  = 3;
    boss.flags      = static_cast<u8>(ENT_ACTIVE);
    boss.bossStatus = static_cast<u8>(1u << 4);  // isBoss
    boss.bossDefIdx = 7;                          // e.g. the floor-35 def

    SnapEntity& mob = snap.entities[1];
    mob.poolIndex  = 4;
    mob.flags      = static_cast<u8>(ENT_ACTIVE);
    mob.bossDefIdx = 0xFF;                        // ordinary monster — no boss def

    u8 buf[MAX_SNAPSHOT_SIZE];
    u32 size = Snapshot::serialize(snap, buf, sizeof(buf), /*isFullSnapshot=*/1);
    REQUIRE(size > 0);

    WorldSnapshot out{};
    REQUIRE(Snapshot::deserialize(out, buf, size));
    REQUIRE(out.entityCount == 2);
    CHECK(out.entities[0].bossDefIdx == 7);
    CHECK(out.entities[1].bossDefIdx == 0xFF);
}

// v16 delta path: bossDefIdx must also survive writeSnapEntity/readSnapEntity (the
// delta encoder ships whole changed slots through those two, not the inline loops).
TEST_CASE("SnapEntity.bossDefIdx: survives the delta encode/decode path") {
    WorldSnapshot baseline{};
    baseline.serverTick  = 10;
    baseline.entityCount = 1;
    baseline.entities[0].poolIndex  = 2;
    baseline.entities[0].flags      = static_cast<u8>(ENT_ACTIVE);
    baseline.entities[0].bossDefIdx = 0xFF;

    WorldSnapshot current = baseline;
    current.serverTick = 11;
    current.entities[0].bossDefIdx = 5;   // slot changed → delta must carry the new byte
    current.entities[0].healthPct  = 200;

    u8 buf[MAX_SNAPSHOT_SIZE];
    u32 size = Snapshot::serializeDelta(buf, sizeof(buf), current, baseline);
    REQUIRE(size > 0);

    WorldSnapshot out{};
    REQUIRE(Snapshot::deserializeDelta(out, buf, size, baseline));
    REQUIRE(out.entityCount == 1);
    CHECK(out.entities[0].bossDefIdx == 5);
}
