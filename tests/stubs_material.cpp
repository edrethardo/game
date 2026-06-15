// stubs_material.cpp — minimal stubs for MaterialSystem symbols pulled in by
// item_loader.cpp (resolveVisuals). The test binary doesn't load GPU resources,
// so these are no-op / return-zero stubs. Only included in dungeon_tests.
#include "renderer/material.h"
#include <cstring>

namespace MaterialSystem {
    void init(const char*)  {}
    void shutdown()         {}

    const Material* get(u8) { return nullptr; }

    // Returns 0 — matches production behavior (0 is the not-found sentinel in the real impl);
    // resolveVisuals isn't exercised by these unit tests anyway.
    u8 getIdByName(const char*) { return 0; }

    u32 count() { return 0; }
}
