#pragma once

#include "core/math.h"

struct Plane {
    Vec3 normal;
    f32  distance;
};

struct Frustum {
    Plane planes[6]; // left, right, bottom, top, near, far
};

struct AABB {
    Vec3 min;
    Vec3 max;
};

inline Frustum extractFrustum(const Mat4& vp) {
    // Griess-Hartmann: extract planes directly from VP matrix rows (column-major layout)
    // Row i is: vp.m[i], vp.m[i+4], vp.m[i+8], vp.m[i+12]
    auto row = [&](int r) -> Vec4 {
        return {vp.m[r], vp.m[r+4], vp.m[r+8], vp.m[r+12]};
    };
    Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);

    auto makePlane = [](Vec4 p) -> Plane {
        Vec3 n = {p.x, p.y, p.z};
        f32  len = length(n);
        if (len < 1e-8f) return {{0,0,0}, 0};
        return {n / len, p.w / len};
    };

    Frustum f;
    f.planes[0] = makePlane({r3.x+r0.x, r3.y+r0.y, r3.z+r0.z, r3.w+r0.w}); // left
    f.planes[1] = makePlane({r3.x-r0.x, r3.y-r0.y, r3.z-r0.z, r3.w-r0.w}); // right
    f.planes[2] = makePlane({r3.x+r1.x, r3.y+r1.y, r3.z+r1.z, r3.w+r1.w}); // bottom
    f.planes[3] = makePlane({r3.x-r1.x, r3.y-r1.y, r3.z-r1.z, r3.w-r1.w}); // top
    f.planes[4] = makePlane({r3.x+r2.x, r3.y+r2.y, r3.z+r2.z, r3.w+r2.w}); // near
    f.planes[5] = makePlane({r3.x-r2.x, r3.y-r2.y, r3.z-r2.z, r3.w-r2.w}); // far
    return f;
}

inline bool isVisible(const Frustum& frustum, const AABB& aabb) {
    for (int i = 0; i < 6; i++) {
        const Plane& p = frustum.planes[i];
        Vec3 pv = {
            p.normal.x >= 0 ? aabb.max.x : aabb.min.x,
            p.normal.y >= 0 ? aabb.max.y : aabb.min.y,
            p.normal.z >= 0 ? aabb.max.z : aabb.min.z
        };
        if (dot(p.normal, pv) + p.distance < 0) return false;
    }
    return true;
}
