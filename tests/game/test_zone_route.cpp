// zone_route — the act's road, its gates, and the one property that matters most: you can always
// finish.
//
// Gating the road is the feature that can silently ruin the game. A gate whose key sits BEHIND that
// same gate locks a character out of the rest of the acts forever, with nothing on screen to say
// which of fifteen links is at fault and no way to recover but a new hero. So the headline test here
// is not "the gate works" — it is "walking the chain from the very start, honouring every gate, the
// next objective is always reachable", played out over the whole quest order.

#include <doctest/doctest.h>
#include "game/zone_route.h"

#include <string>
#include <cstdlib>

namespace {

// Simulate a character standing at `START` who has completed exactly the first `k` quests.
constexpr u8 START = 52;   // The Blood Buffer — where the town's north gate lets you out

u64 maskOfFirst(u32 k) {
    u64 m = 0;
    for (u32 i = 0; i < k && i < Quest::COUNT; i++) m |= (1ull << i);
    return m;
}

// Can `goal` be reached from `from` under `mask`, by repeatedly taking nextHop? Bounded, so a
// routing cycle fails the test rather than hanging the suite.
bool walkable(u8 from, u8 goal, u64 mask) {
    u8 at = from;
    for (u32 steps = 0; steps < 64; steps++) {
        if (at == goal) return true;
        const ZoneRoute::Hop h = ZoneRoute::nextHop(at, goal, mask);
        if (h.kind == ZoneRoute::HopKind::NONE) return false;
        at = h.dest;
    }
    return false;
}

} // namespace

TEST_CASE("the acts can always be finished — every objective is reachable in chain order") {
    // THE test. At each point in the chain the hero has done exactly the quests before it, which is
    // the only state a real playthrough can be in, and from the start of the acts the next objective
    // must be walkable. A gate that depends on its own quest fails here immediately.
    for (u32 done = 0; done < Quest::COUNT; done++) {
        const u64 mask = maskOfFirst(done);
        const u8  goal = ZoneRoute::objectiveZone(mask);
        CAPTURE(done);
        CAPTURE(goal);
        REQUIRE(goal != 0);                      // there IS an outstanding objective
        CHECK(walkable(START, goal, mask));
    }
    // ...and with everything done there is no objective left, which is what ends the run.
    CHECK(ZoneRoute::objectiveZone(maskOfFirst(Quest::COUNT)) == 0);
}

TEST_CASE("the objective is reachable from wherever the previous one left you") {
    // Stronger than the test above, and closer to what actually happens: you finish a quest standing
    // in its zone, so the next hop is computed from THERE — which for an interior means routing back
    // out through a portal first. A greedy "walk toward the higher floor" rule fails exactly here.
    for (u32 done = 1; done < Quest::COUNT; done++) {
        const u64 mask = maskOfFirst(done);
        const u8  from = Quest::QUESTS[done - 1].zoneFloor;   // where the last quest was finished
        const u8  goal = ZoneRoute::objectiveZone(mask);
        CAPTURE(from);
        CAPTURE(goal);
        CHECK(walkable(from, goal, mask));
    }
}

TEST_CASE("the road onward is SHUT until the local quest is done") {
    // Without this the chain is decoration: you could walk from the town to the act boss without
    // touching a quest. Checked on the two links that carry the acts' set-piece beats.

    // The Field of Unmerged Branches -> the Deadlock Woods needs the field cleared AND TristRAM done.
    CHECK_FALSE(ZoneRoute::linkOpen(56, 58, 0));
    // Its portal into TristRAM is D2's Cairn Stones: clearing the field is what opens it.
    CHECK_FALSE(ZoneRoute::linkOpen(56, 57, 0));

    u64 m = 0;
    m |= (1ull << Quest::bitFor(56));                       // Align the Standing Stones
    CHECK(ZoneRoute::linkOpen(56, 57, m));                  // ...the portal opens
    CHECK_FALSE(ZoneRoute::linkOpen(56, 58, m));            // ...but the road still waits on TristRAM
    m |= (1ull << Quest::bitFor(57));                       // The Search for Deckard Cache
    CHECK(ZoneRoute::linkOpen(56, 58, m));

    // Act 2's finale is behind Piccadilly's own quest — the rift-forcing beat.
    CHECK_FALSE(ZoneRoute::linkOpen(65, 66, 0));
    CHECK(ZoneRoute::linkOpen(65, 66, 1ull << Quest::bitFor(65)));
}

TEST_CASE("going BACK is never gated") {
    // A hero who wanders into an interior under-levelled has to be able to leave, and the walk home
    // must always work. Gating a retreat is how a player gets stuck with no message at all.
    for (u32 i = 0; i < Zone::COUNT; i++) {
        const Zone::ZoneDef& z = Zone::ZONES[i];
        CAPTURE(z.name);
        if (z.returnFloor != Zone::NO_LINK) CHECK(ZoneRoute::linkOpen(z.floor, z.returnFloor, 0));
        for (u8 d = 0; d < static_cast<u8>(Zone::Dir::COUNT); d++) {
            const u8 nb = z.neighbour[d];
            if (nb == Zone::NO_LINK || nb > z.floor) continue;   // only the backward links
            CHECK(ZoneRoute::linkOpen(z.floor, nb, 0));
        }
    }
}

TEST_CASE("a hop names a real link out of the zone you are standing in") {
    // nextHop's answer is fed straight to the driver, which walks at a border or presses interact at
    // a portal. A hop naming a direction the zone has no gate on would march the bot into a wall.
    for (u32 done = 0; done < Quest::COUNT; done++) {
        const u64 mask = maskOfFirst(done);
        const u8  goal = ZoneRoute::objectiveZone(mask);
        for (u32 i = 0; i < Zone::COUNT; i++) {
            const u8 from = Zone::ZONES[i].floor;
            const ZoneRoute::Hop h = ZoneRoute::nextHop(from, goal, mask);
            if (h.kind == ZoneRoute::HopKind::NONE) continue;
            CAPTURE(from);
            CAPTURE(goal);
            const Zone::ZoneDef* z = Zone::find(from);
            REQUIRE(z != nullptr);
            if (h.kind == ZoneRoute::HopKind::EDGE) {
                REQUIRE(h.dir != Zone::Dir::COUNT);
                CHECK(z->neighbour[static_cast<u8>(h.dir)] == h.dest);
            } else {
                CHECK((z->poiFloor == h.dest || z->returnFloor == h.dest));
            }
        }
    }
}

TEST_CASE("the in-zone task matches what the quest actually asks") {
    CHECK(ZoneRoute::taskFor(53, 0) == ZoneRoute::Task::CLEAR_ZONE);  // Free the Allocation
    CHECK(ZoneRoute::taskFor(55, 0) == ZoneRoute::Task::SLAY);        // The Rebaser
    CHECK(ZoneRoute::taskFor(59, 0) == ZoneRoute::Task::TRAVEL);      // REACH completes on arrival
    CHECK(ZoneRoute::taskFor(52, 0) == ZoneRoute::Task::TRAVEL);      // hosts no quest at all

    // A finished quest stops asking, or the bot would grind a cleared zone forever.
    CHECK(ZoneRoute::taskFor(53, 1ull << Quest::bitFor(53)) == ZoneRoute::Task::TRAVEL);
}

TEST_CASE("every SLAY objective names the boss its zone actually spawns") {
    // The bot fights toward the zone's named boss. If the quest names something else, it would
    // "complete" the zone by killing a random mob, or never complete at all.
    for (u32 i = 0; i < Quest::COUNT; i++) {
        const Quest::QuestDef& q = Quest::QUESTS[i];
        if (q.trigger != Quest::Trigger::SLAY) continue;
        const Zone::ZoneDef* z = Zone::find(q.zoneFloor);
        CAPTURE(q.name);
        REQUIRE(z != nullptr);
        REQUIRE(z->boss != nullptr);
        CHECK(std::string(z->boss) == std::string(q.target));
    }
}

// The whole act, walked. Env-gated (the BALANCE_REPORT / LEVEL_PREVIEW pattern) because it is a
// REPORT, not an assertion — but it is the fastest way to see what the bot will actually do, and to
// spot a route that technically works while wandering somewhere absurd.
//
//   ZONE_ROUTE_DUMP=1 ./build/tests/dungeon_tests -tc="*zone route dump*"
TEST_CASE("zone route dump" * doctest::skip()) {
    if (!std::getenv("ZONE_ROUTE_DUMP")) return;
    u64 mask = 0;
    u8  at   = START;
    MESSAGE("start: " << std::string(Zone::find(at)->name));
    for (u32 guard = 0; guard < 200; guard++) {
        const u8 goal = ZoneRoute::objectiveZone(mask);
        if (goal == 0) { MESSAGE("ACTS COMPLETE"); break; }
        if (at == goal) {
            const Quest::QuestDef* q = Quest::forZone(goal);
            MESSAGE("  DO   [" << (int)at << "] " << std::string(Zone::find(at)->name)
                    << "  quest='" << std::string(q->name) << "'");
            mask |= (1ull << Quest::bitFor(goal));
            continue;
        }
        const ZoneRoute::Hop h = ZoneRoute::nextHop(at, goal, mask);
        REQUIRE(h.kind != ZoneRoute::HopKind::NONE);
        MESSAGE("  " << std::string(h.kind == ZoneRoute::HopKind::EDGE ? "walk  " : "portal")
                << " [" << (int)at << "] -> [" << (int)h.dest << "] " << std::string(Zone::find(h.dest)->name));
        at = h.dest;
    }
}
