// test_champion.cpp — champion affix roll rules.
//
// The roll is pure (floor + rng -> mask), which is exactly the part worth pinning: an illegal mask
// slipping through produces an unfightable monster (EXTRA_FAST + TELEPORTING closes every gap you
// make AND outruns you), and a mask the client can't colour produces an invisible elite. Neither
// fails to compile, and neither shows up in a smoke test — you'd find them in a player's review.

#include <doctest/doctest.h>
#include "game/champion.h"
#include <initializer_list>

TEST_CASE("Champion: affix count scales with depth") {
    CHECK(Champion::affixCountForFloor(1)  == 1);
    CHECK(Champion::affixCountForFloor(15) == 1);
    CHECK(Champion::affixCountForFloor(16) == 2);
    CHECK(Champion::affixCountForFloor(35) == 2);
    CHECK(Champion::affixCountForFloor(36) == 3);
    CHECK(Champion::affixCountForFloor(150) == 3);   // Hell floor 50 — must not run away
}

TEST_CASE("Champion: the exclusion rules reject the illegal combos") {
    // Unfightable pairing.
    CHECK_FALSE(Champion::affixesValid(ChampAffix::EXTRA_FAST | ChampAffix::TELEPORTING, true));
    CHECK(Champion::affixesValid(ChampAffix::EXTRA_FAST, true));
    CHECK(Champion::affixesValid(ChampAffix::TELEPORTING, true));

    // HEALTH_LINK splits damage onto minions — without minions it advertises a power it lacks.
    CHECK_FALSE(Champion::affixesValid(ChampAffix::HEALTH_LINK, /*hasMinions=*/false));
    CHECK(Champion::affixesValid(ChampAffix::HEALTH_LINK, /*hasMinions=*/true));
}

TEST_CASE("Champion: rollAffixes never emits an illegal mask") {
    // Sweep a wide slice of the rng space at every depth band and both pack shapes. A rejection bug
    // here is rare-by-construction, so a single sample would happily pass while shipping broken.
    for (u32 floor : {1u, 15u, 16u, 35u, 36u, 100u, 150u}) {
        for (bool hasMinions : {false, true}) {
            for (u32 seed = 1; seed < 2000; seed++) {
                u32 rng = seed;
                const u8 mask = Champion::rollAffixes(floor, hasMinions, rng);
                REQUIRE(Champion::affixesValid(mask, hasMinions));
            }
        }
    }
}

TEST_CASE("Champion: rollAffixes yields the requested number of affixes") {
    for (u32 floor : {1u, 20u, 40u}) {
        const u8 want = Champion::affixCountForFloor(floor);
        for (u32 seed = 1; seed < 500; seed++) {
            u32 rng = seed;
            const u8 mask = Champion::rollAffixes(floor, /*hasMinions=*/true, rng);

            u8 count = 0;
            for (u8 b = 0; b < ChampAffix::COUNT; b++)
                if (mask & static_cast<u8>(1u << b)) count++;

            // The exclusion table can legitimately cost at most one slot (a drawn candidate that
            // would make the mask illegal is skipped), so `want` is an upper bound, never a floor
            // of zero: a champion with no affixes is just a big monster.
            CHECK(count >= 1);
            CHECK(count <= want);
        }
    }
}

TEST_CASE("Champion: rollAffixes is deterministic for a given rng state") {
    // The host rolls and ships the mask on the wire, so determinism isn't load-bearing for co-op —
    // but a roll that can't be reproduced can't be debugged from a seed either.
    u32 a = 12345, b = 12345;
    CHECK(Champion::rollAffixes(40, true, a) == Champion::rollAffixes(40, true, b));
    CHECK(a == b);   // the stream advanced identically
}

TEST_CASE("Champion: rollAffixes advances the rng state") {
    u32 rng = 999;
    const u32 before = rng;
    Champion::rollAffixes(20, true, rng);
    CHECK(rng != before);   // a caller looping on one state must not get the same champion forever
}

TEST_CASE("Champion: tintFor is total over every possible mask") {
    // The CLIENT derives the tell from the replicated byte alone, so every one of the 256 masks must
    // produce a colour. A mask that fell through to black would render an invisible champion.
    for (u32 mask = 0; mask <= 0xFF; mask++) {
        const Vec3 c = Champion::tintFor(static_cast<u8>(mask));
        const bool lit = (c.x > 0.0f) || (c.y > 0.0f) || (c.z > 0.0f);
        CHECK(lit);
    }
}

TEST_CASE("Champion: a non-champion is untinted") {
    const Vec3 c = Champion::tintFor(ChampAffix::NONE);
    CHECK(c.x == doctest::Approx(1.0f));
    CHECK(c.y == doctest::Approx(1.0f));
    CHECK(c.z == doctest::Approx(1.0f));
}

TEST_CASE("Champion: every affix bit has a name") {
    // A nameless affix would show up as an empty string in the kill feed / debug overlay — the same
    // class of "ships, looks fine, says nothing" bug as the tooltips.
    for (u8 b = 0; b < ChampAffix::COUNT; b++) {
        const u8 bit = static_cast<u8>(1u << b);
        const char* n = Champion::affixName(bit);
        REQUIRE(n != nullptr);
        CHECK(n[0] != '\0');
    }
}

TEST_CASE("Champion: pack-size constants stay inside the entity budget") {
    // MAX_ENTITIES is 128 and is NOT being raised. Two packs of leader+minions must not be able to
    // crowd out the floor's normal spawns.
    const u32 worstCasePackBodies =
        Champion::MAX_PACKS_PER_FLOOR * (1u + Champion::MAX_MINIONS);
    CHECK(worstCasePackBodies <= 10);
    CHECK(Champion::MIN_MINIONS <= Champion::MAX_MINIONS);
    CHECK(Champion::MIN_FLOOR >= 1);
}
