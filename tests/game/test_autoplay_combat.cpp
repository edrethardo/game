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

TEST_CASE("kites: backs off when a MELEE target is inside engageMin") {
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, -5.0f}; t.dist = 5.0f; t.hasLOS = true;  // 5 m < 11 m floor
    t.isRanged = false; t.attackRange = 2.0f;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.moveBack);          // Ranged kite floor vs a closer: retreat is real spacing
}

TEST_CASE("does NOT kite away from a RANGED enemy inside engageMin") {
    // Aaron, watching the bot: "it runs away from ranged enemies". Backing off only buys spacing
    // from something that must CLOSE to hurt you; an archer/caster shoots you the whole way, so the
    // retreat gives up ground, breaks the bot's own aim, and changes nothing about the incoming fire.
    BotView v = selfAt({0,0,0});
    BotTarget t{}; t.pos = {0, 1.7f, -5.0f}; t.dist = 5.0f; t.hasLOS = true;  // 5 m < the 11 m floor
    t.isRanged = true; t.attackRange = 12.0f;
    v.targets = &t; v.targetCount = 1;
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK_FALSE(out.moveBack);    // HOLD the ground
    CHECK_FALSE(out.moveFwd);     // ...and don't charge either: we're already inside the band
    CHECK(out.fire);              // but keep shooting it
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
    t.attackRange = 2.0f;                                                     // a MELEE closer
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

// The rate cap alone (v1) swept at constant speed and stopped dead on arrival — still a machine.
// stepAngle is now PROPORTIONAL (speed ∝ remaining error) *and* rate-capped, so these cases pin
// both regimes: the cap governs a big error, the gain governs the last stretch.
static constexpr f32 kDt = 1.0f / 60.0f;

TEST_CASE("stepAngle is rate-capped when the error is large") {
    // gain 6 on a π/2 error would want 6*1.571 = 9.4 rad/s; the 3 rad/s cap must win, exactly.
    const f32 got = stepAngle(0.0f, kPi * 0.5f, 6.0f, 3.0f, kDt);
    CHECK(got == doctest::Approx(3.0f * kDt));
    // A cap high enough to never bind hands the step back to the gain: one tick consumes
    // 1-exp(-gain*dt) of the error, NOT all of it and not a fixed rate*dt.
    const f32 eased = stepAngle(0.0f, kPi * 0.5f, 6.0f, 1000.0f, kDt);
    CHECK(eased == doctest::Approx(kPi * 0.5f * (1.0f - expf(-6.0f * kDt))));
    // ...and at THIS error the gain wants to move FASTER than the cap allows — which is the whole
    // reason the far-field cap exists. (Gain alone would start a 180° turn at ~19 rad/s.)
    CHECK(eased > 3.0f * kDt);
}

TEST_CASE("stepAngle eases out: each step is a bounded fraction of the remaining error") {
    // The signature of the new model — the step SHRINKS as the crosshair converges, where the old
    // constant-rate version made the same-size step every tick until it stopped dead.
    const f32 gain = 6.0f, cap = 1000.0f;        // cap lifted so the gain is what's under test
    f32 a = 0.0f; const f32 target = 0.6f;
    f32 prevStep = 1e9f;
    for (u32 i = 0; i < 80; i++) {
        const f32 before = a;
        a = stepAngle(a, target, gain, cap, kDt);
        const f32 step = a - before;
        CHECK(step > 0.0f);                      // always makes progress
        CHECK(step < (target - before));         // ...but never all of it: no snap
        CHECK(step < prevStep);                  // strictly decelerating = the ease-out
        prevStep = step;
    }
    CHECK(a == doctest::Approx(target).epsilon(0.01));   // and it does arrive
}

TEST_CASE("stepAngle lands exactly instead of asymptoting forever") {
    // An exponential approach never actually ARRIVES, so a hair-width residual would sit there
    // being chased every tick. Inside kSnapEps it lands.
    CHECK(stepAngle(0.0f, 5e-5f, 6.0f, 3.0f, kDt) == doctest::Approx(5e-5f));
    // ...and a degenerate gain that would consume the whole error lands too, never overshoots.
    CHECK(stepAngle(0.0f, 0.05f, 5000.0f, 1e6f, kDt) == doctest::Approx(0.05f));
    CHECK(stepAngle(0.0f, -0.05f, 5000.0f, 1e6f, kDt) == doctest::Approx(-0.05f));
    // Across the seam: 0.283 rad apart, one huge step covers it, so it lands ON desired.
    CHECK(stepAngle(3.0f, -3.0f, 5000.0f, 1e6f, kDt) == doctest::Approx(-3.0f));
}

TEST_CASE("stepAngle crosses the seam the SHORT way, not backwards") {
    // +3.0 -> -3.0: the short way is INCREASING through π. A sign slip here sweeps ~5.7 rad the
    // wrong way — the single worst-looking failure mode of the whole feature.
    const f32 up = stepAngle(3.0f, -3.0f, 6.0f, 3.0f, kDt);
    CHECK(up > 3.0f);
    // 0.283 rad of error is inside the cap's reach, so the GAIN governs the size of the step.
    const f32 d = angleDelta(3.0f, -3.0f);
    CHECK(up == doctest::Approx(3.0f + d * (1.0f - expf(-6.0f * kDt))));
    // ...and the mirror case goes the other way.
    const f32 down = stepAngle(-3.0f, 3.0f, 6.0f, 3.0f, kDt);
    CHECK(down < -3.0f);
}

TEST_CASE("stepAngle is a no-op on zero dt and on an already-equal angle") {
    CHECK(stepAngle(1.234f, -2.0f, 6.0f, 3.0f, 0.0f) == doctest::Approx(1.234f));
    CHECK(stepAngle(1.234f, 1.234f, 6.0f, 3.0f, kDt) == doctest::Approx(1.234f));
    CHECK(stepAngle(1.234f, -2.0f, 0.0f, 0.0f, kDt) == doctest::Approx(1.234f));  // no gain, no rate
}

TEST_CASE("stepAngle converges on the target without overshooting") {
    f32 a = 0.0f;
    for (u32 i = 0; i < 200; i++) a = stepAngle(a, 2.5f, 6.0f, 3.0f, kDt);
    CHECK(a == doctest::Approx(2.5f));
    // ...and from the far side, so a sign error in the ease can't hide.
    f32 b = 5.0f;
    for (u32 i = 0; i < 200; i++) b = stepAngle(b, 2.5f, 6.0f, 3.0f, kDt);
    CHECK(b == doctest::Approx(2.5f));
}

TEST_CASE("stepAngle is tick-rate independent: same angle at the same wall-clock time") {
    // The exponential form is integrated exactly, so halving dt and doubling the step count must
    // land in the same place. A naive gain*err*dt would drift apart here — and the engine really
    // does double-step (Engine::run catches up to 4 sim steps in one frame).
    const f32 gain = 6.0f, cap = 1000.0f, target = 1.2f;
    f32 slow = 0.0f;  for (u32 i = 0; i < 30; i++)  slow = stepAngle(slow, target, gain, cap, kDt);
    f32 fast = 0.0f;  for (u32 i = 0; i < 60; i++)  fast = stepAngle(fast, target, gain, cap, kDt * 0.5f);
    CHECK(fast == doctest::Approx(slow).epsilon(0.001));
}

TEST_CASE("stepAngle at the shipped tune is far slower than the v1 constant-rate sweep") {
    // Aaron's feel note in numbers. v1 moved 7 rad/s (0.1167 rad/tick) for ANY error, however
    // small; the shipped gain/cap pair is bounded well under that and collapses as it converges.
    const f32 gain = 6.0f, fine = 2.8f;
    const f32 v1FineStep = 7.0f * kDt;                                              // 0.1167 rad
    CHECK(stepAngle(0.0f, 3.0f,  gain, fine, kDt) == doctest::Approx(fine * kDt));  // worst case
    CHECK(fine * kDt < v1FineStep * 0.45f);                     // even saturated, well under v1
    // A 10° correction — the common tracking case — now moves a fifth of what v1 moved.
    const f32 tenDeg = 10.0f * kPi / 180.0f;
    CHECK(stepAngle(0.0f, tenDeg, gain, fine, kDt) < v1FineStep * 0.2f);
}

// ---------------------------------------------------------------------------------------------
// FIRE ALIGNMENT GATE. decideCombat decides `fire` from the DESIRED aim, but the driver only EASES
// the real crosshair toward it (stepAngle above) — so the bot used to pull the trigger mid-turn and
// spray every wall the crosshair swept across (measured live: 22% of its shots had geometry between
// muzzle and target, at a mean yaw error of 0.47 rad / 27°). aimOnTarget is the gate.
// ---------------------------------------------------------------------------------------------
TEST_CASE("aimOnTarget: holds fire while the crosshair is still sweeping onto the target") {
    // 27° off — the measured mean error of a pre-fix wall shot. Nothing may fire there.
    CHECK_FALSE(aimOnTarget(0.0f, 0.0f, 0.471f, 0.0f, /*melee=*/false));
    CHECK(aimOnTarget(0.0f, 0.0f, 0.0f, 0.0f, false));            // arrived
    CHECK(aimOnTarget(0.0f, 0.0f, FIRE_ALIGN_RAD * 0.9f, 0.0f, false));
    CHECK_FALSE(aimOnTarget(0.0f, 0.0f, FIRE_ALIGN_RAD * 1.1f, 0.0f, false));
}

TEST_CASE("aimOnTarget: PITCH must converge too for a ranged shot, but never for melee") {
    // A balcony/pit fight is a pitch problem, not a yaw one — a converged yaw with the crosshair
    // still pointing at the floor is exactly the shot that eats the ledge.
    CHECK_FALSE(aimOnTarget(0.0f, 0.0f, 0.0f, 0.5f, /*melee=*/false));
    CHECK(aimOnTarget(0.0f, 0.0f, 0.0f, 0.5f, /*melee=*/true));   // horizontal cone ignores pitch
}

TEST_CASE("aimOnTarget: melee keeps swinging inside its wide cone") {
    // weapons.json gives the sword a 70° cone judged HORIZONTALLY, so ±35° (0.61 rad) all connects.
    // A melee bot that waited for pinpoint alignment would stand there not swinging.
    CHECK(aimOnTarget(0.0f, 0.0f, 0.35f, 0.0f, /*melee=*/true));   // 20°: well inside the arc
    CHECK_FALSE(aimOnTarget(0.0f, 0.0f, 0.35f, 0.0f, /*melee=*/false));  // same angle, ranged: no
    CHECK(FIRE_ALIGN_MELEE_RAD < 0.61f);        // must stay inside the real half-cone
    CHECK(FIRE_ALIGN_MELEE_RAD > FIRE_ALIGN_RAD);
}

TEST_CASE("aimOnTarget: the tolerance clears the ease's steady-state tracking lag") {
    // The ease is a first-order lag, so a MOVING target leaves a PERMANENT error of
    // (its angular rate / gain). At the shipped gain of 6 a target crossing at 0.4 rad/s sits
    // 0.067 rad off centre forever — a tolerance under that would MUTE the bot against anything
    // that moves, which is the failure mode to watch for when re-tuning.
    constexpr f32 kGain = 6.0f, kCrossRate = 0.4f, kWobbleAmp = 0.011f;
    CHECK(FIRE_ALIGN_RAD > kCrossRate / kGain + kWobbleAmp);   // lag + the wobble laid on top of it
    // Drive it for real, exactly as the driver does: the aim eases toward (desired + wobble) while
    // the gate compares it against the un-wobbled `desired` (the wobble is imprecision we ACCEPT).
    // A strafing enemy must not silence the bot.
    f32 aim = 0.0f, desired = 0.0f;
    bool firedLate = false;
    for (u32 i = 0; i < 600; i++) {             // 10 s
        desired += kCrossRate * kDt;
        f32 wy, wp; aimWobble(i, wy, wp);
        aim = stepAngle(aim, desired + wy, kGain, 2.8f, kDt);
        if (i > 120) firedLate = aimOnTarget(aim, 0.0f, desired, 0.0f, false);
        if (i > 120 && !firedLate) break;       // a single dry tick after settling fails the case
    }
    CHECK(firedLate);
}

TEST_CASE("aimOnTarget: folds the ±π seam like angleDelta (a spinning bot still shoots)") {
    // Player::yaw is never re-wrapped by the engine, so after a few minutes it sits several turns
    // outside ±π. A bare subtraction would report a multi-revolution error and mute the bot forever.
    CHECK(aimOnTarget(6.0f * kPi, 0.0f, 0.0f, 0.0f, false));
    CHECK(aimOnTarget(0.0f, 0.0f, 4.0f * kPi + 0.02f, 0.0f, false));
    CHECK_FALSE(aimOnTarget(0.0f, 0.0f, 4.0f * kPi + 0.5f, 0.0f, false));
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

// ---------------------------------------------------------------------------------------------
// (B) GAP-CLOSER ROLL — a ranged enemy plinking from outside our band is charged with a dodge
// (4 m of travel + 0.3 s of i-frames beats walking into its fire).
// ---------------------------------------------------------------------------------------------
static BotView meleeBotVs(f32 dist, bool targetRanged, BotTarget& t) {
    BotView v = selfAt({0,0,0});
    v.buildCell = 3*1+1;                  // Moderate / Melee: band 0 .. 0.60 x range
    v.weaponRange = 3.0f; v.weaponProjSpeed = 0.0f; v.weaponIsMelee = true;
    v.dodgeCooldown = 0.0f;
    t = BotTarget{};
    t.pos = {0, 1.7f, -dist}; t.dist = dist; t.hasLOS = true;
    t.isRanged = targetRanged; t.attackRange = targetRanged ? 12.0f : 2.0f;
    v.targets = &t; v.targetCount = 1;
    return v;
}

TEST_CASE("rolls INTO a ranged enemy it is trying to close on") {
    BotTarget t; BotView v = meleeBotVs(9.0f, true, t);   // 9 m >> the 1.8 m melee band
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.moveFwd);      // must be held on the SAME tick: the roll direction comes from WASD
    CHECK(out.dodge);
}

TEST_CASE("does not gap-close roll when the dodge is on cooldown or mid-roll") {
    {
        BotTarget t; BotView v = meleeBotVs(9.0f, true, t);
        v.dodgeCooldown = 0.4f;
        BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
        CHECK(out.moveFwd);
        CHECK_FALSE(out.dodge);
    }
    {
        BotTarget t; BotView v = meleeBotVs(9.0f, true, t);
        v.rolling = true;
        CHECK_FALSE(decideCombat(v, doctrineFor(v.buildCell)).dodge);
    }
}

TEST_CASE("does not gap-close roll at a MELEE enemy — it is walking into us anyway") {
    BotTarget t; BotView v = meleeBotVs(9.0f, false, t);
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK(out.moveFwd);
    CHECK_FALSE(out.dodge);   // burning the roll on a closer that closes itself wastes the i-frames
}

TEST_CASE("does not gap-close roll while still turning onto the target") {
    // The roll direction is the CURRENT yaw (PlayerController::computeRollDirection), and the aim is
    // now rate-limited, so rolling before the turn lands would launch the bot sideways into geometry.
    BotTarget t; BotView v = meleeBotVs(9.0f, true, t);
    v.yaw = kPi;                                  // facing 180° away from the target at -Z
    CHECK_FALSE(decideCombat(v, doctrineFor(v.buildCell)).dodge);
}

TEST_CASE("does not gap-close roll at a ranged enemy already inside the band") {
    BotTarget t; BotView v = meleeBotVs(1.0f, true, t);   // inside 0.60 x 3 m: no closing wanted
    BotIntent out = decideCombat(v, doctrineFor(v.buildCell));
    CHECK_FALSE(out.moveFwd);
    CHECK_FALSE(out.dodge);
}

// ---------------------------------------------------------------------------------------------
// (C) TIMED PERFECT BLOCK — a HELD block only ever earns BLOCKED (0.5x); PERFECT (full negation)
// needs the RAISE to be inside the 0.2 s window before the hit (Combat::classifyBlock).
// ---------------------------------------------------------------------------------------------
static BotView tankBotVs(f32 dist, f32 enemyAttackTimer, BotTarget& t) {
    BotView v = selfAt({0,0,0});
    v.buildCell = 3*0+1;                  // Tanky / Melee: d.blocks = true
    v.weaponRange = 3.0f; v.weaponProjSpeed = 0.0f; v.weaponIsMelee = true;
    t = BotTarget{};
    t.pos = {0, 1.7f, -dist}; t.dist = dist; t.hasLOS = true;
    t.attackRange = 2.0f; t.attackTimer = enemyAttackTimer;
    v.targets = &t; v.targetCount = 1;
    return v;
}

TEST_CASE("raises the block only when a swing is actually imminent") {
    BotTarget t; BotView v = tankBotVs(1.8f, 0.08f, t);   // in its reach, 0.08 s from swinging
    CHECK(decideCombat(v, doctrineFor(v.buildCell)).block);
}

TEST_CASE("does not turtle: a distant swing timer means no block") {
    BotTarget t; BotView v = tankBotVs(1.8f, 1.2f, t);
    CHECK_FALSE(decideCombat(v, doctrineFor(v.buildCell)).block);   // holding wastes the window + 0.4x speed
}

TEST_CASE("does not block a swing that cannot reach us") {
    BotTarget t; BotView v = tankBotVs(9.0f, 0.05f, t);   // 9 m away, 2 m reach: it is not swinging at US
    CHECK_FALSE(decideCombat(v, doctrineFor(v.buildCell)).block);
}

TEST_CASE("blocks for a target it is NOT aiming at") {
    // The thing about to hit you is often not the thing you are shooting.
    BotView v = selfAt({0,0,0});
    v.buildCell = 3*0+1; v.weaponRange = 3.0f; v.weaponProjSpeed = 0.0f; v.weaponIsMelee = true;
    BotTarget ts[2];
    ts[0] = {}; ts[0].pos = {0,1.7f,-1.5f}; ts[0].dist = 1.5f; ts[0].hasLOS = true;
    ts[0].attackRange = 2.0f; ts[0].attackTimer = 2.0f;          // the aim target, not swinging
    ts[1] = {}; ts[1].pos = {1.5f,1.7f,0}; ts[1].dist = 1.5f; ts[1].hasLOS = true;
    ts[1].attackRange = 2.0f; ts[1].attackTimer = 0.05f;         // flanker, about to hit
    v.targets = ts; v.targetCount = 2;
    CHECK(decideCombat(v, doctrineFor(v.buildCell)).block);
}

TEST_CASE("ignores a STALE negative attack timer") {
    // The STRAFE-state ranged fire only resets attackTimer when it has LOS, so an enemy holding a
    // shot behind cover drifts it arbitrarily negative and reads as "forever about to swing". Live,
    // that was 213 of 275 block raises — constant flapping at 0.4x move speed for nothing.
    BotTarget t; BotView v = tankBotVs(1.8f, -3.4f, t);
    CHECK_FALSE(decideCombat(v, doctrineFor(v.buildCell)).block);
}

TEST_CASE("does not time a block against a RANGED attacker") {
    // attackTimer says when the SHOT LEAVES, not when it lands; the bolt then flies for dist/16 s,
    // so raising now would expire the perfect window long before impact.
    BotTarget t; BotView v = tankBotVs(1.8f, 0.08f, t);
    t.isRanged = true; t.attackRange = 12.0f;
    CHECK_FALSE(decideCombat(v, doctrineFor(v.buildCell)).block);
}

TEST_CASE("lets a stale block GO so the next raise re-opens the perfect window") {
    BotTarget t; BotView v = tankBotVs(1.8f, 0.08f, t);
    v.blockHeld = 0.25f;                                  // already past the 0.2 s PERFECT tier
    CHECK_FALSE(decideCombat(v, doctrineFor(v.buildCell)).block);
}

TEST_CASE("a glass-cannon doctrine never blocks — it dodges") {
    BotTarget t; BotView v = tankBotVs(1.8f, 0.05f, t);
    v.buildCell = 3*2+1;                                  // Glass / Melee: d.blocks = false
    CHECK_FALSE(decideCombat(v, doctrineFor(v.buildCell)).block);
}
