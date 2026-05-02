#pragma once

#include "core/types.h"

struct Texture {
    u32 handle = 0;
    s32 width  = 0;
    s32 height = 0;
};

namespace TextureSystem {
    Texture load(const char* path);
    Texture create(const u8* pixels, s32 width, s32 height, s32 channels);
    Texture createWhite();
    void destroy(Texture& tex);
    void bind(const Texture& tex, u32 unit = 0);
}
