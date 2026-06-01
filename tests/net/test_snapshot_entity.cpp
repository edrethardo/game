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
