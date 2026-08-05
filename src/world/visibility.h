#pragma once
// visibility.h — "can the camera actually see this?", answered against the level grid.
//
// WHY THIS EXISTS. The renderer culls by FRUSTUM only. On a flat floor that is nearly enough, but
// on a maze or a stacked hall almost everything in the frustum is behind a wall or on another
// storey, and none of it is rejected: measured on FOUR_STORY, 114 entities produced 165 draw calls
// per frame and the level submitted a *perfectly constant* 55 submeshes regardless of where the
// camera pointed. All of that geometry is shaded into the depth buffer and then covered up, which
// is overdraw the Switch pays for at a 2.5x lower GPU clock in handheld.
//
// The test reuses `Raycast::cast` — the same slab-aware grid DDA the melee cone's LOS gate, the
// enemy AI and the Autoplay bot already use — rather than introducing a second notion of "can A see
// B". That matters on stacked floors specifically: the raycast understands platform slabs, so a
// storey below the camera is correctly reported as hidden by the floor between them.
//
// CONSERVATIVE BY CONSTRUCTION. Culling something genuinely visible is a visual bug (an enemy that
// pops in as you round a corner), which is far worse than drawing something hidden. So every helper
// here answers "MIGHT this be visible", samples several points rather than one, and the callers add
// hysteresis on top — once something is shown it stays shown for a few frames, so a single
// unlucky sample can never make it flicker.
//
// Pure and engine-free: takes a grid and points, returns bools. Unit-tested on hand-built grids in
// tests/world/test_visibility.cpp.

#include "core/types.h"
#include "core/math.h"
#include "world/level_grid.h"
#include "world/raycast.h"

namespace Visibility {

// Anything closer than this is never culled, whatever the geometry says. Two reasons: a body
// pressed against the far side of a thin pillar can have a sliver showing that a handful of sample
// rays will miss, and the saving on a nearby object is negligible anyway — the win is in the long
// tail across a maze, not in the thing standing next to you.
inline constexpr f32 MIN_CULL_DISTANCE = 6.0f;

// True when the straight segment eye->target is unobstructed by level geometry.
//
// `Raycast::cast` reports the FIRST blocking cell along the ray, so the segment is clear when it
// either hits nothing or hits something further away than the target itself. The epsilon keeps a
// target sitting flush against a wall (a torch bracket, an enemy backed into a corner) from being
// occluded by the very surface it stands on.
inline bool segmentClear(const LevelGrid& grid, Vec3 eye, Vec3 target) {
    const Vec3 d = target - eye;
    const f32  dist = length(d);
    if (dist <= 0.001f) return true;
    const RayHit h = Raycast::cast(grid, eye, d, dist);
    return !h.hit || h.distance >= dist - 0.15f;
}

// MIGHT any part of the world-space box be visible from `eye`?
//
// Samples the box CENTRE plus its eight corners, pulled slightly inward so a corner exactly on a
// wall plane does not self-occlude. Nine rays is a lot per object, which is why callers gate this
// behind a frustum test and a distance floor — by the time it runs, the candidate set is small.
inline bool boxVisible(const LevelGrid& grid, Vec3 eye, Vec3 boxMin, Vec3 boxMax) {
    const Vec3 c = (boxMin + boxMax) * 0.5f;
    if (lengthSq(c - eye) < MIN_CULL_DISTANCE * MIN_CULL_DISTANCE) return true;
    if (segmentClear(grid, eye, c)) return true;

    // Shrink toward the centre by 10% so samples sit inside the volume, not on its skin.
    const Vec3 lo = c + (boxMin - c) * 0.9f;
    const Vec3 hi = c + (boxMax - c) * 0.9f;
    const f32 xs[2] = {lo.x, hi.x};
    const f32 ys[2] = {lo.y, hi.y};
    const f32 zs[2] = {lo.z, hi.z};
    for (u32 i = 0; i < 8; i++) {
        const Vec3 p{xs[i & 1], ys[(i >> 1) & 1], zs[(i >> 2) & 1]};
        if (segmentClear(grid, eye, p)) return true;
    }
    return false;
}

// MIGHT an entity standing at `feet` with the given half-extents be visible from `eye`?
//
// A body is tall and thin, so the samples that matter are vertical (head visible over a slab edge,
// legs visible under a balcony) plus one lateral pair (a shoulder past a doorway). Five rays rather
// than boxVisible's nine, because this runs on every entity every frame and a body is a much
// simpler shape than a 16x16 m section of level.
inline bool entityVisible(const LevelGrid& grid, Vec3 eye, Vec3 feet, Vec3 halfExtents) {
    const Vec3 centre{feet.x, feet.y + halfExtents.y, feet.z};
    if (lengthSq(centre - eye) < MIN_CULL_DISTANCE * MIN_CULL_DISTANCE) return true;
    if (segmentClear(grid, eye, centre)) return true;
    if (segmentClear(grid, eye, Vec3{feet.x, feet.y + halfExtents.y * 1.9f, feet.z})) return true;
    if (segmentClear(grid, eye, Vec3{feet.x, feet.y + 0.1f, feet.z})) return true;

    // Lateral pair, perpendicular to the eye->target direction on the XZ plane, so the offset is
    // always ACROSS the line of sight where a sliver would actually show — offsetting along world X
    // would collapse to nothing whenever the target happens to be due east.
    Vec3 d = centre - eye; d.y = 0.0f;
    const f32 len = length(d);
    if (len > 0.001f) {
        const Vec3 side{-d.z / len * halfExtents.x, 0.0f, d.x / len * halfExtents.x};
        if (segmentClear(grid, eye, centre + side)) return true;
        if (segmentClear(grid, eye, centre - side)) return true;
    }
    return false;
}

} // namespace Visibility
