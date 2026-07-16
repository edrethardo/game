#pragma once
// teleport_dest.h — the ONE destination resolver for every player teleport / blink / dash-warp
// (Holy Smite, Phase Dash, Shadow Strike, Shadow Step).
//
// Each of those used to validate its landing spot its own way — a thin center ray with a
// 0.5 m back-off, a single cell-center solid check, or nothing at all — and every variant
// had a way to wedge the player: a dash that "stops on the first enemy" stopped at its
// CENTER (inside the body — inescapable inside anything Butcher-sized), and "1 m behind the
// target" is inside the wall whenever the target hugs one. Once the player's AABB overlaps a
// solid cell, moveAndSlide blocks every axis and they are stuck for good.
//
// Standalone .cpp (not skill_system) so tests/game/test_teleport_dest.cpp can link it against
// just level_grid + raycast without dragging the whole skill system in.

#include "core/types.h"
#include "core/math.h"

struct LevelGrid;
struct EntityPool;

namespace Teleport {
    // Returns the point nearest to `desired` along the start→desired line where the player's
    // full XZ footprint clears the grid AND every living entity body, with Y snapped to the
    // landing cell's floor. Marches from `desired` back toward `start` in 0.25 m steps; each
    // candidate is also LOS-checked from `start` so a sample can never land in a sealed
    // pocket on the far side of a thin wall. Falls back to `start` — a guaranteed-valid
    // no-op — when the entire line is blocked. Callers pass their UNVALIDATED ideal landing
    // point (the enemy's position, "1 m behind", the dash endpoint) and use the result.
    Vec3 resolveDest(const LevelGrid& grid, EntityPool& entities, Vec3 start, Vec3 desired);
}
