// test_free_play.cpp — unit tests for the post-clear Free-Play predicates (game/free_play.h).
#include "doctest/doctest.h"
#include "game/free_play.h"
#include <string>

using namespace FreePlay;

TEST_CASE("saveCleared: true only for Hell past floor 50") {
    CHECK(saveCleared(57, 2));        // the wanderer (save_01)
    CHECK(saveCleared(51, 2));        // just beat Hell 50
    CHECK(saveCleared(51, 3));        // >= Hell: a future harder tier still counts as cleared
    CHECK_FALSE(saveCleared(50, 2));  // on Hell 50, not yet beaten
    CHECK_FALSE(saveCleared(57, 1));  // Nightmare (difficulty 1), not Hell
    CHECK_FALSE(saveCleared(30, 2));  // mid-Hell
    CHECK_FALSE(saveCleared(20, 0));  // Normal
}

TEST_CASE("clampFloor keeps floor in [1,50]") {
    CHECK(clampFloor(1) == 1);
    CHECK(clampFloor(50) == 50);
    CHECK(clampFloor(25) == 25);
    CHECK(clampFloor(0) == 1);
    CHECK(clampFloor(51) == 50);
    CHECK(clampFloor(-5) == 1);
    CHECK(clampFloor(999) == 50);
}

// Written against DIFFICULTY_COUNT rather than the literal tier numbers: this case had pinned
// [0,2], so adding Inferno failed it for the RIGHT reason but told the reader nothing about what
// broke. Count-relative, it keeps testing the clamp's contract at whatever the top tier is.
TEST_CASE("clampDifficulty keeps difficulty in [0, DIFFICULTY_COUNT-1]") {
    const s32 top = static_cast<s32>(DIFFICULTY_COUNT) - 1;
    CHECK(clampDifficulty(0) == 0);
    CHECK(clampDifficulty(top) == top);
    CHECK(clampDifficulty(-1) == 0);
    CHECK(clampDifficulty(top + 1) == top);
    CHECK(clampDifficulty(999) == top);
}

// Inferno is the 4th tier (2026-08-02). Pinned because several systems key off the COUNT — the
// ladder's promotion bound, the save-load clamp, the unlock-file sanitize and the balance sweep —
// and a silent change here would move all of them at once.
TEST_CASE("Inferno is the final tier and every tier names itself") {
    CHECK(DIFFICULTY_COUNT == 4);
    CHECK(FINAL_DIFFICULTY == 3);
    CHECK(std::string(difficultyName(0)) == "Normal");
    CHECK(std::string(difficultyName(1)) == "Nightmare");
    CHECK(std::string(difficultyName(2)) == "Hell");
    CHECK(std::string(difficultyName(3)) == "Inferno");
    // Out of range must stay printf-safe rather than return nullptr.
    CHECK(std::string(difficultyName(200)) == "Normal");
}

// The "cleared" threshold deliberately did NOT follow the ladder up to Inferno: every hero who beat
// Hell before Inferno existed is stored as difficulty 2 / floor 51+, and this predicate is what
// gives them the town and Free-Play. Raising it to FINAL_DIFFICULTY would un-clear all of them.
TEST_CASE("adding a tier does not un-clear existing Hell heroes") {
    CHECK(saveCleared(51, 2));
    CHECK(saveCleared(57, 2));
    CHECK(saveCleared(51, FINAL_DIFFICULTY));
}
