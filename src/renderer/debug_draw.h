#pragma once

#include "core/math.h"
#include "renderer/frustum.h"

namespace DebugDraw {
    void init();
    void shutdown();

    void clear();
    void flush(const Mat4& viewProjection);

    void line(Vec3 start, Vec3 end, Vec3 color);
    void box(const AABB& bounds, Vec3 color);     // 12 lines
    void ray(Vec3 origin, Vec3 dir, f32 len, Vec3 color);
    void cross(Vec3 pos, f32 size, Vec3 color);   // 3 axis lines

    void setEnabled(bool enabled);
    bool isEnabled();
}
