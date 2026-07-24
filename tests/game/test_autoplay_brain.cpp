// tests/game/test_autoplay_brain.cpp — the priority state machine that composes survive/fight/
// travel/descend into one BotIntent per tick. Pure: BotView in, BotIntent out.
#include "doctest/doctest.h"
#include "game/autoplay_brain.h"
#include <cmath>   // fabsf

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

TEST_CASE("FIGHT skips a distant out-of-band target: travel toward the EXIT, not the straggler") {
    // A far LOS enemy must NOT preempt travel. Melee build (weaponRange 2, engageMax 0.60 =>
    // engage ceiling max(1.2, THREAT_RADIUS=12) = 12 m); the target at 40 m is well beyond it, so
    // the brain ignores the straggler and pushes along the flow field. The exit (flow) is toward +Z
    // while the straggler sits toward -Z — so the discriminator is the HEADING: an unbounded FIGHT
    // branch would face/walk toward the target (yaw ~0), the fixed brain faces the exit (yaw ~pi).
    // This is the VERTICAL_HALL dense-floor failure mode: far targets pulling the bot off-route.
    BotView v = baseView(); v.buildCell = 3*1 + 1;   // Moderate Melee
    v.weaponRange = 2.0f; v.flowDir = Vec3{0,0,1};   // exit is +Z
    BotTarget t{}; t.pos = {0,1.7f,-40}; t.dist = 40; t.hasLOS = true;   // straggler is -Z
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decide(v);
    CHECK_FALSE(out.fire);
    // Travelling toward +Z while still facing -Z (yaw 0): the WALK is resolved against the CURRENT
    // facing, so the correct first step is backward, and the aim turns to catch up.
    CHECK(out.moveBack);
    CHECK(fabsf(out.aimYaw) == doctest::Approx(3.14159f).epsilon(0.03)); // faces +Z (exit, yaw ~+-pi), not -Z (~0)
}

// --- TRAVEL movement is a WASD decomposition, not "hold W" ---------------------------------------
// applyBotIntent EASES the aim, so the facing lags the heading for a few tenths of a second after
// every turn. Holding W through that lag walks the bot wherever it happens to be pointing, which in
// the Descent's 3-wide corridors is the wall — the observed "looking at walls and hugging corners".
// Resolving the heading against the CURRENT facing means the bot travels the right way immediately
// and straightens up as the turn completes.
TEST_CASE("TRAVEL: heading dead ahead of the current facing is plain forward") {
    BotView v = baseView(); v.yaw = 0.0f; v.flowDir = Vec3{0,0,-1};   // yaw 0 faces -Z
    BotIntent out = decide(v);
    CHECK(out.moveFwd);
    CHECK_FALSE(out.moveBack); CHECK_FALSE(out.moveLeft); CHECK_FALSE(out.moveRight);
    CHECK(out.aimYaw == doctest::Approx(0.0f).epsilon(0.03));
}

TEST_CASE("TRAVEL: a heading 90 degrees off the facing STRAFES instead of walking into the wall") {
    BotView v = baseView(); v.yaw = 0.0f; v.flowDir = Vec3{1,0,0};   // exit is +X; bot still faces -Z
    BotIntent out = decide(v);
    CHECK(out.moveRight);                       // +X is dead right of a -Z facing
    CHECK_FALSE(out.moveFwd); CHECK_FALSE(out.moveBack);
    CHECK(out.aimYaw == doctest::Approx(-1.5708f).epsilon(0.03));  // yaw facing +X (atan2f(-1,-0))
}

TEST_CASE("TRAVEL: a diagonal heading engages both axes") {
    BotView v = baseView(); v.yaw = 0.0f; v.flowDir = normalize(Vec3{1,0,-1});  // forward-right
    BotIntent out = decide(v);
    CHECK(out.moveFwd);
    CHECK(out.moveRight);
    CHECK_FALSE(out.moveBack); CHECK_FALSE(out.moveLeft);
}

TEST_CASE("TRAVEL: once the eased aim has caught up the strafe decays to plain forward") {
    // Same exit as the strafe case, but the bot has finished turning. The lateral component is gone
    // — so the sidestep is a transient of the turn, not a permanent crab-walk.
    BotView v = baseView(); v.yaw = -1.5708f; v.flowDir = Vec3{1,0,0};
    BotIntent out = decide(v);
    CHECK(out.moveFwd);
    CHECK_FALSE(out.moveLeft); CHECK_FALSE(out.moveRight);
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
