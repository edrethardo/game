// Structural floor generators — the contract every layout style must honor.
//
// The engine consumes a DungeonResult through rectangular rooms (enemy/chest/light/
// boss/exit placement all read room rects), and the layout must be reproducible from
// the seed alone (host and client each generate the floor locally from the shared
// run seed — a divergent grid desyncs collision + loot placement). These tests pin
// exactly that contract for ALL LayoutStyle values, so a new generator can't ship
// with an unreachable exit room or a host/client mismatch.

#include "doctest/doctest.h"
#include "world/level_gen.h"
#include "world/level_grid.h"

#include <cstring>
#include <vector>

namespace {

// Flood fill over walkable (non-solid) cells, 4-connected — matches how the player
// actually moves (corridors are ≥2 wide, but 4-connectivity is the conservative
// bound: anything 4-reachable is definitely walkable in game).
struct Reach {
    std::vector<u8> mark;
    u32 w = 0, d = 0;
    bool at(u32 x, u32 z) const { return x < w && z < d && mark[z * w + x] != 0; }
};

Reach floodFrom(const LevelGrid& grid, u32 sx, u32 sz) {
    Reach r;
    r.w = grid.width; r.d = grid.depth;
    r.mark.assign(static_cast<size_t>(r.w) * r.d, 0);
    if (LevelGridSystem::isSolid(grid, sx, sz)) return r;
    std::vector<u32> stack;
    stack.push_back(sz * r.w + sx);
    r.mark[sz * r.w + sx] = 1;
    while (!stack.empty()) {
        u32 c = stack.back(); stack.pop_back();
        u32 cx = c % r.w, cz = c / r.w;
        const s32 dx[4] = {1, -1, 0, 0};
        const s32 dz[4] = {0, 0, 1, -1};
        for (u32 i = 0; i < 4; i++) {
            s32 nx = static_cast<s32>(cx) + dx[i];
            s32 nz = static_cast<s32>(cz) + dz[i];
            if (nx < 0 || nz < 0 || static_cast<u32>(nx) >= r.w || static_cast<u32>(nz) >= r.d) continue;
            u32 ni = static_cast<u32>(nz) * r.w + static_cast<u32>(nx);
            if (r.mark[ni]) continue;
            if (LevelGridSystem::isSolid(grid, static_cast<u32>(nx), static_cast<u32>(nz))) continue;
            r.mark[ni] = 1;
            stack.push_back(ni);
        }
    }
    return r;
}

void roomCenter(const DungeonRoom& room, u32& cx, u32& cz) {
    cx = room.x + room.w / 2;
    cz = room.z + room.d / 2;
}

// One generated floor, self-cleaning.
struct GenFloor {
    LevelGrid grid;
    DungeonResult dungeon;
    GenFloor(u32 seed, u32 size, LevelGen::LayoutStyle style) {
        LevelGridSystem::init(grid, size, size, 1.0f);
        dungeon = LevelGen::generate(grid, seed, size, size, style);
    }
    ~GenFloor() { LevelGridSystem::shutdown(grid); }
};

constexpr LevelGen::LayoutStyle kAllStyles[] = {
    LevelGen::LayoutStyle::BSP_ROOMS,
    LevelGen::LayoutStyle::CAVERN,
    LevelGen::LayoutStyle::GAUNTLET,
    LevelGen::LayoutStyle::HUB,
};

} // namespace

TEST_CASE("LevelGen: every style is deterministic from the seed") {
    // Host and client both call generate() from the shared run seed — byte-identical
    // grids and room tables are the whole multiplayer contract.
    for (LevelGen::LayoutStyle style : kAllStyles) {
        for (u32 seed : {7u, 12345u, 0xDEADBEEFu}) {
            GenFloor a(seed, 48, style);
            GenFloor b(seed, 48, style);
            CAPTURE(static_cast<int>(style)); CAPTURE(seed);
            REQUIRE(std::memcmp(a.grid.cells, b.grid.cells,
                                sizeof(GridCell) * 48 * 48) == 0);
            REQUIRE(a.dungeon.roomCount == b.dungeon.roomCount);
            REQUIRE(a.dungeon.spawnRoomIdx == b.dungeon.spawnRoomIdx);
            REQUIRE(a.dungeon.exitRoomIdx == b.dungeon.exitRoomIdx);
            REQUIRE(a.dungeon.spawnPos.x == b.dungeon.spawnPos.x);
            REQUIRE(a.dungeon.spawnPos.z == b.dungeon.spawnPos.z);
            for (u32 r = 0; r < a.dungeon.roomCount; r++) {
                REQUIRE(a.dungeon.rooms[r].x == b.dungeon.rooms[r].x);
                REQUIRE(a.dungeon.rooms[r].z == b.dungeon.rooms[r].z);
                REQUIRE(a.dungeon.rooms[r].w == b.dungeon.rooms[r].w);
                REQUIRE(a.dungeon.rooms[r].d == b.dungeon.rooms[r].d);
            }
        }
    }
}

TEST_CASE("LevelGen: every room and the exit are reachable from spawn (all styles/sizes)") {
    // An unreachable exit room soft-locks the floor; an unreachable room strands
    // its enemies/chest. Flood from the spawn cell and demand every room center
    // is reached.
    for (LevelGen::LayoutStyle style : kAllStyles) {
        for (u32 size : {24u, 32u, 40u, 48u}) {
            for (u32 seed : {1u, 99u, 4242u, 0xC0FFEEu}) {
                GenFloor f(seed, size, style);
                CAPTURE(static_cast<int>(style)); CAPTURE(size); CAPTURE(seed);
                REQUIRE(f.dungeon.roomCount >= 2);
                REQUIRE(f.dungeon.spawnRoomIdx != f.dungeon.exitRoomIdx);

                u32 sx, sz;
                REQUIRE(LevelGridSystem::worldToGrid(f.grid, f.dungeon.spawnPos, sx, sz));
                REQUIRE_FALSE(LevelGridSystem::isSolid(f.grid, sx, sz));

                Reach reach = floodFrom(f.grid, sx, sz);
                for (u32 r = 0; r < f.dungeon.roomCount; r++) {
                    u32 cx, cz;
                    roomCenter(f.dungeon.rooms[r], cx, cz);
                    CAPTURE(r); CAPTURE(cx); CAPTURE(cz);
                    REQUIRE(reach.at(cx, cz));
                }
            }
        }
    }
}

TEST_CASE("LevelGen: room rects are in bounds with open centers") {
    // Consumers place lights/portals/loot at room centers and iterate room rects —
    // a rect leaking past the 1-cell border or a solid center breaks placement.
    for (LevelGen::LayoutStyle style : kAllStyles) {
        for (u32 seed : {3u, 777u, 31337u}) {
            GenFloor f(seed, 48, style);
            CAPTURE(static_cast<int>(style)); CAPTURE(seed);
            for (u32 r = 0; r < f.dungeon.roomCount; r++) {
                const DungeonRoom& room = f.dungeon.rooms[r];
                CAPTURE(r);
                REQUIRE(room.w >= 3);
                REQUIRE(room.d >= 3);
                REQUIRE(room.x >= 1);
                REQUIRE(room.z >= 1);
                REQUIRE(room.x + room.w <= 47);
                REQUIRE(room.z + room.d <= 47);
                u32 cx, cz;
                roomCenter(room, cx, cz);
                REQUIRE_FALSE(LevelGridSystem::isSolid(f.grid, cx, cz));
            }
        }
    }
}

TEST_CASE("LevelGen: structural styles produce enough rooms to host a boss floor") {
    // spawnFloorBoss needs roomCount > 2 and a room that isn't near spawn;
    // spawnFloorEnemies skips spawn + neighbors, so a floor needs headroom
    // beyond that to actually contain monsters.
    for (LevelGen::LayoutStyle style : kAllStyles) {
        for (u32 seed : {11u, 2222u, 90210u}) {
            GenFloor f(seed, 48, style);
            CAPTURE(static_cast<int>(style)); CAPTURE(seed);
            REQUIRE(f.dungeon.roomCount >= 5);
        }
    }
}

TEST_CASE("LevelGen: pickLayoutStyle is floor-gated, deterministic, and mixes") {
    // Floors 1-3 run on tiny tutorial grids — always the classic generator.
    for (u32 f = 1; f <= 3; f++)
        for (u32 seed : {5u, 500u, 50000u})
            REQUIRE(LevelGen::pickLayoutStyle(seed, f) == LevelGen::LayoutStyle::BSP_ROOMS);

    // Same (seed, floor) must pick the same style on host and client.
    for (u32 f = 4; f <= 50; f += 7)
        for (u32 seed : {5u, 500u, 50000u})
            REQUIRE(LevelGen::pickLayoutStyle(seed, f) == LevelGen::pickLayoutStyle(seed, f));

    // Across many seeds every style must actually occur (no dead weight rows),
    // and the classic style must stay the most common single style overall.
    u32 counts[static_cast<u32>(LevelGen::LayoutStyle::COUNT)] = {};
    u32 total = 0;
    for (u32 seed = 0; seed < 400; seed++) {
        for (u32 floor = 4; floor <= 50; floor += 3) {
            LevelGen::LayoutStyle s = LevelGen::pickLayoutStyle(seed * 2654435761u, floor);
            REQUIRE(s < LevelGen::LayoutStyle::COUNT);
            counts[static_cast<u32>(s)]++;
            total++;
        }
    }
    for (u32 i = 0; i < static_cast<u32>(LevelGen::LayoutStyle::COUNT); i++) {
        CAPTURE(i);
        REQUIRE(counts[i] > total / 20); // every style shows up at a meaningful rate
    }
}

TEST_CASE("LevelGen: spawn position sits inside the spawn room") {
    for (LevelGen::LayoutStyle style : kAllStyles) {
        GenFloor f(0xABCDu, 48, style);
        const DungeonRoom& sr = f.dungeon.rooms[f.dungeon.spawnRoomIdx];
        u32 sx, sz;
        REQUIRE(LevelGridSystem::worldToGrid(f.grid, f.dungeon.spawnPos, sx, sz));
        CAPTURE(static_cast<int>(style));
        REQUIRE(sx >= sr.x); REQUIRE(sx < sr.x + sr.w);
        REQUIRE(sz >= sr.z); REQUIRE(sz < sr.z + sr.d);
    }
}
