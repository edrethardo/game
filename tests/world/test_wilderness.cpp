// test_wilderness.cpp — the overworld zone terrain (LayoutStyle::WILDERNESS, world/level_gen.cpp).
//
// The failure modes here are all SILENT, which is why they are pinned:
//   * fewer than 5 rooms and generate() swaps the whole style out for BSP — the zone would quietly
//     become a dungeon floor and nobody would see an error;
//   * a room centre inside a rock clump, and every consumer that places enemies/shrines/lights from
//     room centres puts them inside solid rock (VERTICAL_HALL and FOUR_STORY both shipped this bug);
//   * a CELL_CEILING anywhere and the mesher lids the zone — an "outdoor" world with a roof;
//   * a hole in the border ring and the player walks off the edge of the world.
#include "doctest/doctest.h"
#include "world/level_gen.h"
#include "world/level_grid.h"
#include <vector>

namespace {

// Flood from a seed cell over walkable ground; returns the reached set.
std::vector<u8> flood(const LevelGrid& g, u32 sx, u32 sz) {
    std::vector<u8> seen(static_cast<size_t>(g.width) * g.depth, 0);
    if (!LevelGridSystem::isInBounds(g, sx, sz)) return seen;
    std::vector<u32> stack;
    const auto idx = [&](u32 x, u32 z) { return z * g.width + x; };
    if (LevelGridSystem::getCell(g, sx, sz).flags & CELL_SOLID) return seen;
    stack.push_back(idx(sx, sz));
    seen[idx(sx, sz)] = 1;
    while (!stack.empty()) {
        const u32 cur = stack.back(); stack.pop_back();
        const u32 x = cur % g.width, z = cur / g.width;
        const s32 dx[4] = {1, -1, 0, 0}, dz[4] = {0, 0, 1, -1};
        for (u32 d = 0; d < 4; d++) {
            const s32 nx = static_cast<s32>(x) + dx[d], nz = static_cast<s32>(z) + dz[d];
            if (nx < 0 || nz < 0 || nx >= static_cast<s32>(g.width) || nz >= static_cast<s32>(g.depth)) continue;
            const u32 ni = idx(static_cast<u32>(nx), static_cast<u32>(nz));
            if (seen[ni]) continue;
            if (LevelGridSystem::getCell(g, static_cast<u32>(nx), static_cast<u32>(nz)).flags & CELL_SOLID) continue;
            seen[ni] = 1;
            stack.push_back(ni);
        }
    }
    return seen;
}

} // namespace

TEST_CASE("wilderness: well-formed across many seeds and both zone grid sizes") {
    for (u32 size : {44u, 52u}) {
        for (u32 seed = 1; seed <= 64; seed++) {
            LevelGrid grid;
            LevelGridSystem::init(grid, size, size, 1.0f);
            DungeonResult r = LevelGen::generate(grid, seed * 7919u + 13u, size, size,
                                                 LevelGen::LayoutStyle::WILDERNESS);

            // (a) >= 5 rooms, or generate() silently replaced the style with BSP.
            REQUIRE_MESSAGE(r.roomCount >= 5, "seed ", seed, " size ", size,
                            " produced ", r.roomCount, " rooms — the BSP fallback would replace it");

            // (b) Open sky everywhere: an outdoor zone must have NO ceiling cells at all.
            // (c) Border ring fully solid — the generator never opens edges; the caller does.
            u32 ceilCells = 0;
            for (u32 z = 0; z < size; z++) {
                for (u32 x = 0; x < size; x++) {
                    const GridCell& c = LevelGridSystem::getCell(grid, x, z);
                    if (c.flags & CELL_CEILING) ceilCells++;
                    const bool border = (x == 0 || z == 0 || x == size - 1 || z == size - 1);
                    if (border)
                        REQUIRE_MESSAGE((c.flags & CELL_SOLID) != 0, "hole in the border at ", x, ",", z);
                }
            }
            CHECK_MESSAGE(ceilCells == 0, "seed ", seed, " roofed the outdoors (", ceilCells, " cells)");

            // (d) Every room centre is OPEN — the invariant every placement consumer assumes.
            for (u32 i = 0; i < r.roomCount; i++) {
                const u32 cx = r.rooms[i].x + r.rooms[i].w / 2;
                const u32 cz = r.rooms[i].z + r.rooms[i].d / 2;
                REQUIRE(LevelGridSystem::isInBounds(grid, cx, cz));
                CHECK_MESSAGE((LevelGridSystem::getCell(grid, cx, cz).flags & CELL_SOLID) == 0,
                              "seed ", seed, " room ", i, " centre is inside rock");
            }

            // (e) Every room centre is mutually REACHABLE. A clump ring could otherwise fence one
            // off, and a zone with an unreachable region is a zone whose waypoint or POI entrance
            // may be unreachable too.
            const u32 c0x = r.rooms[0].x + r.rooms[0].w / 2;
            const u32 c0z = r.rooms[0].z + r.rooms[0].d / 2;
            const std::vector<u8> seen = flood(grid, c0x, c0z);
            for (u32 i = 1; i < r.roomCount; i++) {
                const u32 cx = r.rooms[i].x + r.rooms[i].w / 2;
                const u32 cz = r.rooms[i].z + r.rooms[i].d / 2;
                CHECK_MESSAGE(seen[cz * size + cx] != 0,
                              "seed ", seed, " room ", i, " is fenced off from room 0");
            }
        }
    }
}

TEST_CASE("wilderness: terrain actually has obstacles (it is not an empty plain)") {
    // An empty field is the worst case for the draw-call budget — nothing occludes anything and the
    // only culling is frustum-based. If the clump scatter ever silently stops emitting, this fails.
    u32 seedsWithCover = 0;
    for (u32 seed = 1; seed <= 32; seed++) {
        LevelGrid grid;
        LevelGridSystem::init(grid, 52, 52, 1.0f);
        LevelGen::generate(grid, seed * 104729u + 7u, 52, 52, LevelGen::LayoutStyle::WILDERNESS);
        u32 interiorSolid = 0;
        for (u32 z = 1; z < 51; z++)
            for (u32 x = 1; x < 51; x++)
                if (LevelGridSystem::getCell(grid, x, z).flags & CELL_SOLID) interiorSolid++;
        if (interiorSolid > 40) seedsWithCover++;
        // ...but it must stay a FIELD, not a maze: heavy fill would make it a cavern with no roof.
        CHECK_MESSAGE(interiorSolid < (50 * 50) / 3, "seed ", seed, " is too dense to read as outdoors");
    }
    CHECK(seedsWithCover >= 30);
}

TEST_CASE("wilderness is deterministic — host and client must carve the same terrain") {
    // Zones replicate as 6 bytes (floor, difficulty, seed); if the carve is not reproducible, a
    // guest walks a different world than the host and nothing reports it.
    for (u32 seed : {1u, 12345u, 999983u}) {
        LevelGrid a, b;
        LevelGridSystem::init(a, 52, 52, 1.0f);
        LevelGridSystem::init(b, 52, 52, 1.0f);
        LevelGen::generate(a, seed, 52, 52, LevelGen::LayoutStyle::WILDERNESS);
        LevelGen::generate(b, seed, 52, 52, LevelGen::LayoutStyle::WILDERNESS);
        for (u32 i = 0; i < 52u * 52u; i++) {
            const u32 x = i % 52, z = i / 52;
            REQUIRE(LevelGridSystem::getCell(a, x, z).flags ==
                    LevelGridSystem::getCell(b, x, z).flags);
        }
    }
}
