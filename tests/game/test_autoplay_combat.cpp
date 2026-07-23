// tests/game/test_autoplay_combat.cpp — pure combat policy: pick a target, aim (leading for
// projectiles), and decide fire/move per the doctrine's engagement band. No engine — BotView is
// hand-built.
//
// AIM-SIGN CONVENTION (load-bearing): the engine forward is {-sinYaw*cosPitch, sinPitch,
// -cosYaw*cosPitch} (player.cpp:80), so at yaw 0 forward = (0,0,-1) = -Z, and the inverse used
// engine-wide is yaw = atan2f(-dir.x, -dir.z) (see engine_arena.cpp:55 "yaw = atan2(-dx,-dz)").
// STRAIGHT AHEAD is therefore -Z, so the "target ahead" cases below sit at -Z and pin aimYaw ~ 0;
// +Z would be yaw ~ +/-pi (behind) and would pin the WRONG convention.
#include "doctest/doctest.h"
#include "game/autoplay_combat.h"
#include "game/autoplay_doctrine.h"

using namespace Autoplay;

static BotView selfAt(Vec3 p) {
    BotView v{}; v.pos = p; v.eyeHeight = 1.7f; v.hp = 100; v.maxHp = 100;
    v.energy = 100; v.maxEnergy = 100; v.onGround = true; v.weaponRange = 20.0f;
    v.weaponProjSpeed = 40.0f; v.weaponIsMelee = false; v.buildCell = 3*1+2; // Moderate/Ranged
    v.onNormalFloor = true; return v;
}

TEST_CASE("aims at the only target and fires when inside the engagement band") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, -15.0f}; t.dist = 15.0f; t.hasLOS = true;  // -Z = straight ahead
    v.targets = &t; v.targetCount = 1;
    const Doctrine d = doctrineFor(v.buildCell);
    BotIntent out = decideCombat(v, d);
    // Ranged band 0.55-1.0 x 20 = 11..20 m; 15 m is inside => fire, aim toward -Z (yaw 0).
    CHECK(out.fire);
    CHECK(out.aimYaw == doctest::Approx(0.0f).epsilon(0.02));  // -Z is yaw 0 in the engine convention
}

TEST_CASE("holds fire and advances when the target is beyond engageMax") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, -40.0f}; t.dist = 40.0f; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK_FALSE(out.fire);        // 40 m > 20 m max range
    CHECK(out.moveFwd);           // close the distance
}

TEST_CASE("kites: backs off when the target is inside engageMin") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, -5.0f}; t.dist = 5.0f; t.hasLOS = true;  // 5 m < 11 m floor
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.moveBack);          // Ranged kite floor: retreat
}

TEST_CASE("does not fire without line of sight") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, -15.0f}; t.dist = 15.0f; t.hasLOS = false;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK_FALSE(out.fire);
}

TEST_CASE("leads a crossing target: aim yaw is offset ahead of its position") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, -15.0f}; t.vel = {8.0f, 0, 0};  // ahead (-Z), moving +X fast
    t.dist = 15.0f; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.fire);
    CHECK(out.aimYaw < 0.0f);     // lead point is +X of the target => aim rotates toward +X (yaw<0)
}

TEST_CASE("picks the nearest LOS target when several exist") {
    BotView v = selfAt({0,0,0});
    BotTarget ts[2];
    ts[0] = {}; ts[0].pos = {0,1.7f,-30}; ts[0].dist = 30; ts[0].hasLOS = true;
    ts[1] = {}; ts[1].pos = {0,1.7f,-14}; ts[1].dist = 14; ts[1].hasLOS = true;
    v.targets = ts; v.targetCount = 2;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.fire);              // the 14 m one is in band
}
