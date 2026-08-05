// test_level_preview.cpp — an ASCII previewer for the REAL level generators.
//
// Not a test: an env-gated dump, on the same rails as the balance lab's BALANCE_REPORT case. It
// exists because the only way to look at a generated level used to be launching the game, walking
// around it, and forming an impression — which is slow, non-reproducible, and hopeless for the
// question you actually have while authoring a layout ("is the waypoint anchor inside a rock?").
//
// It runs the SAME LevelGen::generate the engine runs, so what you see is what ships — no second
// implementation to drift.
//
//   LEVEL_PREVIEW=wilderness:1234:52 ./build/tests/dungeon_tests -tc="*level preview*"
//   LEVEL_PREVIEW=cavern:7:48       ./build/tests/dungeon_tests -tc="*level preview*"
//
// Format is <style>:<seed>:<size>; seed and size are optional (defaults 1 and 52). Wrapper with
// nicer output and multi-seed contact sheets: tools/level_preview.py
#include "doctest/doctest.h"
#include "world/level_gen.h"
#include "world/level_grid.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct StyleName { const char* name; LevelGen::LayoutStyle style; };
const StyleName kStyles[] = {
    {"rooms",      LevelGen::LayoutStyle::BSP_ROOMS},
    {"cavern",     LevelGen::LayoutStyle::CAVERN},
    {"gauntlet",   LevelGen::LayoutStyle::GAUNTLET},
    {"hub",        LevelGen::LayoutStyle::HUB},
    {"vertical",   LevelGen::LayoutStyle::VERTICAL_HALL},
    {"descent",    LevelGen::LayoutStyle::FOUR_STORY},
    {"wilderness", LevelGen::LayoutStyle::WILDERNESS},
};

// One glyph per cell, chosen so the shape of a level is readable at a glance in a terminal.
char glyphFor(const GridCell& c) {
    if (c.flags & CELL_SOLID)    return '#';
    if (c.flags & CELL_JUMPPAD)  return '^';
    if (c.flags & CELL_LEDGE)    return 'L';
    if (c.platCount > 0)         return '=';   // a walk-under slab (balcony/catwalk/storey)
    if (c.flags & CELL_LAVA)     return '~';
    if (!(c.flags & CELL_FLOOR)) return '?';   // neither solid nor floor — a generator bug
    // Open ground: mark the roofless cells differently, because "is this outdoors" is invisible
    // otherwise and it is the single property an overworld zone must get right.
    return (c.flags & CELL_CEILING) ? '.' : ' ';
}

} // namespace

TEST_CASE("level preview" * doctest::skip(true)) {
    const char* spec = std::getenv("LEVEL_PREVIEW");
    if (!spec || !*spec) return;

    char buf[128];
    std::snprintf(buf, sizeof buf, "%s", spec);
    char* styleTok = std::strtok(buf, ":");
    char* seedTok  = std::strtok(nullptr, ":");
    char* sizeTok  = std::strtok(nullptr, ":");

    LevelGen::LayoutStyle style = LevelGen::LayoutStyle::WILDERNESS;
    bool known = false;
    for (const StyleName& s : kStyles)
        if (styleTok && std::strcmp(styleTok, s.name) == 0) { style = s.style; known = true; break; }
    if (!known) {
        std::printf("level_preview: unknown style '%s'. Known:", styleTok ? styleTok : "(null)");
        for (const StyleName& s : kStyles) std::printf(" %s", s.name);
        std::printf("\n");
        return;
    }

    const u32 seed = seedTok ? static_cast<u32>(std::strtoul(seedTok, nullptr, 10)) : 1u;
    u32 size = sizeTok ? static_cast<u32>(std::strtoul(sizeTok, nullptr, 10)) : 52u;
    if (size < 16) size = 16;
    if (size > 64) size = 64;   // the minimap/cavern fixed buffers — see zone_def.h

    LevelGrid grid;
    LevelGridSystem::init(grid, size, size, 1.0f);
    const DungeonResult r = LevelGen::generate(grid, seed, size, size, style);

    // Mark room centres over the terrain: 0-9 then a-z. Every placement consumer (enemies, shrines,
    // chests, lights, the boss) works from these, so seeing them ON the map is the point.
    static char overlay[64][64];
    for (u32 z = 0; z < size; z++)
        for (u32 x = 0; x < size; x++) overlay[z][x] = 0;
    for (u32 i = 0; i < r.roomCount && i < 36; i++) {
        const u32 cx = r.rooms[i].x + r.rooms[i].w / 2;
        const u32 cz = r.rooms[i].z + r.rooms[i].d / 2;
        if (cx < size && cz < size) overlay[cz][cx] = (i < 10) ? char('0' + i) : char('a' + (i - 10));
    }

    std::printf("\n=== %s  seed=%u  size=%ux%u  rooms=%u  spawnRoom=%u  exitRoom=%u ===\n",
                LevelGen::styleName(style), seed, size, size,
                r.roomCount, r.spawnRoomIdx, r.exitRoomIdx);
    std::printf("legend: '#' solid  ' ' open sky (no ceiling)  '.' roofed floor  '=' slab  "
                "'^' jump pad  'L' ledge  '~' lava  '?' MALFORMED  0-9/a-z room centre\n");
    for (u32 z = 0; z < size; z++) {
        std::printf("|");
        for (u32 x = 0; x < size; x++)
            std::printf("%c", overlay[z][x] ? overlay[z][x]
                                            : glyphFor(LevelGridSystem::getCell(grid, x, z)));
        std::printf("|\n");
    }

    // A quick census — the numbers you would otherwise squint at the map to estimate.
    u32 solid = 0, open = 0, roofed = 0, malformed = 0;
    for (u32 z = 0; z < size; z++)
        for (u32 x = 0; x < size; x++) {
            const GridCell& c = LevelGridSystem::getCell(grid, x, z);
            if (c.flags & CELL_SOLID) solid++;
            else if (!(c.flags & CELL_FLOOR)) malformed++;
            else if (c.flags & CELL_CEILING) roofed++;
            else open++;
        }
    const u32 cells = size * size;
    std::printf("cells=%u  solid=%u (%u%%)  open-sky=%u  roofed=%u  malformed=%u\n",
                cells, solid, (solid * 100) / cells, open, roofed, malformed);
    if (malformed) std::printf("WARNING: %u cells are neither solid nor floor\n", malformed);
}
