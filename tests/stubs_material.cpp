// stubs_material.cpp — minimal stubs for MaterialSystem symbols pulled in by
// item_loader.cpp (resolveVisuals). The test binary doesn't load GPU resources,
// so these are no-op / return-zero stubs. Only included in dungeon_tests.
#include "renderer/material.h"
#include <cstring>

namespace MaterialSystem {
    void init(const char*)  {}
    void shutdown()         {}

    const Material* get(u8) { return nullptr; }

    // Always returns -1 (not found) — resolveVisuals is not exercised by unit tests.
    u8 getIdByName(const char*) { return 0; }

    u32 count() { return 0; }
}
