#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "game/player.h"
#include "game/game_constants.h"
#include "net/net_player.h"
#include "platform/input.h"
#include <cmath>

static constexpr f32 MAX_PITCH = 89.0f * 3.14159265f / 180.0f;

// ---------------------------------------------------------------------------
// Shared movement logic (operates on raw values, shared by Player & NetPlayer)
// ---------------------------------------------------------------------------
static Vec3 s_lastForward = {0, 0, -1};

static void applyMovement(Vec3& position, Vec3& velocity, f32& yaw, f32& pitch,
                           bool& onGround, bool noclip,
                           f32 moveSpeed, f32 sensitivity,
                           f32 lookDX, f32 lookDY,
                           bool w, bool s, bool a, bool d, bool jump,
                           f32 dt)
{
    // Mouse/stick/gyro look — float precision, no integer quantization
    yaw   -= lookDX * sensitivity;
    pitch -= lookDY * sensitivity;
    if (pitch >  MAX_PITCH) pitch =  MAX_PITCH;
    if (pitch < -MAX_PITCH) pitch = -MAX_PITCH;

    f32 cosP = cosf(pitch);
    f32 sinP = sinf(pitch);
    f32 cosY = cosf(yaw);
    f32 sinY = sinf(yaw);

    Vec3 forward = normalize(Vec3{-sinY * cosP, sinP, -cosY * cosP});
    s_lastForward = forward; // cache for retrieval

    Vec3 flatForward = normalize(Vec3{-sinY, 0.0f, -cosY});
    Vec3 right       = normalize(cross(flatForward, {0.0f, 1.0f, 0.0f}));

    Vec3 move = {0, 0, 0};
    if (w) move += flatForward;
    if (s) move -= flatForward;
    if (d) move += right;
    if (a) move -= right;

    if (noclip) {
        if (w) move += forward;
        if (s) move -= forward;
        move = (lengthSq(move) > 0.0001f) ? normalize(move) : Vec3{0,0,0};
        position += move * (moveSpeed * dt);
        velocity = {0, 0, 0};
        return;
    }

    Vec3 horzMove = (lengthSq(move) > 0.0001f) ? normalize(move) * moveSpeed : Vec3{0,0,0};
    velocity.x = horzMove.x;
    velocity.z = horzMove.z;

    if (jump && onGround) {
        velocity.y = 8.0f;
        onGround   = false;
    }

    if (!onGround) {
        velocity.y -= 20.0f * dt;
    }
}

// ---------------------------------------------------------------------------
// Original: reads Input:: directly
// ---------------------------------------------------------------------------
void PlayerController::update(Player& player, f32 dt) {
    // Accumulate look deltas as float to preserve sub-pixel gyro/stick precision
    s32 rawMx, rawMy;
    Input::getMouseDelta(rawMx, rawMy);
    f32 mx = static_cast<f32>(rawMx);
    f32 my = static_cast<f32>(rawMy);

    // Add right stick look (controller)
    f32 rsX = Input::getStickX(true);
    f32 rsY = Input::getStickY(true);
    if (rsX != 0.0f || rsY != 0.0f) {
        f32 stickScale = Input::getStickSensitivity() * 60.0f;
        mx += rsX * stickScale;
        my += rsY * stickScale * (Input::getStickInvertY() ? -1.0f : 1.0f);
    }

    // Add gyro aiming — angular velocity mapped to look delta
    f32 gyroDx, gyroDy;
    Input::getGyro(gyroDx, gyroDy);
    if (gyroDx != 0.0f || gyroDy != 0.0f) {
        mx += gyroDx * Input::getGyroSensitivity();
        my += gyroDy * Input::getGyroSensitivity() * (Input::getGyroInvertY() ? -1.0f : 1.0f);
    }

    // Apply slow debuff (e.g., from boss cleaver hit)
    f32 effectiveSpeed = player.moveSpeed * GameConst::SPEED_MULT;
    // Soul Harvest speed bonus: +5% per stack
    if (player.soulHarvestStacks > 0 && player.soulHarvestTimer > 0.0f) {
        effectiveSpeed *= (1.0f + player.soulHarvestStacks * 0.05f);
    }
    // Wanderer adrenaline move speed bonus: +5% per stack (requires adrenaline upgrade at floor 30)
    if (player.adrenalineUpgraded && player.dodgeState.counterStacks > 0) {
        effectiveSpeed *= (1.0f + player.dodgeState.counterStacks * 0.05f);
    }
    if (player.slowTimer > 0.0f) {
        effectiveSpeed *= 0.4f; // 60% slow
        player.slowTimer -= dt;
    }

    // Merge keyboard + left stick for movement
    bool w = Input::isActionDown(GameAction::MOVE_FORWARD)  || Input::getStickY(false) < -0.3f;
    bool s = Input::isActionDown(GameAction::MOVE_BACKWARD) || Input::getStickY(false) > 0.3f;
    bool a = Input::isActionDown(GameAction::MOVE_LEFT)     || Input::getStickX(false) < -0.3f;
    bool d = Input::isActionDown(GameAction::MOVE_RIGHT)    || Input::getStickX(false) > 0.3f;

    // --- Wanderer dodge roll activation ---
    // Triggered on Shift press when not already rolling and cooldown has expired.
    if (Input::isActionPressed(GameAction::DODGE) && !player.dodgeState.rolling
        && player.dodgeState.cooldownTimer <= 0.0f) {
        DodgeState& ds = player.dodgeState;
        ds.rolling = true;
        ds.rollTimer = 0.5f;
        ds.rollAngle = 0.0f;
        ds.pitchAngle = 0.0f;

        // Direction from WASD, or forward if no directional input held
        f32 cosY = cosf(player.yaw);
        f32 sinY = sinf(player.yaw);
        Vec3 flatFwd   = normalize(Vec3{-sinY, 0.0f, -cosY});
        Vec3 flatRight = normalize(cross(flatFwd, {0.0f, 1.0f, 0.0f}));

        Vec3 dir = {0, 0, 0};
        if (w) dir += flatFwd;
        if (s) dir -= flatFwd;
        if (d) dir += flatRight;
        if (a) dir -= flatRight;
        if (lengthSq(dir) < 0.001f) dir = flatFwd;
        ds.rollDirection = normalize(dir);

        // Camera rotation axis depends on dodge direction relative to facing:
        // Forward  = front flip (pitch), Backward = back flip (pitch),
        // Left/Right = barrel roll, Diagonal = blend of both.
        f32 fwdDot   = dot(ds.rollDirection, flatFwd);   // +1 forward, -1 back
        f32 rightDot = dot(ds.rollDirection, flatRight);  // +1 right, -1 left
        ds.pitchWeight = fabsf(fwdDot);   // how much front/back flip
        ds.rollWeight  = fabsf(rightDot);  // how much barrel roll
        // Normalize so they sum to 1 (pure direction = 1.0, diagonal ≈ 0.707 each)
        f32 totalW = ds.pitchWeight + ds.rollWeight;
        if (totalW > 0.001f) { ds.pitchWeight /= totalW; ds.rollWeight /= totalW; }
        else { ds.pitchWeight = 1.0f; ds.rollWeight = 0.0f; } // fallback: front flip
        ds.pitchSign = (fwdDot >= 0.0f) ? 1 : -1;  // forward = front flip, back = back flip
        ds.rollSign  = (rightDot >= 0.0f) ? 1 : -1; // right = CW, left = CCW

        // I-frames: invulnerability for the first 0.3s of the roll (60% of duration)
        player.invulnTimer = 0.3f;
    }

    // During roll, zero out look deltas so camera stays fixed (no mouse look)
    if (player.dodgeState.rolling) {
        mx = 0.0f;
        my = 0.0f;
    }

    applyMovement(player.position, player.velocity, player.yaw, player.pitch,
                  player.onGround, player.noclip,
                  effectiveSpeed, player.sensitivity,
                  mx, my,
                  w, s, a, d,
                  Input::isActionPressed(GameAction::JUMP),
                  dt);

    // --- Wanderer dodge roll tick ---
    // Override velocity during roll for a consistent 4m distance (8m/s × 0.5s).
    {
        DodgeState& ds = player.dodgeState;
        if (ds.rolling) {
            constexpr f32 ROLL_SPEED = 8.0f; // 4m over 0.5s
            player.velocity.x = ds.rollDirection.x * ROLL_SPEED;
            player.velocity.z = ds.rollDirection.z * ROLL_SPEED;

            ds.rollTimer -= dt;

            // Smooth 360° rotation using cubic ease-in-out (3t²-2t³).
            // Blends between barrel roll (sideways) and pitch flip (forward/back)
            // based on dodge direction.
            constexpr f32 ROLL_DURATION = 0.5f;
            constexpr f32 TWO_PI = 2.0f * 3.14159265f;
            f32 t = 1.0f - (ds.rollTimer / ROLL_DURATION); // 0→1 over duration
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            f32 smooth = t * t * (3.0f - 2.0f * t);
            ds.rollAngle  = smooth * TWO_PI * static_cast<f32>(ds.rollSign)  * ds.rollWeight;
            ds.pitchAngle = smooth * TWO_PI * static_cast<f32>(ds.pitchSign) * ds.pitchWeight;

            if (ds.rollTimer <= 0.0f) {
                // Roll finished — reset angle and start cooldown
                ds.rolling = false;
                ds.rollTimer = 0.0f;
                ds.rollAngle = 0.0f;
                ds.pitchAngle = 0.0f;
                ds.cooldownTimer = 1.0f;
            }
        } else if (ds.cooldownTimer > 0.0f) {
            ds.cooldownTimer -= dt;
            if (ds.cooldownTimer < 0.0f) ds.cooldownTimer = 0.0f;
        }
    }

    player.forward = s_lastForward;
}

// ---------------------------------------------------------------------------
// Network-aware: applies a NetInput struct to a Player
// ---------------------------------------------------------------------------
void PlayerController::updateFromInput(Player& player, const NetInput& input, f32 dt) {
    applyMovement(player.position, player.velocity, player.yaw, player.pitch,
                  player.onGround, player.noclip,
                  player.moveSpeed, player.sensitivity,
                  input.mouseDeltaX, input.mouseDeltaY,
                  (input.moveFlags & INPUT_FORWARD)  != 0,
                  (input.moveFlags & INPUT_BACKWARD) != 0,
                  (input.moveFlags & INPUT_LEFT)     != 0,
                  (input.moveFlags & INPUT_RIGHT)    != 0,
                  (input.moveFlags & INPUT_JUMP)     != 0,
                  dt);
    player.forward = s_lastForward;
}

// ---------------------------------------------------------------------------
// Network-aware: applies a NetInput struct to a NetPlayer
// ---------------------------------------------------------------------------
void PlayerController::updateNetPlayerFromInput(NetPlayer& np, const NetInput& input, f32 dt) {
    // Apply slow debuff (same as singleplayer PlayerController::update)
    f32 effectiveSpeed = np.moveSpeed * GameConst::SPEED_MULT;
    if (np.slowTimer > 0.0f) {
        effectiveSpeed *= 0.4f; // 60% slow
        np.slowTimer -= dt;
    }
    // Freeze stops all movement
    if (np.freezeTimer > 0.0f) {
        effectiveSpeed = 0.0f;
    }
    applyMovement(np.position, np.velocity, np.yaw, np.pitch,
                  np.onGround, np.noclip,
                  effectiveSpeed, np.sensitivity,
                  input.mouseDeltaX, input.mouseDeltaY,
                  (input.moveFlags & INPUT_FORWARD)  != 0,
                  (input.moveFlags & INPUT_BACKWARD) != 0,
                  (input.moveFlags & INPUT_LEFT)     != 0,
                  (input.moveFlags & INPUT_RIGHT)    != 0,
                  (input.moveFlags & INPUT_JUMP)     != 0,
                  dt);

    // Server-side Wanderer dodge: grant i-frames for the roll duration.
    // Full dodge movement is client-predicted; server only needs to track invulnerability.
    if ((input.extFlags & INPUT_EX_DODGE) && np.invulnTimer <= 0.0f) {
        np.invulnTimer = 0.3f;
    }
}

// ---------------------------------------------------------------------------
// Capture current Input:: into a NetInput
// ---------------------------------------------------------------------------
NetInput PlayerController::captureLocalInput(u32 tick, u8 weaponId) {
    NetInput input = {};
    input.tick = tick;
    input.weaponId = weaponId;

    // Merge keyboard + left stick for movement flags
    u8 flags = 0;
    if (Input::isActionDown(GameAction::MOVE_FORWARD)  || Input::getStickY(false) < -0.3f) flags |= INPUT_FORWARD;
    if (Input::isActionDown(GameAction::MOVE_BACKWARD) || Input::getStickY(false) > 0.3f)  flags |= INPUT_BACKWARD;
    if (Input::isActionDown(GameAction::MOVE_RIGHT)    || Input::getStickX(false) > 0.3f)  flags |= INPUT_RIGHT;
    if (Input::isActionDown(GameAction::MOVE_LEFT)     || Input::getStickX(false) < -0.3f) flags |= INPUT_LEFT;
    if (Input::isActionPressed(GameAction::JUMP))    flags |= INPUT_JUMP;
    if (Input::isActionDown(GameAction::FIRE))       flags |= INPUT_FIRE;
    if (Input::isActionDown(GameAction::TARGET_LOCK)) flags |= INPUT_LOCK;
    input.moveFlags = flags;

    s32 rawMx, rawMy;
    Input::getMouseDelta(rawMx, rawMy);
    f32 mx = static_cast<f32>(rawMx);
    f32 my = static_cast<f32>(rawMy);
    // Add right stick look delta
    f32 rsX = Input::getStickX(true);
    f32 rsY = Input::getStickY(true);
    if (rsX != 0.0f || rsY != 0.0f) {
        f32 stickScale = Input::getStickSensitivity() * 60.0f;
        mx += rsX * stickScale;
        my += rsY * stickScale * (Input::getStickInvertY() ? -1.0f : 1.0f);
    }
    // Add gyro aiming
    f32 gyroDx, gyroDy;
    Input::getGyro(gyroDx, gyroDy);
    if (gyroDx != 0.0f || gyroDy != 0.0f) {
        mx += gyroDx * Input::getGyroSensitivity();
        my += gyroDy * Input::getGyroSensitivity() * (Input::getGyroInvertY() ? -1.0f : 1.0f);
    }
    // Quantize to s16 for network serialization
    s32 imx = static_cast<s32>(mx);
    s32 imy = static_cast<s32>(my);
    if (imx >  32767) imx =  32767;
    if (imx < -32768) imx = -32768;
    if (imy >  32767) imy =  32767;
    if (imy < -32768) imy = -32768;
    input.mouseDeltaX = static_cast<s16>(imx);
    input.mouseDeltaY = static_cast<s16>(imy);

    // Extended input flags — unified keyboard + gamepad
    u8 ext = 0;
    if (Input::isActionPressed(GameAction::POTION))       ext |= INPUT_EX_POTION;
    if (Input::isActionPressed(GameAction::RELOAD))        ext |= INPUT_EX_RELOAD;
    if (Input::isActionPressed(GameAction::CLASS_SKILL))   ext |= INPUT_EX_SKILL;
    if (Input::isActionPressed(GameAction::BOOT_SKILL))    ext |= INPUT_EX_BOOT_SKILL;
    if (Input::isActionPressed(GameAction::HELMET_SKILL))  ext |= INPUT_EX_HELM_SKILL;
    if (Input::isActionPressed(GameAction::INVENTORY))     ext |= INPUT_EX_INVENTORY;
    if (Input::isActionPressed(GameAction::DODGE))         ext |= INPUT_EX_DODGE;
    input.extFlags = ext;
    input.skillSlot = 0; // set by engine before sending

    return input;
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------
void PlayerController::applyToCamera(const Player& player, Camera& cam) {
    cam.position = player.position + Vec3{0.0f, player.eyeHeight, 0.0f};
    cam.yaw      = player.yaw;
    cam.pitch    = player.pitch;
    cam.roll     = player.dodgeState.rollAngle;  // barrel roll component (sideways dodge)
    // Pitch flip component (forward/back dodge) — added to cam.pitch, not clamped
    cam.pitch   += player.dodgeState.pitchAngle;
    cam.forward  = normalize(Vec3{
        -sinf(player.yaw) * cosf(player.pitch),
         sinf(player.pitch),
        -cosf(player.yaw) * cosf(player.pitch)
    });
    cam.right = normalize(cross(cam.forward, {0.0f, 1.0f, 0.0f}));
}
