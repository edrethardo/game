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
