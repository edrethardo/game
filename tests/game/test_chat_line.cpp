// test_chat_line.cpp — the HUD chat line format rule.
//
// Two defects lived in Engine::addChatMessage for the life of the project and neither was
// reachable by a test while the rule was inline there: a 48-byte buffer that cut nine of the ten
// quest blurbs mid-sentence, and an unconditional "%s: %s" that gave every SPEAKERLESS line a
// stray leading ": ". Both are pinned here against the REAL quest table, so a blurb rewritten
// past the buffer — or a re-added unconditional prefix — fails by name instead of shipping.
//
// Nothing here may touch the engine or GL: chat_line.h is header-only and engine-free.
#include "../../external/doctest/doctest.h"
#include "game/chat_line.h"
#include "game/quest_def.h"
#include <cstring>
#include <string>   // CAPTURE renders a bare const char* as a POINTER, not the text


TEST_CASE("a speakerless line carries no prefix") {
    char buf[Chat::LINE_LEN];

    // Empty speaker — how quest offers, act completions, gate refusals and pickup names all call in.
    Chat::format(buf, sizeof(buf), "", "Something is still holding the Den. Clear it.");
    REQUIRE(std::string(buf) == "Something is still holding the Den. Clear it.");
    REQUIRE(buf[0] != ':');

    // A null speaker must behave the same, not crash or print "(null)".
    Chat::format(buf, sizeof(buf), nullptr, "Waypoint");
    REQUIRE(std::string(buf) == "Waypoint");
}

TEST_CASE("a spoken line keeps its speaker and separator") {
    char buf[Chat::LINE_LEN];
    Chat::format(buf, sizeof(buf), "Stash", "Your backpack is full.");
    REQUIRE(std::string(buf) == "Stash: Your backpack is full.");
}

TEST_CASE("every quest name and blurb fits the chat line untruncated") {
    // The reported bug. `format` returns the length the line WOULD have needed, so >= capacity is
    // exactly "this was cut", with no second copy of the format string to go stale.
    char buf[Chat::LINE_LEN];
    for (u32 i = 0; i < Quest::COUNT; i++) {
        const Quest::QuestDef& q = Quest::QUESTS[i];
        CAPTURE(std::string(q.name));

        const u32 nameLen = Chat::format(buf, sizeof(buf), "", q.name);
        REQUIRE(nameLen < Chat::LINE_LEN);
        REQUIRE(std::string(buf) == q.name);

        CAPTURE(std::string(q.blurb));
        const u32 blurbLen = Chat::format(buf, sizeof(buf), "", q.blurb);
        REQUIRE(blurbLen < Chat::LINE_LEN);
        REQUIRE(std::string(buf) == q.blurb);   // byte-identical: nothing was dropped
    }
}

TEST_CASE("The Rebaser's offer keeps the clause that says what to do") {
    // The named symptom: at 48 bytes this stopped after "back. S" and lost the instruction.
    const Quest::QuestDef* rebaser = nullptr;
    for (u32 i = 0; i < Quest::COUNT; i++)
        if (std::strcmp(Quest::QUESTS[i].name, "The Rebaser") == 0) rebaser = &Quest::QUESTS[i];
    REQUIRE(rebaser != nullptr);

    char buf[Chat::LINE_LEN];
    Chat::format(buf, sizeof(buf), "", rebaser->blurb);
    const std::string line(buf);
    CAPTURE(line);
    REQUIRE(line.find("Stop whatever is rewriting it.") != std::string::npos);
    REQUIRE(line.back() == '.');       // reads to the end of a sentence, not mid-word
    REQUIRE(line.front() != ':');      // and with no stray leading separator
}

TEST_CASE("format never overruns its buffer and always terminates") {
    // A caller may still hand in a message longer than the line; truncating is fine, running off
    // the end is not. The guard bytes catch a write past `cap`.
    char buf[16];
    std::memset(buf, '#', sizeof(buf));
    char guarded[8 + 4];
    std::memset(guarded, '#', sizeof(guarded));
    const u32 want = Chat::format(guarded, 8, "Speaker", "a message far longer than eight bytes");
    REQUIRE(guarded[7] == '\0');                    // NUL-terminated inside cap
    for (u32 i = 8; i < sizeof(guarded); i++)
        REQUIRE(guarded[i] == '#');                 // nothing written past cap
    REQUIRE(want >= 8);                             // and the truncation is reported

    // Degenerate inputs answer safely rather than crashing.
    REQUIRE(Chat::format(nullptr, 8, "a", "b") == 0);
    REQUIRE(Chat::format(buf, 0, "a", "b") == 0);
}
