// test_arena.cpp — pure PvP deathmatch rules for Arena mode (game/arena.h).
//
// Pins the three rules the engine trusts blindly: kill credit lands on the right slot
// (an unattributed death credits NO one), the win fires exactly at KILL_TARGET, and the
// respawn pad is the max-min-distance pick with a deterministic tie-break — host and
// clients must agree on the pad without it ever being on the wire.

#include "doctest/doctest.h"
#include "game/arena.h"

TEST_CASE("Arena: recordKill credits the killer and detects the win") {
    Arena::Score s{};
    u8 winner = 0xFF;
    for (u32 i = 0; i < Arena::KILL_TARGET - 1; i++)
        CHECK(!Arena::recordKill(s, 2, winner));
    CHECK(s.kills[2] == Arena::KILL_TARGET - 1);
    CHECK(winner == 0xFF);                       // untouched until the win
    CHECK(Arena::recordKill(s, 2, winner));      // the KILL_TARGET-th kill wins
    CHECK(winner == 2);
}

TEST_CASE("Arena: unattributed deaths credit no one") {
    Arena::Score s{};
    u8 winner = 0xFF;
    CHECK(!Arena::recordKill(s, 0xFF, winner));  // environmental / unknown killer
    CHECK(!Arena::recordKill(s, Arena::MAX_COMBATANTS, winner));  // out-of-range slot
    for (u32 i = 0; i < Arena::MAX_COMBATANTS; i++) CHECK(s.kills[i] == 0);
    CHECK(winner == 0xFF);
}

TEST_CASE("Arena: independent scores, first past the post wins") {
    Arena::Score s{};
    u8 winner = 0xFF;
    for (u32 i = 0; i < Arena::KILL_TARGET - 1; i++) {
        CHECK(!Arena::recordKill(s, 0, winner));
        CHECK(!Arena::recordKill(s, 1, winner));
    }
    CHECK(Arena::recordKill(s, 1, winner));      // slot 1 lands its 10th first
    CHECK(winner == 1);
    CHECK(s.kills[0] == Arena::KILL_TARGET - 1);
}

TEST_CASE("Arena: farthestPad maximizes distance to the nearest hostile") {
    const Vec3 pads[4] = {{2, 0, 2}, {34, 0, 2}, {2, 0, 34}, {34, 0, 34}};
    const Vec3 foes[2] = {{4, 0, 4}, {30, 0, 6}};    // both crowd the north pads
    CHECK(Arena::farthestPad(pads, 4, foes, 2) == 2); // SW pad is farthest from both

    // A hostile camping the SW pad pushes the pick to SE.
    const Vec3 camper[1] = {{2, 0, 34}};
    CHECK(Arena::farthestPad(pads, 4, camper, 1) == 1);
}

TEST_CASE("Arena: farthestPad with no hostiles is deterministic (pad 0)") {
    const Vec3 pads[4] = {{2, 0, 2}, {34, 0, 2}, {2, 0, 34}, {34, 0, 34}};
    CHECK(Arena::farthestPad(pads, 4, nullptr, 0) == 0);
}

TEST_CASE("Arena: farthestPad ignores Y (pads and players share the floor plane)") {
    const Vec3 pads[2] = {{0, 0, 0}, {10, 0, 0}};
    const Vec3 foe[1]  = {{1, 50.0f, 0}};            // absurd Y must not matter
    CHECK(Arena::farthestPad(pads, 2, foe, 1) == 1);
}

// --- WB-297: the loot-escalation rules are the mode's economy — pin the curve. -------------
TEST_CASE("Arena loot: the wave ramp is monotonic and capped at the ladder end") {
    u8 prev = 0;
    for (u32 w = 0; w < 20; w++) {
        const u8 lvl = Arena::lootItemLevel(w);
        CHECK(lvl >= prev);          // never weaker than the wave before
        CHECK(lvl <= 50);            // the game's own ladder end is the cap
        prev = lvl;
    }
    CHECK(Arena::lootItemLevel(0) >= 6);    // wave 0 already beats a starting weapon
    CHECK(Arena::lootItemLevel(8) == 50);   // ~2 minutes to the top: first-to-5, not a marathon
}

TEST_CASE("Arena loot: late waves force legendary, early waves never do") {
    for (u32 w = 0; w < 4; w++) CHECK_FALSE(Arena::lootWaveForcesLegendary(w));
    bool any = false;
    for (u32 w = 4; w < 12; w++) any = any || Arena::lootWaveForcesLegendary(w);
    CHECK(any);
}

TEST_CASE("Arena loot: the anchor pick never repeats the previous anchor") {
    for (u32 roll = 0; roll < 40; roll++)
        for (s32 last = 0; last < 6; last++) {
            const u32 pick = Arena::nextLootAnchor(roll, last, 6);
            CHECK(pick < 6);
            CHECK(static_cast<s32>(pick) != last);
        }
    // Degenerate single-anchor map: the rule must still terminate (repeat allowed there).
    CHECK(Arena::nextLootAnchor(7, 0, 1) == 0);
}
