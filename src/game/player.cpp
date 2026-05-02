#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "game/player.h"
#include "platform/input.h"
#include <cmath>

static constexpr f32 MAX_PITCH = 89.0f * 3.14159265f / 180.0f;

void PlayerController::update(Player& player, f32 dt) {
    // --- Mouse look ---
    s32 mx, my;
    Input::getMouseDelta(mx, my);
    player.yaw   -= mx * player.sensitivity;
    player.pitch -= my * player.sensitivity;
    if (player.pitch >  MAX_PITCH) player.pitch =  MAX_PITCH;
    if (player.pitch < -MAX_PITCH) player.pitch = -MAX_PITCH;

    // Recompute forward/right from yaw/pitch
    Vec3 forward = normalize(Vec3{
        -sinf(player.yaw) * cosf(player.pitch),
         sinf(player.pitch),
        -cosf(player.yaw) * cosf(player.pitch)
    });
    Vec3 flatForward = normalize(Vec3{-sinf(player.yaw), 0.0f, -cosf(player.yaw)});
    Vec3 right       = normalize(cross(flatForward, {0.0f, 1.0f, 0.0f}));

    // --- Movement intent ---
    Vec3 move = {0, 0, 0};
    if (Input::isKeyDown(SDL_SCANCODE_W)) move += flatForward;
    if (Input::isKeyDown(SDL_SCANCODE_S)) move -= flatForward;
    if (Input::isKeyDown(SDL_SCANCODE_D)) move += right;
    if (Input::isKeyDown(SDL_SCANCODE_A)) move -= right;

    if (player.noclip) {
        if (Input::isKeyDown(SDL_SCANCODE_W)) move += forward;
        if (Input::isKeyDown(SDL_SCANCODE_S)) move -= forward;
        move = (lengthSq(move) > 0.0001f) ? normalize(move) : Vec3{0,0,0};
        player.position += move * (player.moveSpeed * dt);
        player.velocity = {0, 0, 0};
        return;
    }

    // Horizontal velocity from input
    Vec3 horzMove = (lengthSq(move) > 0.0001f) ? normalize(move) * player.moveSpeed : Vec3{0,0,0};
    player.velocity.x = horzMove.x;
    player.velocity.z = horzMove.z;

    // Jump
    if (Input::isKeyPressed(SDL_SCANCODE_SPACE) && player.onGround) {
        player.velocity.y = 8.0f;
        player.onGround   = false;
    }

    // Gravity accumulated here; Collision::moveAndSlide owns position update.
    if (!player.onGround) {
        player.velocity.y -= 20.0f * dt;
    }

    (void)forward; // used only in noclip
}

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
