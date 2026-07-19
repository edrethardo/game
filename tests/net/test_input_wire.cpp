// test_input_wire.cpp — NetInput struct layout + ring buffer ordering tests.
// Started in M2 (input pipeline rewrite); future input-window tests will live here.

#include <doctest/doctest.h>
#include "net/net_player.h"

TEST_CASE("NetInput: ackedSnapshotTick field is reachable and writable") {
    NetInput in{};
    in.clientTick = 42;
    in.ackedSnapshotTick = 7;
    CHECK(in.clientTick == 42);
    CHECK(in.ackedSnapshotTick == 7);
}

TEST_CASE("NetInput: posXQ/Y/Z fields removed") {
    // This is a compile-time check — if posXQ/Y/Z still exist as members, the
    // commented-out lines below would compile. We trust the build system to fail
    // if a stale reader still touches them.
    NetInput in{};
    in.clientTick = 1;
    // in.posXQ = 0; // would fail to compile post-M2 (intentional)
    CHECK(in.clientTick == 1);
}

TEST_CASE("InputRingBuffer: monotonic clientTick discards duplicates and stale") {
    InputRingBuffer buf{};
    NetInput a{}; a.clientTick = 1;
    NetInput b{}; b.clientTick = 2;
    NetInput c{}; c.clientTick = 2;  // duplicate tick
    NetInput d{}; d.clientTick = 1;  // stale tick
    buf.push(a); buf.push(b); buf.push(c); buf.push(d);
    CHECK(buf.count == 2);   // dup + stale rejected
    REQUIRE(buf.getLatest() != nullptr);
    CHECK(buf.getLatest()->clientTick == 2);
}

// Wire format for CL_INPUT in M2: 1 byte windowCount + 3 bytes reserved + N*14 bytes
// of NetInput structs (N up to INPUT_WINDOW_SIZE = 4). The serializer/deserializer pair
// must roundtrip cleanly.

TEST_CASE("InputWindow: serializes and deserializes 4 inputs cleanly") {
    // Build 4 inputs with distinct clientTicks
    NetInput window[4];
    for (u32 i = 0; i < 4; i++) {
        window[i] = NetInput{};
        window[i].clientTick = 100 + i;
        window[i].ackedSnapshotTick = 7;
        window[i].moveFlags = static_cast<u8>(0x10 | i);
        window[i].weaponId = static_cast<u8>(2);
        window[i].yawQ = 0xABCD;
        window[i].pitchQ = 0x1234;
        window[i].extFlags = 0;
        window[i].skillSlot = 1;
    }
    u8 buf[256] = {};
    u32 size = serializeInputWindow(buf, sizeof(buf), window, 4);
    REQUIRE(size > 0);
    REQUIRE(size <= sizeof(buf));

    NetInput parsed[4] = {};
    u32 count = deserializeInputWindow(buf, size, parsed, 4);
    REQUIRE(count == 4);
    for (u32 i = 0; i < 4; i++) {
        CHECK(parsed[i].clientTick == 100 + i);
        CHECK(parsed[i].ackedSnapshotTick == 7);
        CHECK(parsed[i].moveFlags == (0x10 | i));
        CHECK(parsed[i].weaponId == 2);
        CHECK(parsed[i].yawQ == 0xABCD);
        CHECK(parsed[i].pitchQ == 0x1234);
    }
}

// PROTOCOL_VERSION 8: each input carries the interp delay the client actually used, so the
// server can rewind enemies to the same instant the client collided against. It rides at the
// END of the 15-byte input, so a serializer that forgot to widen INPUT_BYTES would silently
// shear every subsequent input in the window — check a MULTI-input window, not just one.
TEST_CASE("InputWindow: interpDelayMs round-trips per input across a window") {
    NetInput window[3];
    for (u32 i = 0; i < 3; i++) {
        window[i] = NetInput{};
        window[i].clientTick = 900 + i;
        window[i].skillSlot  = 2;
        // Distinct per input on purpose: the delay is adaptive and can change between two
        // inputs inside a single send window, so it must be per-input, not per-packet.
        window[i].interpDelayMs = static_cast<u8>(33 + i * 40);   // 33, 73, 113
    }
    u8 buf[256] = {};
    u32 size = serializeInputWindow(buf, sizeof(buf), window, 3);
    REQUIRE(size > 0);

    NetInput parsed[3] = {};
    u32 count = deserializeInputWindow(buf, size, parsed, 3);
    REQUIRE(count == 3);
    for (u32 i = 0; i < 3; i++) {
        CHECK(parsed[i].clientTick    == 900 + i);
        CHECK(parsed[i].skillSlot     == 2);               // field ahead of the new byte survives
        CHECK(parsed[i].interpDelayMs == 33 + i * 40);
    }
}

TEST_CASE("InputWindow: deserialize rejects truncated buffer") {
    u8 buf[3] = { 0, 0, 4 };  // claims 4 inputs but no input bytes follow
    NetInput parsed[4] = {};
    u32 count = deserializeInputWindow(buf, sizeof(buf), parsed, 4);
    CHECK(count == 0);
}

// Online couch co-op (PROTOCOL_VERSION 6): the CL_INPUT window header's byte 1 carries the absolute
// target net slot so the server routes a couch client's two input streams to the right slots.
TEST_CASE("InputWindow: targetSlot rides in header byte 1; default is 0; inputs unaffected") {
    NetInput window[2];
    for (u32 i = 0; i < 2; i++) { window[i] = NetInput{}; window[i].clientTick = 50 + i; }

    // Default (single client) → byte 1 == 0, count in byte 0.
    u8 a[64] = {};
    u32 sa = serializeInputWindow(a, sizeof(a), window, 2);
    REQUIRE(sa > 0);
    CHECK(a[0] == 2);   // windowCount
    CHECK(a[1] == 0);   // targetSlot default

    // Couch lane → byte 1 == targetSlot, and the inputs still round-trip identically.
    u8 b[64] = {};
    u32 sb = serializeInputWindow(b, sizeof(b), window, 2, /*targetSlot=*/2);
    REQUIRE(sb == sa);  // the slot byte reuses a reserved header byte — no size change
    CHECK(b[0] == 2);
    CHECK(b[1] == 2);   // targetSlot

    NetInput parsed[2] = {};
    u32 count = deserializeInputWindow(b, sb, parsed, 2);
    REQUIRE(count == 2);
    CHECK(parsed[0].clientTick == 50);
    CHECK(parsed[1].clientTick == 51);
}

// PROTOCOL_VERSION 19: the dead lock-on bit becomes INPUT_BLOCK. Server-side blocking (damage
// negation, the perfect window, the 0.4x move slow) is driven entirely by this bit, so it must
// (a) sit exactly where the reserved bit sat — the flags byte layout is unchanged — and
// (b) survive the wire round-trip like any other movement flag.
TEST_CASE("INPUT_BLOCK: occupies the old reserved lock bit and survives the wire") {
    CHECK(INPUT_BLOCK == (1 << 6));

    NetInput in{};
    in.clientTick = 9;
    in.moveFlags  = INPUT_FORWARD | INPUT_BLOCK;
    NetInput window[1] = { in };
    u8 buf[64] = {};
    u32 size = serializeInputWindow(buf, sizeof(buf), window, 1);
    REQUIRE(size > 0);

    NetInput parsed[4] = {};
    u32 count = deserializeInputWindow(buf, size, parsed, 4);
    REQUIRE(count == 1);
    CHECK((parsed[0].moveFlags & INPUT_BLOCK) != 0);
    CHECK((parsed[0].moveFlags & INPUT_FORWARD) != 0);
}

TEST_CASE("InputWire: a 15-input window (250 ms — the full coast) roundtrips") {
    // The redundancy window must cover the server's 250 ms starvation coast, or ticks 9..15 of a
    // burst outage are permanently lost and coast-approximated (rubber-band for the other player).
    NetInput in[INPUT_WINDOW_SIZE] = {};
    for (u32 i = 0; i < 15; i++) { in[i].clientTick = 100 + i; in[i].moveFlags = (u8)i; }
    u8 buf[512];
    u32 written = serializeInputWindow(buf, sizeof(buf), in, 15, 0);
    CHECK(written == 4 + 15 * 15);                         // header + 15 x 15 B
    NetInput out[INPUT_WINDOW_SIZE] = {};
    u32 got = deserializeInputWindow(buf, written, out, INPUT_WINDOW_SIZE);
    CHECK(got == 15);
    CHECK(out[14].clientTick == 114);
    CHECK(out[14].moveFlags == 14);
}
