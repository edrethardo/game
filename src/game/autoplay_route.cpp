// autoplay_route.cpp — see autoplay_route.h. A BFS flow field seeded from ONE arbitrary goal cell,
// mirroring LevelGridSystem::buildFlowField / DescentField (same 4-connected expansion, same 0xFE/0xFF
// encoding, same string-pulled cell-centre readout) — the difference is only what it is seeded from.
#include "game/autoplay_route.h"
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace Autoplay {

// Direction table, IDENTICAL to level_grid.cpp / autoplay_descent.cpp: index = stored code, value =
// the cell step. Only even indices (cardinals) are stored (4-connected — a diagonal clips a corner a
// ~1-cell body cannot fit through); the full 8 keep the encoding interchangeable with flowDir.
static constexpr s32 kDx[8] = { 1, 1, 0,-1,-1,-1, 0, 1};
static constexpr s32 kDz[8] = { 0, 1, 1, 1, 0,-1,-1,-1};

// Walkable for routing: on the grid and not a wall. (Lava/pads are hazards the caller's veto handles;
// a flat boss floor — the only place this field is used — has neither, so keeping it to "not solid"
// keeps the field defined everywhere the body can actually stand.)
static inline bool routable(const LevelGrid& g, u32 x, u32 z) {
    return !LevelGridSystem::isSolid(g, x, z);
}

bool ensureRouteField(RouteField& f, const LevelGrid& g, Vec3 goalPos, u32 stamp) {
    const u32 total = g.width * g.depth;
    if (total == 0) { f.valid = false; return false; }

    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, goalPos, gx, gz)) { f.valid = false; return false; }

    // Already current? Same floor, grid size, AND goal cell — a moving goal that stayed in its cell
    // needs no rebuild, so this is cheap to call every tick.
    if (f.dir && f.stamp == stamp && f.width == g.width && f.depth == g.depth &&
        f.goalX == static_cast<s32>(gx) && f.goalZ == static_cast<s32>(gz))
        return f.valid;

    if (f.cap < total) {   // grown from the grid, never a fixed "biggest we ship" (see buildFlowField)
        u8* grown = static_cast<u8*>(std::realloc(f.dir, total));
        if (!grown) { f.valid = false; return false; }
        f.dir = grown; f.cap = total;
    }
    f.width = g.width; f.depth = g.depth; f.stamp = stamp;
    f.goalX = static_cast<s32>(gx); f.goalZ = static_cast<s32>(gz);
    std::memset(f.dir, 0xFF, total);   // 0xFF = unreachable

    // The goal cell must be walkable to seed; if the goal sits in a wall (shouldn't for a boss), snap
    // to the nearest walkable of its 4 neighbours so a body-radius offset still routes.
    if (!routable(g, gx, gz)) {
        bool seeded = false;
        for (u8 d = 0; d < 8 && !seeded; d += 2) {
            const s32 nx = static_cast<s32>(gx) + kDx[d], nz = static_cast<s32>(gz) + kDz[d];
            if (nx < 0 || nz < 0 || static_cast<u32>(nx) >= g.width || static_cast<u32>(nz) >= g.depth) continue;
            if (routable(g, static_cast<u32>(nx), static_cast<u32>(nz))) { gx = nx; gz = nz; seeded = true; }
        }
        if (!seeded) { f.valid = false; return false; }
    }

    u32* queue = static_cast<u32*>(std::malloc(sizeof(u32) * total));
    if (!queue) { f.valid = false; return false; }
    u32 head = 0, tail = 0;

    f.dir[gz * g.width + gx] = 0xFE;             // 0xFE = "at the goal"
    queue[tail++] = gz * g.width + gx;

    while (head < tail) {                        // BFS outward, 4-connected
        const u32 idx = queue[head++];
        const u32 cx = idx % g.width, cz = idx / g.width;
        for (u8 dir = 0; dir < 8; dir += 2) {
            const s32 nx = static_cast<s32>(cx) + kDx[dir];
            const s32 nz = static_cast<s32>(cz) + kDz[dir];
            if (nx < 0 || nz < 0 ||
                static_cast<u32>(nx) >= g.width || static_cast<u32>(nz) >= g.depth) continue;
            const u32 nIdx = static_cast<u32>(nz) * g.width + static_cast<u32>(nx);
            if (f.dir[nIdx] != 0xFF) continue;   // already routed
            if (!routable(g, static_cast<u32>(nx), static_cast<u32>(nz))) continue;
            // Neighbour's route points BACK toward us — reverse of the step (dir+4 mod 8).
            f.dir[nIdx] = static_cast<u8>((dir + 4) & 7);
            queue[tail++] = nIdx;
        }
    }

    std::free(queue);
    f.valid = true;
    return true;
}

// Can a body of `radius` walk the straight segment a->b without any part entering a wall? Samples the
// segment's four AABB corners + centre — the same shape DescentField uses, so the string-pull below
// can never cut a corner the body would clip.
static bool clearRun(const LevelGrid& g, Vec3 a, Vec3 b, f32 radius) {
    const f32 dx = b.x - a.x, dz = b.z - a.z;
    const f32 dist = std::sqrt(dx * dx + dz * dz);
    if (dist < 1e-4f) return true;
    const f32 step = g.cellSize * 0.25f;
    const s32 n = static_cast<s32>(dist / step) + 1;
    const f32 inv = 1.0f / static_cast<f32>(n);
    const f32 ox[5] = { 0,  radius,  radius, -radius, -radius };
    const f32 oz[5] = { 0,  radius, -radius,  radius, -radius };
    for (s32 i = 0; i <= n; i++) {
        const f32 t  = static_cast<f32>(i) * inv;
        const f32 px = a.x + dx * t, pz = a.z + dz * t;
        for (u8 k = 0; k < 5; k++) {
            u32 cx, cz;
            if (!LevelGridSystem::worldToGrid(g, Vec3{px + ox[k], 0.0f, pz + oz[k]}, cx, cz)) return false;
            if (!routable(g, cx, cz)) return false;
        }
    }
    return true;
}

Vec3 routeDirection(const RouteField& f, const LevelGrid& g, Vec3 pos) {
    if (!f.valid || !f.dir) return {0, 0, 0};
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) return {0, 0, 0};
    if (gx >= f.width || gz >= f.depth) return {0, 0, 0};
    if (f.dir[gz * f.width + gx] >= 8) return {0, 0, 0};   // 0xFE at the goal / 0xFF unreachable

    // STRING-PULLED lookahead (identical to DescentField): the field is 4-connected, so its route
    // across open floor is a staircase; steering at the immediately-next cell swings the heading ~90°
    // at every cell boundary (jitter) and aims into the inside corner each leg turns around (wall-hug).
    // So walk the field a few cells forward and steer at the FARTHEST waypoint reachable in a clear
    // straight run — the diagonal the 4-connected field was not allowed to emit. Width-tested, so it
    // can never cut a corner the body would clip; falls back to the next cell at a genuine tight turn.
    constexpr u8  kLookahead  = 8;
    constexpr f32 kBodyRadius = 0.35f;
    Vec3 wp[kLookahead];
    u8   n = 0;
    {
        u32 cx = gx, cz = gz;
        for (u8 i = 0; i < kLookahead; i++) {
            const u8 d = f.dir[cz * f.width + cx];
            if (d >= 8) break;                   // reached the goal (0xFE)
            const s32 nx = static_cast<s32>(cx) + kDx[d];
            const s32 nz = static_cast<s32>(cz) + kDz[d];
            if (nx < 0 || nz < 0 ||
                static_cast<u32>(nx) >= f.width || static_cast<u32>(nz) >= f.depth) break;
            cx = static_cast<u32>(nx); cz = static_cast<u32>(nz);
            wp[n++] = Vec3{(cx + 0.5f) * g.cellSize, pos.y, (cz + 0.5f) * g.cellSize};
        }
    }
    if (n == 0) return {0, 0, 0};

    Vec3 goal = wp[0];                            // next-cell centre, the safe floor
    for (u8 i = n; i-- > 1; ) {                   // farthest reachable first
        if (clearRun(g, pos, wp[i], kBodyRadius)) { goal = wp[i]; break; }
    }
    const f32 dx = goal.x - pos.x, dz = goal.z - pos.z;
    const f32 len = std::sqrt(dx * dx + dz * dz);
    if (len < 0.01f) return {0, 0, 0};
    return {dx / len, 0.0f, dz / len};
}

void freeRouteField(RouteField& f) {
    std::free(f.dir);
    f.dir = nullptr; f.cap = 0; f.valid = false;
    f.width = f.depth = 0; f.goalX = f.goalZ = -1; f.stamp = 0xFFFFFFFFu;
}

} // namespace Autoplay
