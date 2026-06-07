#pragma once

#include "core/types.h"

// Screenshot — saves the current OpenGL back buffer to a PNG on disk.
//
// Wired to the F8 debug key (see Engine::handleDebugKeys) so designers can grab clean
// "hero shots" of the live 3D scene for marketing / Steam store art. Pair it with the
// F10 "cinematic" toggle (hides the HUD) and F2 noclip free-cam for framing. The captured
// PNGs feed tools/gen_steam_capsules.py.
//
// This is NOT a per-frame path — capture happens only on explicit user request — so the
// transient heap allocation for the pixel readback (too large for the 1 MB FrameAllocator)
// is deliberate and fine here.
namespace Screenshot {
    // Reads the (w x h) back buffer via glReadPixels and writes a 24-bit RGB PNG to `path`.
    // Rows are flipped vertically because GL's framebuffer origin is bottom-left while PNG
    // is top-down. Returns true on success; logs and returns false otherwise.
    bool capture(const char* path, u32 w, u32 h);
}
