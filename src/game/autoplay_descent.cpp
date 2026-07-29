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
                        f32 storyY, u32 stamp, Vec3 exitPos) {
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
    // No holes on this story == we are on L0 (the bottom). Seed from the EXIT DOOR instead, so L0
    // routing is a pad-avoiding path to the door — the shared flat exit field does not dodge pads and
    // launched the bot back up a return lift the instant it reached L0. Only when the exit is actually
    // on this story (it always is — L0) and its cell is walkable; otherwise fall back to invalid (the
    // driver then uses the flat field, the old behaviour).
    if (tail == 0 && std::fabs(exitPos.y - storyY) <= PLATFORM_STEP_TOLERANCE) {
        // The exit route is PAD-AVOIDING, never "padded only" — clear the flag pass 1 may have set.
        // Bug it fixes: pass 1 sets paddedOnly=true BEFORE it knows it will seed anything, and on L0
        // (no slab below, so no holes at all) it seeds nothing, then we fall through to here. Leaving
        // paddedOnly stuck true stood the driver's pad veto DOWN on L0, so the bot walked straight onto
        // the return-lift pads under the L1 holes and got flung back up above the exit ("jumped above
        // the exit and too dumb to drop again"). L0 must AVOID pads to reach the door.
        f.paddedOnly = false;
        u32 ex, ez;
        if (LevelGridSystem::worldToGrid(g, exitPos, ex, ez) && !LevelGridSystem::isSolid(g, ex, ez)) {
            f.dir[ez * g.width + ex] = 0xFE;                  // the way OUT, treated like a way down
            queue[tail++] = ez * g.width + ex;
        }
    }
    if (tail == 0) {                                          // genuinely no way out from here
        std::free(queue);
        f.valid = false; f.paddedOnly = false;
        return false;
    }

    // --- BFS outward, in TWO TIERS. Each cell stores the direction pointing back toward the hole it
    // was found from (the REVERSE of the step we took: dir+4 mod 8 — the same trick the exit field
    // uses). The single-pass version left the router blind wherever the pad-exclusion severed the maze.
    //
    // TIER 1 (clean): expand over non-pad cells only, so the route it hands back never steps onto a
    // return-lift / dead-end pad — the ordinary, preferred descent.
    // TIER 2 (recovery): re-expand from the WHOLE tier-1 network, this time ALLOWING pad cells, to fill
    // any pocket the pads carved off from the holes. Without it a bot that wanders (or is dropped) into
    // a pad-walled pocket read 0xFF forever, got no descent heading, fell back to the flat exit field —
    // which on an upper storey routes toward ABOVE the L0 door, not a hole — and froze next to a pad it
    // was "afraid" to use (measured: Sorcerer stuck 12 min on floor 9, deaths flatlined, HP full). A
    // recovery cell routes back toward the clean network across the pad; the driver's veto lets the bot
    // take that one step once pad-avoiding routing is boxed in, so the bounce breaks the pocket. Tier 2
    // only ever touches cells tier 1 left unreachable, so a floor with no severed pockets is untouched.
    auto flood = [&](bool allowPads) {
        while (head < tail) {
            const u32 idx = queue[head++];
            const u32 cx = idx % g.width, cz = idx / g.width;
            for (u8 dir = 0; dir < 8; dir += 2) {             // cardinals only (see header)
                const s32 nx = static_cast<s32>(cx) + kDx[dir];
                const s32 nz = static_cast<s32>(cz) + kDz[dir];
                if (nx < 0 || nz < 0 ||
                    static_cast<u32>(nx) >= g.width || static_cast<u32>(nz) >= g.depth) continue;
                const u32 nIdx = static_cast<u32>(nz) * g.width + static_cast<u32>(nx);
                if (f.dir[nIdx] != 0xFF) continue;            // already has a route (or is a hole/seed)
                if (LevelGridSystem::isSolid(g, static_cast<u32>(nx), static_cast<u32>(nz))) continue;
                if (!allowPads &&
                    (LevelGridSystem::getCell(g, static_cast<u32>(nx), static_cast<u32>(nz)).flags
                     & CELL_JUMPPAD)) continue;               // tier 1: no pads
                f.dir[nIdx] = static_cast<u8>((dir + 4) & 7);
                queue[tail++] = nIdx;
            }
        }
    };
    flood(false);        // tier 1: clean descent
    head = 0;            // re-read the whole tier-1 queue (still in [0,tail)) as tier-2 seeds
    flood(true);        // tier 2: recovery flood through pads into any severed pocket

    std::free(queue);
    f.valid = true;
    return true;
}

// Can a body of `radius` walk the straight line a->b without any part of it entering a wall or a
// jump pad? Samples the segment and tests the four AABB corners plus the centre at each sample —
// the same shape as Pathfinder's segmentTraversable, reimplemented here because that one is file-
// static and this file deliberately links against nothing but the grid.
static bool clearRun(const LevelGrid& g, Vec3 a, Vec3 b, f32 radius) {
    const f32 dx = b.x - a.x, dz = b.z - a.z;
    const f32 dist = std::sqrt(dx * dx + dz * dz);
    if (dist < 1e-4f) return true;
    const f32 step = g.cellSize * 0.25f;                  // fine enough never to tunnel a 1-cell wall
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
            if (!routable(g, cx, cz)) return false;       // wall, or a pad the field is avoiding
        }
    }
    return true;
}

Vec3 descentDirection(const DescentField& f, const LevelGrid& g, Vec3 pos) {
    if (!f.valid || !f.dir) return {0, 0, 0};
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) return {0, 0, 0};
    if (gx >= f.width || gz >= f.depth) return {0, 0, 0};
    if (f.dir[gz * f.width + gx] >= 8) return {0, 0, 0};        // 0xFE at a hole / 0xFF unreachable

    // STRING-PULLED LOOKAHEAD, not "steer at the next cell".
    //
    // The field expands 4-CONNECTED (diagonals would clip corners a body cannot fit through), so its
    // route across open floor is a STAIRCASE: north, east, north, east. Steering at the immediately
    // next cell centre therefore swings the desired heading ~90 degrees every time the bot crosses a
    // cell boundary — which is both symptoms the bot was showing on a Descent floor at once. It is
    // the jitter ("the aim jittering seems to happen when on 4 story floors searching for the hole"):
    // measured, the field emitted 111-degree single-tick heading steps, ~50x the ~2 degrees the
    // combat aim contributes, every time the travel-heading commit released. And it is the
    // wall-hugging, because each leg of a staircase aims diagonally INTO the inside corner that the
    // next leg is about to turn around.
    //
    // So walk the field forward a few cells and steer at the FARTHEST waypoint still reachable in a
    // clear straight run — ordinary string-pulling, and the straight line through a staircase is the
    // diagonal the 4-connected expansion was not allowed to emit. The run is width-tested, so the
    // shortcut can never cut a corner the body would clip: this REMOVES corner contact rather than
    // trading jitter for it. Falling back to the next-cell centre keeps the old behaviour whenever
    // nothing further is reachable (a genuine tight corner), which is exactly when the staircase is
    // the correct path anyway.
    constexpr u8  kLookahead   = 8;      // cells; ~8 m of corridor, well past a single turn
    constexpr f32 kBodyRadius  = 0.35f;  // the bot's ~0.3 m half-width plus a little margin
    Vec3 wp[kLookahead];
    u8   n = 0;
    {   // Follow the field forward, collecting cell centres, stopping at a hole or a dead code.
        u32 cx = gx, cz = gz;
        for (u8 i = 0; i < kLookahead; i++) {
            const u8 d = f.dir[cz * f.width + cx];
            if (d >= 8) break;                          // reached a hole (0xFE) — nothing further
            const s32 nx = static_cast<s32>(cx) + kDx[d];
            const s32 nz = static_cast<s32>(cz) + kDz[d];
            if (nx < 0 || nz < 0 ||
                static_cast<u32>(nx) >= f.width || static_cast<u32>(nz) >= f.depth) break;
            cx = static_cast<u32>(nx); cz = static_cast<u32>(nz);
            wp[n++] = Vec3{(cx + 0.5f) * g.cellSize, pos.y, (cz + 0.5f) * g.cellSize};
        }
    }
    if (n == 0) return {0, 0, 0};

    Vec3 goal = wp[0];                                   // the old behaviour, as the floor
    for (u8 i = n; i-- > 1; ) {                          // farthest first
        if (clearRun(g, pos, wp[i], kBodyRadius)) { goal = wp[i]; break; }
    }
    const f32 dx = goal.x - pos.x, dz = goal.z - pos.z;
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

f32 nearestDropHole(const DescentField& f, const LevelGrid& g, Vec3 pos, Vec3& out) {
    if (!f.valid || !f.dir) return 1e9f;
    f32 best = 1e9f;
    // Linear scan of the field. It is only ~2k cells and the driver calls this once a tick on Descent
    // floors only, which is the same budget the field's own rebuild already pays a few times a floor.
    for (u32 z = 0; z < f.depth; z++) {
        for (u32 x = 0; x < f.width; x++) {
            if (f.dir[z * f.width + x] != 0xFE) continue;      // 0xFE = a way down (or the L0 exit)
            const f32 wx = (static_cast<f32>(x) + 0.5f) * g.cellSize;
            const f32 wz = (static_cast<f32>(z) + 0.5f) * g.cellSize;
            const f32 dx = wx - pos.x, dz = wz - pos.z;
            const f32 d  = std::sqrt(dx * dx + dz * dz);
            if (d < best) { best = d; out = Vec3{wx, pos.y, wz}; }
        }
    }
    return best;
}

Vec3 padEscapeDirection(const DescentField& f, const LevelGrid& g, Vec3 pos) {
    if (!f.valid || !f.dir) return {0, 0, 0};
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) return {0, 0, 0};
    if (gx >= f.width || gz >= f.depth) return {0, 0, 0};
    // Expanding ring search: the FIRST cell that is routable (a real field code, so the descent
    // network can carry the bot onward from there) and is NOT itself a pad. 6 cells is far enough to
    // clear a 3x3 pad node plus its neighbours and stays a trivial scan.
    constexpr s32 kMaxRing = 6;
    for (s32 ring = 1; ring <= kMaxRing; ring++) {
        for (s32 dz = -ring; dz <= ring; dz++) {
            for (s32 dx = -ring; dx <= ring; dx++) {
                // Perimeter of this ring only — the interior was covered by a previous, closer ring.
                if (dx > -ring && dx < ring && dz > -ring && dz < ring) continue;
                const s32 cx = static_cast<s32>(gx) + dx, cz = static_cast<s32>(gz) + dz;
                if (cx < 0 || cz < 0 || cx >= static_cast<s32>(f.width) || cz >= static_cast<s32>(f.depth))
                    continue;
                const u32 idx = static_cast<u32>(cz) * f.width + static_cast<u32>(cx);
                if (f.dir[idx] >= 8) continue;                                  // unreachable / a hole
                if (LevelGridSystem::isSolid(g, static_cast<u32>(cx), static_cast<u32>(cz))) continue;
                if (LevelGridSystem::getCell(g, static_cast<u32>(cx), static_cast<u32>(cz)).flags
                        & CELL_JUMPPAD) continue;                               // another lift: no
                const f32 wx = (static_cast<f32>(cx) + 0.5f) * g.cellSize;
                const f32 wz = (static_cast<f32>(cz) + 0.5f) * g.cellSize;
                Vec3 d{wx - pos.x, 0.0f, wz - pos.z};
                const f32 len = std::sqrt(d.x * d.x + d.z * d.z);
                if (len < 0.01f) continue;
                return Vec3{d.x / len, 0.0f, d.z / len};
            }
        }
    }
    return {0, 0, 0};
}

bool descentNextIsPad(const DescentField& f, const LevelGrid& g, Vec3 pos) {
    if (!f.valid || !f.dir) return false;
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) return false;
    if (gx >= f.width || gz >= f.depth) return false;
    const u8 d = f.dir[gz * f.width + gx];
    if (d >= 8) return false;                             // at a hole/goal (0xFE) or unreachable (0xFF)
    const s32 nx = static_cast<s32>(gx) + kDx[d];
    const s32 nz = static_cast<s32>(gz) + kDz[d];
    if (nx < 0 || nz < 0 ||
        static_cast<u32>(nx) >= f.width || static_cast<u32>(nz) >= f.depth) return false;
    return (LevelGridSystem::getCell(g, static_cast<u32>(nx), static_cast<u32>(nz)).flags
            & CELL_JUMPPAD) != 0;
}

void freeDescentField(DescentField& f) {
    std::free(f.dir);
    f.dir = nullptr; f.cap = 0; f.valid = false; f.paddedOnly = false;
    f.width = f.depth = 0; f.stamp = 0xFFFFFFFFu; f.storyY = 1e9f;
}

} // namespace Autoplay
