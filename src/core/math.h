#pragma once

#include "core/types.h"
#include <cmath>

inline f32 radians(f32 degrees) { return degrees * 3.14159265358979f / 180.0f; }

// ---- Vec2 ----

struct Vec2 {
    f32 x, y;
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x+b.x, a.y+b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x-b.x, a.y-b.y}; }
inline Vec2 operator*(Vec2 a, f32 s)  { return {a.x*s, a.y*s}; }

// ---- Vec3 ----

struct Vec3 {
    f32 x, y, z;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec3 operator*(Vec3 a, f32 s)  { return {a.x*s, a.y*s, a.z*s}; }
inline Vec3 operator*(f32 s, Vec3 a)  { return {a.x*s, a.y*s, a.z*s}; }
inline Vec3 operator/(Vec3 a, f32 s)  { return {a.x/s, a.y/s, a.z/s}; }
inline Vec3 operator-(Vec3 a)         { return {-a.x, -a.y, -a.z}; }

inline Vec3& operator+=(Vec3& a, Vec3 b) { a.x+=b.x; a.y+=b.y; a.z+=b.z; return a; }
inline Vec3& operator-=(Vec3& a, Vec3 b) { a.x-=b.x; a.y-=b.y; a.z-=b.z; return a; }

inline f32 dot(Vec3 a, Vec3 b)   { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline f32 length(Vec3 a)        { return sqrtf(dot(a, a)); }
inline f32 lengthSq(Vec3 a)      { return dot(a, a); }

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x
    };
}

inline Vec3 normalize(Vec3 a) {
    f32 len = length(a);
    if (len < 1e-8f) return {0, 0, 0};
    return a / len;
}

// ---- Vec4 ----

struct Vec4 {
    f32 x, y, z, w;
};

inline Vec4 vec4(Vec3 v, f32 w) { return {v.x, v.y, v.z, w}; }
inline Vec4 operator+(Vec4 a, Vec4 b) { return {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}; }
inline Vec4 operator*(Vec4 a, f32 s)  { return {a.x*s, a.y*s, a.z*s, a.w*s}; }
inline f32 dot(Vec4 a, Vec4 b) { return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w; }

// ---- Mat4 ----
// Column-major: m[col*4 + row]
// Column 0 = m[0..3], Column 1 = m[4..7], etc.

struct Mat4 {
    f32 m[16];

    const f32* ptr() const { return m; }

    static Mat4 identity() {
        Mat4 r = {};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    static Mat4 translate(Vec3 t) {
        Mat4 r = identity();
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    static Mat4 scale(Vec3 s) {
        Mat4 r = {};
        r.m[0]  = s.x;
        r.m[5]  = s.y;
        r.m[10] = s.z;
        r.m[15] = 1.0f;
        return r;
    }

    static Mat4 rotateX(f32 rad) {
        f32 c = cosf(rad), s = sinf(rad);
        Mat4 r = identity();
        r.m[5]  =  c; r.m[9]  = -s;
        r.m[6]  =  s; r.m[10] =  c;
        return r;
    }

    static Mat4 rotateY(f32 rad) {
        f32 c = cosf(rad), s = sinf(rad);
        Mat4 r = identity();
        r.m[0]  =  c; r.m[8]  =  s;
        r.m[2]  = -s; r.m[10] =  c;
        return r;
    }

    static Mat4 rotateZ(f32 rad) {
        f32 c = cosf(rad), s = sinf(rad);
        Mat4 r = identity();
        r.m[0] =  c; r.m[4] = -s;
        r.m[1] =  s; r.m[5] =  c;
        return r;
    }

    // Rotation by `rad` about an arbitrary axis (need NOT be unit — normalized here; zero -> identity).
    // Standard Rodrigues matrix, written in this file's column-major m[col*4+row] layout (verified:
    // substituting axis=(0,1,0) reproduces rotateY above exactly). Used for the directional
    // dodge-roll tumble, where the axis = up x dodgeDir is generally not a cardinal axis.
    static Mat4 rotate(Vec3 axis, f32 rad) {
        f32 len = sqrtf(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
        if (len < 1e-6f) return identity();
        f32 x = axis.x/len, y = axis.y/len, z = axis.z/len;
        f32 c = cosf(rad), s = sinf(rad), t = 1.0f - c;
        Mat4 m = identity();
        // Column-major: m.m[col*4 + row] = R[row][col]. Columns laid out per-row below.
        m.m[0]=t*x*x+c;    m.m[4]=t*x*y-s*z;  m.m[8] =t*x*z+s*y;
        m.m[1]=t*x*y+s*z;  m.m[5]=t*y*y+c;    m.m[9] =t*y*z-s*x;
        m.m[2]=t*x*z-s*y;  m.m[6]=t*y*z+s*x;  m.m[10]=t*z*z+c;
        return m;
    }

    static Mat4 perspective(f32 fovY, f32 aspect, f32 nearZ, f32 farZ) {
        f32 f = 1.0f / tanf(fovY * 0.5f);
        Mat4 r = {};
        r.m[0]  = f / aspect;
        r.m[5]  = f;
        r.m[10] = (farZ + nearZ) / (nearZ - farZ);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
        return r;
    }

    static Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
        Vec3 f = normalize(target - eye);
        Vec3 r = normalize(cross(f, up));
        Vec3 u = cross(r, f);

        Mat4 result = identity();
        result.m[0]  =  r.x; result.m[4]  =  r.y; result.m[8]  =  r.z;
        result.m[1]  =  u.x; result.m[5]  =  u.y; result.m[9]  =  u.z;
        result.m[2]  = -f.x; result.m[6]  = -f.y; result.m[10] = -f.z;
        result.m[12] = -dot(r, eye);
        result.m[13] = -dot(u, eye);
        result.m[14] =  dot(f, eye);
        return result;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r = {};
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            r.m[col*4 + row] =
                a.m[0*4 + row] * b.m[col*4 + 0] +
                a.m[1*4 + row] * b.m[col*4 + 1] +
                a.m[2*4 + row] * b.m[col*4 + 2] +
                a.m[3*4 + row] * b.m[col*4 + 3];
        }
    }
    return r;
}

inline Vec4 operator*(const Mat4& m, Vec4 v) {
    return {
        m.m[0]*v.x + m.m[4]*v.y + m.m[8]*v.z  + m.m[12]*v.w,
        m.m[1]*v.x + m.m[5]*v.y + m.m[9]*v.z  + m.m[13]*v.w,
        m.m[2]*v.x + m.m[6]*v.y + m.m[10]*v.z + m.m[14]*v.w,
        m.m[3]*v.x + m.m[7]*v.y + m.m[11]*v.z + m.m[15]*v.w
    };
}
