// Enemy definition loader — parses enemies.json into EnemyDefTable.
// Separate from boss/item loaders to keep concerns isolated.

#pragma once

#include "game/enemy_def.h"

namespace EnemyLoader {
    // Parse enemies.json and fill the EnemyDefTable. Returns false on error.
    bool load(const char* path, EnemyDefTable& table);

    // Resolve mesh/material name strings to runtime IDs.
    // Call after mesh + material systems are initialized.
    void resolveVisuals(EnemyDefTable& table);
}
