// tests/game/test_autoplay_brain.cpp — the priority state machine that composes survive/fight/
// travel/descend into one BotIntent per tick. Pure: BotView in, BotIntent out.
#include "doctest/doctest.h"
#include "game/autoplay_brain.h"

using namespace Autoplay;

static BotView baseView() {
    BotView v{}; v.eyeHeight = 1.7f; v.hp = 100; v.maxHp = 100; v.energy = 100; v.maxEnergy = 100;
    v.onGround = true; v.weaponRange = 20.0f; v.weaponProjSpeed = 40.0f; v.buildCell = 3*1+2;
    v.onNormalFloor = true; v.flowValid = true; v.flowDir = Vec3{0,0,1}; return v;
}

TEST_CASE("SURVIVE wins: low HP with potion ready => drink") {
    BotView v = baseView(); v.hp = 20; v.potionReady = true;   // 20% < Moderate 50% threshold
    BotIntent out = decide(v);
    CHECK(out.potion);
}

TEST_CASE("FIGHT over TRAVEL: an in-band LOS target => fire, not walk to exit") {
    BotView v = baseView();
    BotTarget t{}; t.pos = {0,1.7f,15}; t.dist = 15; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decide(v);
    CHECK(out.fire);
}

TEST_CASE("TRAVEL when no targets: face the flow direction and walk forward") {
    BotView v = baseView(); v.flowDir = Vec3{1,0,0};   // exit is +X
    BotIntent out = decide(v);
    CHECK(out.moveFwd);
    CHECK(out.aimYaw == doctest::Approx(-1.5708f).epsilon(0.03));  // yaw facing +X (atan2f(-1,-0))
}

TEST_CASE("DESCEND: at the door with the boss dead => request descent") {
    BotView v = baseView(); v.atExit = true; v.flowDir = Vec3{0,0,0};
    v.doorActive = true; v.distToDoor = 1.0f; v.hasBoss = false; v.bossAlive = false;
    BotIntent out = decide(v);
    CHECK(out.descend);
}

TEST_CASE("no descend while the boss lives (walk into the fight instead)") {
    BotView v = baseView(); v.doorActive = true; v.distToDoor = 1.0f;
    v.hasBoss = true; v.bossAlive = true;
    BotTarget boss{}; boss.pos = {0,1.7f,12}; boss.dist = 12; boss.hasLOS = true; boss.isBoss = true;
    v.targets = &boss; v.targetCount = 1;
    BotIntent out = decide(v);
    CHECK_FALSE(out.descend);
    CHECK(out.fire);               // fight the boss
}

TEST_CASE("idle in a non-normal world (town/arena): no movement, no fire") {
    BotView v = baseView(); v.onNormalFloor = false;
    v.flowDir = Vec3{1,0,0};
    BotIntent out = decide(v);
    CHECK_FALSE(out.moveFwd);
    CHECK_FALSE(out.fire);
    CHECK_FALSE(out.descend);
}

TEST_CASE("stunned: emits no move/fire/dodge (CC correctness even in the brain)") {
    BotView v = baseView(); v.stunned = true;
    BotTarget t{}; t.pos={0,1.7f,15}; t.dist=15; t.hasLOS=true; v.targets=&t; v.targetCount=1;
    BotIntent out = decide(v);
    CHECK_FALSE(out.fire);
    CHECK_FALSE(out.moveFwd);
    CHECK_FALSE(out.dodge);
}
