// autoplay_descent.cpp — see autoplay_descent.h for why the Descent needs its own flow field.
//
// The BFS mirrors LevelGridSystem::buildFlowField (same direction encoding, same 4-connected
// expansion, same steer-to-cell-centre readout) and differs only in what it is seeded from and what
// it refuses to walk over.
#include "game/autoplay_descent.h"
#include "game/autoplay_nav.h"   // Autoplay::botStoryY — the storey rule the seeds are filtered by
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace Autoplay {

// Direction table, IDENTICAL to level_grid.cpp's: index = the stored code, value = the cell step.
// Only the even indices (cardinals) are ever stored because the expansion is 4-connected, but the
// full 8 are kept so the encoding stays interchangeable with LevelGrid::flowDir.
static constexpr s32 kDx[8] = { 1, 1, 0,-1,-1,-1, 0, 1};
static constexpr s32 kDz[8] = { 0, 1, 1, 1, 0,-1,-1,-1};

// Walkable for the descent router: on the grid, not a wall, and NOT a jump pad.
//
// Excluding pads is the whole reason this is not just "the exit field on another story". A Descent
// floor is seeded with jump pads — one filling every dead-end node, plus a return lift under about a
// third of the drop holes — and they fire the instant a body is grounded and throw it up roughly two
// stories. Routing a descent across one undoes the descent. Leaving them out of the field means the
// bot is never even offered the option, rather than being steered onto one and then vetoed off it by
// the driver every tick (which reads as jitter). The cost is that a bot ALREADY standing on a pad
// sits on an unreachable cell and gets {0,0,0} back — deliberately: it is about to be launched
// anyway, and the driver falls through to the exit flow field for that handful of ticks.
static inline bool routable(const LevelGrid& g, u32 x, u32 z) {
    if (LevelGridSystem::isSolid(g, x, z)) return false;
    return (LevelGridSystem::getCell(g, x, z).flags & CELL_JUMPPAD) == 0;
}

bool ensureDescentField(DescentField& f, const LevelGrid& g, const DungeonResult& d,
                        f32 storyY, u32 stamp) {
    const u32 total = g.width * g.depth;
    if (total == 0) { f.valid = false; return false; }

    // Already current? The story is compared on the slab pitch rather than exactly, because storyY
    // comes from effectiveFloorHeight and a body resting on a slab can read a hair under its top.
    if (f.dir && f.stamp == stamp && f.width == g.width && f.depth == g.depth &&
        std::fabs(f.storyY - storyY) <= PLATFORM_STEP_TOLERANCE)
        return f.valid;

    if (f.cap < total) {                       // grown from the grid, never a fixed "biggest we ship"
        u8* grown = static_cast<u8*>(std::realloc(f.dir, total));
        if (!grown) { f.valid = false; return false; }
        f.dir = grown; f.cap = total;
    }
    f.width = g.width; f.depth = g.depth; f.storyY = storyY; f.stamp = stamp;
    std::memset(f.dir, 0xFF, total);           // 0xFF = unreachable

    // Scratch queue sized from the GRID (every routable cell is enqueued at most once). Heap, not a
    // stack array, for the same reason buildFlowField uses one: a fixed buffer sized to today's
    // biggest grid is a time bomb the next grid bump lights.
    u32* queue = static_cast<u32*>(std::malloc(sizeof(u32) * total));
    if (!queue) { f.valid = false; return false; }
    u32 head = 0, tail = 0;

    // --- Seed: every drop hole on THIS story. ---
    // Clean holes first. If the story has none — possible on the deepest level, where holes are only
    // 7% dense — fall back to seeding the padded ones too: bouncing back up is bad, but it is a way
    // down, and a story with no seeds at all would leave the bot with no descent plan whatsoever.
    f.paddedOnly = false;
    for (u8 pass = 0; pass < 2 && tail == 0; pass++) {
        if (pass == 1) f.paddedOnly = true;   // pass 0 found no clean hole: this storey is lifts only
        for (u8 i = 0; i < d.dropHoleCount; i++) {
            const DropHole& h = d.dropHoles[i];
            if (std::fabs(h.surfaceY - storyY) > PLATFORM_STEP_TOLERANCE) continue;
            u32 hx, hz;
            if (!LevelGridSystem::worldToGrid(g, h.pos, hx, hz)) continue;
            if (LevelGridSystem::isSolid(g, hx, hz)) continue;
            // Pass 0 wants clean holes only; pass 1 (reached only if pass 0 seeded nothing) takes any.
            const bool padded = (LevelGridSystem::getCell(g, hx, hz).flags & CELL_JUMPPAD) != 0;
            if (pass == 0 && padded) continue;
            const u32 idx = hz * g.width + hx;
            if (f.dir[idx] == 0xFE) continue;                 // a wide hole spans several cells
            f.dir[idx] = 0xFE;                                // 0xFE = "at a way down"
            queue[tail++] = idx;
        }
    }
    if (tail == 0) {                                          // no way down from here (this is L0)
        std::free(queue);
        f.valid = false; f.paddedOnly = false;
        return false;
    }

    // --- BFS outward. Each cell stores the direction pointing back toward the hole it was found from.
    while (head < tail) {
        const u32 idx = queue[head++];
        const u32 cx = idx % g.width, cz = idx / g.width;
        for (u8 dir = 0; dir < 8; dir += 2) {                 // cardinals only (see header)
            const s32 nx = static_cast<s32>(cx) + kDx[dir];
            const s32 nz = static_cast<s32>(cz) + kDz[dir];
            if (nx < 0 || nz < 0 ||
                static_cast<u32>(nx) >= g.width || static_cast<u32>(nz) >= g.depth) continue;
            const u32 nIdx = static_cast<u32>(nz) * g.width + static_cast<u32>(nx);
            if (f.dir[nIdx] != 0xFF) continue;                // already has a route
            if (!routable(g, static_cast<u32>(nx), static_cast<u32>(nz))) continue;
            // We expanded neighbour -> current, so the neighbour's route direction is the REVERSE of
            // the step we took (dir+4 mod 8 — the same trick the exit field uses).
            f.dir[nIdx] = static_cast<u8>((dir + 4) & 7);
            queue[tail++] = nIdx;
        }
    }

    std::free(queue);
    f.valid = true;
    return true;
}

Vec3 descentDirection(const DescentField& f, const LevelGrid& g, Vec3 pos) {
    if (!f.valid || !f.dir) return {0, 0, 0};
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) return {0, 0, 0};
    if (gx >= f.width || gz >= f.depth) return {0, 0, 0};
    const u8 dir = f.dir[gz * f.width + gx];
    if (dir >= 8) return {0, 0, 0};                            // 0xFE at a hole / 0xFF unreachable

    // Steer at the NEXT CELL'S CENTRE, not just along the axis. This is the anti-wall-hug rule
    // inherited from the exit field: aiming at the centre pulls a body that has drifted toward a
    // corridor wall back into the middle of the corridor as it advances, instead of letting it
    // track along the wall it is already touching.
    const f32 tx = (static_cast<f32>(static_cast<s32>(gx) + kDx[dir]) + 0.5f) * g.cellSize;
    const f32 tz = (static_cast<f32>(static_cast<s32>(gz) + kDz[dir]) + 0.5f) * g.cellSize;
    const f32 dx = tx - pos.x, dz = tz - pos.z;
    const f32 len = std::sqrt(dx * dx + dz * dz);
    if (len < 0.01f) return {0, 0, 0};
    return {dx / len, 0.0f, dz / len};
}

bool atDescentGoal(const DescentField& f, const LevelGrid& g, Vec3 pos) {
    if (!f.valid || !f.dir) return false;
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) return false;
    if (gx >= f.width || gz >= f.depth) return false;
    return f.dir[gz * f.width + gx] == 0xFE;
}

void freeDescentField(DescentField& f) {
    std::free(f.dir);
    f.dir = nullptr; f.cap = 0; f.valid = false; f.paddedOnly = false;
    f.width = f.depth = 0; f.stamp = 0xFFFFFFFFu; f.storyY = 1e9f;
}

} // namespace Autoplay
