// screenshot.cpp — glReadPixels + vertical flip + PNG encode.
//
// This is the single translation unit that owns STB_IMAGE_WRITE_IMPLEMENTATION (mirroring
// how texture.cpp owns STB_IMAGE_IMPLEMENTATION for the loader). It backs the F8 debug-key
// screenshot used to capture store/marketing art (see screenshot.h and
// tools/gen_steam_capsules.py). Because capture is a rare, user-triggered action rather than
// a hot-loop operation, the per-call heap allocation below is intentional — the readback is
// far larger than the 1 MB shared FrameAllocator.

#include "renderer/screenshot.h"

#include <glad/glad.h>

#include "core/log.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <cstdlib>
#include <cstring>

namespace Screenshot {

bool capture(const char* path, u32 w, u32 h) {
    if (w == 0 || h == 0) {
        LOG_ERROR("screenshot: invalid size %ux%u", w, h);
        return false;
    }

    const usize rowBytes = (usize)w * 3;   // tightly packed 24-bit RGB
    const usize total = rowBytes * h;

    u8* pixels = (u8*)std::malloc(total);
    if (!pixels) {
        LOG_ERROR("screenshot: alloc failed (%zu bytes)", total);
        return false;
    }

    // Force tight row packing — default GL_PACK_ALIGNMENT is 4, which would pad odd widths.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGB, GL_UNSIGNED_BYTE, pixels);

    // GL origin is bottom-left, PNG is top-down: swap row y with row (h-1-y) via a scratch row.
    u8* scratch = (u8*)std::malloc(rowBytes);
    if (scratch) {
        for (u32 y = 0; y < h / 2; ++y) {
            u8* top = pixels + (usize)y * rowBytes;
            u8* bot = pixels + (usize)(h - 1 - y) * rowBytes;
            std::memcpy(scratch, top, rowBytes);
            std::memcpy(top, bot, rowBytes);
            std::memcpy(bot, scratch, rowBytes);
        }
        std::free(scratch);
    }

    // Fast lossless encode. PNG is lossless at EVERY compression level — these two knobs only
    // trade file size for speed — and the --record path calls this once per simulated frame:
    // at 1920x1080 the default (level 8 + adaptive filtering) measured ~0.7 fps wall with two
    // concurrent captures, which turns a 28-take trailer shoot into a 6-hour job. Level 1 with
    // filtering off is a several-fold speedup; the larger PNGs are deleted right after the
    // take is encoded to its CRF-18 master anyway.
    stbi_write_png_compression_level = 1;
    stbi_write_force_png_filter = 0;
    const int ok = stbi_write_png(path, (int)w, (int)h, 3, pixels, (int)rowBytes);
    std::free(pixels);

    if (!ok) {
        LOG_ERROR("screenshot: stbi_write_png failed for %s", path);
        return false;
    }
    LOG_INFO("screenshot saved: %s (%ux%u)", path, w, h);
    return true;
}

} // namespace Screenshot
