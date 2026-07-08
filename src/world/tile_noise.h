// tile_noise.h — tiny deterministic 2D value noise for baking subtle per-tile shade variance into
// the level mesh (see level_mesh.cpp). Pure, header-only, SDL/GL-free so it's unit-testable.
#pragma once

#include "core/types.h"
#include <cmath>

namespace TileNoise {

// Integer hash → [0,1). Deterministic in (x, z, seed) so a floor looks identical every time it's
// (re)built (host + clients share levelSeed, so no desync).
inline f32 hash2(s32 x, s32 z, u32 seed) {
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(z) * 668265263u + seed * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<f32>(h & 0xFFFFFFu) / static_cast<f32>(0x1000000u);  // [0,1)
}

inline f32 smooth(f32 t) { return t * t * (3.0f - 2.0f * t); }

// Smooth value noise in [0,1] sampled at (x, z) (caller scales the frequency). Bilinear blend of
// the four surrounding cell hashes with smoothstep interpolation → soft blobs, not salt-and-pepper.
inline f32 value(f32 x, f32 z, u32 seed) {
    s32 xi = static_cast<s32>(std::floor(x));
    s32 zi = static_cast<s32>(std::floor(z));
    f32 fx = smooth(x - static_cast<f32>(xi));
    f32 fz = smooth(z - static_cast<f32>(zi));
    f32 a = hash2(xi,     zi,     seed);
    f32 b = hash2(xi + 1, zi,     seed);
    f32 c = hash2(xi,     zi + 1, seed);
    f32 d = hash2(xi + 1, zi + 1, seed);
    return (a * (1.0f - fx) + b * fx) * (1.0f - fz)
         + (c * (1.0f - fx) + d * fx) * fz;  // [0,1]
}

} // namespace TileNoise
