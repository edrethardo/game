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

// Platform-specific asset path. On Switch, romfs bundles the assets/ directory
// at the root, so "assets/shaders/basic.vert" → "romfs:/shaders/basic.vert".
// On PC, paths are used as-is. Uses a small ring buffer of static char arrays
// so multiple ASSET_PATH calls in one expression don't alias.
//
// SAVE_PATH wraps writable file paths (save.dat, controls.json).
// On Switch, writes go to the app's storage directory.
#ifdef __SWITCH__
    #include <cstdio>
    #include <cstring>
    inline const char* assetPath(const char* p) {
        static char bufs[4][256];
        static int idx = 0;
        char* buf = bufs[idx++ & 3];
        // Skip "assets/" prefix (7 chars) and prepend romfs:/
        std::snprintf(buf, 256, "romfs:/%s", p + 7);
        return buf;
    }
    #define ASSET_PATH(p) assetPath(p)
    #define SAVE_PATH(p) (p)  // Switch CWD is writable via sdmc
#else
    #define ASSET_PATH(p) (p)
    #define SAVE_PATH(p) (p)
#endif
