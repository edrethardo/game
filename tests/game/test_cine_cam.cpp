// test_cine_cam.cpp — the cinematic camera's two primitives (WB-270).
//
// Everything here is pure arithmetic, which is the point of cine_cam.h being engine-free: a wrong
// orbit radius or a glide that loops would otherwise only show up as a ruined take after a full
// capture run.
#include "../../external/doctest/doctest.h"
#include "game/cine_cam.h"

#include <cmath>
#include <initializer_list>

using namespace CineCam;

TEST_CASE("orbit: constant radius, full lap period, gaze at the centre") {
    Path p;
    REQUIRE(parse("orbit:14,14,8,12,3,1.2", p));
    REQUIRE(p.mode == Mode::ORBIT);

    // Radius holds at every sample point.
    for (u32 tick : {0u, 100u, 359u, 720u, 1500u}) {
        const Pose q = eval(p, tick);
        const f32 dx = q.position.x - 14.0f, dz = q.position.z - 14.0f;
        CAPTURE(tick);
        REQUIRE(std::sqrt(dx * dx + dz * dz) == doctest::Approx(8.0f).epsilon(0.001));
        REQUIRE(q.position.y == doctest::Approx(3.0f));
    }

    // One lap is exactly lapSec: tick 0 and tick 12*60 are the same point.
    const Pose t0 = eval(p, 0), t1 = eval(p, 12 * 60);
    REQUIRE(t0.position.x == doctest::Approx(t1.position.x).epsilon(0.001));
    REQUIRE(t0.position.z == doctest::Approx(t1.position.z).epsilon(0.001));

    // The gaze faces the centre: walking the pose's own forward convention from the eye must
    // CLOSE the horizontal distance to the centre to ~zero.
    const Pose q = eval(p, 217);
    const f32 fx = -std::sin(q.yaw) * std::cos(q.pitch);
    const f32 fy =  std::sin(q.pitch);
    const f32 fz = -std::cos(q.yaw) * std::cos(q.pitch);
    const f32 dx = 14.0f - q.position.x, dy = 1.2f - q.position.y, dz = 14.0f - q.position.z;
    const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    REQUIRE(q.position.x + fx * dist == doctest::Approx(14.0f).epsilon(0.01));
    REQUIRE(q.position.y + fy * dist == doctest::Approx(1.2f).epsilon(0.01));
    REQUIRE(q.position.z + fz * dist == doctest::Approx(14.0f).epsilon(0.01));
}

TEST_CASE("glide: endpoints, constant speed, and it holds at the end instead of looping") {
    Path p;
    REQUIRE(parse("glide:0,9,0:28,2,28:8", p));
    REQUIRE(p.mode == Mode::GLIDE);
    REQUIRE(!p.hasLook);

    const Pose start = eval(p, 0);
    REQUIRE(start.position.x == doctest::Approx(0.0f));
    REQUIRE(start.position.y == doctest::Approx(9.0f));

    const Pose mid = eval(p, 4 * 60);        // half the 8 s
    REQUIRE(mid.position.x == doctest::Approx(14.0f).epsilon(0.001));
    REQUIRE(mid.position.y == doctest::Approx(5.5f).epsilon(0.001));

    // Past the end: CLAMPED at B, never wrapping back to A. A glide that loops ruins the tail
    // of every take that runs a second long, which is every take.
    const Pose late = eval(p, 20 * 60);
    REQUIRE(late.position.x == doctest::Approx(28.0f));
    REQUIRE(late.position.z == doctest::Approx(28.0f));

    // Without a look point the gaze follows the travel direction (here: +X+Z, downhill).
    REQUIRE(std::sin(mid.pitch) < 0.0f);     // descending — pitch looks down
}

TEST_CASE("glide with a gaze point keeps looking at it from both ends") {
    Path p;
    REQUIRE(parse("glide:0,3,0:28,3,0:6:14,1,14", p));
    REQUIRE(p.hasLook);
    // From the left end the target is to the right (+X); from the right end to the left (-X).
    // In the engine convention forward.x = -sin(yaw): +X needs sin(yaw) < 0.
    const Pose l = eval(p, 0);
    const Pose r = eval(p, 6 * 60);
    REQUIRE(std::sin(l.yaw) < 0.0f);
    REQUIRE(std::sin(r.yaw) > 0.0f);
}

TEST_CASE("malformed specs never arm") {
    Path p;
    // Each of these once armed would only be discovered after a ruined capture run.
    REQUIRE(!parse("", p));
    REQUIRE(!parse("orbit:", p));
    REQUIRE(!parse("orbit:1,2,0,12", p));          // zero radius
    REQUIRE(!parse("orbit:1,2,8,0", p));           // zero period
    REQUIRE(!parse("dolly:1,2,3", p));             // unknown primitive
    REQUIRE(!parse("glide:0,0,0:1,1,1:0", p));     // zero duration
    REQUIRE(!parse("glide:0,0,0:1,1,1:5:junk", p));// malformed gaze point
    REQUIRE(!parse("glide:0,0,0:1,1,1:5x", p));    // trailing garbage
    REQUIRE(p.mode == Mode::OFF);                  // out stays untouched on every failure
}
