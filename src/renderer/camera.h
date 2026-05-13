#pragma once

#include "core/types.h"
#include "core/math.h"
#include <cmath>

// ScreenShake — decaying sinusoidal offset applied to camera position each render frame.
// trigger() only replaces the current shake if the new intensity is stronger.
// update() returns a Vec3 world-space offset; caller adds it to camera.position.
struct ScreenShake {
    f32 intensity = 0.0f;
    f32 decay     = 0.0f;
    f32 frequency = 25.0f;
    f32 timer     = 0.0f;

    void trigger(f32 newIntensity, f32 duration) {
        if (newIntensity > intensity) {
            intensity = newIntensity;
            decay = newIntensity / duration;
        }
    }

    Vec3 update(f32 dt) {
        if (intensity <= 0.0f) return {0, 0, 0};
        timer += dt * frequency;
        intensity -= decay * dt;
        if (intensity < 0.0f) intensity = 0.0f;
        // Three sine waves at different frequencies give an organic 3D wobble
        f32 ox = sinf(timer * 1.0f) * intensity;
        f32 oy = sinf(timer * 1.3f) * intensity;
        f32 oz = sinf(timer * 0.7f) * intensity;
        return {ox, oy, oz};
    }
};

struct Camera {
    Vec3 position  = {0.0f, 1.7f, 5.0f};
    f32  yaw       = 0.0f;   // radians
    f32  pitch     = 0.0f;   // radians, clamped
    f32  fovY      = 70.0f;  // degrees
    f32  nearPlane = 0.1f;
    f32  farPlane  = 200.0f;

    // Previous-tick state for render interpolation (reduces gyro lag)
    Vec3 prevPosition = {0.0f, 1.7f, 5.0f};
    f32  prevYaw      = 0.0f;
    f32  prevPitch    = 0.0f;

    // Computed each frame
    Vec3 forward;
    Vec3 right;
    Mat4 view;
    Mat4 projection;
    Mat4 viewProjection;

    ScreenShake shake;
};

namespace CameraSystem {
    // computeMatrices builds view/projection/viewProjection from position+yaw+pitch.
    void computeMatrices(Camera& cam, f32 aspectRatio);
}
