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

TEST_CASE("engagement range: projectile weapons carry no authored range and must not read as 0") {
    // items.json authors baseRange ONLY for melee/hitscan; every wand/bow/staff/crossbow has none.
    // A 0 here multiplies the whole doctrine band to 0, so the bot can never fire — the second half
    // of the sorcerer bug.
    CHECK(botWeaponRange(4.2f, 0.0f)  == doctest::Approx(4.2f));    // sword: authored, verbatim
    CHECK(botWeaponRange(50.0f, 0.0f) == doctest::Approx(50.0f));   // pistol: authored, verbatim
    CHECK(botWeaponRange(0.0f, 18.4f) > 12.0f);                     // wand: derived from flight
    CHECK(botWeaponRange(0.0f, 28.8f) == doctest::Approx(24.0f));   // fast bolt: capped, not 86 m
    CHECK(botWeaponRange(0.0f, 0.0f)  > 0.0f);                      // data hole must never mute fire
}

TEST_CASE("fires WHILE kiting: a target inside engageMin is still shot at") {
    // The kite floor is a MOVEMENT rule (back up to restore spacing), not a fire rule — you always
    // shoot the thing in your face while retreating. Gating fire on the band made a swarmed
    // ranged/caster bot backpedal forever without ever shooting (sorcerers stuck on floor 1).
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, -5.0f}; t.dist = 5.0f; t.hasLOS = true;  // 5 m < the 11 m floor
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.moveBack);          // still kites out
    CHECK(out.fire);              // ...and shoots on the way
}

TEST_CASE("magic build fires while backing out of its own kite floor") {
    BotView v = selfAt({0,0,0});
    v.buildCell = 3*1+0;          // Moderate / Magic: band 0.30-0.75 x 20 = 6..15 m
    BotTarget t{}; t.pos = {0, 1.7f, -3.0f}; t.dist = 3.0f; t.hasLOS = true;  // inside the 6 m floor
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.moveBack);
    CHECK(out.fire);
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

TEST_CASE("glass-cannon melee dodges a closer despite a zero kite-floor") {
    // Cell 7 = Glass/Melee: dodgesProactively AND engageMin=0. lo=0 would make a lo-based dodge
    // threshold `dist<0` (dead), so the reach-based fallback dodgeRef = engageMax*range must fire it.
    BotView v = selfAt({0,0,0});
    v.buildCell = 3*2+1;                          // Glass Cannon / Melee
    v.weaponRange = 2.0f; v.weaponProjSpeed = 0.0f; v.weaponIsMelee = true; v.dodgeCooldown = 0.0f;
    // dodgeRef = 0.60*2.0 = 1.2; trigger = dodgeRef*0.6 = 0.72 m. 0.5 m is inside.
    BotTarget t{}; t.pos = {0, 1.7f, -0.5f}; t.dist = 0.5f; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.dodge);
}

TEST_CASE("glass-cannon melee holds the dodge when the closer is past the reach reference") {
    BotView v = selfAt({0,0,0});
    v.buildCell = 3*2+1;                          // Glass Cannon / Melee
    v.weaponRange = 2.0f; v.weaponProjSpeed = 0.0f; v.weaponIsMelee = true; v.dodgeCooldown = 0.0f;
    // 1.9 m is well outside the 0.72 m dodge trigger.
    BotTarget t{}; t.pos = {0, 1.7f, -1.9f}; t.dist = 1.9f; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK_FALSE(out.dodge);
}

TEST_CASE("casts a castable class skill while engaging (a Magic build plays its build)") {
    BotView v = selfAt({0,0,0});
    v.buildCell = 3*1+0;                          // Moderate / Magic — its skills ARE its damage
    BotTarget t{}; t.pos = {0, 1.7f, -10.0f}; t.dist = 10.0f; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    v.castableSkill[0] = true;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.classSkillSlot == 0);
}

TEST_CASE("never presses a skill that would no-op: nothing castable => no cast") {
    BotView v = selfAt({0,0,0});
    v.buildCell = 3*1+0;
    BotTarget t{}; t.pos = {0, 1.7f, -10.0f}; t.dist = 10.0f; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;             // castableSkill all false (locked/no energy/on CD)
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.classSkillSlot == -1);
}

TEST_CASE("does not cast at nothing: no LOS target => no cast") {
    BotView v = selfAt({0,0,0});
    v.castableSkill[0] = v.castableSkill[1] = true;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));   // targetCount 0
    CHECK(out.classSkillSlot == -1);
}

TEST_CASE("prefers the lowest castable slot") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, -15.0f}; t.dist = 15.0f; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    v.castableSkill[2] = v.castableSkill[3] = true;   // 0/1 unavailable this tick
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.classSkillSlot == 2);
}

TEST_CASE("casts while kiting a target inside the engage floor") {
    // Same rationale as the fire fix: a swarm in your face is exactly when a caster wants its nova.
    BotView v = selfAt({0,0,0});
    v.buildCell = 3*1+0;                          // Magic: 6 m kite floor
    BotTarget t{}; t.pos = {0, 1.7f, -2.0f}; t.dist = 2.0f; t.hasLOS = true;
    v.targets = &t; v.targetCount = 1;
    v.castableSkill[0] = true;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.moveBack);
    CHECK(out.classSkillSlot == 0);
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

// ---------------------------------------------------------------------------------------------
// (A) SMOOTH AIM — stepAngle / angleDelta. The driver used to write the lead-corrected yaw straight
// onto the player (an instant snap = the aimbot look); it now eases there at a bounded turn rate.
// The ±π seam is the whole risk: a naive `desired - current` sends the bot the LONG way round
// (a 340° sweep for a 20° correction), which reads far worse than the snap it replaces.
// ---------------------------------------------------------------------------------------------
static constexpr f32 kPi = 3.14159265358979f;

TEST_CASE("angleDelta takes the shortest signed path across the seam") {
    CHECK(angleDelta(0.0f, 1.0f)   == doctest::Approx(1.0f));
    CHECK(angleDelta(1.0f, 0.0f)   == doctest::Approx(-1.0f));
    // 3.0 -> -3.0 is +0.283 the short way (through π), NOT -6.0.
    CHECK(angleDelta(3.0f, -3.0f)  == doctest::Approx(2.0f * kPi - 6.0f));
    CHECK(angleDelta(-3.0f, 3.0f)  == doctest::Approx(6.0f - 2.0f * kPi));
    // The engine never re-wraps player yaw, so after a long run it can sit many turns out of range.
    CHECK(angleDelta(0.0f, 4.0f * kPi + 0.5f) == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(angleDelta(6.0f * kPi, -0.5f)       == doctest::Approx(-0.5f).epsilon(0.001));
}

TEST_CASE("stepAngle moves by exactly rate*dt when the target is far") {
    // 8 rad/s over one 60 Hz tick = 0.1333 rad; a π/2 error is many steps away.
    const f32 got = stepAngle(0.0f, kPi * 0.5f, 8.0f, 1.0f / 60.0f);
    CHECK(got == doctest::Approx(8.0f / 60.0f));
}

TEST_CASE("stepAngle snaps exactly when the target is within one step") {
    CHECK(stepAngle(0.0f, 0.05f, 8.0f, 1.0f / 60.0f) == doctest::Approx(0.05f));
    CHECK(stepAngle(0.0f, -0.05f, 8.0f, 1.0f / 60.0f) == doctest::Approx(-0.05f));
    // Across the seam: 0.283 rad apart, one big step covers it, so it lands ON desired.
    CHECK(stepAngle(3.0f, -3.0f, 100.0f, 1.0f / 60.0f) == doctest::Approx(-3.0f));
}

TEST_CASE("stepAngle crosses the seam the SHORT way, not backwards") {
    // +3.0 -> -3.0: the short way is INCREASING through π. A sign slip here sweeps ~5.7 rad the
    // wrong way — the single worst-looking failure mode of the whole feature.
    const f32 up = stepAngle(3.0f, -3.0f, 8.0f, 1.0f / 60.0f);
    CHECK(up > 3.0f);
    CHECK(up == doctest::Approx(3.0f + 8.0f / 60.0f));
    // ...and the mirror case goes the other way.
    const f32 down = stepAngle(-3.0f, 3.0f, 8.0f, 1.0f / 60.0f);
    CHECK(down < -3.0f);
}

TEST_CASE("stepAngle is a no-op on zero dt and on an already-equal angle") {
    CHECK(stepAngle(1.234f, -2.0f, 8.0f, 0.0f) == doctest::Approx(1.234f));
    CHECK(stepAngle(1.234f, 1.234f, 8.0f, 1.0f / 60.0f) == doctest::Approx(1.234f));
    CHECK(stepAngle(1.234f, -2.0f, 0.0f, 1.0f / 60.0f) == doctest::Approx(1.234f));
}

TEST_CASE("stepAngle converges on the target without overshooting") {
    f32 a = 0.0f;
    for (u32 i = 0; i < 200; i++) a = stepAngle(a, 2.5f, 8.0f, 1.0f / 60.0f);
    CHECK(a == doctest::Approx(2.5f));
}

TEST_CASE("aim wobble is small, deterministic and actually moves") {
    f32 y0, p0, y1, p1;
    aimWobble(1000, y0, p0);
    aimWobble(1000, y1, p1);
    CHECK(y0 == doctest::Approx(y1));            // deterministic: no rand()
    CHECK(p0 == doctest::Approx(p1));
    for (u32 tick = 0; tick < 4000; tick += 7) { // bounded: never enough to miss a body
        f32 y, p; aimWobble(tick, y, p);
        CHECK(fabsf(y) < 0.02f);                 // < ~1.2 deg
        CHECK(fabsf(p) < 0.02f);
    }
    f32 ya, pa, yb, pb;                          // ...but it is not a constant
    aimWobble(0, ya, pa); aimWobble(120, yb, pb);
    CHECK(fabsf(ya - yb) > 1e-4f);
}
