// MinionScale — a summoned minion hits with its summoner's weapon.
//
// The Tinkerer and the Combat Engineer are the two classes whose damage is meant to COME from their
// minions, and they were the only two whose damage did not improve when they found a better weapon:
// drones and turrets were flat authored numbers scaled by a floor ramp and nothing else. Across a
// 3 h all-class dungeon soak they finished last and second-last, Tinkerer at 367 wdps against a
// Wanderer's 38,371.
//
// The property that makes this safe to ship is the FLOOR: early game the share is smaller than the
// authored number, so low levels are byte-identical to before. It can only lift the top end.

#include <doctest/doctest.h>
#include "game/minion_scale.h"

TEST_CASE("early game is unchanged for the DRONES — the authored value is a floor, not a base") {
    // A starting weapon is 6-18 base damage (items.json, levels 1-5). At that end the drone shares
    // sit below the authored numbers, so those minions keep exactly the damage they have always had
    // and none of the existing low-level tuning moves.
    const f32 startingWeapon = 18.0f;
    CHECK(MinionScale::damage(6.0f, 1.0f, startingWeapon, MinionScale::SHARE_SPIDER_DRONE)
          == doctest::Approx(6.0f));
    CHECK(MinionScale::damage(7.0f, 1.0f, startingWeapon, MinionScale::SHARE_SWARM_DRONE)
          == doctest::Approx(7.0f));
}

TEST_CASE("the TURRET is the deliberate exception, and that is a bug being fixed") {
    // The Combat Engineer's turret was authored at 3.0 damage and — unlike every other minion — the
    // floor ramp was never applied to it at all: its HP scaled with depth and its damage did not.
    // So it stayed at 3 on floor 1 and on floor 50 alike, which is not a balance choice, it is an
    // omission. It is therefore the one minion whose damage moves at LOW level too (3 -> 5.4 with a
    // starting weapon), and that is the point rather than a regression.
    const f32 startingWeapon = 18.0f;
    CHECK(MinionScale::damage(3.0f, 1.0f, startingWeapon, MinionScale::SHARE_TURRET)
          == doctest::Approx(5.4f));
    CHECK(MinionScale::damage(3.0f, 1.0f, startingWeapon, MinionScale::SHARE_TURRET) > 3.0f);
}

TEST_CASE("a better weapon makes a better minion") {
    // The whole point. Same floor, same minion, ten times the weapon.
    const f32 weak = MinionScale::damage(6.0f, 1.0f, 40.0f, MinionScale::SHARE_SPIDER_DRONE);
    const f32 geared = MinionScale::damage(6.0f, 1.0f, 400.0f, MinionScale::SHARE_SPIDER_DRONE);
    CHECK(geared > weak);
    CHECK(geared == doctest::Approx(100.0f));   // 400 x 0.25
}

TEST_CASE("the floor ramp still applies when it is the larger term") {
    // Deep floors without good gear must not get WORSE than they were. The authored ramp is still
    // there and still wins whenever it is bigger.
    const f32 deepNoGear = MinionScale::damage(6.0f, 12.0f, 18.0f, MinionScale::SHARE_SPIDER_DRONE);
    CHECK(deepNoGear == doctest::Approx(72.0f));   // 6 x 12, the share never gets a look in
}

TEST_CASE("shares are ordered by how many of each a build fields") {
    // A swarm bat must not be worth as much as the single turret or the elite queen, or the swarm is
    // strictly the best option at every gear level and the other minions are decoration.
    CHECK(MinionScale::SHARE_SWARM_DRONE < MinionScale::SHARE_SPIDER_DRONE);
    CHECK(MinionScale::SHARE_SPIDER_DRONE <= MinionScale::SHARE_TURRET);
    CHECK(MinionScale::SHARE_TURRET < MinionScale::SHARE_SWARM_QUEEN);
    // ...and none of them approaches the summoner's own swing, or minions replace playing the class.
    CHECK(MinionScale::SHARE_SWARM_QUEEN < 0.5f);
}

TEST_CASE("degenerate inputs cannot produce a negative or NaN minion") {
    CHECK(MinionScale::damage(6.0f, 1.0f, 0.0f, MinionScale::SHARE_TURRET) == doctest::Approx(6.0f));
    CHECK(MinionScale::damage(0.0f, 1.0f, 0.0f, MinionScale::SHARE_TURRET) == doctest::Approx(0.0f));
}
