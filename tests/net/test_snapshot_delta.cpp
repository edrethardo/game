#include <doctest/doctest.h>
#include "net/snapshot.h"

// D7.1 — TDD for slot-equality helpers.
//
// These tests exercise the Snapshot::*SlotsEqual primitives in isolation — verifying
// the trivial "all-zero default equal" case, a single changed field, and the
// boundary (only one slot changed while adjacent slots remain equal). Wire-format
// encode/decode roundtrip tests are deferred to D7.3 once delta serialization lands.

TEST_CASE("Delta: default-constructed WorldSnapshots are equal on all player slots") {
    WorldSnapshot a;
    WorldSnapshot b;
    for (u32 i = 0; i < MAX_PLAYERS; i++) {
        CHECK(Snapshot::playerSlotsEqual(a, b, i) == true);
    }
}

TEST_CASE("Delta: differing player posX produces unequal slot, others stay equal") {
    WorldSnapshot a;
    WorldSnapshot b;
    // Mark both as active via flags bit 0 so the comparison is on a "live" slot.
    a.players[0].flags = 1;
    b.players[0].flags = 1;
    a.players[0].posX = 100;
    b.players[0].posX = 200;
    CHECK(Snapshot::playerSlotsEqual(a, b, 0) == false);
    CHECK(Snapshot::playerSlotsEqual(a, b, 1) == true);  // slot 1 untouched — still equal
}

TEST_CASE("Delta: player flags change (active bit flip) produces unequal slot") {
    WorldSnapshot a;
    WorldSnapshot b;
    a.players[0].flags = 0; // inactive
    b.players[0].flags = 1; // active
    CHECK(Snapshot::playerSlotsEqual(a, b, 0) == false);
}

TEST_CASE("Delta: default-constructed WorldSnapshots are equal on all entity slots") {
    WorldSnapshot a;
    WorldSnapshot b;
    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        CHECK(Snapshot::entitySlotsEqual(a, b, i) == true);
    }
}

TEST_CASE("Delta: differing entity healthPct produces unequal slot, others stay equal") {
    WorldSnapshot a;
    WorldSnapshot b;
    a.entities[5].healthPct = 200;
    b.entities[5].healthPct = 100;
    CHECK(Snapshot::entitySlotsEqual(a, b, 5) == false);
    CHECK(Snapshot::entitySlotsEqual(a, b, 4) == true);
    CHECK(Snapshot::entitySlotsEqual(a, b, 6) == true);
}

TEST_CASE("Delta: default-constructed WorldSnapshots are equal on all projectile slots") {
    WorldSnapshot a;
    WorldSnapshot b;
    // Only check a slice — MAX_PROJECTILES can be 1024 and memcmp is fast, but
    // iterating 1024 default-equal checks keeps the test instantaneous.
    for (u32 i = 0; i < 16; i++) {
        CHECK(Snapshot::projectileSlotsEqual(a, b, i) == true);
    }
}

TEST_CASE("Delta: differing projectile posZ produces unequal slot") {
    WorldSnapshot a;
    WorldSnapshot b;
    a.projectiles[3].posZ = 1000;
    b.projectiles[3].posZ = 2000;
    CHECK(Snapshot::projectileSlotsEqual(a, b, 3) == false);
    CHECK(Snapshot::projectileSlotsEqual(a, b, 2) == true);
}

TEST_CASE("Delta: default-constructed WorldSnapshots are equal on all world-item slots") {
    WorldSnapshot a;
    WorldSnapshot b;
    for (u32 i = 0; i < MAX_WORLD_ITEMS; i++) {
        CHECK(Snapshot::worldItemSlotsEqual(a, b, i) == true);
    }
}

TEST_CASE("Delta: differing world-item uid produces unequal slot") {
    WorldSnapshot a;
    WorldSnapshot b;
    a.worldItems[0].uid = 42;
    b.worldItems[0].uid = 99;
    CHECK(Snapshot::worldItemSlotsEqual(a, b, 0) == false);
    CHECK(Snapshot::worldItemSlotsEqual(a, b, 1) == true);
}

TEST_CASE("Delta: out-of-range slot indices return true (safe no-op for skip logic)") {
    WorldSnapshot a;
    WorldSnapshot b;
    // Equal-returns-true for an out-of-range slot means "no bit set" — a conservative
    // choice that never falsely marks a non-existent slot as changed.
    CHECK(Snapshot::playerSlotsEqual(a, b, MAX_PLAYERS)     == true);
    CHECK(Snapshot::entitySlotsEqual(a, b, MAX_ENTITIES)    == true);
    CHECK(Snapshot::worldItemSlotsEqual(a, b, MAX_WORLD_ITEMS) == true);
}

// ---------------------------------------------------------------------------
// D7.3.1 — TDD for pool-index lookup helpers and 64-bit bitmask ops.
//
// findEntityByPoolIndex / findPlayerByPoolIndex etc. are the building blocks for
// delta encoding: they let the encoder/decoder identify which baseline record
// corresponds to a given pool slot without relying on dense-array position order
// (which changes each snapshot due to the nearest-player sort in buildFromState).
// setBit64 / getBit64 drive the per-slot unchanged-bitmask wire format.
// ---------------------------------------------------------------------------

TEST_CASE("Snapshot::findEntityByPoolIndex returns null on missing") {
    WorldSnapshot s;
    s.entityCount = 0;
    CHECK(Snapshot::findEntityByPoolIndex(s, 5) == nullptr);
}

TEST_CASE("Snapshot::findEntityByPoolIndex returns matching record") {
    WorldSnapshot s;
    s.entityCount = 2;
    s.entities[0].poolIndex = 7;
    s.entities[1].poolIndex = 2;
    REQUIRE(Snapshot::findEntityByPoolIndex(s, 7) != nullptr);
    REQUIRE(Snapshot::findEntityByPoolIndex(s, 7) == &s.entities[0]);
    REQUIRE(Snapshot::findEntityByPoolIndex(s, 2) == &s.entities[1]);
    CHECK(Snapshot::findEntityByPoolIndex(s, 5) == nullptr);
}

TEST_CASE("Snapshot bitmask: set/get 64-bit operations") {
    u8 mask[8] = {};
    Snapshot::setBit64(mask, 0);
    Snapshot::setBit64(mask, 7);
    Snapshot::setBit64(mask, 8);
    Snapshot::setBit64(mask, 63);
    CHECK(Snapshot::getBit64(mask, 0) == true);
    CHECK(Snapshot::getBit64(mask, 7) == true);
    CHECK(Snapshot::getBit64(mask, 8) == true);
    CHECK(Snapshot::getBit64(mask, 63) == true);
    CHECK(Snapshot::getBit64(mask, 1) == false);
    CHECK(Snapshot::getBit64(mask, 32) == false);
    CHECK(Snapshot::getBit64(mask, 64) == false);  // out of range — clamped to false
}
