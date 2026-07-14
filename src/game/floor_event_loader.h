#pragma once

#include "game/floor_event.h"

// Parses assets/config/events.json into a FloorEventTable. Kept separate from floor_event.cpp so the
// pure selection logic stays free of the JSON dependency (and therefore unit-testable).
namespace FloorEventLoader {
    // Returns false if the file is missing or malformed. Not fatal: a game with no floor events is
    // a valid game, so the caller simply ends up with an empty table.
    bool load(const char* path, FloorEventTable& out);
}
