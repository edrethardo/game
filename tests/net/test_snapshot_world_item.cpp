// D8 — round-trip tests for the SnapWorldItem rolled-stat fields (damage, bonusHealth,
// itemLevel, affixCount, affixes[]). These exist because client-pickup prediction (D4.2)
// reads wi.item.damage directly from Client::mirrorWorldItems, and the inventory HUD
// shows whatever damage the mirror put there. If the wire layout drops a field — or
// the writer/reader disagree by one byte — every multiplayer-client pickup reverts to
// "Damage: 0", which is the user-visible bug this layer must guard against.

#include <doctest/doctest.h>
#include "net/snapshot.h"
#include "net/packet.h"           // MAX_SNAPSHOT_SIZE
#include "game/item.h"            // Affix, AffixType, MAX_AFFIXES_PER_ITEM

TEST_CASE("D8 SnapWorldItem: full-snapshot serialize/deserialize round-trips rolled stats") {
    // Build a snapshot with one world item carrying a representative set of rolled
    // stats — damage in the SMG range, bonusHealth in the armor-affix range, level 12,
    // and two affixes covering both DAMAGE_FLAT (whole-number) and HEALTH_FLAT (use
    // an explicitly fractional value below) so we catch f32 truncation bugs — e.g.
    // accidentally promoting through an integer type on the wire would zero the .25.
    WorldSnapshot snap;
    snap.serverTick = 7;
    snap.worldItemCount = 1;
    snap.worldItems[0].slotIndex       = 3;
    snap.worldItems[0].rarity          = static_cast<u8>(Rarity::MAGIC);
    snap.worldItems[0].defId           = 17;
    snap.worldItems[0].uid             = 0xA1B2C3D4;
    snap.worldItems[0].posX            = Quantize::packPos(4.0f);
    snap.worldItems[0].posY            = Quantize::packPos(0.5f);
    snap.worldItems[0].posZ            = Quantize::packPos(-2.0f);
    snap.worldItems[0].ownerSlot       = 1;
    snap.worldItems[0].exclusiveTimerQ = 75;
    snap.worldItems[0].damage          = 37.5f;
    snap.worldItems[0].bonusHealth     = 12.25f;
    snap.worldItems[0].itemLevel       = 12;
    snap.worldItems[0].affixCount      = 2;
    snap.worldItems[0].affixes[0].type  = AffixType::DAMAGE_FLAT;
    snap.worldItems[0].affixes[0].value = 5.0f;
    snap.worldItems[0].affixes[1].type  = AffixType::HEALTH_FLAT;
    snap.worldItems[0].affixes[1].value = 0.25f;

    // Round-trip through the full serializer.
    u8 buf[MAX_SNAPSHOT_SIZE] = {};
    u32 n = Snapshot::serialize(snap, buf, sizeof(buf), /*isFullSnapshot=*/1);
    REQUIRE(n > 0);

    WorldSnapshot decoded;
    REQUIRE(Snapshot::deserialize(decoded, buf, n));

    REQUIRE(decoded.worldItemCount == 1);
    const SnapWorldItem& dw = decoded.worldItems[0];

    // Fixed-prefix sanity (regression cover for the original 16-byte layout).
    CHECK(dw.slotIndex == 3);
    CHECK(dw.rarity    == static_cast<u8>(Rarity::MAGIC));
    CHECK(dw.defId     == 17);
    CHECK(dw.uid       == 0xA1B2C3D4u);
    CHECK(dw.ownerSlot == 1);
    CHECK(dw.exclusiveTimerQ == 75);

    // D8 — rolled stats. doctest::Approx because f32 round-trips bit-exactly here but
    // future quantization changes would still want a tolerance-based check.
    CHECK(dw.damage      == doctest::Approx(37.5f));
    CHECK(dw.bonusHealth == doctest::Approx(12.25f));
    CHECK(dw.itemLevel   == 12);
    CHECK(dw.affixCount  == 2);
    CHECK(dw.affixes[0].type  == AffixType::DAMAGE_FLAT);
    CHECK(dw.affixes[0].value == doctest::Approx(5.0f));
    CHECK(dw.affixes[1].type  == AffixType::HEALTH_FLAT);
    CHECK(dw.affixes[1].value == doctest::Approx(0.25f));
    // Unused affix slots stay at the Affix{} default so worldItemSlotsEqual's memcmp
    // is deterministic across encode/decode boundaries.
    CHECK(dw.affixes[2].value == doctest::Approx(0.0f));
    CHECK(dw.affixes[3].value == doctest::Approx(0.0f));
}

TEST_CASE("D8 SnapWorldItem: zero-affix item round-trips with the minimum wire size") {
    // The MIN size code path (no affixes) is the common case for low-tier loot; it
    // must round-trip cleanly even though the affix loop has zero iterations.
    WorldSnapshot snap;
    snap.worldItemCount = 1;
    snap.worldItems[0].slotIndex = 0;
    snap.worldItems[0].defId     = 5;
    snap.worldItems[0].uid       = 42;
    snap.worldItems[0].damage    = 6.0f;
    snap.worldItems[0].itemLevel = 1;
    snap.worldItems[0].affixCount = 0;

    u8 buf[MAX_SNAPSHOT_SIZE] = {};
    u32 n = Snapshot::serialize(snap, buf, sizeof(buf), 1);
    REQUIRE(n > 0);

    WorldSnapshot decoded;
    REQUIRE(Snapshot::deserialize(decoded, buf, n));

    REQUIRE(decoded.worldItemCount == 1);
    CHECK(decoded.worldItems[0].defId      == 5);
    CHECK(decoded.worldItems[0].uid        == 42u);
    CHECK(decoded.worldItems[0].damage     == doctest::Approx(6.0f));
    CHECK(decoded.worldItems[0].itemLevel  == 1);
    CHECK(decoded.worldItems[0].affixCount == 0);
}

TEST_CASE("D8 SnapWorldItem: delta round-trip carries the rolled stats on a changed slot") {
    // The delta serializer routes a changed world item through writeSnapWorldItem /
    // readSnapWorldItem (separate code path from the full serialize loop above). This
    // case proves both wire-path families agree.
    WorldSnapshot baseline;
    baseline.serverTick = 100;
    // Baseline has no world items — the new pickup is a fresh slot, guaranteed
    // "changed" by the per-slot equality check.

    WorldSnapshot current;
    current.serverTick = 101;
    current.worldItemCount = 1;
    current.worldItems[0].slotIndex   = 5;
    current.worldItems[0].defId       = 9;
    current.worldItems[0].uid         = 1234;
    current.worldItems[0].damage      = 23.0f;
    current.worldItems[0].bonusHealth = 7.5f;
    current.worldItems[0].itemLevel   = 4;
    current.worldItems[0].affixCount  = 1;
    current.worldItems[0].affixes[0].type  = AffixType::DAMAGE_PCT;
    current.worldItems[0].affixes[0].value = 15.0f;

    u8 buf[4096] = {};
    u32 size = Snapshot::serializeDelta(buf, sizeof(buf), current, baseline);
    REQUIRE(size > 0);

    WorldSnapshot decoded;
    REQUIRE(Snapshot::deserializeDelta(decoded, buf, size, baseline));

    REQUIRE(decoded.worldItemCount == 1);
    const SnapWorldItem& dw = decoded.worldItems[0];
    CHECK(dw.slotIndex == 5);
    CHECK(dw.defId == 9);
    CHECK(dw.damage      == doctest::Approx(23.0f));
    CHECK(dw.bonusHealth == doctest::Approx(7.5f));
    CHECK(dw.itemLevel   == 4);
    CHECK(dw.affixCount  == 1);
    CHECK(dw.affixes[0].type  == AffixType::DAMAGE_PCT);
    CHECK(dw.affixes[0].value == doctest::Approx(15.0f));
}

TEST_CASE("D8 SnapWorldItem: deserialize clamps a corrupt affixCount to MAX_AFFIXES_PER_ITEM") {
    // A byzantine / corrupt packet that claims affixCount > MAX must not index past
    // the destination affix array. We synthesize the over-claim by setting affixCount
    // on the source (which the writer's own clamp also caps), but the receiver's clamp
    // is the safety net we test here — even if a future writer bug skipped its clamp,
    // the reader stays in-bounds.
    WorldSnapshot snap;
    snap.worldItemCount = 1;
    snap.worldItems[0].defId      = 1;
    snap.worldItems[0].affixCount = MAX_AFFIXES_PER_ITEM + 7;  // intentional over-claim
    // Fill all writable slots so the writer at least has data for the clamp's worth.
    for (u32 a = 0; a < MAX_AFFIXES_PER_ITEM; a++) {
        snap.worldItems[0].affixes[a].type  = AffixType::DAMAGE_FLAT;
        snap.worldItems[0].affixes[a].value = static_cast<f32>(a + 1);
    }

    u8 buf[MAX_SNAPSHOT_SIZE] = {};
    u32 n = Snapshot::serialize(snap, buf, sizeof(buf), 1);
    REQUIRE(n > 0);

    WorldSnapshot decoded;
    REQUIRE(Snapshot::deserialize(decoded, buf, n));

    CHECK(decoded.worldItems[0].affixCount == MAX_AFFIXES_PER_ITEM);
}
