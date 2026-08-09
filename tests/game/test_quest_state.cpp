// test_quest_state.cpp — the pure quest state machine.
//
// Every test here builds a Quest::Progress by hand. Nothing in this file may touch the engine,
// GL, or a live level: the whole point of quest_state.h being header-only and engine-free is
// that the act's rules are testable without booting anything.
#include "../../external/doctest/doctest.h"
#include "game/quest_state.h"
#include <string>   // CAPTURE renders a bare const char* as a POINTER, not the text


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
        CAPTURE(std::string(q.name));
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
        CAPTURE(std::string(Quest::QUESTS[i].name));
        REQUIRE(Quest::QUESTS[i].objectives[0].trigger == Quest::Trigger::TALK);
    }
}

// A SLAY objective's target must be a non-empty name, or the kill hook can never match it.
TEST_CASE("SLAY objectives name a target") {
    for (u32 i = 0; i < Quest::COUNT; i++) {
        for (u32 o = 0; o < Quest::QUESTS[i].objectiveCount; o++) {
            const Quest::ObjectiveDef& od = Quest::QUESTS[i].objectives[o];
            if (od.trigger != Quest::Trigger::SLAY) continue;
            CAPTURE(std::string(Quest::QUESTS[i].name));
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
        CAPTURE(std::string(Quest::GIVERS[g].name));
        REQUIRE(found);
    }
}

// Every quest's DEED trigger must be one the engine can currently satisfy. A quest authored with a
// trigger that has no implementation is not merely incomplete — ZoneRoute::linkOpen gates the
// onward road on zoneSettled(), so it SEALS the act and strands the character. Add a trigger to
// this list only in the same commit that makes something able to fire it. (Task 14 adds ACTIVATE
// here, in the same commit that spawns the five Cairn Stone fixtures.)
TEST_CASE("every quest's deed trigger is one the engine can satisfy") {
    for (u32 i = 0; i < Quest::COUNT; i++) {
        const Quest::ObjectiveDef* deed = Quest::deedObjective(Quest::QUESTS[i]);
        CAPTURE(std::string(Quest::QUESTS[i].name));
        REQUIRE(deed != nullptr);
        const bool implemented = deed->trigger == Quest::Trigger::CLEAR_ZONE
                              || deed->trigger == Quest::Trigger::SLAY
                              || deed->trigger == Quest::Trigger::REACH;
        REQUIRE(implemented);
    }
}

// Quest 0 (zone 53, Free the Allocation) is TALK + CLEAR_ZONE. Quest 1 (zone 55) is TALK + SLAY.
TEST_CASE("offer moves a locked quest to OFFERED and is idempotent") {
    Quest::Progress p{};
    Quest::offer(p, 0);
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::OFFERED));
    Quest::offer(p, 0);
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::OFFERED));
}

TEST_CASE("offer never demotes a quest that is already further along") {
    Quest::Progress p{};
    p.state[0] = static_cast<u8>(Quest::State::COMPLETE);
    Quest::offer(p, 0);
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::COMPLETE));
}

TEST_CASE("talking ticks the TALK objective and advances OFFERED to ACTIVE") {
    Quest::Progress p{};
    Quest::offer(p, 0);
    Quest::noteTalk(p, 0);
    REQUIRE(p.obj[0][0] == 1);
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::ACTIVE));
}

TEST_CASE("clearing the zone completes a TALK+CLEAR_ZONE quest") {
    Quest::Progress p{};
    Quest::offer(p, 0);
    Quest::noteTalk(p, 0);
    Quest::noteCleared(p, 0);
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::COMPLETE));
    REQUIRE((Quest::completionMask(p) & 1ull) != 0);
}

// THE non-blocking rule, stated as a test. A quest whose deed is done in the field completes even
// though its TALK objective was never ticked. Sabotage check: making TALK a prerequisite fails
// this by name.
TEST_CASE("TALK is never a prerequisite - field completion works unspoken") {
    Quest::Progress p{};
    Quest::offer(p, 0);
    Quest::noteCleared(p, 0);              // never talked to anyone
    REQUIRE(p.obj[0][0] == 0);             // TALK still unticked
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::COMPLETE));
}

// A v6 hero migrates in with COMPLETE states and ZEROED objectives — migrateFromMask has no detail
// to restore. Re-deriving state from `obj` unconditionally would therefore un-complete every quest
// they had already finished the moment they walked back into the hub and greeted its giver, and
// ZoneRoute would re-seal a road they had already walked. COMPLETE is terminal.
TEST_CASE("a COMPLETE quest is never demoted by a later mutation") {
    Quest::Progress p{};
    Quest::migrateFromMask(p, 1ull << 0);   // finished under v6; no objective detail survives
    REQUIRE(p.obj[0][0] == 0);
    Quest::noteTalk(p, 0);                  // walks back into town and greets Akara
    REQUIRE(p.state[0] == static_cast<u8>(Quest::State::COMPLETE));
    REQUIRE((Quest::completionMask(p) & 1ull) != 0);
}

TEST_CASE("SLAY matches only its named target") {
    Quest::Progress p{};
    Quest::offer(p, 1);
    Quest::noteKill(p, 1, "Bit Rat");
    REQUIRE(p.state[1] != static_cast<u8>(Quest::State::COMPLETE));
    Quest::noteKill(p, 1, "The Garbage Collector");
    REQUIRE(p.state[1] == static_cast<u8>(Quest::State::COMPLETE));
}

TEST_CASE("SLAY tolerates a null enemy name") {
    Quest::Progress p{};
    Quest::offer(p, 1);
    Quest::noteKill(p, 1, nullptr);
    REQUIRE(p.state[1] != static_cast<u8>(Quest::State::COMPLETE));
}

// ACTIVATE stores WHICH fixtures were used, not how many. A zone is rebuilt from its seed on every
// entry, so a bare count could not tell which stones were already lit and re-entry would re-light
// the wrong ones.
//
// The objective is LOCATED rather than hard-coded, because the table has none yet: quest 56 (the
// Cairn Stones) deliberately keeps CLEAR_ZONE until the five stone fixtures exist, since a trigger
// nothing can fire seals the road to TristRAM. So this pins the inert case today and turns itself
// into the real bitmask pin the moment the stones are authored — no second edit to remember.
TEST_CASE("ACTIVATE stores a bitmask and reports popcount progress") {
    u8 qi = 0xFF, oi = 0xFF;
    for (u32 i = 0; i < Quest::COUNT && qi == 0xFF; i++)
        for (u32 o = 0; o < Quest::QUESTS[i].objectiveCount; o++)
            if (Quest::QUESTS[i].objectives[o].trigger == Quest::Trigger::ACTIVATE) {
                qi = static_cast<u8>(i); oi = static_cast<u8>(o); break;
            }

    if (qi == 0xFF) {
        // Nothing authored yet. Pin the half that IS reachable: a quest owning no ACTIVATE
        // objective must ignore the hook outright, never bank the bit on some other objective.
        Quest::Progress p{};
        Quest::offer(p, 2);
        Quest::noteActivate(p, 2, 0);
        Quest::noteActivate(p, 2, 3);
        for (u32 o = 0; o < Quest::MAX_OBJ; o++) REQUIRE(p.obj[2][o] == 0);
        REQUIRE(p.state[2] != static_cast<u8>(Quest::State::COMPLETE));
        return;
    }

    const u8 need = Quest::QUESTS[qi].objectives[oi].required;
    CAPTURE(std::string(Quest::QUESTS[qi].name));

    Quest::Progress p{};
    Quest::offer(p, qi);
    Quest::noteActivate(p, qi, 0);
    Quest::noteActivate(p, qi, 0);                  // repeat must not double-count
    REQUIRE(Quest::objectiveProgress(p, qi, oi) == 1);
    if (need > 1) REQUIRE(p.state[qi] != static_cast<u8>(Quest::State::COMPLETE));

    for (u8 f = 1; f < need; f++) Quest::noteActivate(p, qi, f);
    REQUIRE(Quest::objectiveProgress(p, qi, oi) == need);
    REQUIRE(p.state[qi] == static_cast<u8>(Quest::State::COMPLETE));

    // A fixture ordinal past the quest's own count is not one it owns.
    if (need < 8) {
        Quest::noteActivate(p, qi, need);
        REQUIRE(Quest::objectiveProgress(p, qi, oi) == need);
    }
}

TEST_CASE("out-of-range quest and fixture indices are ignored, not written") {
    Quest::Progress p{};
    Quest::offer(p, 200);
    Quest::noteTalk(p, 200);
    Quest::noteActivate(p, 2, 99);
    REQUIRE(Quest::completionMask(p) == 0ull);
    REQUIRE(Quest::objectiveProgress(p, 2, 1) == 0);
}

// The never-strand invariant, walked over the whole table: doing every quest's deed in authored
// order, with nobody ever spoken to, must finish both acts. If any quest could not complete this
// way, a player who never found its giver would be permanently blocked on the road.
TEST_CASE("every quest completes without ever talking to a giver") {
    Quest::Progress p{};
    for (u32 i = 0; i < Quest::COUNT; i++) {
        Quest::offer(p, static_cast<u8>(i));
        const Quest::QuestDef& q = Quest::QUESTS[i];
        for (u32 o = 0; o < q.objectiveCount; o++) {
            switch (q.objectives[o].trigger) {
                case Quest::Trigger::CLEAR_ZONE: Quest::noteCleared(p, static_cast<u8>(i)); break;
                case Quest::Trigger::REACH:      Quest::noteReached(p, static_cast<u8>(i)); break;
                case Quest::Trigger::SLAY:
                    Quest::noteKill(p, static_cast<u8>(i), q.objectives[o].target); break;
                case Quest::Trigger::ACTIVATE:
                    for (u8 f = 0; f < q.objectives[o].required; f++)
                        Quest::noteActivate(p, static_cast<u8>(i), f);
                    break;
                case Quest::Trigger::TALK:  break;   // deliberately never fired
                default: break;
            }
        }
        CAPTURE(std::string(q.name));
        REQUIRE(p.state[i] == static_cast<u8>(Quest::State::COMPLETE));
    }
    REQUIRE(Quest::actComplete(Quest::completionMask(p), 1));
    REQUIRE(Quest::actComplete(Quest::completionMask(p), 2));
}

// The v7 payload must survive a byte-level round trip at exactly the size the writer emits. This
// is the shape check; the real-file check is the manual fixture load in the plan's next step.
TEST_CASE("quest progress round-trips through a byte buffer at the serialized size") {
    Quest::Progress out{};
    Quest::offer(out, 0);
    Quest::noteTalk(out, 0);
    Quest::offer(out, 5);
    out.state[9] = static_cast<u8>(Quest::State::COMPLETE);
    // Objective bytes written DIRECTLY rather than through noteActivate: no quest owns an ACTIVATE
    // objective until Task 14 flips quest 56, so the mutator would be a silent no-op here and the
    // round trip would prove nothing about obj[] at all. What this test pins is the SERIALIZED
    // SHAPE — that every byte of both arrays survives — so it wants arbitrary bytes, not a
    // realistic play state.
    out.obj[2][1] = 0b00010001;   // the Cairn bitmask shape: stones 0 and 4
    out.obj[7][0] = 1;

    u8 buf[Quest::MAX_QUESTS + Quest::MAX_QUESTS * Quest::MAX_OBJ] = {};
    u32 w = 0;
    for (u32 i = 0; i < Quest::MAX_QUESTS; i++) buf[w++] = out.state[i];
    for (u32 i = 0; i < Quest::MAX_QUESTS; i++)
        for (u32 o = 0; o < Quest::MAX_OBJ; o++) buf[w++] = out.obj[i][o];
    REQUIRE(w == sizeof(buf));

    Quest::Progress in{};
    u32 r = 0;
    for (u32 i = 0; i < Quest::MAX_QUESTS; i++) in.state[i] = buf[r++];
    for (u32 i = 0; i < Quest::MAX_QUESTS; i++)
        for (u32 o = 0; o < Quest::MAX_OBJ; o++) in.obj[i][o] = buf[r++];

    for (u32 i = 0; i < Quest::MAX_QUESTS; i++) {
        REQUIRE(in.state[i] == out.state[i]);
        for (u32 o = 0; o < Quest::MAX_OBJ; o++) REQUIRE(in.obj[i][o] == out.obj[i][o]);
    }
    REQUIRE(Quest::completionMask(in) == Quest::completionMask(out));
    REQUIRE(in.obj[2][1] == 0b00010001);   // the raw byte, not objectiveProgress — see above
}

// ---- Which quest a giver is currently holding ---------------------------------------------------

// Giver 0 (Akara) holds quests 0 and 1; giver 1 (Charsi) holds 2, 3 and 4.
TEST_CASE("a giver offers its first unfinished quest, in table order") {
    Quest::Progress p{};
    REQUIRE(Quest::giverOutstanding(p, 0) == 0);

    p.state[0] = static_cast<u8>(Quest::State::COMPLETE);
    REQUIRE(Quest::giverOutstanding(p, 0) == 1);

    p.state[1] = static_cast<u8>(Quest::State::COMPLETE);
    REQUIRE(Quest::giverOutstanding(p, 0) == 0xFF);   // nothing left to give
}

TEST_CASE("a giver with an in-progress quest keeps offering that one") {
    Quest::Progress p{};
    Quest::offer(p, 0);
    Quest::noteTalk(p, 0);
    REQUIRE(Quest::giverOutstanding(p, 0) == 0);      // ACTIVE, not COMPLETE - still theirs
}

TEST_CASE("an unknown giver index yields nothing rather than reading out of bounds") {
    Quest::Progress p{};
    REQUIRE(Quest::giverOutstanding(p, 200) == 0xFF);
}

TEST_CASE("each giver's quests all belong to that giver's own act") {
    for (u32 i = 0; i < Quest::COUNT; i++) {
        const Quest::QuestDef& q = Quest::QUESTS[i];
        const Quest::GiverDef& g = Quest::GIVERS[q.giverIdx];
        CAPTURE(std::string(q.name));
        CAPTURE(std::string(g.name));
        // Act 1's hub is the town (98), Act 2's is Null Terminus (61). A giver standing in the
        // wrong hub can never be reached at the point its quest is relevant.
        REQUIRE(Quest::actOf(q.zoneFloor) == (g.hubFloor == 98 ? 1 : 2));
    }
}
