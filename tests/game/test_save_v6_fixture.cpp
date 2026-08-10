// test_save_v6_fixture.cpp — the v6 -> v7 quest migration, run against a REAL save file.
//
// `test_quest_state.cpp` already pins migrateFromMask's arithmetic against hand-built masks. This
// file pins something it cannot: that the bytes an actual v6 save carries, in the layout the actual
// writer produced, still yield a hero with both acts intact.
//
// The distinction is not academic. The fixture was very nearly a v2 file: every save_NN.dat in the
// REPO ROOT is a stale v2/v3 leftover, while the game reads from a VS Code snap path entirely
// elsewhere. A migration test written against those would have exercised the v2 branch, passed, and
// proven nothing about v6 — which is the version the bump was actually leaving behind.
//
// What this deliberately does NOT do is re-read the file through a second parser. Reimplementing
// engine_persist's reader here would be a copy that drifts from it, and a copy that agrees with
// itself is worthless. It reads exactly ONE thing — the trailing questMask, whose offset is fixed by
// the v6 layout (the per-player block ends waypointMask:u64, questMask:u64) — and asserts the
// migration turns it into the completions it stands for.
#include "../../external/doctest/doctest.h"
#include "game/quest_state.h"

#include <cstdio>
#include <string>

#ifndef DUNGEON_REPO_ROOT
#define DUNGEON_REPO_ROOT "."
#endif

namespace {

std::string fixturePath() {
    return std::string(DUNGEON_REPO_ROOT) + "/tests/fixtures/save_v6_fixture.dat";
}

// The file's version header (its first u32) and its trailing questMask (its last u64). Returns false
// if the fixture is missing or too short to hold either.
bool readFixture(u32& versionOut, u64& questMaskOut) {
    std::FILE* f = std::fopen(fixturePath().c_str(), "rb");
    if (!f) return false;

    bool ok = std::fread(&versionOut, sizeof(u32), 1, f) == 1;
    if (ok) ok = std::fseek(f, -static_cast<long>(sizeof(u64)), SEEK_END) == 0;
    if (ok) ok = std::fread(&questMaskOut, sizeof(u64), 1, f) == 1;

    std::fclose(f);
    return ok;
}

} // namespace

TEST_CASE("the v6 save fixture is genuinely v6 and carries completed quests") {
    u32 version = 0;
    u64 questMask = 0;
    REQUIRE(readFixture(version, questMask));

    // If this fires, the fixture was replaced by a file from a different era and every assertion
    // below is testing the wrong migration path.
    REQUIRE(version == 6);
    // A fixture with no completions cannot prove the migration PRESERVES anything — it would pass
    // just as happily against a migration that discarded everything.
    REQUIRE(questMask != 0);
}

TEST_CASE("a real v6 save's quest mask migrates to the same completions") {
    u32 version = 0;
    u64 questMask = 0;
    REQUIRE(readFixture(version, questMask));

    Quest::Progress p{};
    Quest::migrateFromMask(p, questMask);

    // Every bit the file carried is a COMPLETE quest, and nothing else became one.
    for (u32 i = 0; i < Quest::MAX_QUESTS && i < 64; i++) {
        const bool wasDone = (questMask & (1ull << i)) != 0;
        CAPTURE(i);
        REQUIRE(p.state[i] == static_cast<u8>(wasDone ? Quest::State::COMPLETE
                                                      : Quest::State::LOCKED));
    }

    // And the derived mask reproduces the file, which is what every live consumer still reads.
    REQUIRE(Quest::completionMask(p) == questMask);
}

TEST_CASE("the v6 fixture's hero keeps both acts after migration") {
    u32 version = 0;
    u64 questMask = 0;
    REQUIRE(readFixture(version, questMask));

    Quest::Progress p{};
    Quest::migrateFromMask(p, questMask);
    const u64 derived = Quest::completionMask(p);

    // THE failure this whole fixture exists to catch: a hero who finished the acts under v6 loading
    // as though they had not, with the next autosave writing that loss back permanently. The fixture
    // carries all ten quests, so both acts must read complete on the other side of the migration.
    REQUIRE(Quest::actComplete(derived, 1));
    REQUIRE(Quest::actComplete(derived, 2));
}
