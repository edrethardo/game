// test_shrine_buff.cpp — a shrine buff must never leave anything behind.
//
// This is the regression suite for a bug that reached a live character. VITALITY raises maxHealth
// for 45 s. The original code derived the amount to hand back from shrineBuffValue at expiry, and
// only did so if the buff slot still SAID vitality — but there is exactly one slot, so taking any
// other shrine while vitality was live overwrote it and the max-HP grant was never returned. It
// became permanent, compounded every time it recurred, and was written into the save file.
//
// A real Hell-50 paladin ended up at 44,922 maxHealth against a legitimate ~1,195: about eleven
// leaked shrines, compounding at 1.4x each. It silently made the game trivial, and it was mistaken
// for a difficulty-tuning problem — a balance change was shipped against the phantom number before
// anyone read the save file.
//
// Nothing here crashes, nothing fails to compile, and no smoke test catches it. Hence these.

#include <doctest/doctest.h>
#include "game/shrine.h"

namespace {

// The minimal surface Shrine::apply/tick/revokeHealth touch — stands in for Player and NetPlayer,
// which is precisely the point: both go through the same code, so both are covered.
struct FakePlayer {
    f32 health            = 130.0f;
    f32 maxHealth         = 130.0f;
    u8  shrineBuff        = ShrineBuff::NONE;
    f32 shrineBuffValue   = 0.0f;
    f32 shrineBuffTimer   = 0.0f;
    f32 shrineHealthBonus = 0.0f;
};

// Run the buff out.
void expire(FakePlayer& p) {
    for (int i = 0; i < 60 * 60 && p.shrineBuffTimer > 0.0f; i++)
        Shrine::tick(p, 1.0f / 60.0f);
}

} // namespace

TEST_CASE("Shrine: VITALITY grants max HP and gives all of it back on expiry") {
    FakePlayer p;
    const f32 base = p.maxHealth;

    Shrine::apply(p, ShrineBuff::VITALITY);
    CHECK(p.maxHealth == doctest::Approx(base * 1.4f));
    CHECK(p.health    == doctest::Approx(base * 1.4f));   // healed by the same amount: no bar lurch

    expire(p);
    CHECK(p.maxHealth == doctest::Approx(base));           // exactly back to where it started
    CHECK(p.shrineHealthBonus == doctest::Approx(0.0f));
    CHECK(p.shrineBuff == ShrineBuff::NONE);
}

TEST_CASE("Shrine: taking a DIFFERENT shrine while VITALITY is live does not orphan the max HP") {
    // THE BUG. Vitality, then speed before it expires. The old expiry checked `shrineBuff ==
    // VITALITY`, which by then read SPEED — so the +40% was never returned and became permanent.
    FakePlayer p;
    const f32 base = p.maxHealth;

    Shrine::apply(p, ShrineBuff::VITALITY);
    REQUIRE(p.maxHealth == doctest::Approx(base * 1.4f));

    Shrine::apply(p, ShrineBuff::SPEED);                   // overwrite the slot
    CHECK(p.maxHealth == doctest::Approx(base));           // vitality handed back immediately
    CHECK(p.shrineHealthBonus == doctest::Approx(0.0f));

    expire(p);
    CHECK(p.maxHealth == doctest::Approx(base));           // and stays there
}

TEST_CASE("Shrine: VITALITY does not stack with itself") {
    // Re-taking vitality used to add 40% of the ALREADY-boosted max and subtract only one grant's
    // worth, so each re-take leaked a permanent multiple.
    FakePlayer p;
    const f32 base = p.maxHealth;

    Shrine::apply(p, ShrineBuff::VITALITY);
    Shrine::apply(p, ShrineBuff::VITALITY);
    CHECK(p.maxHealth == doctest::Approx(base * 1.4f));    // refreshed, NOT 1.96x

    expire(p);
    CHECK(p.maxHealth == doctest::Approx(base));
}

TEST_CASE("Shrine: eleven shrines in a row leave max HP exactly where it started") {
    // The live save's actual shape: ~11 leaked vitality shrines compounding to 1.4^11 = 40x.
    // Interleave the types the way a real run does, and never expire cleanly between them.
    FakePlayer p;
    const f32 base = p.maxHealth;

    const u8 seq[] = {ShrineBuff::VITALITY, ShrineBuff::SPEED,    ShrineBuff::VITALITY,
                      ShrineBuff::POWER,    ShrineBuff::VITALITY, ShrineBuff::VITALITY,
                      ShrineBuff::SPEED,    ShrineBuff::VITALITY, ShrineBuff::POWER,
                      ShrineBuff::VITALITY, ShrineBuff::VITALITY};
    for (u8 b : seq) Shrine::apply(p, b);

    expire(p);
    CHECK(p.maxHealth == doctest::Approx(base));           // was: base * 40x
    CHECK(p.shrineHealthBonus == doctest::Approx(0.0f));
}

TEST_CASE("Shrine: a non-VITALITY shrine never touches max HP") {
    FakePlayer p;
    const f32 base = p.maxHealth;

    Shrine::apply(p, ShrineBuff::POWER);
    CHECK(p.maxHealth == doctest::Approx(base));
    Shrine::apply(p, ShrineBuff::SPEED);
    CHECK(p.maxHealth == doctest::Approx(base));

    expire(p);
    CHECK(p.maxHealth == doctest::Approx(base));
}

TEST_CASE("Shrine: current HP is clamped under the cap when VITALITY is revoked") {
    // On revoke the player must not be left sitting above their own maximum.
    FakePlayer p;
    Shrine::apply(p, ShrineBuff::VITALITY);
    p.health = p.maxHealth;            // topped up while buffed
    expire(p);
    CHECK(p.health <= p.maxHealth);
    CHECK(p.maxHealth == doctest::Approx(130.0f));
}

TEST_CASE("Shrine: revokeHealth is idempotent") {
    FakePlayer p;
    const f32 base = p.maxHealth;
    Shrine::apply(p, ShrineBuff::VITALITY);
    Shrine::revokeHealth(p);
    Shrine::revokeHealth(p);           // a second call must not keep draining max HP
    Shrine::revokeHealth(p);
    CHECK(p.maxHealth == doctest::Approx(base));
}

TEST_CASE("Shrine: the buff expires at all (it is not permanent)") {
    // Player::shrineBuff existed for a long time with no duration field at all — a buff, once set,
    // would never have ended. Pin that the timer actually runs out.
    FakePlayer p;
    Shrine::apply(p, ShrineBuff::POWER);
    CHECK(p.shrineBuffTimer == doctest::Approx(Shrine::DURATION_SEC));
    expire(p);
    CHECK(p.shrineBuff == ShrineBuff::NONE);
    CHECK(p.shrineBuffTimer == doctest::Approx(0.0f));
}
