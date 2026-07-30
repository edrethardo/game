// test_vertical_hall.cpp — VERTICAL_HALL ("Stacked Loop") layout invariants. The style must never
// collapse to the BSP fallback (roomCount >= 5), must carry real two-story content (the CELL_PLATFORM
// balcony ring + the four graduated-slab pinwheel-ramp StoryPortals for the cross-story chase), must
// keep the central VOID genuinely open (far more bare floor than slab, or there's no atrium to look
// down into), must give a reachable spawn/exit on OPPOSITE stories — and, since the interior-wall
// pass, must carry real ground-story WALLS without ever sealing an area off the loop. The FEEL (the
// drop into the void, sniper fire from the balcony, the catwalk jump, fighting through the doorways)
// is a playtest concern; these pin the structure so a regression can't silently ship a flat
// parking-lot, an unreachable exit, or a ramp that ends mid-air.
//
// Runs at BOTH 44 (the original size, kept as the migration baseline) and 52 (what startGame forces
// live). The 52 run is what caught the VH_RAMP bug: a fixed 12-cell ramp was sized for 44's 14-cell
// bands, so on 52's 16-18 cell bands every ramp top hung 2-4 cells short of its balcony.

#include <doctest/doctest.h>
#include "world/level_gen.h"
#include "world/level_grid.h"
#include "world/collision.h"   // STEP_UP_HEIGHT — the walk-up threshold the ramp gradient must respect
#include <algorithm>

#include <cmath>
#include <vector>

static u32 countFlag(const LevelGrid& g, u8 flag) {
    u32 n = 0;
    for (u32 i = 0; i < g.width * g.depth; i++)
        if (g.cells[i].flags & flag) n++;
    return n;
}

// A cell counts as walkable AT a story when its effective floor for feet at storyY is within tol of
// storyY. Ground uses the step tolerance (a ramp FOOT cell reads 0.25 m); the 3 m story uses a tight
// 0.2 so a BFS can't leak down a ramp's 2.75 m cell and pretend the descent is balcony.
static bool onStory(const LevelGrid& g, u32 x, u32 z, f32 storyY, f32 tol) {
    if (LevelGridSystem::isSolid(g, x, z)) return false;
    return std::fabs(LevelGridSystem::effectiveFloorHeight(g, x, z, storyY) - storyY) < tol;
}

// 4-connected flood over cells on a story; returns region size, optionally the visited mask.
static u32 floodStory(const LevelGrid& g, u32 sx, u32 sz, f32 storyY, f32 tol,
                      std::vector<u8>* visitedOut = nullptr) {
    std::vector<u8>  vis(g.width * g.depth, 0);
    std::vector<u32> stack;
    if (!onStory(g, sx, sz, storyY, tol)) return 0;
    vis[sz * g.width + sx] = 1;
    stack.push_back(sz * g.width + sx);
    u32 n = 0;
    while (!stack.empty()) {
        const u32 cur = stack.back(); stack.pop_back(); n++;
        const u32 cx = cur % g.width, cz = cur / g.width;
        const s32 dx[4] = {1, -1, 0, 0}, dz[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
            const s32 nx = (s32)cx + dx[k], nz = (s32)cz + dz[k];
            if (nx < 0 || nz < 0 || nx >= (s32)g.width || nz >= (s32)g.depth) continue;
            const u32 ni = (u32)nz * g.width + (u32)nx;
            if (vis[ni]) continue;
            if (!onStory(g, (u32)nx, (u32)nz, storyY, tol)) continue;
            vis[ni] = 1;
            stack.push_back(ni);
        }
    }
    if (visitedOut) *visitedOut = vis;
    return n;
}

TEST_CASE("VERTICAL_HALL generates a valid two-story floor across many seeds and both grid sizes") {
    for (u32 N : {44u, 52u}) {
        for (u32 seed = 1; seed <= 64; seed++) {
            CAPTURE(N); CAPTURE(seed);
            LevelGrid g;
            LevelGridSystem::init(g, N, N, 1.0f);
            DungeonResult r = LevelGen::generate(g, seed, N, N, LevelGen::LayoutStyle::VERTICAL_HALL);

            CHECK(r.roomCount >= 5);                     // never falls back to BSP (the Stacked Loop has 9 areas)
            CHECK(countFlag(g, CELL_PLATFORM) > 0);      // the four balconies + catwalks + pinwheel ramps
            CHECK(r.portalCount == 4);                   // the four pinwheel ramps, for the cross-story chase
            CHECK(r.spawnRoomIdx != r.exitRoomIdx);      // spawn (a corner) and exit (a far balcony) differ
            CHECK(lengthSq(r.spawnBalconyPos) > 0.0f);   // both are explicit positions
            CHECK(lengthSq(r.exitBalconyPos) > 0.0f);
            // Entrance is on the opposite STORY from the exit: exactly one sits on the balcony (y≈3m).
            CHECK((r.spawnBalconyPos.y > 1.0f) != (r.exitBalconyPos.y > 1.0f));
            // The central VOID must be genuinely OPEN — far more bare floor than balcony slab, or there's
            // no void to look down into / drop through. (Balcony cells carry BOTH CELL_FLOOR and
            // CELL_PLATFORM, so the open ground is the CELL_FLOOR-without-slab remainder.) This is also
            // the upper bound on the interior-wall pass: walls thin sightlines, they don't fill the floor.
            u32 slabCells = countFlag(g, CELL_PLATFORM), floorCells = countFlag(g, CELL_FLOOR);
            CHECK(floorCells > slabCells);               // the void + the arcades under the balconies dominate

            // INTERIOR WALLS exist: the doorway walls on the 8 corner↔arcade seams plus the cover runs
            // in the corners/void. Pillars alone were ~44 cells; the wall pass sits well above 100.
            {
                u32 innerSolid = 0;
                for (u32 z = 1; z < N - 1; z++)
                    for (u32 x = 1; x < N - 1; x++)
                        if (LevelGridSystem::getCell(g, x, z).flags & CELL_SOLID) innerSolid++;
                CHECK(innerSolid >= 100);
            }

            // ONE jump-pad, spawn-side. Two pads under the catwalk crossing were the floor's biggest
            // shortcut (pad → catwalk → exit balcony in seconds); the single pad is recovery, not a taxi.
            CHECK(countFlag(g, CELL_JUMPPAD) == 1);

            // Every RAMP TOP must open onto a real balcony at the 3 m story: flood from the top over
            // cells whose 3 m surface is intact and require a region far bigger than the ramp's own flat
            // tail. This is the guard that fails when a ramp ends mid-air short of its balcony (the
            // 52-grid VH_RAMP bug — the region was the 2-6 tail cells, not the 200+ cell balcony).
            for (u8 p = 0; p < r.portalCount; p++) {
                u32 tx, tz;
                REQUIRE(LevelGridSystem::worldToGrid(g, r.portals[p].highPos, tx, tz));
                CHECK(floodStory(g, tx, tz, 3.0f, 0.2f) >= 40);
            }

            // GROUND LOOP connectivity: from the ground endpoint, the ground story must reach every
            // area's centre AND every ramp foot — the guard that the wall pass (doors + slab-guard
            // archways) can never seal an area, an exit, or the chase-up route off the loop.
            {
                const Vec3 down = (r.spawnBalconyPos.y > 1.0f) ? r.exitBalconyPos : r.spawnBalconyPos;
                u32 dx0, dz0;
                REQUIRE(LevelGridSystem::worldToGrid(g, down, dx0, dz0));
                std::vector<u8> vis;
                CHECK(floodStory(g, dx0, dz0, 0.0f, PLATFORM_STEP_TOLERANCE, &vis) > 0);
                for (u32 i = 0; i < r.roomCount; i++) {
                    const DungeonRoom& rm = r.rooms[i];
                    const u32 cx = rm.x + rm.w / 2, cz = rm.z + rm.d / 2;
                    CAPTURE(i);
                    CHECK(vis[cz * g.width + cx] == 1);
                }
                for (u8 p = 0; p < r.portalCount; p++) {
                    u32 fx, fz;
                    REQUIRE(LevelGridSystem::worldToGrid(g, r.portals[p].lowPos, fx, fz));
                    CAPTURE((int)p);
                    CHECK(vis[fz * g.width + fx] == 1);
                }
            }

            // The DIAGONAL-service rule — the fix for "it's a 5 second walk". The exit-side balcony must
            // be served by the ramp whose FOOT is the far corner: the portal nearest the balcony endpoint
            // (its top) must have its foot on the OPPOSITE side of the floor from the ground endpoint, in
            // BOTH axes. A same-side ramp foot is the one-band shortcut this rule forbids.
            {
                const Vec3 up   = (r.spawnBalconyPos.y > 1.0f) ? r.spawnBalconyPos : r.exitBalconyPos;
                const Vec3 down = (r.spawnBalconyPos.y > 1.0f) ? r.exitBalconyPos  : r.spawnBalconyPos;
                f32 bestD2 = 1e30f; Vec3 foot{};
                for (u8 p = 0; p < r.portalCount; p++) {
                    const f32 dx = r.portals[p].highPos.x - up.x, dz = r.portals[p].highPos.z - up.z;
                    const f32 d2 = dx * dx + dz * dz;
                    if (d2 < bestD2) { bestD2 = d2; foot = r.portals[p].lowPos; }
                }
                const f32 mid = (f32)N * 0.5f;   // grid centre in world units (cellSize 1)
                CHECK(((down.x < mid) != (foot.x < mid)));   // foot across the centre in X…
                CHECK(((down.z < mid) != (foot.z < mid)));   // …and in Z: the diagonal corner
            }

            LevelGridSystem::shutdown(g);
        }
    }
}

TEST_CASE("VERTICAL_HALL falls back to BSP on a grid too small for the wide two-story layout") {
    LevelGrid g;
    LevelGridSystem::init(g, 32, 32, 1.0f);
    DungeonResult r = LevelGen::generate(g, 12345u, 32, 32, LevelGen::LayoutStyle::VERTICAL_HALL);
    CHECK(r.roomCount >= 5);                         // BSP fallback still yields a playable floor
    CHECK(r.portalCount == 0);                       // no story portals — it's a plain BSP floor
    LevelGridSystem::shutdown(g);
}

// Migration guard (Part A foundation, spec 3.7): every legacy single-slab writer routes through
// setPlatform (replace-to-single), so no shipped cell may carry more than one slab. The determinism
// memcmp can't catch an accidental addPlatform (accumulate) — this test is the guard that can.
TEST_CASE("Migration: generated VERTICAL_HALL cells never carry more than one slab") {
    for (u32 N : {44u, 52u}) {
        for (u32 seed = 1; seed <= 16; seed++) {
            LevelGrid g;
            LevelGridSystem::init(g, N, N, 1.0f);
            LevelGen::generate(g, seed, N, N, LevelGen::LayoutStyle::VERTICAL_HALL);
            for (u32 i = 0; i < g.width * g.depth; i++)
                CHECK(g.cells[i].platCount <= 1);
            LevelGridSystem::shutdown(g);
        }
    }
}

TEST_CASE("Migration: setPlatform collapses an overlapping-band double-write to one slab (arena pattern)") {
    // The arena writes a perimeter band at qu12, then a corner ramp/stairwell overwrites the same cell
    // at another height — the junction that would fabricate a phantom slab under addPlatform.
    GridCell c = {};
    c.flags = CELL_FLOOR;
    LevelGridSystem::setPlatform(c, 12, 4);   // perimeter band
    LevelGridSystem::setPlatform(c, 8, 4);    // ramp/stairwell overwrites the junction cell
    CHECK(c.platCount == 1);
    CHECK(c.platHeight[0] == 8);              // last write wins — no accumulation
    CHECK(c.platHeight[1] == 0);
}

// EVERY RAMP STEP MUST BE WALKABLE. The pinwheel ramps are the only route from the ground story to
// the balconies, and `ramp()` raises the graduated slab by exactly 1 qu (0.25 m) per cell — half the
// 0.4 m STEP_UP_HEIGHT — so a body walks the whole climb without jumping.
//
// This is pinned because the project spent a long time believing the opposite. CLAUDE.md asserted
// that "a ramp's final step onto the balcony is ~0.5 m and STEP_UP_HEIGHT is 0.4, so that step is
// unwalkable by construction and a hop is the only way up", and an Autoplay fix (the vhClimbing
// margin) was built on that premise. Dumping the real slab profile showed every step is 0.25 m and
// no ramp cell is pinned under a slab: the bots stalling at y=2.50 were NOT failing an unwalkable
// riser. A wrong belief about geometry sent two fixes to the wrong place, so the geometry now states
// itself in a test — if someone later makes the climb genuinely unwalkable, this fails rather than
// quietly resurrecting that theory.
TEST_CASE("VERTICAL_HALL ramps are walkable end to end (no step exceeds STEP_UP_HEIGHT)") {
    for (u32 N : {44u, 52u}) {
        for (u32 seed = 1; seed <= 12; seed++) {
            LevelGrid g;
            LevelGridSystem::init(g, N, N, 2.0f);
            const DungeonResult r = LevelGen::generate(g, seed, N, N,
                                                       LevelGen::LayoutStyle::VERTICAL_HALL);
            REQUIRE(r.portalCount > 0);
            for (u8 k = 0; k < r.portalCount; k++) {
                const Vec3 lo = r.portals[k].lowPos, hi = r.portals[k].highPos;
                const s32 x0 = (s32)(lo.x / g.cellSize), z0 = (s32)(lo.z / g.cellSize);
                const s32 x1 = (s32)(hi.x / g.cellSize), z1 = (s32)(hi.z / g.cellSize);
                const s32 dx = (x1 > x0) - (x1 < x0), dz = (z1 > z0) - (z1 < z0);
                const s32 n  = std::max(std::abs(x1 - x0), std::abs(z1 - z0));
                f32 prev = 0.0f;                       // the corner floor the foot abuts
                for (s32 i = 0; i <= n; i++) {
                    const u32 x = (u32)(x0 + dx * i), z = (u32)(z0 + dz * i);
                    f32 top = 0.0f;
                    for (u8 pi = 0; pi < LevelGridSystem::platformCount(g, x, z); pi++)
                        top = std::fmax(top, LevelGridSystem::getPlatformTop(g, x, z, pi));
                    CHECK(top - prev <= STEP_UP_HEIGHT);
                    // ...and the climb must never trap the body under the slab above it.
                    CHECK_FALSE(LevelGridSystem::bodyPinnedUnderSlab(g, x, z, top));
                    prev = top;
                }
                // The top must actually reach the balcony story, not end in mid-air.
                CHECK(prev == doctest::Approx(hi.y));
            }
            LevelGridSystem::shutdown(g);
        }
    }
}

// THE BROKEN CATWALK'S GAP IS RECORDED AS A JUMPABLE EDGE — and the record must agree with the
// geometry it describes. jumpLinks is the ledger the two-story route field reads to treat the gap as
// an edge; if it ever drifts from the carve (wrong cells, wrong height, gap not actually 2 void
// cells) the bot would confidently jump into a wall or a hole that isn't there.
TEST_CASE("VERTICAL_HALL records the broken-catwalk jump links, and they match the geometry") {
    for (u32 N : {44u, 52u}) {
        for (u32 seed = 1; seed <= 12; seed++) {
            LevelGrid g;
            LevelGridSystem::init(g, N, N, 1.0f);
            const DungeonResult r = LevelGen::generate(g, seed, N, N,
                                                       LevelGen::LayoutStyle::VERTICAL_HALL);
            REQUIRE(r.jumpLinkCount == 2);   // one per catwalk row (the catwalk is 2 wide)
            for (u8 k = 0; k < r.jumpLinkCount; k++) {
                const Vec3 a = r.jumpLinks[k].a, b = r.jumpLinks[k].b;
                CHECK(a.y == doctest::Approx(3.0f));
                CHECK(b.y == doctest::Approx(3.0f));
                CHECK(a.z == doctest::Approx(b.z));           // a jump runs along one row
                const u32 ax = (u32)(a.x / g.cellSize), az = (u32)(a.z / g.cellSize);
                const u32 bx = (u32)(b.x / g.cellSize);
                // Both lips are 3 m slab cells...
                CHECK(LevelGridSystem::hasPlatform(g, ax, az));
                CHECK(LevelGridSystem::getPlatformTop(g, ax, az) == doctest::Approx(3.0f));
                CHECK(LevelGridSystem::hasPlatform(g, bx, az));
                CHECK(LevelGridSystem::getPlatformTop(g, bx, az) == doctest::Approx(3.0f));
                // ...separated by EXACTLY the 2-cell void gap, and every between-cell is slab-free
                // (this is the "record can't drift from the carve" pin).
                CHECK(bx - ax == 3);
                for (u32 x = ax + 1; x < bx; x++)
                    CHECK_FALSE(LevelGridSystem::hasPlatform(g, x, az));
            }
            LevelGridSystem::shutdown(g);
        }
    }
}

// THE LINK IS LOAD-BEARING: with it the upper storey is ONE component (any ramp top reaches every
// balcony at 3 m), without it the WEST balcony is ISOLATED. The first half is the property the whole
// navigation rework rests on — "crest any ramp -> walk the upper loop -> door" is only the shortest
// route because the link completes the loop. The second half pins the reason the link exists at all,
// so a future carve change that quietly heals the gap (making the link dead code) fails here and has
// to say so.
TEST_CASE("VERTICAL_HALL upper storey: connected WITH the jump links, W balcony isolated WITHOUT") {
    for (u32 N : {44u, 52u}) {
        for (u32 seed = 1; seed <= 8; seed++) {
            LevelGrid g;
            LevelGridSystem::init(g, N, N, 1.0f);
            const DungeonResult r = LevelGen::generate(g, seed, N, N,
                                                       LevelGen::LayoutStyle::VERTICAL_HALL);
            REQUIRE(r.portalCount == 4);
            REQUIRE(r.jumpLinkCount == 2);

            // 4-connected flood over 3 m slab cells, optionally hopping the recorded links.
            auto flood = [&](u32 sx, u32 sz, bool useLinks, std::vector<u8>& vis) {
                std::vector<std::pair<u32,u32>> q{{sx, sz}};
                vis.assign(g.width * g.depth, 0);
                vis[sz * g.width + sx] = 1;
                auto push = [&](u32 x, u32 z) {
                    if (x >= g.width || z >= g.depth || vis[z * g.width + x]) return;
                    if (!LevelGridSystem::hasPlatform(g, x, z)) return;
                    if (std::fabs(LevelGridSystem::getPlatformTop(g, x, z) - 3.0f) > 0.01f) return;
                    vis[z * g.width + x] = 1; q.push_back({x, z});
                };
                while (!q.empty()) {
                    auto [x, z] = q.back(); q.pop_back();
                    push(x + 1, z); push(x - 1, z); push(x, z + 1); push(x, z - 1);
                    if (useLinks) {
                        for (u8 k = 0; k < r.jumpLinkCount; k++) {
                            const u32 ax = (u32)(r.jumpLinks[k].a.x / g.cellSize),
                                      az = (u32)(r.jumpLinks[k].a.z / g.cellSize),
                                      bx = (u32)(r.jumpLinks[k].b.x / g.cellSize);
                            if (x == ax && z == az) push(bx, az);
                            if (x == bx && z == az) push(ax, az);
                        }
                    }
                }
            };
            // Balcony centres: the ramp TOPS are on the balconies (one per balcony by construction),
            // so "every ramp top reaches every other ramp top" == "the upper storey is one loop".
            std::vector<u8> vis;
            for (u8 from = 0; from < 4; from++) {
                const u32 sx = (u32)(r.portals[from].highPos.x / g.cellSize);
                const u32 sz = (u32)(r.portals[from].highPos.z / g.cellSize);
                flood(sx, sz, /*useLinks=*/true, vis);
                for (u8 to = 0; to < 4; to++) {
                    const u32 tx = (u32)(r.portals[to].highPos.x / g.cellSize);
                    const u32 tz = (u32)(r.portals[to].highPos.z / g.cellSize);
                    CHECK(vis[tz * g.width + tx] == 1);
                }
            }
            // Negative pin: WITHOUT the links, at least one ramp-top pair is unreachable (the W
            // balcony's). Assert the disconnection exists rather than naming W — the carve owns
            // which arm carries the gap, the test only owns that a gap severs the loop.
            u32 disconnected = 0;
            {
                const u32 sx = (u32)(r.portals[0].highPos.x / g.cellSize);
                const u32 sz = (u32)(r.portals[0].highPos.z / g.cellSize);
                flood(sx, sz, /*useLinks=*/false, vis);
                for (u8 to = 0; to < 4; to++) {
                    const u32 tx = (u32)(r.portals[to].highPos.x / g.cellSize);
                    const u32 tz = (u32)(r.portals[to].highPos.z / g.cellSize);
                    if (!vis[tz * g.width + tx]) disconnected++;
                }
            }
            CHECK(disconnected > 0);
            LevelGridSystem::shutdown(g);
        }
    }
}
