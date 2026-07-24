// test_resurrect_cap.cpp — which corpses a necromancer is allowed to raise (game/entity.h
// corpseRaisable + RESURRECT_MAX).
//
// The rule is shared by the SUMMONER role and the HEALER role's no-one-to-heal fallback, which is
// why it lives in one inline predicate instead of as two copies of five `continue`s. Each guard
// fails in a way you would only notice in play, not in a smoke test: raising a boss re-locks a floor
// the player already cleared, raising a corpse whose deathTimer has run out resurrects a pool slot
// that has already been handed to something else, and no cap at all means one necromancer standing
// over one body raises it every 3 s forever.

#include <doctest/doctest.h>
#include "game/entity.h"

namespace {
// A corpse in the state the AI's pool scan would find it in: dead, hostile, still within its
// death timer, not a boss, never raised.
Entity freshCorpse() {
    Entity e{};
    e.flags      = ENT_ACTIVE | ENT_DEAD;
    e.deathTimer = 1.0f;
    e.isBoss     = false;
    e.timesRevived = 0;
    return e;
}
} // namespace

TEST_CASE("resurrect: a fresh hostile corpse is raisable") {
    CHECK(corpseRaisable(freshCorpse()));
}

TEST_CASE("resurrect: a corpse is spent after RESURRECT_MAX raises") {
    Entity e = freshCorpse();
    // Walk it up to the cap the way the AI does — one increment per raise.
    for (u8 i = 0; i < RESURRECT_MAX; i++) {
        CHECK(corpseRaisable(e));      // still eligible on every raise up to the cap
        e.timesRevived++;
    }
    CHECK_FALSE(corpseRaisable(e));    // the (RESURRECT_MAX+1)-th is refused
    CHECK(e.timesRevived == RESURRECT_MAX);
}

TEST_CASE("resurrect: the cap is exactly 10 raises") {
    // Pinned as a number because it is a balance decision, not an implementation detail: a change
    // here changes how long a necromancer fight can be stalled.
    CHECK(RESURRECT_MAX == 10);
    Entity e = freshCorpse();
    e.timesRevived = 9;  CHECK(corpseRaisable(e));
    e.timesRevived = 10; CHECK_FALSE(corpseRaisable(e));
}

TEST_CASE("resurrect: a spent corpse stays spent past the cap") {
    // timesRevived is a u8 and nothing decrements it, but the test states the intent: once over the
    // line it never comes back, however the counter got there.
    Entity e = freshCorpse();
    e.timesRevived = 255;
    CHECK_FALSE(corpseRaisable(e));
}

TEST_CASE("resurrect: the living, the friendly, and bosses are never raised") {
    Entity alive = freshCorpse(); alive.flags = ENT_ACTIVE;            // not dead
    CHECK_FALSE(corpseRaisable(alive));

    Entity pet = freshCorpse(); pet.flags |= ENT_FRIENDLY;             // your side, not theirs
    CHECK_FALSE(corpseRaisable(pet));

    Entity boss = freshCorpse(); boss.isBoss = true;                   // would re-lock the floor exit
    CHECK_FALSE(corpseRaisable(boss));

    Entity expiring = freshCorpse(); expiring.deathTimer = 0.0f;       // slot about to be freed
    CHECK_FALSE(corpseRaisable(expiring));
}

TEST_CASE("resurrect: a default-constructed entity is a clean slate") {
    // EntitySystem::spawn clears timesRevived for exactly this reason — a recycled slot that kept a
    // spent corpse's tally would spawn a brand-new enemy that could never be raised.
    Entity e{};
    CHECK(e.timesRevived == 0);
}
