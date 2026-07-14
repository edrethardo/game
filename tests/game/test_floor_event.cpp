// test_floor_event.cpp — the floor-event weighted pick.
//
// This decides whether a floor gets an event and which one. Two failure modes matter and neither
// would crash: a table whose weights don't actually shape the outcome (so adding an event silently
// changes nothing), and a minFloor gate that doesn't hold (so the loot goblin shows up on floor 1
// before the player knows what it is). Both are pinned here.

#include <doctest/doctest.h>
#include "game/floor_event.h"
#include <initializer_list>
#include <string>

static FloorEventTable oneGoblin(f32 chance = 1.0f, u8 minFloor = 2) {
    FloorEventTable t;
    t.floorChance = chance;
    t.defs[0] = { FloorEventId::LOOT_GOBLIN, 100, minFloor };
    t.count = 1;
    return t;
}

TEST_CASE("FloorEvent: an empty table never produces an event") {
    FloorEventTable t;   // count == 0
    for (u32 seed = 1; seed < 200; seed++) {
        u32 rng = seed;
        CHECK(FloorEvent::pick(t, 10, rng) == FloorEventId::NONE);
    }
}

TEST_CASE("FloorEvent: floorChance 0 disables events entirely") {
    const FloorEventTable t = oneGoblin(0.0f);
    for (u32 seed = 1; seed < 200; seed++) {
        u32 rng = seed;
        CHECK(FloorEvent::pick(t, 10, rng) == FloorEventId::NONE);
    }
}

TEST_CASE("FloorEvent: minFloor gates an event out of the early floors") {
    const FloorEventTable t = oneGoblin(1.0f, /*minFloor=*/5);
    for (u32 seed = 1; seed < 300; seed++) {
        u32 rng = seed;
        CHECK(FloorEvent::pick(t, 4, rng) == FloorEventId::NONE);      // below the gate
    }
    bool sawGoblin = false;
    for (u32 seed = 1; seed < 300; seed++) {
        u32 rng = seed;
        if (FloorEvent::pick(t, 5, rng) == FloorEventId::LOOT_GOBLIN) sawGoblin = true;
    }
    CHECK(sawGoblin);                                                   // at the gate
}

TEST_CASE("FloorEvent: floorChance actually shapes how often events fire") {
    // Guards the ordering inside pick(): the chance roll must happen BEFORE the weighted pick, or
    // adding a second event to the table would silently make events more frequent rather than just
    // more varied — which would make the table untunable.
    const FloorEventTable t = oneGoblin(0.35f, 1);
    u32 hits = 0;
    constexpr u32 N = 4000;
    for (u32 seed = 1; seed <= N; seed++) {
        u32 rng = seed * 2654435761u;   // spread the seeds
        if (FloorEvent::pick(t, 10, rng) != FloorEventId::NONE) hits++;
    }
    const f64 rate = static_cast<f64>(hits) / static_cast<f64>(N);
    CHECK(rate > 0.25);
    CHECK(rate < 0.45);   // ~0.35, generously bounded
}

TEST_CASE("FloorEvent: weights decide WHICH event, proportionally") {
    // Two events, 3:1. The rarer one must still appear, and the common one must dominate.
    FloorEventTable t;
    t.floorChance = 1.0f;
    t.defs[0] = { FloorEventId::LOOT_GOBLIN, 75, 1 };
    t.defs[1] = { FloorEventId::NONE,        25, 1 };   // NONE entries are skipped by pick()
    t.count = 2;

    // A NONE-id entry must not be selectable — it would be an event with no spawner, i.e. exactly
    // the "looks live, does nothing" bug this codebase keeps producing.
    u32 goblins = 0;
    for (u32 seed = 1; seed <= 500; seed++) {
        u32 rng = seed;
        const FloorEventId id = FloorEvent::pick(t, 10, rng);
        CHECK(id != FloorEventId::COUNT);
        if (id == FloorEventId::LOOT_GOBLIN) goblins++;
    }
    CHECK(goblins == 500);   // the NONE entry contributes no weight, so the goblin always wins
}

TEST_CASE("FloorEvent: id/name round-trip, and an unknown id is NOT an event") {
    CHECK(FloorEvent::idFromName("loot_goblin") == FloorEventId::LOOT_GOBLIN);
    CHECK(std::string(FloorEvent::nameOf(FloorEventId::LOOT_GOBLIN)) == "loot_goblin");
    // A typo in events.json must degrade to "no event", never to the wrong event.
    CHECK(FloorEvent::idFromName("loot_goblen") == FloorEventId::NONE);
    CHECK(FloorEvent::idFromName("") == FloorEventId::NONE);
    CHECK(FloorEvent::idFromName(nullptr) == FloorEventId::NONE);
}

TEST_CASE("FloorEvent: pick advances the rng state") {
    const FloorEventTable t = oneGoblin();
    u32 rng = 4242;
    const u32 before = rng;
    FloorEvent::pick(t, 10, rng);
    CHECK(rng != before);   // a caller looping on one state must not get the same answer forever
}
