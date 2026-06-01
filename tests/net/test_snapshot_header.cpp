#include <doctest/doctest.h>
#include "net/snapshot.h"
#include "net/net.h"
#include "net/packet.h"  // MAX_SNAPSHOT_SIZE

// Wire-format regression tests for the packet-header flags byte (data[1]),
// where bit 0 carries isFullSnapshot. The previous wire format put this byte
// inside the snapshot payload at different offsets for full vs delta, which
// made client-side routing impossible without already knowing the layout —
// see Client::receiveSnapshot. The bit now lives in the packet header so
// both layouts agree on its location.

TEST_CASE("Snapshot header: full snapshot sets flags bit 0 (isFullSnapshot)") {
    WorldSnapshot snap;
    snap.serverTick      = 42;
    snap.playerCount     = 0;
    snap.entityCount     = 0;
    snap.worldItemCount  = 0;
    snap.projectileCount = 0;

    u8 buf[MAX_SNAPSHOT_SIZE];
    u32 size = Snapshot::serialize(snap, buf, sizeof(buf), /*isFullSnapshot=*/1);
    REQUIRE(size >= 2);
    // Packet header: data[0] is the type, data[1] is flags. Bit 0 of flags = isFullSnapshot.
    CHECK(buf[0] == static_cast<u8>(NetPacketType::SV_SNAPSHOT));
    CHECK((buf[1] & 0x01) == 0x01);
}

TEST_CASE("Snapshot header: non-full serialize clears flags bit 0") {
    // Snapshot::serialize is also used for the broadcast path; the in-payload
    // byte is preserved for backward consumers, but the header bit must mirror
    // it so the client routes correctly regardless of which path produced the
    // packet.
    WorldSnapshot snap;
    snap.serverTick      = 7;
    snap.playerCount     = 0;
    snap.entityCount     = 0;
    snap.worldItemCount  = 0;
    snap.projectileCount = 0;

    u8 buf[MAX_SNAPSHOT_SIZE];
    u32 size = Snapshot::serialize(snap, buf, sizeof(buf), /*isFullSnapshot=*/0);
    REQUIRE(size >= 2);
    CHECK((buf[1] & 0x01) == 0x00);
}

TEST_CASE("Snapshot header: flags byte's upper bits stay zero (reserved)") {
    // The packet-header flags byte uses bit 0 for isFullSnapshot. Bits 1-7 are
    // reserved and must remain 0 so future flag additions don't silently
    // collide with existing consumers.
    WorldSnapshot snap;
    snap.serverTick = 1;
    snap.playerCount = 0;
    snap.entityCount = 0;
    snap.worldItemCount = 0;
    snap.projectileCount = 0;

    u8 buf[MAX_SNAPSHOT_SIZE];
    u32 size = Snapshot::serialize(snap, buf, sizeof(buf), /*isFullSnapshot=*/1);
    REQUIRE(size >= 2);
    CHECK((buf[1] & 0xFE) == 0x00);
}
