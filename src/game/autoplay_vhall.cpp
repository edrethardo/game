// autoplay_vhall.cpp — see autoplay_vhall.h for why VERTICAL_HALL needs a two-story flow field.
//
// The BFS mirrors DescentField/buildFlowField (same direction encoding, 4-connected expansion,
// steer-to-cell-centre readout) and adds the second story layer plus the ramp-foot vertical bridge.
#include "game/autoplay_vhall.h"
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace Autoplay {

// Direction table, IDENTICAL to level_grid.cpp / autoplay_descent.cpp. Only even indices (cardinals)
// are stored (4-connected expansion); the full 8 keep the encoding interchangeable with flowDir.
static constexpr s32 kDx[8] = { 1, 1, 0,-1,-1,-1, 0, 1};
static constexpr s32 kDz[8] = { 0, 1, 1, 1, 0,-1,-1,-1};

// The walkable SURFACE height of node (cell, story): ground = the base floor, upper = the highest
// slab top. Upper is only meaningful where hasPlatform (guarded by nodeWalkable).
static inline f32 nodeHeight(const LevelGrid& g, u32 x, u32 z, u8 story) {
    return story == 0 ? LevelGridSystem::getFloorHeight(g, x, z)
                      : LevelGridSystem::getPlatformTop(g, x, z);
}

// Only ground roofed at (near) full BALCONY height counts as walkable ground for routing. A balcony
// or catwalk sits at 3 m, so its arcade (~2.75 m of headroom) is a fine place to walk; a RAMP is a
// graduated slab that descends to head height and below, and the ground UNDER a ramp is a trap — the
// body cannot fit under the low part, and where it can fit the field routes it along under the ramp
// toward slabs it can never climb, then wedges (measured, repeatedly, on the geared paladin). So the
// ground story is the OPEN floor plus the balcony arcades only; the ramp is mounted from the open
// ground beside its FOOT (a cross-story step), never from underneath.
static constexpr f32 kBodyClearance = 0.8f;   // a slab hanging lower than this can't be walked under

// Is node (cell, story) somewhere a body can stand? Upper: a cell that carries a slab (balcony /
// ramp / catwalk), stood ON. Ground: any non-wall cell, EXCEPT one roofed by a slab too low to fit
// under (the low part of a ramp) — routing the bot there wedges it.
static inline bool nodeWalkable(const LevelGrid& g, u32 x, u32 z, u8 story) {
    if (LevelGridSystem::isSolid(g, x, z)) return false;
    if (story == 1) return LevelGridSystem::hasPlatform(g, x, z);
    if (LevelGridSystem::hasPlatform(g, x, z) &&
        LevelGridSystem::getPlatformUnderside(g, x, z) < kBodyClearance) return false;
    return true;
}

bool ensureVHallField(VHallField& f, const LevelGrid& g, Vec3 doorPos, u32 stamp) {
    const u32 cells = g.width * g.depth;
    if (cells == 0) { f.valid = false; return false; }
    const u32 total = cells * 2;   // two story layers

    // Already current? (same floor, same grid size, same door story)
    if (f.dir && f.stamp == stamp && f.width == g.width && f.depth == g.depth &&
        std::fabs(f.doorY - doorPos.y) < 0.5f)
        return f.valid;

    if (f.cap < total) {
        u8* grown = static_cast<u8*>(std::realloc(f.dir, total));
        if (!grown) { f.valid = false; return false; }
        f.dir = grown; f.cap = total;
    }
    f.width = g.width; f.depth = g.depth; f.stamp = stamp; f.doorY = doorPos.y;
    std::memset(f.dir, 0xFF, total);   // 0xFF = unreachable

    auto idx = [&](u32 x, u32 z, u8 s) -> u32 { return s * cells + z * g.width + x; };

    // Scratch queue of packed node indices (0..total). Heap, sized from the grid (each node enqueued
    // at most once), same rationale as the other fields.
    u32* queue = static_cast<u32*>(std::malloc(sizeof(u32) * total));
    if (!queue) { f.valid = false; return false; }
    u32 head = 0, tail = 0;

    // --- Seed: the door's own (cell, story). The door story is upper iff it sits above the ground. ---
    u32 dx, dz;
    if (!LevelGridSystem::worldToGrid(g, doorPos, dx, dz)) { std::free(queue); f.valid = false; return false; }
    const u8 doorStory = (doorPos.y > 1.5f) ? 1 : 0;
    if (!nodeWalkable(g, dx, dz, doorStory)) {
        // The door cell should always be walkable at its story; if some geometry edge case says
        // otherwise, fail soft — the caller keeps the flat flow field.
        std::free(queue); f.valid = false; return false;
    }
    f.dir[idx(dx, dz, doorStory)] = 0xFE;
    queue[tail++] = idx(dx, dz, doorStory);

    // --- BFS outward over (cell, story) nodes. ---
    while (head < tail) {
        const u32 node = queue[head++];
        const u8  s    = static_cast<u8>(node / cells);
        const u32 rem  = node % cells;
        const u32 cx   = rem % g.width, cz = rem / g.width;
        const f32 h    = nodeHeight(g, cx, cz, s);

        // HORIZONTAL, CROSS-STORY: for each 4-connected neighbour try BOTH stories. A step is legal
        // when the neighbour node is walkable AND its surface height is within one step of ours —
        // which the collision step-up/step-down handles. This is what models mounting a ramp: an open
        // GROUND cell (0 m) beside the ramp FOOT connects to that foot's UPPER node (slab ~0.25 m,
        // within a step), so the ground field routes the bot to the foot and steps it up; from the
        // foot the graduated ramp is one continuous height-continuous run up to the balcony and door.
        // Same-story steps (ground->ground, balcony->balcony, ramp->ramp) are just the ns==s case.
        for (u8 dir = 0; dir < 8; dir += 2) {
            const s32 nx = static_cast<s32>(cx) + kDx[dir];
            const s32 nz = static_cast<s32>(cz) + kDz[dir];
            if (nx < 0 || nz < 0 ||
                static_cast<u32>(nx) >= g.width || static_cast<u32>(nz) >= g.depth) continue;
            const u32 ux = static_cast<u32>(nx), uz = static_cast<u32>(nz);
            for (u8 ns = 0; ns < 2; ns++) {
                if (!nodeWalkable(g, ux, uz, ns)) continue;
                const u32 nIdx = idx(ux, uz, ns);
                if (f.dir[nIdx] != 0xFF) continue;
                if (std::fabs(h - nodeHeight(g, ux, uz, ns)) > PLATFORM_STEP_TOLERANCE) continue;
                // The neighbour's route direction points BACK toward us (reverse of the step).
                f.dir[nIdx] = static_cast<u8>((dir + 4) & 7);
                queue[tail++] = nIdx;
            }
        }
    }

    std::free(queue);
    f.valid = true;
    return true;
}

// The bot's story in its current cell: upper when the feet are on the slab (the same rule collision
// uses — feet within a step below the slab top), else ground.
static inline u8 botStory(const LevelGrid& g, u32 x, u32 z, f32 feetY) {
    if (!LevelGridSystem::hasPlatform(g, x, z)) return 0;
    return feetY >= LevelGridSystem::getPlatformTop(g, x, z) - PLATFORM_STEP_TOLERANCE ? 1 : 0;
}

Vec3 vhallDirection(const VHallField& f, const LevelGrid& g, Vec3 pos) {
    if (!f.valid || !f.dir) return {0, 0, 0};
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) return {0, 0, 0};
    if (gx >= f.width || gz >= f.depth) return {0, 0, 0};
    const u32 cells = f.width * f.depth;
    const u8  s   = botStory(g, gx, gz, pos.y);
    const u8  dir = f.dir[s * cells + gz * f.width + gx];
    if (dir >= 8) return {0, 0, 0};                        // 0xFE at the door / 0xFF unreachable

    // Steer at the NEXT CELL'S CENTRE (the anti-wall-hug rule): aiming at the centre pulls a body
    // that has drifted toward a wall or a ramp edge back into the middle as it advances.
    const f32 tx = (static_cast<f32>(static_cast<s32>(gx) + kDx[dir]) + 0.5f) * g.cellSize;
    const f32 tz = (static_cast<f32>(static_cast<s32>(gz) + kDz[dir]) + 0.5f) * g.cellSize;
    const f32 ddx = tx - pos.x, ddz = tz - pos.z;
    const f32 len = std::sqrt(ddx * ddx + ddz * ddz);
    if (len < 0.01f) return {0, 0, 0};
    return {ddx / len, 0.0f, ddz / len};
}

bool atVHallGoal(const VHallField& f, const LevelGrid& g, Vec3 pos) {
    if (!f.valid || !f.dir) return false;
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) return false;
    if (gx >= f.width || gz >= f.depth) return false;
    const u32 cells = f.width * f.depth;
    const u8  s = botStory(g, gx, gz, pos.y);
    return f.dir[s * cells + gz * f.width + gx] == 0xFE;
}

void freeVHallField(VHallField& f) {
    std::free(f.dir);
    f.dir = nullptr; f.cap = 0; f.valid = false;
    f.width = f.depth = 0; f.stamp = 0xFFFFFFFFu; f.doorY = 0.0f;
}

} // namespace Autoplay
