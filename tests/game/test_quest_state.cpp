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

// Every quest must carry at least one objective, or the Journal has an empty body and the quest
// can never complete. A data lint, not a logic test — the kind that catches an authoring slip.
TEST_CASE("every authored quest has at least one objective and a narration") {
    for (u32 i = 0; i < Quest::COUNT; i++) {
        const Quest::QuestDef& q = Quest::QUESTS[i];
        CAPTURE(q.name);
        REQUIRE(q.objectiveCount >= 1);
        REQUIRE(q.objectiveCount <= Quest::MAX_OBJ);
        REQUIRE(q.narration != nullptr);
        REQUIRE(q.narration[0] != '\0');
        REQUIRE(q.giverIdx < Quest::GIVER_COUNT);
    }
}

// Every quest keeps a TALK objective, and it is always objective 0 — the Journal draws them in
// order and "speak to the giver" is the first beat of every D2 quest.
TEST_CASE("every quest opens with a TALK objective") {
    for (u32 i = 0; i < Quest::COUNT; i++) {
        CAPTURE(Quest::QUESTS[i].name);
        REQUIRE(Quest::QUESTS[i].objectives[0].trigger == Quest::Trigger::TALK);
    }
}

// A SLAY objective's target must be a non-empty name, or the kill hook can never match it.
TEST_CASE("SLAY objectives name a target") {
    for (u32 i = 0; i < Quest::COUNT; i++) {
        for (u32 o = 0; o < Quest::QUESTS[i].objectiveCount; o++) {
            const Quest::ObjectiveDef& od = Quest::QUESTS[i].objectives[o];
            if (od.trigger != Quest::Trigger::SLAY) continue;
            CAPTURE(Quest::QUESTS[i].name);
            REQUIRE(od.target != nullptr);
            REQUIRE(od.target[0] != '\0');
        }
    }
}

// Each giver must actually have quests, or an NPC stands in the hub with nothing to say.
TEST_CASE("every giver hands out at least one quest") {
    for (u32 g = 0; g < Quest::GIVER_COUNT; g++) {
        bool found = false;
        for (u32 i = 0; i < Quest::COUNT && !found; i++)
            if (Quest::QUESTS[i].giverIdx == g) found = true;
        CAPTURE(Quest::GIVERS[g].name);
        REQUIRE(found);
    }
}
