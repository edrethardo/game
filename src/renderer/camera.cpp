#include "renderer/camera.h"
#include <cmath>

void CameraSystem::computeMatrices(Camera& cam, f32 aspectRatio) {
    // Rebuild forward/right from yaw/pitch (set by PlayerController)
    cam.forward = normalize(Vec3{
        -sinf(cam.yaw) * cosf(cam.pitch),
         sinf(cam.pitch),
        -cosf(cam.yaw) * cosf(cam.pitch)
    });
    cam.right = normalize(cross(cam.forward, {0.0f, 1.0f, 0.0f}));

    Vec3 target     = cam.position + cam.forward;
    cam.view        = Mat4::lookAt(cam.position, target, {0.0f, 1.0f, 0.0f});
    cam.projection  = Mat4::perspective(radians(cam.fovY), aspectRatio,
                                        cam.nearPlane, cam.farPlane);
    cam.viewProjection = cam.projection * cam.view;
}
