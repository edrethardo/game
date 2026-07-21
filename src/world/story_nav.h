#pragma once
// story_nav.h — pure two-story navigation helpers for VERTICAL_HALL floors. The enemy CHASE uses
// these to decide whether its target is on the OTHER story and, if so, which ramp end to walk to.
// Kept pure (grid/dungeon in, XZ/bool out) so they're unit-testable independent of the AI (tests/
// world/test_story_nav.cpp) — the crowd_control.h / arena.h / lead_assist.h pattern.

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"
#include "world/level_gen.h"

namespace StoryNav {

// Which story a body's FEET are on in the cell under xzPos: true = upper (standing on the slab top),
// false = ground. Mirrors the collision story selector (effectiveFloorHeight) — a body is "upper"
// when its feet are within a step of the slab top. A cell with no slab is always the ground story.
inline bool onUpperStory(const LevelGrid& g, Vec3 xzPos, f32 feetY) {
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, xzPos, gx, gz)) return false;
    if (!LevelGridSystem::hasPlatform(g, gx, gz))         return false;
    return feetY >= LevelGridSystem::getPlatformTop(g, gx, gz) - PLATFORM_STEP_TOLERANCE;
}

// The XZ goal a body on story `fromUpper` should walk toward to reach story `toUpper`: the nearest
// ramp END on its OWN story (the foot if climbing up, the top if heading down). Crossing that end
// carries it via the ramp (the entity story-snap does the actual vertical move). Returns `from`
// unchanged when already on the target story or when the floor has no ramps.
inline Vec3 nearestPortalGoal(const DungeonResult& d, Vec3 from, bool fromUpper, bool toUpper) {
    if (fromUpper == toUpper || d.portalCount == 0) return from;
    Vec3 best = from;
    f32  bestD2 = 1e30f;
    for (u8 i = 0; i < d.portalCount; i++) {
        const Vec3 nearEnd = fromUpper ? d.portals[i].highPos : d.portals[i].lowPos;
        const f32 dx = nearEnd.x - from.x, dz = nearEnd.z - from.z;
        const f32 d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = nearEnd; }
    }
    return best;
}

} // namespace StoryNav
