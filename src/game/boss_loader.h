// Boss definition loader — parses bosses.json into BossDefTable.
// Separate from item loaders to keep boss concerns isolated.

#pragma once

#include "game/boss_def.h"

namespace BossLoader {
    // Parse bosses.json and fill the BossDefTable. Returns false on error.
    bool load(const char* path, BossDefTable& table);

    // Resolve mesh/material name strings to runtime IDs.
    // Call after mesh + material systems are initialized.
    void resolveVisuals(BossDefTable& table);
}
