// test_quest_state.cpp — the pure quest state machine.
//
// Every test here builds a Quest::Progress by hand. Nothing in this file may touch the engine,
// GL, or a live level: the whole point of quest_state.h being header-only and engine-free is
// that the act's rules are testable without booting anything.
#include "../../external/doctest/doctest.h"
#include "game/quest_state.h"

TEST_CASE("quest progress starts empty and reports nothing complete") {
    Quest::Progress p{};
    for (u32 i = 0; i < Quest::MAX_QUESTS; i++)
        REQUIRE(p.state[i] == static_cast<u8>(Quest::State::LOCKED));
    REQUIRE(Quest::completionMask(p) == 0ull);
}

TEST_CASE("completionMask sets exactly the bits of COMPLETE quests") {
    Quest::Progress p{};
    p.state[0] = static_cast<u8>(Quest::State::COMPLETE);
    p.state[3] = static_cast<u8>(Quest::State::COMPLETE);
    p.state[1] = static_cast<u8>(Quest::State::ACTIVE);    // must NOT appear in the mask
    p.state[2] = static_cast<u8>(Quest::State::OFFERED);   // must NOT appear in the mask

    const u64 m = Quest::completionMask(p);
    REQUIRE((m & (1ull << 0)) != 0);
    REQUIRE((m & (1ull << 3)) != 0);
    REQUIRE((m & (1ull << 1)) == 0);
    REQUIRE((m & (1ull << 2)) == 0);
}

// THE migration test. A v6 save carries a u64 questMask with real completions; loading one and
// reading the new v7 tail as zeros would silently un-complete both acts for every existing hero.
TEST_CASE("v6 quest mask migrates to COMPLETE states, bit for bit") {
    const u64 legacy = (1ull << 0) | (1ull << 2) | (1ull << 9);
    Quest::Progress p{};
    Quest::migrateFromMask(p, legacy);

    for (u32 i = 0; i < Quest::MAX_QUESTS; i++) {
        const bool wasDone = (legacy & (1ull << i)) != 0;
        REQUIRE(p.state[i] == static_cast<u8>(wasDone ? Quest::State::COMPLETE
                                                      : Quest::State::LOCKED));
    }
    // Round trip: the derived mask must reproduce the file it came from.
    REQUIRE(Quest::completionMask(p) == legacy);
}

TEST_CASE("migration ignores bits above the quest table") {
    Quest::Progress p{};
    Quest::migrateFromMask(p, ~0ull);           // every bit set, including 32..63

    // `1ull << 64` is UB, so the all-set mask cannot be written as a shift once MAX_QUESTS
    // reaches the width of the type it is carried in.
    const u64 allQuests = Quest::MAX_QUESTS >= 64 ? ~0ull
                                                  : ((1ull << Quest::MAX_QUESTS) - 1ull);
    REQUIRE(Quest::completionMask(p) == allQuests);

    // The overflow this test exists to catch lands in `obj` (state[32] and obj[0][0] are
    // adjacent), NOT in the mask — asserting the mask alone re-tests completionMask's bound and
    // passes against a migrateFromMask that writes past the table.
    for (u32 q = 0; q < Quest::MAX_QUESTS; q++)
        for (u32 o = 0; o < Quest::MAX_OBJ; o++)
            REQUIRE(p.obj[q][o] == 0);
}
