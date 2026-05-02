#pragma once

#include "core/types.h"
#include "core/math.h"

struct Camera {
    Vec3 position  = {0.0f, 1.7f, 5.0f};
    f32  yaw       = 0.0f;   // radians
    f32  pitch     = 0.0f;   // radians, clamped
    f32  fovY      = 70.0f;  // degrees
    f32  nearPlane = 0.1f;
    f32  farPlane  = 200.0f;

    // Computed each frame
    Vec3 forward;
    Vec3 right;
    Mat4 view;
    Mat4 projection;
    Mat4 viewProjection;
};

namespace CameraSystem {
    // computeMatrices builds view/projection/viewProjection from position+yaw+pitch.
    void computeMatrices(Camera& cam, f32 aspectRatio);
}
