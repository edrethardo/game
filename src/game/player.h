#pragma once

#include "core/types.h"
#include "core/math.h"
#include "renderer/camera.h"

struct EntityHandle;
struct NetInput;
struct NetPlayer;

struct Player {
    Vec3 position   = {0.0f, 0.0f, 0.0f}; // feet position (bottom of collider)
    Vec3 velocity   = {0.0f, 0.0f, 0.0f};
    f32  eyeHeight  = 1.7f;    // metres above feet
    f32  yaw        = 0.0f;    // radians
    f32  pitch      = 0.0f;    // radians, clamped to ±89°
    f32  moveSpeed  = 6.0f;    // m/s (scaled by SPEED_MULT at runtime)
    f32  sensitivity = 0.002f; // radians per pixel
    bool onGround   = false;
    bool noclip     = false;   // fly freely, no collision

    // Combat
    f32  health         = 100.0f;
    f32  maxHealth      = 100.0f;
    f32  damageFlashTimer = 0.0f;
    f32  hitShakeTimer    = 0.0f;  // screen shake on taking damage
    f32  slowTimer        = 0.0f;  // movement speed debuff countdown
    f32  poisonTimer      = 0.0f;
    f32  poisonDps        = 0.0f;
    f32  burnTimer        = 0.0f;
    f32  burnDps          = 0.0f;
    f32  freezeTimer      = 0.0f;  // halves movement speed
    f32  invulnTimer      = 0.0f;  // damage immunity countdown (respawn/floor entry)

    // Shield blocking (Ctrl/Shift)
    bool blocking         = false;
    f32  blockTimer        = 0.0f;  // time since block started (for perfect block window)

    // Soft target lock
    u16  lockIndex      = 0xFFFF; // entity index (or 0xFFFF if none)
    u16  lockGeneration = 0;
    bool lockActive     = false;
    f32  lockLosTimer   = 0.0f;   // time since LOS was broken

    // Cached forward vector (computed once per frame in update)
    Vec3 forward = {0.0f, 0.0f, -1.0f};
};

namespace PlayerController {
    // Original: reads Input:: directly (used for singleplayer and local capture)
    void update(Player& player, f32 dt);

    // Network-aware: applies a NetInput struct instead of reading Input::
    void updateFromInput(Player& player, const NetInput& input, f32 dt);

    // Apply to NetPlayer (same logic, different struct)
    void updateNetPlayerFromInput(NetPlayer& np, const NetInput& input, f32 dt);

    void applyToCamera(const Player& player, Camera& cam);

    // Capture current Input:: state into a NetInput
    NetInput captureLocalInput(u32 tick, u8 weaponId);
}
