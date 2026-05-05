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

    // Apply slow debuff (e.g., from boss cleaver hit)
    f32 effectiveSpeed = player.moveSpeed * GameConst::SPEED_MULT;
    if (player.slowTimer > 0.0f) {
        effectiveSpeed *= 0.4f; // 60% slow
        player.slowTimer -= dt;
    }

    applyMovement(player.position, player.velocity, player.yaw, player.pitch,
                  player.onGround, player.noclip,
                  effectiveSpeed, player.sensitivity,
                  mx, my,
                  Input::isKeyDown(SDL_SCANCODE_W),
                  Input::isKeyDown(SDL_SCANCODE_S),
                  Input::isKeyDown(SDL_SCANCODE_A),
                  Input::isKeyDown(SDL_SCANCODE_D),
                  Input::isKeyPressed(SDL_SCANCODE_SPACE),
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
    applyMovement(np.position, np.velocity, np.yaw, np.pitch,
                  np.onGround, np.noclip,
                  np.moveSpeed * GameConst::SPEED_MULT, np.sensitivity,
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

    u8 flags = 0;
    if (Input::isKeyDown(SDL_SCANCODE_W)) flags |= INPUT_FORWARD;
    if (Input::isKeyDown(SDL_SCANCODE_S)) flags |= INPUT_BACKWARD;
    if (Input::isKeyDown(SDL_SCANCODE_D)) flags |= INPUT_RIGHT;
    if (Input::isKeyDown(SDL_SCANCODE_A)) flags |= INPUT_LEFT;
    if (Input::isKeyPressed(SDL_SCANCODE_SPACE)) flags |= INPUT_JUMP;
    if (Input::isMouseButtonDown(SDL_BUTTON_LEFT)) flags |= INPUT_FIRE;
    if (Input::isMouseButtonDown(SDL_BUTTON_MIDDLE)) flags |= INPUT_LOCK;
    input.moveFlags = flags;

    s32 mx, my;
    Input::getMouseDelta(mx, my);
    // Clamp to s16 range
    if (mx >  32767) mx =  32767;
    if (mx < -32768) mx = -32768;
    if (my >  32767) my =  32767;
    if (my < -32768) my = -32768;
    input.mouseDeltaX = static_cast<s16>(mx);
    input.mouseDeltaY = static_cast<s16>(my);

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
