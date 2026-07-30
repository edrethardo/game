// autoplay_vhall.cpp — see autoplay_vhall.h for why VERTICAL_HALL needs a two-story flow field and
// why it is executed by a NODE-COMMITTED follower rather than a per-tick direction re-read.
//
// The route build mirrors DescentField/buildFlowField (same direction encoding, 4-connected
// expansion, steer-to-cell-centre readout) and adds the second story layer, the ramp-foot vertical
// bridge, the recorded JUMP-LINK edges (the broken catwalk), and a per-node remaining distance.
#include "game/autoplay_vhall.h"
#include "game/autoplay_nav.h"   // VH_BODY_CLEARANCE — shared with the travel veto
#include "world/story_nav.h"     // StoryNav::planVault — the pure takeoff-geometry gate
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace Autoplay {

// Direction table, IDENTICAL to level_grid.cpp / autoplay_descent.cpp. Only even indices (cardinals)
// are stored (4-connected expansion); the full 8 keep the encoding interchangeable with flowDir.
static constexpr s32 kDx[8] = { 1, 1, 0,-1,-1,-1, 0, 1};
static constexpr s32 kDz[8] = { 0, 1, 1, 1, 0,-1,-1,-1};

// Route cost of crossing a jump link, in walk-steps. The gap is 2 cells + the takeoff, but the real
// price is the RISK (a miss is a fall to the void and a re-climb), so it is costed a little above
// its literal length: the field only routes through the jump when it genuinely shortens the route
// (the isolated W balcony), not to shave a corner.
static constexpr u16 kJumpCost = 3;

// The walkable SURFACE height of node (cell, story): ground = the base floor, upper = the highest
// slab top. Upper is only meaningful where hasPlatform (guarded by nodeWalkable).
static inline f32 nodeHeight(const LevelGrid& g, u32 x, u32 z, u8 story) {
    return story == 0 ? LevelGridSystem::getFloorHeight(g, x, z)
                      : LevelGridSystem::getPlatformTop(g, x, z);
}

// Is node (cell, story) somewhere a body can stand? Upper: a cell that carries a slab (balcony /
// ramp / catwalk), stood ON. Ground: any non-wall cell, EXCEPT one roofed by a slab too low to fit
// under (the low part of a ramp) — routing the bot there wedges it. The clearance rule lives in
// LevelGridSystem::bodyPinnedUnderSlab, shared with the travel veto and the teleport resolver.
static inline bool nodeWalkable(const LevelGrid& g, u32 x, u32 z, u8 story) {
    if (LevelGridSystem::isSolid(g, x, z)) return false;
    if (story == 1) return LevelGridSystem::hasPlatform(g, x, z);
    if (LevelGridSystem::bodyPinnedUnderSlab(g, x, z, LevelGridSystem::getFloorHeight(g, x, z)))
        return false;
    return true;
}

bool ensureVHallField(VHallField& f, const LevelGrid& g, const DungeonResult& dg,
                      Vec3 doorPos, u32 stamp, bool useJumpLinks) {
    const u32 cells = g.width * g.depth;
    if (cells == 0) { f.valid = false; return false; }
    const u32 total = cells * 2;   // two story layers

    // Already current? (same floor, same grid size, same door story, same edge set)
    if (f.dir && f.dist && f.stamp == stamp && f.width == g.width && f.depth == g.depth &&
        std::fabs(f.doorY - doorPos.y) < 0.5f && f.linksUsed == useJumpLinks)
        return f.valid;

    if (f.cap < total) {
        u8*  gd = static_cast<u8*>(std::realloc(f.dir, total));
        if (!gd) { f.valid = false; return false; }
        f.dir = gd;
        u16* gt = static_cast<u16*>(std::realloc(f.dist, total * sizeof(u16)));
        if (!gt) { f.valid = false; return false; }   // f.dir stays owned/freed normally
        f.dist = gt; f.cap = total;
    }
    f.width = g.width; f.depth = g.depth; f.stamp = stamp; f.doorY = doorPos.y;
    f.linksUsed = useJumpLinks;
    std::memset(f.dir, 0xFF, total);                    // 0xFF = unreachable
    for (u32 i = 0; i < total; i++) f.dist[i] = 0xFFFF;

    auto idx = [&](u32 x, u32 z, u8 s) -> u32 { return s * cells + z * g.width + x; };

    // Resolve the recorded jump links to (upper) node indices once. A link only becomes an edge when
    // both lips are real walkable upper nodes — a malformed record degrades to "no edge", never to a
    // route through geometry that does not exist.
    u32 lipNode[MAX_JUMP_LINKS][2];
    u8  linkCount = 0;
    if (useJumpLinks) {
        for (u8 k = 0; k < dg.jumpLinkCount && k < MAX_JUMP_LINKS; k++) {
            u32 ax, az, bx, bz;
            if (!LevelGridSystem::worldToGrid(g, dg.jumpLinks[k].a, ax, az)) continue;
            if (!LevelGridSystem::worldToGrid(g, dg.jumpLinks[k].b, bx, bz)) continue;
            if (!nodeWalkable(g, ax, az, 1) || !nodeWalkable(g, bx, bz, 1)) continue;
            lipNode[linkCount][0] = idx(ax, az, 1);
            lipNode[linkCount][1] = idx(bx, bz, 1);
            linkCount++;
        }
    }

    // Scratch: an SPFA work ring + in-queue flags. Costs are non-negative and tiny (1 and kJumpCost),
    // so SPFA converges in a handful of sweeps; the in-queue flag bounds occupancy at `total`, so the
    // ring never overflows. Heap scratch at rebuild time only (~3 rebuilds per floor), freed on exit —
    // the same discipline as the other fields' queues.
    u32* ring = static_cast<u32*>(std::malloc(sizeof(u32) * total));
    u8*  inQ  = static_cast<u8*>(std::calloc(total, 1));
    if (!ring || !inQ) { std::free(ring); std::free(inQ); f.valid = false; return false; }
    u32 head = 0, count = 0;
    auto push = [&](u32 n) {
        if (inQ[n]) return;
        ring[(head + count) % total] = n; count++; inQ[n] = 1;
    };

    // --- Seed: the door's own (cell, story). The door story is upper iff it sits above the ground. ---
    u32 dx, dz;
    if (!LevelGridSystem::worldToGrid(g, doorPos, dx, dz)) {
        std::free(ring); std::free(inQ); f.valid = false; return false;
    }
    const u8 doorStory = (doorPos.y > 1.5f) ? 1 : 0;
    if (!nodeWalkable(g, dx, dz, doorStory)) {
        // The door cell should always be walkable at its story; if some geometry edge case says
        // otherwise, fail soft — the caller keeps the flat flow field.
        std::free(ring); std::free(inQ); f.valid = false; return false;
    }
    f.dir[idx(dx, dz, doorStory)]  = 0xFE;
    f.dist[idx(dx, dz, doorStory)] = 0;
    push(idx(dx, dz, doorStory));

    // --- Relax outward over (cell, story) nodes. Same adjacency as ever (4-connected, both stories,
    // height-continuous within PLATFORM_STEP_TOLERANCE — what models mounting a ramp), plus the jump
    // links as upper-to-upper edges. dir[n] points from n back toward the door (reverse of the step
    // that reached it); a node reached ACROSS a link gets the jump code instead of a direction.
    while (count > 0) {
        const u32 node = ring[head % total]; head++; count--; inQ[node] = 0;
        const u8  s    = static_cast<u8>(node / cells);
        const u32 rem  = node % cells;
        const u32 cx   = rem % g.width, cz = rem / g.width;
        const f32 h    = nodeHeight(g, cx, cz, s);
        const u16 nd   = static_cast<u16>(f.dist[node] + 1);

        for (u8 dir = 0; dir < 8; dir += 2) {
            const s32 nx = static_cast<s32>(cx) + kDx[dir];
            const s32 nz = static_cast<s32>(cz) + kDz[dir];
            if (nx < 0 || nz < 0 ||
                static_cast<u32>(nx) >= g.width || static_cast<u32>(nz) >= g.depth) continue;
            const u32 ux = static_cast<u32>(nx), uz = static_cast<u32>(nz);
            for (u8 ns = 0; ns < 2; ns++) {
                if (!nodeWalkable(g, ux, uz, ns)) continue;
                if (std::fabs(h - nodeHeight(g, ux, uz, ns)) > PLATFORM_STEP_TOLERANCE) continue;
                const u32 nIdx = idx(ux, uz, ns);
                if (nd >= f.dist[nIdx]) continue;
                f.dist[nIdx] = nd;
                f.dir[nIdx]  = static_cast<u8>((dir + 4) & 7);   // back toward `node`
                push(nIdx);
            }
        }
        if (s == 1) {
            const u16 jd = static_cast<u16>(f.dist[node] + kJumpCost);
            for (u8 k = 0; k < linkCount; k++) {
                u32 other;
                if      (node == lipNode[k][0]) other = lipNode[k][1];
                else if (node == lipNode[k][1]) other = lipNode[k][0];
                else continue;
                if (jd >= f.dist[other]) continue;
                f.dist[other] = jd;
                f.dir[other]  = 0xFD;                            // route from `other` = jump this link
                push(other);
            }
        }
    }

    std::free(ring); std::free(inQ);
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
    if (dir >= 8) return {0, 0, 0};             // 0xFD jump / 0xFE at the door / 0xFF unreachable

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
    std::free(f.dist);
    f.dir = nullptr; f.dist = nullptr; f.cap = 0; f.valid = false;
    f.width = f.depth = 0; f.stamp = 0xFFFFFFFFu; f.doorY = 0.0f; f.linksUsed = false;
}

// ---------------------------------------------------------------------------------------------
// Node readout + follower.

VHallStep vhallNextStep(const VHallField& f, const LevelGrid& g, const DungeonResult& dg, Vec3 pos) {
    VHallStep out;
    if (!f.valid || !f.dir || !f.dist) return out;
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) return out;
    if (gx >= f.width || gz >= f.depth) return out;
    const u32 cells  = f.width * f.depth;
    const u8  s      = botStory(g, gx, gz, pos.y);
    const u32 botIdx = s * cells + gz * f.width + gx;
    out.distHere = f.dist[botIdx];
    const u8 d = f.dir[botIdx];
    if (d == 0xFE || d == 0xFF) return out;              // at the door / unreachable: nothing to commit

    if (d == 0xFD) {
        // The route from here is a JUMP: this cell is a lip of one of the recorded links; the
        // committed node is the OTHER lip.
        for (u8 k = 0; k < dg.jumpLinkCount && k < MAX_JUMP_LINKS; k++) {
            u32 ax, az, bx, bz;
            if (!LevelGridSystem::worldToGrid(g, dg.jumpLinks[k].a, ax, az)) continue;
            if (!LevelGridSystem::worldToGrid(g, dg.jumpLinks[k].b, bx, bz)) continue;
            if (az != gz && bz != gz) continue;
            u32 lx, lz;
            if      (ax == gx && az == gz) { lx = bx; lz = bz; }
            else if (bx == gx && bz == gz) { lx = ax; lz = az; }
            else continue;
            out.valid = true; out.jump = true;
            out.x = lx; out.z = lz; out.story = 1;
            out.target = { (lx + 0.5f) * g.cellSize, nodeHeight(g, lx, lz, 1), (lz + 0.5f) * g.cellSize };
            return out;
        }
        return out;                                      // stale jump code with no matching link: no step
    }

    // Ordinary step. The dir byte stores only the CELL; the target STORY is re-derived by the same
    // adjacency rule the build used (walkable + height-continuous). At most one story qualifies for
    // any real geometry (a ground node under a low slab is excluded as unwalkable, and a slab a body
    // could step onto from here is by definition within the tolerance while the other story is not);
    // if both ever qualify, the one the route actually descends through — the smaller dist — wins.
    const s32 nx = static_cast<s32>(gx) + kDx[d];
    const s32 nz = static_cast<s32>(gz) + kDz[d];
    if (nx < 0 || nz < 0 ||
        static_cast<u32>(nx) >= g.width || static_cast<u32>(nz) >= g.depth) return out;
    const u32 ux = static_cast<u32>(nx), uz = static_cast<u32>(nz);
    const f32 h  = nodeHeight(g, gx, gz, s);
    u16 bestDist = 0xFFFF; s8 bestS = -1;
    for (u8 ns = 0; ns < 2; ns++) {
        if (!nodeWalkable(g, ux, uz, ns)) continue;
        if (std::fabs(h - nodeHeight(g, ux, uz, ns)) > PLATFORM_STEP_TOLERANCE) continue;
        const u16 nd = f.dist[ns * cells + uz * f.width + ux];
        if (nd < bestDist) { bestDist = nd; bestS = static_cast<s8>(ns); }
    }
    if (bestS < 0) return out;
    out.valid = true;
    out.x = ux; out.z = uz; out.story = static_cast<u8>(bestS);
    out.target = { (ux + 0.5f) * g.cellSize, nodeHeight(g, ux, uz, out.story), (uz + 0.5f) * g.cellSize };
    return out;
}

// How far off the committed jump axis the REAL facing may be at takeoff. The aim is EASED (it lags
// the desired yaw by up to ~0.4 s), and the feet decompose on the CURRENT facing — a press before
// the turn completes launches diagonally into the void. ~20 degrees.
static constexpr f32 kJumpAlignCos = 0.94f;
// A JUMP_ALIGN that has not taken off in this long is blocked (an enemy body on the lip, a shove
// war) — release and let the ordinary breakoff/escape machinery take its turn...
static constexpr f32 kJumpAlignTimeout = 2.5f;
// ...and do not re-commit a jump for this long afterwards, or the re-pick would re-enter
// JUMP_ALIGN on the very next tick and the "turn" never happens.
static constexpr f32 kJumpRetrySec = 1.5f;

VHallFollowOut vhallFollowTick(VHallFollow& fw, const VHallField& f, const LevelGrid& g,
                               const DungeonResult& dg, Vec3 pos, f32 feetY, f32 yaw,
                               bool onGround, f32 dt) {
    VHallFollowOut out;
    fw.modeT += dt;
    if (fw.jumpCd > 0.0f) fw.jumpCd -= dt;
    if (!f.valid || !f.dir || !f.dist) { fw = VHallFollow{}; return out; }
    u32 gx, gz;
    if (!LevelGridSystem::worldToGrid(g, pos, gx, gz)) { fw = VHallFollow{}; return out; }
    const u32 cells  = f.width * f.depth;
    const u8  s      = botStory(g, gx, gz, feetY);
    const u32 botIdx = s * cells + gz * f.width + gx;
    out.distHere = f.dist[botIdx];

    // --- AIRBORNE DURING A JUMP: steer at the landing and touch NOTHING else. The story read over
    // the gap cells says GROUND, so any release/re-pick here would swap the route graph under the
    // bot mid-arc — the exact silent-redirect failure the follower exists to kill. The landing (or
    // the miss) is adjudicated on the first grounded tick.
    if ((fw.mode == VHF_JUMP_ALIGN || fw.mode == VHF_JUMP_AIR) && !onGround) {
        fw.mode = VHF_JUMP_AIR;
        const Vec3 t{ (fw.x + 0.5f) * g.cellSize, 0.0f, (fw.z + 0.5f) * g.cellSize };
        const f32 ddx = t.x - pos.x, ddz = t.z - pos.z;
        const f32 len = std::sqrt(ddx * ddx + ddz * ddz);
        if (len > 0.05f) out.dir = { ddx / len, 0.0f, ddz / len };
        out.jumpDir = out.dir;
        return out;                                      // wantJump deliberately FALSE: a held press
    }                                                    // re-fires on the landing frame (pogo)

    // --- GROUNDED. A finished arc (successful or missed) releases wholesale: the next pick reads
    // the field from wherever the body actually is, which is the self-healing property.
    if (fw.mode == VHF_JUMP_AIR) fw = VHallFollow{};

    // Release checks on an active commitment.
    if (fw.x >= 0) {
        const bool arrived = (gx == static_cast<u32>(fw.x) && gz == static_cast<u32>(fw.z) &&
                              s == static_cast<u8>(fw.story));
        const u32 cIdx = static_cast<u8>(fw.story) * cells +
                         static_cast<u32>(fw.z) * f.width + static_cast<u32>(fw.x);
        const u16 cDist = f.dist[cIdx];
        const f32 cx = (fw.x + 0.5f) * g.cellSize, cz = (fw.z + 0.5f) * g.cellSize;
        const f32 dxc = pos.x - cx, dzc = pos.z - cz;
        const bool dead   = (cDist == 0xFFFF);
        const bool storyChanged = (s != static_cast<u8>(fw.fromStory));
        // The leash does NOT apply to a jump commitment: its target (the landing) is intrinsically
        // ~3 m away — farther than the leash — so leashing it released and re-picked the SAME jump
        // every tick, which reset modeT and made the blocked-jump timeout unreachable (caught by the
        // truth-table test). A displaced jumper is caught by slidOffLip instead: the geometric
        // premise of the press is "standing on a lip node", not "near the landing".
        const bool leashed = (fw.mode == VHF_WALK) &&
                             (dxc * dxc + dzc * dzc) > (2.5f * 2.5f);          // combat dragged it away
        // Already closer to the door than the commitment by more than the step it represents —
        // shoved PAST it. Never walk backwards to honour a stale latch.
        const bool advanced = (f.dist[botIdx] != 0xFFFF) && (cDist != 0xFFFF) &&
                              (static_cast<u32>(f.dist[botIdx]) + 1 < cDist);
        // A JUMP commitment additionally requires still standing on a lip node: slid off it
        // (grounded, no takeoff) means the geometric premise of the press is gone.
        const bool slidOffLip = (fw.mode == VHF_JUMP_ALIGN) && (f.dir[botIdx] != 0xFD);
        const bool jumpTimeout = (fw.mode == VHF_JUMP_ALIGN) && (fw.modeT > kJumpAlignTimeout);
        if (jumpTimeout) fw.jumpCd = kJumpRetrySec;
        if (arrived || dead || storyChanged || leashed || advanced || slidOffLip || jumpTimeout) {
            const f32 keepCd = fw.jumpCd;   // the backoff must survive the release it triggers
            fw = VHallFollow{};
            fw.jumpCd = keepCd;
        }
    }

    // Pick a fresh commitment.
    if (fw.x < 0) {
        const VHallStep st = vhallNextStep(f, g, dg, pos);
        if (!st.valid) return out;                       // at the door / unreachable: no heading
        if (st.jump && fw.jumpCd > 0.0f) return out;     // blocked-jump backoff: give the escape a turn
        fw.x = static_cast<s16>(st.x); fw.z = static_cast<s16>(st.z);
        fw.story = static_cast<s8>(st.story);
        fw.fromStory = static_cast<s8>(s);
        fw.mode  = st.jump ? VHF_JUMP_ALIGN : VHF_WALK;
        fw.modeT = 0.0f;
        fw.link  = -1;                                   // resolved below for jumps
    }

    // Steer at the committed node's centre (the point servo).
    const Vec3 t{ (fw.x + 0.5f) * g.cellSize,
                  nodeHeight(g, static_cast<u32>(fw.x), static_cast<u32>(fw.z), static_cast<u8>(fw.story)),
                  (fw.z + 0.5f) * g.cellSize };
    const f32 ddx = t.x - pos.x, ddz = t.z - pos.z;
    const f32 len = std::sqrt(ddx * ddx + ddz * ddz);
    if (len > 0.05f) out.dir = { ddx / len, 0.0f, ddz / len };

    if (fw.mode == VHF_JUMP_ALIGN) {
        out.jumpDir = out.dir;
        // Press only when the takeoff is REAL: standing on the lip node, facing where the feet will
        // push (the eased aim has arrived), and the pure vault geometry agrees there is a landing.
        // The press is held across the grounded ticks crossing the lip at speed — the driver's
        // grounded gate turns the hold into one launch on the first grounded frame.
        const Vec3 facing{ -std::sin(yaw), 0.0f, -std::cos(yaw) };
        const bool aligned = (facing.x * out.jumpDir.x + facing.z * out.jumpDir.z) > kJumpAlignCos;
        if (onGround && aligned && f.dir[botIdx] == 0xFD &&
            StoryNav::planVault(g, pos, feetY, out.jumpDir).viable)
            out.wantJump = true;
    }
    return out;
}

} // namespace Autoplay
