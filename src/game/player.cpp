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
                           s32 mouseDX, s32 mouseDY,
                           bool w, bool s, bool a, bool d, bool jump,
                           f32 dt)
{
    // Mouse look
    yaw   -= mouseDX * sensitivity;
    pitch -= mouseDY * sensitivity;
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
    s32 mx, my;
    Input::getMouseDelta(mx, my);

    // Add right stick look (controller) — convert stick deflection to mouse-equivalent delta
    f32 rsX = Input::getStickX(true);
    f32 rsY = Input::getStickY(true);
    if (rsX != 0.0f || rsY != 0.0f) {
        f32 stickScale = Input::getStickSensitivity() * 60.0f;
        mx += static_cast<s32>(rsX * stickScale);
        my += static_cast<s32>(rsY * stickScale * (Input::getStickInvertY() ? -1.0f : 1.0f));
    }

    // Add gyro aiming — angular velocity mapped to look delta
    f32 gyroDx, gyroDy;
    Input::getGyro(gyroDx, gyroDy);
    if (gyroDx != 0.0f || gyroDy != 0.0f) {
        mx += static_cast<s32>(gyroDx * Input::getGyroSensitivity());
        my += static_cast<s32>(gyroDy * Input::getGyroSensitivity() * (Input::getGyroInvertY() ? -1.0f : 1.0f));
    }

    // Apply slow debuff (e.g., from boss cleaver hit)
    f32 effectiveSpeed = player.moveSpeed * GameConst::SPEED_MULT;
    // Soul Harvest speed bonus: +5% per stack
    if (player.soulHarvestStacks > 0 && player.soulHarvestTimer > 0.0f) {
        effectiveSpeed *= (1.0f + player.soulHarvestStacks * 0.05f);
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

    applyMovement(player.position, player.velocity, player.yaw, player.pitch,
                  player.onGround, player.noclip,
                  effectiveSpeed, player.sensitivity,
                  mx, my,
                  w, s, a, d,
                  Input::isActionPressed(GameAction::JUMP),
                  dt);
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

    s32 mx, my;
    Input::getMouseDelta(mx, my);
    // Add right stick look delta
    f32 rsX = Input::getStickX(true);
    f32 rsY = Input::getStickY(true);
    if (rsX != 0.0f || rsY != 0.0f) {
        f32 stickScale = Input::getStickSensitivity() * 60.0f;
        mx += static_cast<s32>(rsX * stickScale);
        my += static_cast<s32>(rsY * stickScale * (Input::getStickInvertY() ? -1.0f : 1.0f));
    }
    // Add gyro aiming
    f32 gyroDx, gyroDy;
    Input::getGyro(gyroDx, gyroDy);
    if (gyroDx != 0.0f || gyroDy != 0.0f) {
        mx += static_cast<s32>(gyroDx * Input::getGyroSensitivity());
        my += static_cast<s32>(gyroDy * Input::getGyroSensitivity() * (Input::getGyroInvertY() ? -1.0f : 1.0f));
    }
    // Clamp to s16 range
    if (mx >  32767) mx =  32767;
    if (mx < -32768) mx = -32768;
    if (my >  32767) my =  32767;
    if (my < -32768) my = -32768;
    input.mouseDeltaX = static_cast<s16>(mx);
    input.mouseDeltaY = static_cast<s16>(my);

    // Extended input flags — unified keyboard + gamepad
    u8 ext = 0;
    if (Input::isActionPressed(GameAction::POTION))       ext |= INPUT_EX_POTION;
    if (Input::isActionPressed(GameAction::RELOAD))        ext |= INPUT_EX_RELOAD;
    if (Input::isActionPressed(GameAction::CLASS_SKILL))   ext |= INPUT_EX_SKILL;
    if (Input::isActionPressed(GameAction::BOOT_SKILL))    ext |= INPUT_EX_BOOT_SKILL;
    if (Input::isActionPressed(GameAction::HELMET_SKILL))  ext |= INPUT_EX_HELM_SKILL;
    if (Input::isActionPressed(GameAction::INVENTORY))     ext |= INPUT_EX_INVENTORY;
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
    cam.forward  = normalize(Vec3{
        -sinf(player.yaw) * cosf(player.pitch),
         sinf(player.pitch),
        -cosf(player.yaw) * cosf(player.pitch)
    });
    cam.right = normalize(cross(cam.forward, {0.0f, 1.0f, 0.0f}));
}
