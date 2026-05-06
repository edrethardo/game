#pragma once

#include <cstdint>
#include <cstddef>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using s8  = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

using f32 = float;
using f64 = double;

using usize = size_t;

// Platform-specific asset path prefix. On Switch, romfs mounts at "romfs:/".
// Use ASSET_PATH("assets/foo.json") to get the correct path on all platforms.
#ifdef __SWITCH__
    #define ASSET_PATH(p) "romfs:/" p
#else
    #define ASSET_PATH(p) p
#endif
