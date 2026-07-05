// test_free_play.cpp — unit tests for the post-clear Free-Play predicates (game/free_play.h).
#include "doctest/doctest.h"
#include "game/free_play.h"

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

TEST_CASE("clampDifficulty keeps difficulty in [0,2]") {
    CHECK(clampDifficulty(0) == 0);
    CHECK(clampDifficulty(2) == 2);
    CHECK(clampDifficulty(-1) == 0);
    CHECK(clampDifficulty(3) == 2);
}
