#pragma once

#include "core/types.h"
#include "core/math.h"
#include "renderer/camera.h"

struct EntityHandle;

struct Player {
    Vec3 position   = {0.0f, 0.0f, 0.0f}; // feet position (bottom of collider)
    Vec3 velocity   = {0.0f, 0.0f, 0.0f};
    f32  eyeHeight  = 1.7f;    // metres above feet
    f32  yaw        = 0.0f;    // radians
    f32  pitch      = 0.0f;    // radians, clamped to ±89°
    f32  moveSpeed  = 6.0f;    // m/s
    f32  sensitivity = 0.002f; // radians per pixel
    bool onGround   = false;
    bool noclip     = false;   // fly freely, no collision

    // Combat
    f32  health         = 100.0f;
    f32  maxHealth      = 100.0f;
    f32  damageFlashTimer = 0.0f;

    // Soft target lock
    u16  lockIndex      = 0xFFFF; // entity index (or 0xFFFF if none)
    u16  lockGeneration = 0;
    bool lockActive     = false;
    f32  lockLosTimer   = 0.0f;   // time since LOS was broken
};

namespace PlayerController {
    void update(Player& player, f32 dt);
    void applyToCamera(const Player& player, Camera& cam);
}
