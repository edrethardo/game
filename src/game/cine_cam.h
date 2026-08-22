// cine_cam.h — the cinematic camera's two primitives: ORBIT and GLIDE (WB-270).
//
// Pure and engine-free (the zone_route.h pattern) so the maths tests without GL: the engine's
// only job is to overwrite the render camera with eval()'s answer while a path is armed.
//
// Deliberately TWO primitives and not a spline system (the user's call): a circle around a point
// and a straight glide A->B with a fixed gaze cover the whole shot list, and every extra knob is
// another way for two takes of the same shot to differ. Everything here is TICK-driven — the same
// tick in, the same camera out — so a --record take is reproducible to the frame.
#pragma once

#include "core/types.h"
#include "core/math.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace CineCam {

// The engine ticks at 60 Hz; a path's clock is the tick count since it was armed.
inline constexpr f32 TICK_DT = 1.0f / 60.0f;

enum struct Mode : u8 { OFF, ORBIT, GLIDE, FOLLOW };
// FOLLOW has no pure eval(): its subject is a LIVE projectile, so the engine computes the pose
// (engine_update_player.cpp) — trail distance/height parsed here, chase logic there. The user's
// shot: a thrown chakram, camera on its tail, wall bounce, kill.

struct Path {
    Mode mode = Mode::OFF;
    // ORBIT: circle around `centre` at `radius`, eye at `height`, one lap per `lapSec`,
    // always looking at the centre (gaze height `lookY`).
    Vec3 centre{};
    f32  radius = 8.0f, height = 3.0f, lapSec = 12.0f, lookY = 1.2f;
    // GLIDE: from `a` to `b` over `secs`, gazing at `look` the whole way (or along the travel
    // direction when no look point was given).
    Vec3 a{}, b{}, look{};
    f32  secs = 8.0f;
    bool hasLook = false;
    // FOLLOW: camera trails the newest player projectile at `followDist` behind and
    // `followHeight` above, gazing along its flight.
    f32  followDist = 2.6f, followHeight = 0.7f;
};

// The camera pose eval() answers with. Forward/right use the engine's own convention
// (forward = {-sin(yaw)cos(pitch), sin(pitch), -cos(yaw)cos(pitch)}) so the caller can stamp a
// Camera without re-deriving anything.
struct Pose {
    Vec3 position{};
    f32  yaw = 0.0f, pitch = 0.0f;
};

// Yaw/pitch that gaze from `from` toward `at`. yawToward (core/math.h) is the engine's single
// answer for the yaw half; the pitch is asin of the normalized rise, matching Player::forward.
inline void gaze(Vec3 from, Vec3 at, f32& outYaw, f32& outPitch) {
    const Vec3 d = at - from;
    const f32 len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 0.0001f) { outYaw = 0.0f; outPitch = 0.0f; return; }
    outYaw   = yawToward(Vec3{d.x, 0.0f, d.z});
    outPitch = asinf(d.y / len);
}

inline Pose eval(const Path& p, u32 tick) {
    Pose out;
    const f32 t = static_cast<f32>(tick) * TICK_DT;
    if (p.mode == Mode::ORBIT) {
        // Counter-clockwise from above, starting on +X of the centre. The lap NEVER ends —
        // an orbit that stops produces a visible hitch mid-take; the cut decides the length.
        const f32 ang = (p.lapSec > 0.001f) ? (t / p.lapSec) * 6.2831853f : 0.0f;
        out.position = { p.centre.x + cosf(ang) * p.radius,
                         p.height,
                         p.centre.z + sinf(ang) * p.radius };
        gaze(out.position, {p.centre.x, p.lookY, p.centre.z}, out.yaw, out.pitch);
    } else if (p.mode == Mode::GLIDE) {
        // Constant speed, CLAMPED at the end: the camera arrives and holds, it never loops —
        // a glide that snaps back to A ruins the take's tail.
        f32 f = (p.secs > 0.001f) ? t / p.secs : 1.0f;
        if (f > 1.0f) f = 1.0f;
        out.position = p.a + (p.b - p.a) * f;
        if (p.hasLook) {
            gaze(out.position, p.look, out.yaw, out.pitch);
        } else {
            // No gaze point: look along the travel direction (the corridor-flythrough case).
            gaze(Vec3{}, p.b - p.a, out.yaw, out.pitch);
        }
    }
    return out;
}

// Parse the --camera spec. Two forms, colon-separated groups, commas inside a group:
//   orbit:<cx>,<cz>,<radius>,<secsPerLap>[,<height>[,<lookY>]]
//   glide:<ax>,<ay>,<az>:<bx>,<by>,<bz>:<secs>[:<lx>,<ly>,<lz>]
// Returns false (and leaves `out` OFF) on anything malformed — a half-parsed camera must never
// arm, because the failure would only show as a wrong shot after a full capture run.
inline bool parse(const char* spec, Path& out) {
    Path p;
    if (!spec || !spec[0]) return false;
    if (std::strncmp(spec, "orbit:", 6) == 0) {
        f32 h = 3.0f, ly = 1.2f;
        const int n = std::sscanf(spec + 6, "%f,%f,%f,%f,%f,%f",
                                  &p.centre.x, &p.centre.z, &p.radius, &p.lapSec, &h, &ly);
        if (n < 4 || p.radius <= 0.0f || p.lapSec <= 0.0f) return false;
        p.height = h; p.lookY = ly;
        p.mode = Mode::ORBIT;
    } else if (std::strncmp(spec, "glide:", 6) == 0) {
        int consumed = 0;
        const int n = std::sscanf(spec + 6, "%f,%f,%f:%f,%f,%f:%f%n",
                                  &p.a.x, &p.a.y, &p.a.z, &p.b.x, &p.b.y, &p.b.z,
                                  &p.secs, &consumed);
        if (n < 7 || p.secs <= 0.0f) return false;
        const char* rest = spec + 6 + consumed;
        if (rest[0] == ':') {
            if (std::sscanf(rest + 1, "%f,%f,%f", &p.look.x, &p.look.y, &p.look.z) != 3)
                return false;
            p.hasLook = true;
        } else if (rest[0] != '\0') {
            return false;   // trailing garbage is a typo, not an intention
        }
        p.mode = Mode::GLIDE;
    } else if (std::strncmp(spec, "follow", 6) == 0) {
        // "follow" or "follow:<dist>[,<height>]"
        const char* rest = spec + 6;
        if (rest[0] == ':') {
            const int n = std::sscanf(rest + 1, "%f,%f", &p.followDist, &p.followHeight);
            if (n < 1 || p.followDist <= 0.0f) return false;
        } else if (rest[0] != '\0') {
            return false;
        }
        p.mode = Mode::FOLLOW;
    } else {
        return false;
    }
    out = p;
    return true;
}

} // namespace CineCam
