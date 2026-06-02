// Tests for collectUnprocessedInputs (net_player.h) — the server's "drain every new
// input once" selection that BOTH movement and activation edges depend on.
//
// Why this exists: activation bits (potion/skill) are single-tick EDGES. The server
// used to read activations via InputRingBuffer::getLatest() — only the newest buffered
// input per server tick. Under normal jitter, ≥2 inputs land between two server ticks,
// so the press input (carrying the edge) is usually NOT the newest → the activation was
// silently dropped for remote/client players. These tests pin the contract that draining
// every unprocessed input catches an edge getLatest() would miss, and that it can't
// double-fire.

#include "doctest/doctest.h"
#include "net/net_player.h"

static NetInput mkInput(u32 tick, u8 extFlags = 0) {
    NetInput in = {};
    in.clientTick = tick;
    in.extFlags   = extFlags;
    return in;
}

TEST_CASE("Input drain: catches an edge in an older input that getLatest() misses") {
    InputRingBuffer buf;
    // Three inputs arrive; only the MIDDLE one (tick 11) carries the skill edge — exactly
    // the case getLatest() drops, because tick 12 (newest) has no edge bit.
    buf.push(mkInput(10, 0));
    buf.push(mkInput(11, INPUT_EX_SKILL));
    buf.push(mkInput(12, 0));

    // Contrast: the OLD path. getLatest() returns tick 12 with no skill bit → press lost.
    REQUIRE(buf.getLatest() != nullptr);
    CHECK(buf.getLatest()->clientTick == 12);
    CHECK((buf.getLatest()->extFlags & INPUT_EX_SKILL) == 0);

    // The drain path: cursor starts at 10 (tick 10 already applied on a prior server tick).
    u32 cursor = 10;
    const NetInput* out[INPUT_BUFFER_SIZE];
    u32 n = collectUnprocessedInputs(buf, cursor, out, INPUT_BUFFER_SIZE);

    // It returns ticks 11 and 12, in order, and the skill edge IS present on tick 11.
    REQUIRE(n == 2);
    CHECK(out[0]->clientTick == 11);
    CHECK((out[0]->extFlags & INPUT_EX_SKILL) != 0);
    CHECK(out[1]->clientTick == 12);
    // Cursor advanced to the newest visited.
    CHECK(cursor == 12);
}

TEST_CASE("Input drain: never revisits a processed tick (no double-fire)") {
    InputRingBuffer buf;
    buf.push(mkInput(20, INPUT_EX_POTION));
    buf.push(mkInput(21, 0));

    u32 cursor = 19;
    const NetInput* out[INPUT_BUFFER_SIZE];
    u32 n1 = collectUnprocessedInputs(buf, cursor, out, INPUT_BUFFER_SIZE);
    REQUIRE(n1 == 2);
    CHECK((out[0]->extFlags & INPUT_EX_POTION) != 0);
    CHECK(cursor == 21);

    // Second call with the advanced cursor: nothing new → the potion edge is NOT seen
    // again. This is the double-fire guard (the press fires exactly once).
    u32 n2 = collectUnprocessedInputs(buf, cursor, out, INPUT_BUFFER_SIZE);
    CHECK(n2 == 0);
    CHECK(cursor == 21);
}

TEST_CASE("Input drain: cursor at/above newest yields nothing") {
    InputRingBuffer buf;
    buf.push(mkInput(5, INPUT_EX_SKILL));
    buf.push(mkInput(6, INPUT_EX_SKILL));

    u32 cursor = 6; // already processed through the newest
    const NetInput* out[INPUT_BUFFER_SIZE];
    CHECK(collectUnprocessedInputs(buf, cursor, out, INPUT_BUFFER_SIZE) == 0);
}

TEST_CASE("Input ring: push() rejects out-of-order ticks (drain contract dependency)") {
    InputRingBuffer buf;
    buf.push(mkInput(12, 0));
    buf.push(mkInput(11, INPUT_EX_SKILL)); // stale (<= newest) → rejected
    CHECK(buf.count == 1);
    REQUIRE(buf.getLatest() != nullptr);
    CHECK(buf.getLatest()->clientTick == 12);
    // The 4-input resend window is what re-lands a reordered tick within its redundancy
    // horizon; push() de-duping by clientTick is why the drain can't double-process it.
}

TEST_CASE("Input drain: respects outCap") {
    InputRingBuffer buf;
    for (u32 t = 1; t <= 5; t++) buf.push(mkInput(t, 0));
    u32 cursor = 0;
    const NetInput* out[3];
    u32 n = collectUnprocessedInputs(buf, cursor, out, 3);
    CHECK(n == 3);
    CHECK(out[0]->clientTick == 1);
    CHECK(out[2]->clientTick == 3);
    // Cursor only advanced as far as it actually consumed — remaining inputs drain next call.
    CHECK(cursor == 3);
}
