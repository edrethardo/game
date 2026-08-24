// engine_arena.cpp — the PvP ARENA (Arena mode, sentinel floor 97).
//
// A deterministic open-sky colosseum where players fight each other: FFA deathmatch,
// first to Arena::KILL_TARGET kills, 3 s auto-respawn (rules: game/arena.h). Built on the
// engine_town.cpp pattern — same sentinel-floor rails (host broadcasts SV_LEVEL_SEED with
// ARENA_SENTINEL_FLOOR, clients build the identical arena), same daylight rendering branch.
//
// The layout is a two-story Quake / Metroid-Prime-Hunters COMBAT HALL (44x44, 4-fold rotational
// symmetry — every spawn corner faces identical geometry):
//   - TIER 0 (ground): the open pit with crate cover, wall-midpoint jump pads, and the covered
//     perimeter ARCADE under the balcony (spawn bays live here, in cover, out of sniper LOS).
//   - TIER 1 (1.5 m): the central tower, reached by four cardinal LEDGE ramps.
//   - TIER 2 (3.0 m): TWO dueling vantages — the perimeter SNIPER BALCONY (CELL_PLATFORM slabs:
//     stand on it, walk under it; open inner edge to drop/fire from; corner slab stairwells and
//     the midpoint pads to get up) and the tower's crown at the same height across the map.
// Verticality rides three opt-in cell flags: CELL_LEDGE (jump-gated risers), CELL_JUMPPAD
// (launch pads), CELL_PLATFORM (walk-under slabs). All are deterministic level geometry built
// from the seed on every peer, so co-op needs NO wire change (posY + onGround are snapshotted).
// No enemies, no loot, no portals spawn here; the exit is the pause menu ("Leave Arena").

#include "engine/engine.h"
#include "platform/input.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "game/game_constants.h"
#include "game/enemy_ai.h"
#include "game/entity.h"     // EntitySystem::spawn — the WB-298 monster interludes
#include <cstring>           // strcmp — monster def lookup by name
#include "renderer/material.h"
#include "renderer/minimap.h"
#include "net/net.h"
#include "net/server.h"
#include "core/log.h"
#include <cmath>
#include <cstdlib>   // getenv — the ARENA_SIZE A/B override; rand — the loot-anchor draw
#include <cstdio>    // snprintf — the loot announce line

// The pure rules assume exactly MAX_PLAYERS combatants — pin it here, at the engine boundary.
static_assert(Arena::MAX_COMBATANTS == MAX_PLAYERS,
              "Arena::MAX_COMBATANTS must track MAX_PLAYERS");

// Arena layout constants — one place, shared by build + spawn placement so they can't drift.
namespace {
    // 36x36 since the bot-evaluation pass (was 44x44 — Aaron: "mach die Arena-Maps kleiner";
    // -33% floor area). EVERY placement below derives from ARENA_W, so the size is one knob:
    // the 44-era literals (tower 19..24, pads at 21/38, columns {10,16,27,33}) were all
    // W-relative facts written out by hand, and shrinking by editing fifteen literals is how
    // a pad ends up inside a ramp.
    // ARENA_SIZE env override (even values 36..44) exists ONLY for the one-binary A/B the
    // bot soak runs — a sequential two-build comparison measures the build, not the map
    // (the project's measured A/B rule). Ship behaviour is the constant below.
    u32 arenaSizeFromEnv() {
        if (const char* e = std::getenv("ARENA_SIZE")) {
            int v = std::atoi(e);
            if (v >= 36 && v <= 44 && (v % 2) == 0) return static_cast<u32>(v);
        }
        return 36;
    }
    const u32 ARENA_W = arenaSizeFromEnv(), ARENA_D = ARENA_W;
    constexpr f32 ARENA_CS = 1.0f;
    const u32 HALF  = ARENA_W / 2;       // tower/pad axis
    const u32 COL_A = ARENA_W / 4 - 1;   // first balcony column (44: 10, 36: 8)
    const u32 COL_B = COL_A + 6;         // second column; mirrors complete the four

    // Yaw that faces `to` from `from` on the XZ plane. Forward is {-sin(yaw), 0, -cos(yaw)}
    // (the engine-wide convention), so yaw = atan2(-dx, -dz).
    f32 yawFromTo(const Vec3& from, const Vec3& to) {
        return std::atan2(-(to.x - from.x), -(to.z - from.z));
    }
}

Vec3 Engine::arenaPad(u8 slot) const { return m_arenaPads[slot % MAX_PLAYERS]; }

// The MAP DISPATCHER. The id rides levelSeed (the value SV_LEVEL_SEED already broadcasts),
// so host and every client carve the same map from the same six bytes — adding maps cost no
// protocol change. Each builder writes m_arenaPads/m_arenaCenter (the maps differ, so the old
// constexpr pad table cannot carry them) and returns the centre it seeded the flow field at.
Vec3 Engine::buildArenaLevel() {
    const u32 mapId = m_level.levelSeed % Arena::MAP_COUNT;
    Vec3 center;
    switch (mapId) {
        case 1:  center = buildArenaCrucible();    break;
        case 2:  center = buildArenaPit();         break;
        case 3:  center = buildArenaMotherboard(); break;
        case 4:  center = buildArenaMainframe();   break;
        default: center = buildArenaCombatHall();  break;
    }
    m_arenaCenter = center;
    LOG_INFO("[ARENA] map %u: %s (%ux%u)", mapId, Arena::mapName(mapId),
             m_level.grid.width, m_level.grid.depth);
    return center;
}

Vec3 Engine::buildArenaCombatHall() {
    LevelGridSystem::init(m_level.grid, ARENA_W, ARENA_D, ARENA_CS);
    // One authored pad (west arcade, beside the first column), the rest by the same quarter
    // turn the geometry uses: (x,z) -> (W - z, x) on cell-centre positions.
    {
        const f32 PAD_X = 1.5f, PAD_Z = COL_A + 0.5f;
        m_arenaPads[0] = {PAD_X,                        0.0f, PAD_Z};
        m_arenaPads[1] = {ARENA_W * ARENA_CS - PAD_Z,   0.0f, PAD_X};
        m_arenaPads[2] = {ARENA_W * ARENA_CS - PAD_X,   0.0f, ARENA_W * ARENA_CS - PAD_Z};
        m_arenaPads[3] = {PAD_Z,                        0.0f, ARENA_W * ARENA_CS - PAD_X};
    }
    // Loot anchors (WB-304): the crown (the contested vantage), the tower top, and the four
    // wall-midpoint pad aprons — never near a spawn bay.
    {
        const f32 H = HALF * ARENA_CS;
        m_arenaLootAnchorCount = 6;
        m_arenaLootAnchors[0] = {H, 3.0f, H};                       // tower crown
        m_arenaLootAnchors[1] = {H, 1.5f, H - 2.5f};                // tower plate, north lip
        m_arenaLootAnchors[2] = {H, 0.0f, 6.5f};                    // north pad apron
        m_arenaLootAnchors[3] = {ARENA_W - 6.5f, 0.0f, H};          // east
        m_arenaLootAnchors[4] = {H, 0.0f, ARENA_D - 6.5f};          // south
        m_arenaLootAnchors[5] = {6.5f, 0.0f, H};                    // west
    }

    u8 sand   = MaterialSystem::getIdByName("arena_sand");
    u8 brick  = MaterialSystem::getIdByName("brick_wall");  // warm perimeter (dark stone reads as void in daylight)
    u8 stone  = MaterialSystem::getIdByName("stone_wall");  // tower / ramp / crown risers
    u8 plank  = MaterialSystem::getIdByName("wood_plank");  // crate clusters
    u8 padMat = MaterialSystem::getIdByName("arena_pad");   // glowing jump-pad tiles

    for (u32 z = 0; z < ARENA_D; z++) {
        for (u32 x = 0; x < ARENA_W; x++) {
            GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
            bool border = (x == 0 || z == 0 || x == ARENA_W - 1 || z == ARENA_D - 1);
            if (border) {
                c.flags = CELL_SOLID;
                c.wallMaterialId = brick;
            } else {
                // NO CELL_CEILING — open sky (the mesher skips the lid, the daylight branch
                // lights it). ceilingHeight doubles as adjacent walls' height: 20 quarter-units
                // = 5 m, tall enough that a balcony jump can't clear the perimeter.
                c.flags = CELL_FLOOR;
                c.floorHeight     = 0;
                c.ceilingHeight   = 20;   // 5 m walls: a balcony jump (3.0 + 0.8 m) cannot clear them
                c.floorMaterialId = sand;
                c.wallMaterialId  = brick;
            }
        }
    }

    // Two-story COMBAT-HALL layout (Quake / Metroid-Prime-Hunters style). Placement is 4-fold
    // rotationally symmetric about the arena centre (the balcony/stairwells/columns via the rotCell
    // helper below, the centre/pads/crates by mirror pairs), so all four spawn corners face
    // identical geometry — no corner is favoured.
    auto solid = [&](u32 sx, u32 sz, u32 w, u32 d, u8 mat) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags = CELL_SOLID;
                c.wallMaterialId = mat;
            }
    };
    // Raise a block into a jump-gated platform: qh = floor height in quarter-units (×0.25 = m),
    // CELL_LEDGE so a body must JUMP or ramp onto it (collision refuses the walk-up — see
    // STEP_UP_HEIGHT). Stone risers, sand top; the mesher draws the riser faces.
    auto raise = [&](u32 sx, u32 sz, u32 w, u32 d, u8 qh) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags = static_cast<u8>(CELL_FLOOR | CELL_LEDGE);
                c.floorHeight     = qh;
                c.floorMaterialId = sand;
                c.wallMaterialId  = stone;   // riser face material
            }
    };
    // Mark a block as JUMP PADS at height qh: standing on one flings you up JUMPPAD_LAUNCH (Quake pad).
    // Glowing tiles so the launch is legible; stone risers under any raised pad ring.
    auto pad = [&](u32 sx, u32 sz, u32 w, u32 d, u8 qh) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags = static_cast<u8>(CELL_FLOOR | CELL_JUMPPAD);
                c.floorHeight     = qh;
                c.floorMaterialId = padMat;
                c.wallMaterialId  = stone;
            }
    };
    // A 2-lane LEDGE STAIRCASE (ramp): `len` cells from (sx,sz) stepping by (dx,dz), height starting
    // at qhTop and dropping ONE quarter-unit per cell — so the inner end meets a tier at qhTop and the
    // outer end meets the pit at qhTop-len+1. 0.25 m steps stay under STEP_UP_HEIGHT, so you climb it
    // from the pit end one step at a time; the LEDGE flag walls off the sides (no snap-up cheese). The
    // second lane is offset by (px,pz).
    auto ramp = [&](u32 sx, u32 sz, s32 dx, s32 dz, s32 px, s32 pz, u8 qhTop, u32 len) {
        for (u32 i = 0; i < len; i++) {
            u8  qh = static_cast<u8>(qhTop - i);
            u32 cx = static_cast<u32>(static_cast<s32>(sx) + dx * static_cast<s32>(i));
            u32 cz = static_cast<u32>(static_cast<s32>(sz) + dz * static_cast<s32>(i));
            raise(cx, cz, 1, 1, qh);
            raise(static_cast<u32>(static_cast<s32>(cx) + px),
                  static_cast<u32>(static_cast<s32>(cz) + pz), 1, 1, qh);
        }
    };

    // Mark a block as PLATFORM SLABS: a second story floating topQ*0.25 m over the cell's normal
    // ground floor, which stays walkable beneath (the arcade). Plank walkway, stone rim faces.
    auto plat = [&](u32 sx, u32 sz, u32 w, u32 d, u8 topQ) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = LevelGridSystem::getCell(m_level.grid, x, z);
                c.flags           = static_cast<u8>(CELL_FLOOR);
                LevelGridSystem::setPlatform(c, topQ, plank);   // authorer: platCount=1, sets CELL_PLATFORM
                c.floorMaterialId = sand;    // the arcade ground beneath stays sand
                c.wallMaterialId  = stone;
            }
    };
    // Rotate a cell k*90° about the arena centre (one turn: (x,z) -> (W-1-z, x)). Building all
    // four corners/walls from ONE template through this guarantees perfect 4-fold symmetry — no
    // spawn corner ever faces different geometry.
    auto rotCell = [&](u32 x, u32 z, u32 k, u32& ox, u32& oz) {
        ox = x; oz = z;
        for (u32 i = 0; i < k; i++) { u32 t = ox; ox = ARENA_W - 1 - oz; oz = t; }
    };

    // --- SECOND STORY: the perimeter SNIPER BALCONY @ 3.0 m (the Combat-Hall signature) --------
    // A 2-cell walkway hugging every wall; open inner edge (drop off / fire into the pit
    // anywhere), covered arcade beneath (underside 2.5 m — 0.7 m of headroom over a body).
    plat(1,  1,           ARENA_W - 2, 2,           12);   // north band (z 1..2)
    plat(1,  ARENA_D - 3,  ARENA_W - 2, 2,           12);   // south band
    plat(1,  3,            2,           ARENA_D - 6, 12);   // west band  (x 1..2)
    plat(ARENA_W - 3, 3,   2,           ARENA_D - 6, 12);   // east band

    // Corner STAIRWELLS: an L-switchback of graduated slabs (0.25 m steps — walkable under
    // STEP_UP_HEIGHT), arcade -> balcony, overwriting band cells. The quiet route up; the pads
    // below are the fast, loud one. Both lanes of each leg step together.
    for (u32 k = 0; k < 4; k++) {
        u32 ox, oz;
        for (u32 i = 0; i < 6; i++)                       // leg A: h 0.25..1.5 m
            for (u32 lane = 1; lane <= 2; lane++) {
                rotCell(8 - i, lane, k, ox, oz);
                plat(ox, oz, 1, 1, static_cast<u8>(1 + i));
            }
        for (u32 cx2 = 1; cx2 <= 2; cx2++)                // corner landing @ 1.5 m
            for (u32 cz2 = 1; cz2 <= 2; cz2++) {
                rotCell(cx2, cz2, k, ox, oz);
                plat(ox, oz, 1, 1, 6);
            }
        for (u32 i = 0; i < 6; i++)                       // leg B: h 1.75..3.0 m, meets the band
            for (u32 lane = 1; lane <= 2; lane++) {
                rotCell(lane, 3 + i, k, ox, oz);
                plat(ox, oz, 1, 1, static_cast<u8>(7 + i));
            }
    }

    // Support COLUMNS: full-height solid pillars on the balcony's inner-edge row — arcade cover
    // below, pillars to strafe around above (the walkway narrows to one cell at each), and the
    // structure that visually carries the slab. Mirror-symmetric pairs (10,33) and (16,27).
    {
        const u32 kColX[4] = {COL_A, COL_B, ARENA_W - 1 - COL_B, ARENA_W - 1 - COL_A};
        for (u32 k = 0; k < 4; k++)
            for (u32 ci = 0; ci < 4; ci++) {
                u32 ox, oz;
                rotCell(kColX[ci], 2, k, ox, oz);
                solid(ox, oz, 1, 1, stone);
            }
    }

    // Wall-midpoint JUMP PADS: pit -> balcony (launch apex 3.6 m; air-steer onto the 3.0 m band
    // edge). Never ON or UNDER a slab — a pad launch must own its full arc.
    pad(HALF - 1, 4, 2, 2, 0);          pad(ARENA_W - 6, HALF - 1, 2, 2, 0);
    pad(HALF - 1, ARENA_D - 6, 2, 2, 0); pad(4, HALF - 1, 2, 2, 0);

    // --- CENTER: tower + crown (solid-riser tiers, as before, shifted to the 44x44 centre).
    // The crown now sits at BALCONY height so the two commanding vantages duel across the map.
    raise(HALF - 3, HALF - 3, 6, 6, 6);    // tower @ 1.5 m, reached by the four ramps
    pad(HALF - 2, HALF - 2, 4, 4, 6);      // crown launch-ring (12 pad cells after the crown overwrite)
    raise(HALF - 1, HALF - 1, 2, 2, 12);   // crown @ 3.0 m — level with the sniper balcony
    ramp(HALF + 3, HALF - 1,  1,  0, 0,  1, 6, 6); // east
    ramp(HALF - 4, HALF - 1, -1,  0, 0,  1, 6, 6); // west
    ramp(HALF - 1, HALF - 4,  0, -1, 1,  0, 6, 6); // north
    ramp(HALF - 1, HALF + 3,  0,  1, 1,  0, 6, 6); // south

    // --- Pit cover: one crate cluster per diagonal quadrant (mirror pairs 10 <-> 32) -----------
    solid(COL_A, COL_A, 2, 2, plank); solid(ARENA_W - 2 - COL_A, COL_A, 2, 2, plank);
    solid(COL_A, ARENA_D - 2 - COL_A, 2, 2, plank); solid(ARENA_W - 2 - COL_A, ARENA_D - 2 - COL_A, 2, 2, plank);

    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid,
                             0xA12E7Au,    // constant seed — deterministic tile shading on every peer
                             m_level.sections, MAX_LEVEL_SECTIONS);
    LevelGridSystem::buildClearanceField(m_level.grid);
    Minimap::init(m_level.grid.width, m_level.grid.depth);
    Vec3 center = {(ARENA_W * 0.5f) * ARENA_CS, 0.0f, (ARENA_D * 0.5f) * ARENA_CS};
    LevelGridSystem::buildFlowField(m_level.grid, center);
    return center;
}

// ---------------------------------------------------------------------------------------------
// THE CRUCIBLE (map 1, 44x44 since WB-302 — "keine kleinen Maps mehr") — a molten LOOP, not a
// star: four spawn causeways feed a square RING ROAD over the lava, the ring's corners widen
// into fighting plateaus with cover, and four inner causeways continue to the centre island
// (pad + 3 m crow's nest). Every route still ends in the middle, but the ring gives the map
// real rotation — you can flank a held island instead of queueing on one bridge. Causeway
// lava gaps stay staggered per lane (jump or slalom, never a dead stop for the bot veto).
Vec3 Engine::buildArenaCrucible() {
    constexpr u32 W = 44; constexpr f32 CS = 1.0f;
    LevelGridSystem::init(m_level.grid, W, W, CS);
    const u8 brick = MaterialSystem::getIdByName("brick_wall");
    const u8 sand  = MaterialSystem::getIdByName("arena_sand");
    const u8 stone = MaterialSystem::getIdByName("stone_wall");
    const u8 lava  = MaterialSystem::getIdByName("hellforge_lava");
    const u8 plank = MaterialSystem::getIdByName("wood_plank");
    const u8 padM  = MaterialSystem::getIdByName("arena_pad");

    auto cellAt = [&](u32 x, u32 z) -> GridCell& { return LevelGridSystem::getCell(m_level.grid, x, z); };
    for (u32 z = 0; z < W; z++)
        for (u32 x = 0; x < W; x++) {
            GridCell& c = cellAt(x, z);
            if (x == 0 || z == 0 || x == W - 1 || z == W - 1) {
                c.flags = CELL_SOLID; c.wallMaterialId = brick;
            } else {
                c.flags = static_cast<u8>(CELL_FLOOR | CELL_LAVA);
                c.floorHeight = 0; c.ceilingHeight = 20;
                c.floorMaterialId = lava; c.wallMaterialId = brick;
            }
        }
    auto stoneCell = [&](u32 x, u32 z) {
        GridCell& c = cellAt(x, z);
        c.flags = CELL_FLOOR; c.floorHeight = 0;
        c.floorMaterialId = sand; c.wallMaterialId = stone;
    };
    auto stoneRect = [&](u32 sx, u32 sz, u32 w, u32 d) {
        for (u32 z = sz; z < sz + d; z++) for (u32 x = sx; x < sx + w; x++) stoneCell(x, z);
    };
    const u32 C = W / 2;   // 22
    // Chebyshev distance from the centre seam (even grid — same measure as The Pit).
    auto cheb = [&](u32 x, u32 z) -> u32 {
        const s32 dx = (2 * (s32)x <= (s32)W - 1) ? ((s32)C - 1 - (s32)x) : ((s32)x - (s32)C);
        const s32 dz = (2 * (s32)z <= (s32)W - 1) ? ((s32)C - 1 - (s32)z) : ((s32)z - (s32)C);
        const s32 d  = (dx > dz) ? dx : dz;
        return static_cast<u32>(d < 0 ? 0 : d);
    };
    // THE RING ROAD: a two-cell square band at radius 12..13.
    for (u32 z = 1; z < W - 1; z++)
        for (u32 x = 1; x < W - 1; x++) {
            const u32 d = cheb(x, z);
            if (d >= 12 && d <= 13) stoneCell(x, z);
        }
    // Ring-corner PLATEAUS (4x4, widened outward) with a cover crate each — the ring's own
    // fighting grounds and loot stops.
    stoneRect(7, 7, 5, 5);           stoneRect(W - 12, 7, 5, 5);
    stoneRect(7, W - 12, 5, 5);      stoneRect(W - 12, W - 12, 5, 5);
    auto crate = [&](u32 x, u32 z) { GridCell& c = cellAt(x, z); c.flags = CELL_SOLID; c.wallMaterialId = plank; };
    crate(8, 8); crate(W - 9, 8); crate(8, W - 9); crate(W - 9, W - 9);
    // Centre island 10x10 + crown furniture.
    stoneRect(C - 5, C - 5, 10, 10);
    crate(C - 4, C - 4); crate(C + 3, C - 4); crate(C - 4, C + 3); crate(C + 3, C + 3);
    // Spawn aprons at the wall midpoints.
    stoneRect(C - 2, 1, 5, 3);  stoneRect(C - 2, W - 4, 5, 3);
    stoneRect(1, C - 2, 3, 5);  stoneRect(W - 4, C - 2, 3, 5);
    // OUTER causeways (apron -> ring) and INNER causeways (ring -> island), two lanes each.
    for (u32 z = 4; z < C - 13; z++)  { stoneCell(C - 1, z); stoneCell(C, z); }
    for (u32 z = C + 14; z < W - 4; z++) { stoneCell(C - 1, z); stoneCell(C, z); }
    for (u32 x = 4; x < C - 13; x++)  { stoneCell(x, C - 1); stoneCell(x, C); }
    for (u32 x = C + 14; x < W - 4; x++) { stoneCell(x, C - 1); stoneCell(x, C); }
    for (u32 z = C - 11; z < C - 5; z++) { stoneCell(C - 1, z); stoneCell(C, z); }
    for (u32 z = C + 5; z < C + 12; z++) { stoneCell(C - 1, z); stoneCell(C, z); }
    for (u32 x = C - 11; x < C - 5; x++) { stoneCell(x, C - 1); stoneCell(x, C); }
    for (u32 x = C + 5; x < C + 12; x++) { stoneCell(x, C - 1); stoneCell(x, C); }
    // The staggered lava gaps: one per lane on every INNER causeway (the contested stretch).
    auto lavaGap = [&](u32 x, u32 z) {
        GridCell& c = cellAt(x, z);
        c.flags = static_cast<u8>(CELL_FLOOR | CELL_LAVA); c.floorMaterialId = lava;
    };
    lavaGap(C - 1, C - 8); lavaGap(C, C - 9);
    lavaGap(C - 1, C + 8); lavaGap(C, C + 7);
    lavaGap(C - 8, C - 1); lavaGap(C - 9, C);
    lavaGap(C + 8, C - 1); lavaGap(C + 7, C);
    // Island centre pad + the 3 m crow's nest on the north edge.
    for (u32 z = C; z <= C + 1; z++)
        for (u32 x = C; x <= C + 1; x++) {
            GridCell& c = cellAt(x, z);
            c.flags = static_cast<u8>(CELL_FLOOR | CELL_JUMPPAD);
            c.floorMaterialId = padM; c.wallMaterialId = stone;
        }
    for (u32 z = C - 4; z <= C - 2; z++)
        for (u32 x = C - 1; x <= C + 1; x++)
            LevelGridSystem::setPlatform(cellAt(x, z), 12, plank);
    // Spawns on the aprons.
    m_arenaPads[0] = {C + 0.5f, 0.0f, 2.0f};
    m_arenaPads[1] = {W - 2.0f, 0.0f, C + 0.5f};
    m_arenaPads[2] = {C + 0.5f, 0.0f, W - 2.0f};
    m_arenaPads[3] = {2.0f,     0.0f, C + 0.5f};
    // Loot anchors: nest, island centre-south, and the four ring plateaus.
    m_arenaLootAnchorCount = 6;
    m_arenaLootAnchors[0] = {C + 0.5f, 3.0f, C - 3.0f};
    m_arenaLootAnchors[1] = {C + 0.5f, 0.0f, C + 3.5f};
    m_arenaLootAnchors[2] = {9.5f, 0.0f, 9.5f};
    m_arenaLootAnchors[3] = {W - 9.5f, 0.0f, 9.5f};
    m_arenaLootAnchors[4] = {W - 9.5f, 0.0f, W - 9.5f};
    m_arenaLootAnchors[5] = {9.5f, 0.0f, W - 9.5f};

    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid, 0xA12E7Bu,
                             m_level.sections, MAX_LEVEL_SECTIONS);
    LevelGridSystem::buildClearanceField(m_level.grid);
    Minimap::init(m_level.grid.width, m_level.grid.depth);
    Vec3 center = {C * CS + 0.5f, 0.0f, C * CS + 0.5f};
    LevelGridSystem::buildFlowField(m_level.grid, center);
    return center;
}

// ---------------------------------------------------------------------------------------------
// THE PIT (map 2, 40x40 since WB-302) — the inverted amphitheatre grown to Quake size, with a
// COVERED ARCADE under the rim deck (the Combat Hall's signature slab trick): three LEDGE tiers
// still fall toward the bowl, but the outer band is now a 3.5 m walkway you stand ON — with a
// 1.5 m-floored tunnel UNDERNEATH that the spawns hide in. Down remains free everywhere; up is
// the corner stairs (bowl->tiers) and the corner stairwells (arcade->deck), or the bowl pads.
// Gravity still funnels the fight; the deck gives snipers a ring and the tunnel gives everyone
// a protected rotation lane.
Vec3 Engine::buildArenaPit() {
    constexpr u32 W = 40; constexpr f32 CS = 1.0f;
    LevelGridSystem::init(m_level.grid, W, W, CS);
    const u8 brick = MaterialSystem::getIdByName("brick_wall");
    const u8 sand  = MaterialSystem::getIdByName("arena_sand");
    const u8 stone = MaterialSystem::getIdByName("stone_wall");
    const u8 plank = MaterialSystem::getIdByName("wood_plank");
    const u8 padM  = MaterialSystem::getIdByName("arena_pad");
    auto cellAt = [&](u32 x, u32 z) -> GridCell& { return LevelGridSystem::getCell(m_level.grid, x, z); };
    auto cheb = [&](u32 x, u32 z) -> u32 {
        const s32 dx = (2 * (s32)x <= (s32)W - 1) ? ((s32)W / 2 - 1 - (s32)x) : ((s32)x - (s32)W / 2);
        const s32 dz = (2 * (s32)z <= (s32)W - 1) ? ((s32)W / 2 - 1 - (s32)z) : ((s32)z - (s32)W / 2);
        const s32 d  = (dx > dz) ? dx : dz;
        return static_cast<u32>(d < 0 ? 0 : d);
    };
    for (u32 z = 0; z < W; z++)
        for (u32 x = 0; x < W; x++) {
            GridCell& c = cellAt(x, z);
            if (x == 0 || z == 0 || x == W - 1 || z == W - 1) {
                c.flags = CELL_SOLID; c.wallMaterialId = brick; continue;
            }
            const u32 d = cheb(x, z);
            c.ceilingHeight = 28;                       // 7 m walls over the 3.5 m deck
            c.floorMaterialId = sand; c.wallMaterialId = stone;
            if (d <= 5) {                               // the bowl
                c.flags = CELL_FLOOR; c.floorHeight = 0;
            } else if (d <= 10) {                       // tier 1 @ 0.75 m (jumpable ledge)
                c.flags = static_cast<u8>(CELL_FLOOR | CELL_LEDGE); c.floorHeight = 3;
            } else if (d <= 14) {                       // tier 2 @ 1.5 m
                c.flags = static_cast<u8>(CELL_FLOOR | CELL_LEDGE); c.floorHeight = 6;
            } else if (d <= 16) {                       // the rim walkway @ 1.5 m (the 24-map's rim)
                c.flags = static_cast<u8>(CELL_FLOOR | CELL_LEDGE); c.floorHeight = 6;
            } else {                                    // the WALL-WALK @ 3.5 m: sniper ring,
                // entered only via the corner stairs. A RAISED LEDGE floor, not a platform —
                // slab-over-raised-floor is a combination no shipped level ever used, and
                // both attempts at it froze every bot dead on its spawn cell (116 samples,
                // 2 distinct positions). The proven vocabulary does the same job.
                c.flags = static_cast<u8>(CELL_FLOOR | CELL_LEDGE); c.floorHeight = 14;
            }
        }
    // Corner stairs bowl -> tier2 (0.25 m steps, the free way up the tiers).
    auto step = [&](u32 x, u32 z, u8 qh) {
        GridCell& c = cellAt(x, z);
        c.flags = static_cast<u8>(CELL_FLOOR | CELL_LEDGE);
        c.floorHeight = qh; c.floorMaterialId = sand; c.wallMaterialId = stone;
    };
    for (u32 i = 0; i < 6; i++) {
        const u8 qh = static_cast<u8>(6 - i);
        step(9 + i, 9 + i, qh);            step(W - 10 - i, 9 + i, qh);
        step(9 + i, W - 10 - i, qh);       step(W - 10 - i, W - 10 - i, qh);
    }
    // Corner STAIRS rim -> wall-walk: 0.25 m raised steps along the walls (walkable both
    // ways under STEP_UP_HEIGHT), so the 3.5 m ring is earned at the corners.
    for (u32 i = 0; i < 8; i++) {
        const u8 qh = static_cast<u8>(7 + i);          // 1.75 m .. 3.5 m
        step(4 + i, 2, qh); step(4 + i, 1, qh);        // north wall, from the west corner
        step(W - 5 - i, W - 3, qh); step(W - 5 - i, W - 2, qh);   // south, from the east
        step(1, 4 + i, qh); step(2, 4 + i, qh);        // west wall
        step(W - 2, W - 5 - i, qh); step(W - 3, W - 5 - i, qh);   // east wall
    }
    // Bowl pads: the loud way out — the 3.6 m apex clears even the 3.5 m deck with steer.
    for (u32 z = W / 2 - 1; z <= W / 2; z++)
        for (u32 x = W / 2 - 1; x <= W / 2; x++) {
            GridCell& c = cellAt(x, z);
            c.flags = static_cast<u8>(CELL_FLOOR | CELL_JUMPPAD);
            c.floorHeight = 0; c.floorMaterialId = padM;
        }
    // Spawns on the rim walkway, wall midpoints (the 24-map's proven spawn story).
    m_arenaPads[0] = {W * 0.5f + 0.5f, 1.5f, 16.5f - 1.0f};
    m_arenaPads[1] = {W - 15.5f, 1.5f, W * 0.5f + 0.5f};
    m_arenaPads[2] = {W * 0.5f + 0.5f, 1.5f, W - 15.5f};
    m_arenaPads[3] = {15.5f, 1.5f, W * 0.5f + 0.5f};
    // Loot anchors: the bowl (all eyes on it), the four tier-2 sides, one deck corner.
    m_arenaLootAnchorCount = 6;
    m_arenaLootAnchors[0] = {W * 0.5f, 0.0f, W * 0.5f};
    m_arenaLootAnchors[1] = {W * 0.5f, 1.5f, 8.5f};
    m_arenaLootAnchors[2] = {W - 8.5f, 1.5f, W * 0.5f};
    m_arenaLootAnchors[3] = {W * 0.5f, 1.5f, W - 8.5f};
    m_arenaLootAnchors[4] = {8.5f, 1.5f, W * 0.5f};
    m_arenaLootAnchors[5] = {11.5f, 3.5f, 1.5f};       // the north wall-walk

    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid, 0xA12E7Cu,
                             m_level.sections, MAX_LEVEL_SECTIONS);
    LevelGridSystem::buildClearanceField(m_level.grid);
    Minimap::init(m_level.grid.width, m_level.grid.depth);
    Vec3 center = {W * 0.5f * CS, 0.0f, W * 0.5f * CS};
    LevelGridSystem::buildFlowField(m_level.grid, center);
    return center;
}

// ---------------------------------------------------------------------------------------------
// THE MOTHERBOARD (map 3, 40x40) — the dungeon-engine joke made walkable: a circuit board. The
// centre "die" is a raised 1.5 m plate reached ONLY by its four trace-bridges (LEDGE rim
// everywhere else), with a 3.5 m "heatsink" slab overhead as the sniper crown. One-cell LAVA
// traces score the board into quadrant alleys (jumpable everywhere — they steer, not seal),
// capacitor pillars and RAM banks break the sightlines. Bigger than the others on purpose
// (Aaron: size is free) — the traces and the single high-value centre still funnel every route
// inward.
Vec3 Engine::buildArenaMotherboard() {
    constexpr u32 W = 40; constexpr f32 CS = 1.0f;
    LevelGridSystem::init(m_level.grid, W, W, CS);
    const u8 brick = MaterialSystem::getIdByName("brick_wall");
    const u8 sand  = MaterialSystem::getIdByName("arena_sand");
    const u8 stone = MaterialSystem::getIdByName("stone_wall");
    const u8 lava  = MaterialSystem::getIdByName("hellforge_lava");
    const u8 plank = MaterialSystem::getIdByName("wood_plank");
    const u8 padM  = MaterialSystem::getIdByName("arena_pad");
    auto cellAt = [&](u32 x, u32 z) -> GridCell& { return LevelGridSystem::getCell(m_level.grid, x, z); };
    for (u32 z = 0; z < W; z++)
        for (u32 x = 0; x < W; x++) {
            GridCell& c = cellAt(x, z);
            if (x == 0 || z == 0 || x == W - 1 || z == W - 1) {
                c.flags = CELL_SOLID; c.wallMaterialId = brick;
            } else {
                c.flags = CELL_FLOOR; c.floorHeight = 0; c.ceilingHeight = 20;
                c.floorMaterialId = sand; c.wallMaterialId = brick;
            }
        }
    const u32 C = W / 2;   // 20
    // LAVA TRACES: one-cell channels on the quadrant seams, with a 2-cell VIA (gap) every 6
    // cells — every trace is jumpable anywhere and walkable through the vias, so they shape
    // movement without ever walling a player in. They stop 6 cells short of the die.
    auto trace = [&](u32 x, u32 z) {
        GridCell& c = cellAt(x, z);
        c.flags = static_cast<u8>(CELL_FLOOR | CELL_LAVA); c.floorMaterialId = lava;
    };
    for (u32 i = 2; i < W - 2; i++) {
        if (i >= C - 6 && i <= C + 6) continue;          // keep the die approach clean
        if ((i % 6) < 2) continue;                       // the vias
        trace(C - 10, i); trace(C + 10, i);              // two vertical seams
        trace(i, C - 10); trace(i, C + 10);              // two horizontal seams
    }
    // THE DIE: an 8x8 plate at 1.5 m. The rim is LEDGE (a 1.5 m ledge is unjumpable — the only
    // ways up are the four 1q-step trace-bridges), so holding the die means holding four ramps.
    for (u32 z = C - 4; z < C + 4; z++)
        for (u32 x = C - 4; x < C + 4; x++) {
            GridCell& c = cellAt(x, z);
            c.flags = static_cast<u8>(CELL_FLOOR | CELL_LEDGE);
            c.floorHeight = 6; c.floorMaterialId = sand; c.wallMaterialId = stone;
        }
    auto bridge = [&](u32 sx, u32 sz, s32 dx, s32 dz) {   // 6 steps, 0.25 m each, two lanes
        for (u32 i = 0; i < 6; i++) {
            const u8 qh = static_cast<u8>(6 - i);
            for (u32 lane = 0; lane < 2; lane++) {
                GridCell& c = cellAt(static_cast<u32>((s32)sx + dx * (s32)i + (dz != 0 ? (s32)lane : 0)),
                                     static_cast<u32>((s32)sz + dz * (s32)i + (dx != 0 ? (s32)lane : 0)));
                c.flags = static_cast<u8>(CELL_FLOOR | CELL_LEDGE);
                c.floorHeight = qh; c.floorMaterialId = sand; c.wallMaterialId = stone;
            }
        }
    };
    bridge(C + 4, C - 1,  1, 0);   // east
    bridge(C - 5, C - 1, -1, 0);   // west
    bridge(C - 1, C + 4,  0, 1);   // south
    bridge(C - 1, C - 5,  0, -1);  // north
    // HEATSINK: the 3.5 m crown slab over the die's centre, reached by the die's own 2x2 pad
    // one cell beside it (never under a slab). King of a hill that is itself on a hill.
    for (u32 z = C - 1; z <= C + 1; z++)
        for (u32 x = C - 1; x <= C + 1; x++)
            LevelGridSystem::setPlatform(cellAt(x, z), 14, plank);
    for (u32 z = C + 2; z <= C + 3; z++)
        for (u32 x = C - 1; x <= C; x++) {
            GridCell& c = cellAt(x, z);
            c.flags = static_cast<u8>(CELL_FLOOR | CELL_JUMPPAD | CELL_LEDGE);
            c.floorHeight = 6; c.floorMaterialId = padM;
        }
    // CAPACITORS (2x2 pillars) + RAM BANKS (1x4 walls): cover and sightline breaks, mirrored
    // per quadrant so no spawn reads a different board.
    auto solidRect = [&](u32 sx, u32 sz, u32 w, u32 d, u8 mat) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = cellAt(x, z); c.flags = CELL_SOLID; c.wallMaterialId = mat;
            }
    };
    solidRect(7, 7, 2, 2, stone);           solidRect(W - 9, 7, 2, 2, stone);
    solidRect(7, W - 9, 2, 2, stone);       solidRect(W - 9, W - 9, 2, 2, stone);
    solidRect(C - 2, 5, 4, 1, plank);       solidRect(C - 2, W - 6, 4, 1, plank);
    solidRect(5, C - 2, 1, 4, plank);       solidRect(W - 6, C - 2, 1, 4, plank);
    // Corner SOCKET spawns: a small 3x3 raised pad (0.5 m, walk-up) so you respawn with one
    // step of high ground and a capacitor between you and the nearest alley.
    auto socket = [&](u32 sx, u32 sz) {
        for (u32 z = sz; z < sz + 3; z++)
            for (u32 x = sx; x < sx + 3; x++) {
                GridCell& c = cellAt(x, z);
                c.flags = CELL_FLOOR; c.floorHeight = 2; c.floorMaterialId = sand;
                c.wallMaterialId = stone;
            }
    };
    socket(2, 2); socket(W - 5, 2); socket(W - 5, W - 5); socket(2, W - 5);
    m_arenaPads[0] = {3.5f,     0.5f, 3.5f};
    m_arenaPads[1] = {W - 3.5f, 0.5f, 3.5f};
    m_arenaPads[2] = {W - 3.5f, 0.5f, W - 3.5f};
    m_arenaPads[3] = {3.5f,     0.5f, W - 3.5f};
    // Loot anchors: the die (the hill worth being king of), the heatsink crown above it, and
    // the four quadrant alleys between the capacitors and the RAM banks.
    m_arenaLootAnchorCount = 6;
    m_arenaLootAnchors[0] = {C * CS, 1.5f, C * CS};
    m_arenaLootAnchors[1] = {C * CS, 3.5f, C * CS};                // heatsink
    m_arenaLootAnchors[2] = {11.0f, 0.0f, 11.0f};
    m_arenaLootAnchors[3] = {W - 11.0f, 0.0f, 11.0f};
    m_arenaLootAnchors[4] = {W - 11.0f, 0.0f, W - 11.0f};
    m_arenaLootAnchors[5] = {11.0f, 0.0f, W - 11.0f};

    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid, 0xA12E7Du,
                             m_level.sections, MAX_LEVEL_SECTIONS);
    LevelGridSystem::buildClearanceField(m_level.grid);
    Minimap::init(m_level.grid.width, m_level.grid.depth);
    Vec3 center = {C * CS, 0.0f, C * CS};
    LevelGridSystem::buildFlowField(m_level.grid, center);
    return center;
}

// ---------------------------------------------------------------------------------------------
// THE MAINFRAME (map 4, 48x48 — WB-303, the Quake-DM signature map). Two storeys of loops
// around ONE prize: a walled 10x10 VAULT in the centre — four door chokepoints, the mega loot
// anchor inside, and a walkable roof reached by the courtyard pads (king of the vault). A
// 3-cell BALCONY ring runs the whole perimeter at 3 m (corner stairwells up, open inner edge
// to drop anywhere), the arcade under it carries the spawns, and server-rack rows break the
// courtyard sightlines. Every loop — balcony, courtyard, vault roof — orbits the same prize.
Vec3 Engine::buildArenaMainframe() {
    constexpr u32 W = 48; constexpr f32 CS = 1.0f;
    LevelGridSystem::init(m_level.grid, W, W, CS);
    const u8 brick = MaterialSystem::getIdByName("brick_wall");
    const u8 sand  = MaterialSystem::getIdByName("arena_sand");
    const u8 stone = MaterialSystem::getIdByName("stone_wall");
    const u8 plank = MaterialSystem::getIdByName("wood_plank");
    const u8 padM  = MaterialSystem::getIdByName("arena_pad");
    auto cellAt = [&](u32 x, u32 z) -> GridCell& { return LevelGridSystem::getCell(m_level.grid, x, z); };
    for (u32 z = 0; z < W; z++)
        for (u32 x = 0; x < W; x++) {
            GridCell& c = cellAt(x, z);
            if (x == 0 || z == 0 || x == W - 1 || z == W - 1) {
                c.flags = CELL_SOLID; c.wallMaterialId = brick;
            } else {
                c.flags = CELL_FLOOR; c.floorHeight = 0; c.ceilingHeight = 24;
                c.floorMaterialId = sand; c.wallMaterialId = brick;
            }
        }
    // BALCONY RING @ 3 m: three cells deep along every wall, arcade beneath (spawn cover).
    auto plat = [&](u32 sx, u32 sz, u32 w, u32 d, u8 topQ) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++)
                LevelGridSystem::setPlatform(cellAt(x, z), topQ, plank);
    };
    plat(1, 1, W - 2, 3, 12);  plat(1, W - 4, W - 2, 3, 12);
    plat(1, 4, 3, W - 8, 12);  plat(W - 4, 4, 3, W - 8, 12);
    // Corner stairwells (graduated slabs, ground -> balcony), one per corner.
    for (u32 i = 0; i < 12; i++) {
        const u8 topQ = static_cast<u8>(1 + i);
        plat(15 - i, 1, 1, 1, topQ); plat(15 - i, 2, 1, 1, topQ);            // north-west run
        plat(W - 16 + i, W - 2, 1, 1, topQ); plat(W - 16 + i, W - 3, 1, 1, topQ); // south-east
        plat(1, W - 16 + i, 1, 1, topQ); plat(2, W - 16 + i, 1, 1, topQ);    // west-south
        plat(W - 2, 15 - i, 1, 1, topQ); plat(W - 3, 15 - i, 1, 1, topQ);    // east-north
    }
    // THE VAULT: a walled 10x10 room dead centre, one 2-wide DOOR per side, roof at 3 m
    // (walkable: the raise is plain floor on top of solid walls? No — the walls are SOLID
    // full-height cells, so the roof is a PLATFORM slab spanning the room: stand on it, and
    // the room below keeps its interior).
    const u32 C = W / 2;   // 24
    auto solidRect = [&](u32 sx, u32 sz, u32 w, u32 d, u8 mat) {
        for (u32 z = sz; z < sz + d; z++)
            for (u32 x = sx; x < sx + w; x++) {
                GridCell& c = cellAt(x, z); c.flags = CELL_SOLID; c.wallMaterialId = mat;
            }
    };
    // Vault walls (ring of solid), then punch the four doors, then slab the roof.
    solidRect(C - 5, C - 5, 10, 1, stone);  solidRect(C - 5, C + 4, 10, 1, stone);
    solidRect(C - 5, C - 4, 1, 8, stone);   solidRect(C + 4, C - 4, 1, 8, stone);
    auto door = [&](u32 x, u32 z) {
        GridCell& c = cellAt(x, z);
        c.flags = CELL_FLOOR; c.floorHeight = 0; c.floorMaterialId = sand; c.wallMaterialId = stone;
    };
    door(C - 1, C - 5); door(C, C - 5); door(C - 1, C + 4); door(C, C + 4);
    door(C - 5, C - 1); door(C - 5, C); door(C + 4, C - 1); door(C + 4, C);
    // The roof: platform slabs over the vault INTERIOR (walls are full-height solid — the
    // slab spans the open room and rests visually on the walls).
    plat(C - 4, C - 4, 8, 8, 12);
    // Courtyard PADS: four launchers midway between vault and balcony — the fast way onto
    // the roof or the ring (3.6 m apex, air-steer).
    auto pad2 = [&](u32 sx, u32 sz) {
        for (u32 z = sz; z < sz + 2; z++)
            for (u32 x = sx; x < sx + 2; x++) {
                GridCell& c = cellAt(x, z);
                c.flags = static_cast<u8>(CELL_FLOOR | CELL_JUMPPAD);
                c.floorHeight = 0; c.floorMaterialId = padM;
            }
    };
    pad2(C - 1, 9); pad2(W - 12, C - 1); pad2(C - 1, W - 11); pad2(9, C - 1);
    // SERVER RACKS: 1x5 solid rows in the quadrants — courtyard cover, mirrored.
    solidRect(11, 13, 1, 5, plank);  solidRect(W - 12, 13, 1, 5, plank);
    solidRect(11, W - 18, 1, 5, plank); solidRect(W - 12, W - 18, 1, 5, plank);
    solidRect(13, 11, 5, 1, plank);  solidRect(W - 18, 11, 5, 1, plank);
    solidRect(13, W - 12, 5, 1, plank); solidRect(W - 18, W - 12, 5, 1, plank);
    // Spawns in the arcade, one per wall, tucked at the quarter points.
    // Clear of the stairwell runs (x 4..15 hug the walls): a spawn under a 0.25 m stair
    // slab pins the body from second zero — measured as four bots farmed by monsters while
    // standing still (69 environmental deaths in one soak).
    m_arenaPads[0] = {20.5f, 0.0f, 2.0f};
    m_arenaPads[1] = {W - 2.0f, 0.0f, 20.5f};
    m_arenaPads[2] = {W - 20.5f, 0.0f, W - 2.0f};
    m_arenaPads[3] = {2.0f, 0.0f, W - 20.5f};
    // Loot anchors: the VAULT INTERIOR (the mega spot — four doors between you and out),
    // the roof, and the four pad courts.
    m_arenaLootAnchorCount = 6;
    m_arenaLootAnchors[0] = {C * CS, 0.0f, C * CS};        // inside the vault
    m_arenaLootAnchors[1] = {C * CS, 3.0f, C * CS};        // the roof
    m_arenaLootAnchors[2] = {C * CS, 0.0f, 10.0f};
    m_arenaLootAnchors[3] = {W - 10.0f, 0.0f, C * CS};
    m_arenaLootAnchors[4] = {C * CS, 0.0f, W - 10.0f};
    m_arenaLootAnchors[5] = {10.0f, 0.0f, C * CS};

    m_level.sectionCount = LevelMeshSystem::buildAll(m_level.grid, 0xA12E7Eu,
                             m_level.sections, MAX_LEVEL_SECTIONS);
    LevelGridSystem::buildClearanceField(m_level.grid);
    Minimap::init(m_level.grid.width, m_level.grid.depth);
    Vec3 center = {C * CS, 0.0f, C * CS};
    LevelGridSystem::buildFlowField(m_level.grid, center);
    return center;
}

// Deliberately empty in v1: no stash, no portals, no NPCs, no loot — the arena is a progression
// firewall. Kept as a function (town symmetry) so future contested pickups have a home.
void Engine::spawnArenaContents(Vec3 /*center*/) {}

// Shared body of enterArena/enterArenaClient: wipe pools, build, reset match state, place the
// local lanes on their pads. Everything both sides do identically lives here.
void Engine::enterArenaCommon() {
    EntitySystem::init(m_entities);
    ProjectileSystem::init(m_projectiles);
    WorldItemSystem::init(m_worldItems);
    Vec3 center = buildArenaLevel();

    m_level.inArena            = true;
    m_level.inTown             = false;
    m_level.inSourceChamber    = false;
    m_level.floorDoorActive    = false;
    m_level.floorHasBoss       = false;
    m_level.townPortalActive   = false;
    m_level.sourcePortalActive = false;
    m_level.exitPortalActive   = false;

    m_arenaScore     = Arena::Score{};
    m_arenaWinner    = 0xFF;
    m_arenaOverTimer = 0.0f;
    m_arenaLootTimer = 0.0f;
    m_arenaLootWave  = 0;
    m_arenaLootLast  = -1;
    m_arenaMonsterTimer = 0.0f;
    m_arenaMonsterLast  = -1;
    for (u32 i = 0; i < MAX_PLAYERS; i++) m_arenaRespawn[i] = 0.0f;
    for (u32 i = 0; i < ARENA_FEED_LINES; i++) m_arenaFeed[i] = ArenaFeedEntry{};

    // THE NAKED SPAWN (WB-296, the Quake-mode rule): whatever hero walked in — a Continue
    // character in full mythics included — fights as a BARE CLASS: starting weapon, empty bag,
    // no armor, class-base stats. equipFreshLane is exactly that wipe. The save is untouched
    // (saveCharacter hard-refuses in-arena and arenaLeaveToMenu never saves), so the hero's
    // real gear is back on the next Continue; only the ARENA never sees it.
    //
    // ORDER IS LOAD-BEARING: the wipe writes m_localPlayers[lane], but the placement loop
    // below writes the ALIAS for the active lane and the function ends by persisting the
    // alias over the array — so without adopting the wipe into the alias first, the active
    // lane would keep its old HP/gear-derived stats (the enterTown alias rule, in reverse).
    for (u8 lane = 0; lane < m_splitPlayerCount && lane < MAX_LOCAL_PLAYERS; lane++)
        equipFreshLane(lane);
    m_localPlayer = m_localPlayers[m_localPlayerIndex];

    // Local lanes onto their pads (lane index == net slot for host/SP locals). The alias write
    // happens OUTSIDE the per-player swap at every call site, so persist the lane array
    // explicitly or next frame's swapInPlayer erases the placement (the enterTown rule).
    for (u8 lane = 0; lane < m_splitPlayerCount && lane < MAX_LOCAL_PLAYERS; lane++) {
        Player& p = (lane == m_localPlayerIndex) ? m_localPlayer : m_localPlayers[lane];
        p.position    = m_arenaPads[lane];
        p.yaw         = yawFromTo(m_arenaPads[lane], m_arenaCenter);
        p.pitch       = 0.0f;
        p.invulnTimer = 1.0f;
        p.lastHitByPlayerSlot = 0xFF;
    }
    m_localPlayers[m_localPlayerIndex] = m_localPlayer;
    snapCameraToPlayer();

    spawnArenaContents(center);
    EnemyAI::setTownMode(false);
    m_gameState = GameState::IN_GAME;
    Input::setRelativeMouseMode(true);
}

// Host/SP: enter the arena and (if hosting) pull every connected client in with us.
void Engine::enterArena() {
    enterArenaCommon();

    // A host reaching the arena via CONTINUE (or the --arena dev door) skips startGame() — the ONLY
    // path that activates and seeds the host's own NetPlayer slot (engine_startgame.cpp:809; a host
    // is refused by onPlayerJoin by design). On a fresh process m_players[activeNetSlot()] is then
    // still a default NetPlayer{}: inactive, position {0,0,0}, health 100. The seating loop below is
    // gated on `active`, so it SKIPS the host slot, and the first gameUpdate's
    // syncNetPlayerToLocalPlayer copies {0,0,0}/health-100 over the pad — wedging the host in the
    // corner border wall ("arena spawns out of bounds") AND resetting a Continue'd hero's HP. It's
    // only intermittent because a startGame earlier in the same process leaves the slot active.
    // Seed the slot here from m_localPlayer (enterArenaCommon just placed it on its pad with the
    // loaded HP/class), mirroring startGame's slot-0 setup; the seating loop then applies the pad.
    // Harmless re-assert when a prior startGame already activated the slot.
    if (m_netRole == NetRole::SERVER) {
        const u8 hostSlot  = activeNetSlot();
        NetPlayer& host    = m_players[hostSlot];
        host.active        = true;
        host.slotIndex     = hostSlot;
        host.playerClass   = m_playerClasses[m_localPlayerIndex];
        host.baseMaxHealth = m_localPlayer.baseMaxHealth;
        host.maxHealth     = m_localPlayer.maxHealth;
        host.health        = m_localPlayer.health;
        host.moveSpeed     = m_localPlayer.moveSpeed;
        host.weaponState.currentWeapon = 0;
    }

    // Seat every active NetPlayer on its own pad; the pad is also its respawn point.
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (!m_players[pi].active) continue;
        m_players[pi].position      = m_arenaPads[pi];
        m_players[pi].yaw           = yawFromTo(m_arenaPads[pi], m_arenaCenter);
        m_players[pi].spawnPosition = m_arenaPads[pi];
        m_players[pi].invulnTimer   = 1.0f;
        m_players[pi].isDead        = false;
    }

    if (m_netRole == NetRole::SERVER) {
        // A host can reach the arena WITHOUT startGame (Continue hero, --arena dev door) —
        // wire the server callbacks or joiners connect but never get seated (no onPlayerJoin).
        wireServerNet();
        Net::broadcastLevelSeed(GameConst::ARENA_SENTINEL_FLOOR, m_difficulty, m_level.levelSeed);
        Server::updateLevel(m_level.levelSeed, GameConst::ARENA_SENTINEL_FLOOR, m_difficulty);
        // Republish lobby data now: entering the arena bypasses FLOOR_TRANSITION (the usual
        // republish trigger), so without this the Steam browser row keeps the stale pre-arena floor
        // until the next join. updateSteamLobbyRoster advertises floor 97 while inArena → "Arena".
        updateSteamLobbyRoster();
    }
    LOG_INFO("Entered the ARENA (host) — first to %u.", Arena::KILL_TARGET);
}

// Client: mirror of enterArena, driven by the sentinel-floor SV_LEVEL_SEED / join-accept.
// Our real position/score arrive from the server (snapshot + ARENA_SCORES).
void Engine::enterArenaClient() {
    // Join-accept routed here INSTEAD of startGame, so the client-side net wiring (snapshot,
    // SV_EVENT, clock sync) must happen here or this client is connected but deaf.
    wireClientNet();
    enterArenaCommon();
    // The naked-spawn wipe above changed this client's inventory AFTER the join-time
    // CL_INVENTORY_SYNC — re-sync, or the server keeps deriving PvP stats (ccResist,
    // armor) from the gear the wipe just removed.
    sendInventorySync(0, activeNetSlot());
    LOG_INFO("Entered the ARENA (client).");
}

// ---------------------------------------------------------------------------------------------
// The deathmatch loop — kill credit, auto-respawn, match end. Authority-only except where
// noted (the client mirrors score/feed/over via the ARENA_* events, engine.cpp onEvent).
// ---------------------------------------------------------------------------------------------

#include "audio/audio.h"
#include "net/packet.h"
#include <cstring>

extern Engine* s_engine;

// Push one line into the kill-feed ring (newest first). Runs on every machine: the authority
// pushes directly from arenaHandleDeath, clients from the ARENA_KILL event.
void Engine::arenaPushFeed(u8 killerSlot, u8 victimSlot) {
    for (u32 i = ARENA_FEED_LINES - 1; i > 0; i--) m_arenaFeed[i] = m_arenaFeed[i - 1];
    m_arenaFeed[0] = {killerSlot, victimSlot, 4.0f};
}

// SERVER: ship the full per-slot score table — the mid-join refresh (join-accept is a fixed
// legacy layout, so scores ride this additive event instead). toSlot 0xFF = broadcast.
void Engine::arenaSendScores(u8 toSlot) {
    if (m_netRole != NetRole::SERVER) return;
    u8 buf[sizeof(PacketHeader) + 1 + MAX_PLAYERS];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
    hdr->type = NetPacketType::SV_EVENT;
    hdr->flags = 0;
    hdr->seq = 0;
    u32 off = sizeof(PacketHeader);
    buf[off++] = static_cast<u8>(NetEventType::ARENA_SCORES);
    for (u32 i = 0; i < MAX_PLAYERS; i++)
        buf[off++] = static_cast<u8>(m_arenaScore.kills[i] > 255 ? 255 : m_arenaScore.kills[i]);
    if (toSlot == 0xFF) Net::broadcastReliable(buf, off);
    else                Net::sendReliable(toSlot, buf, off);
}

// AUTHORITY: a combatant died. Credits the killer, feeds every peer's HUD, starts the victim's
// respawn clock, and ends the match on the KILL_TARGET-th kill. Death sites (the serverNetPost
// remote check and the local-lane death path) call this exactly once per death.
void Engine::arenaHandleDeath(u8 victimSlot, u8 killerSlot) {
    // One parseable line per death: the arena soak's primary dataset (kill cadence,
    // per-slot totals). Logged before the early-outs so environmental deaths show too.
    LOG_INFO("[ARENA] death: victim=%u killer=%u scores=%u/%u/%u/%u",
             victimSlot, killerSlot,
             m_arenaScore.kills[0], m_arenaScore.kills[1],
             m_arenaScore.kills[2], m_arenaScore.kills[3]);
    if (!m_level.inArena || m_arenaOverTimer > 0.0f) return;
    m_arenaRespawn[victimSlot] = Arena::RESPAWN_DELAY;
    arenaPushFeed(killerSlot, victimSlot);
    u8 winner = 0xFF;
    bool over = Arena::recordKill(m_arenaScore, killerSlot, winner);
    LOG_INFO("Arena: player %u slew player %u (%u/%u)", killerSlot, victimSlot,
             killerSlot < MAX_PLAYERS ? m_arenaScore.kills[killerSlot] : 0, Arena::KILL_TARGET);
    if (m_netRole == NetRole::SERVER && killerSlot < MAX_PLAYERS) {
        u8 buf[sizeof(PacketHeader) + 4];
        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
        hdr->type = NetPacketType::SV_EVENT;
        hdr->flags = 0;
        hdr->seq = 0;
        u32 off = sizeof(PacketHeader);
        buf[off++] = static_cast<u8>(NetEventType::ARENA_KILL);
        buf[off++] = killerSlot;
        buf[off++] = victimSlot;
        buf[off++] = static_cast<u8>(m_arenaScore.kills[killerSlot]);
        Net::broadcastReliable(buf, off);
    }
    if (over) beginArenaOver(winner);
}

// AUTHORITY: revive a slot on the pad farthest from its living enemies (the handleRespawnRequest
// field-set: full HP, zeroed velocity, 1.5 s spawn protection).
void Engine::arenaRespawnSlot(u8 slot) {
    Vec3 hostiles[MAX_PLAYERS];
    u32  hostileCount = 0;
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (pi == slot) continue;
        if (pi < m_splitPlayerCount) {
            if (!m_playerDead[pi]) hostiles[hostileCount++] = m_localPlayers[pi].position;
        } else if (m_players[pi].active && !m_players[pi].isDead) {
            hostiles[hostileCount++] = m_players[pi].position;
        }
    }
    u32 pad = Arena::farthestPad(m_arenaPads, MAX_PLAYERS, hostiles, hostileCount);
    Vec3 pos = m_arenaPads[pad];
    f32  yaw = yawFromTo(pos, m_arenaCenter);

    NetPlayer& np = m_players[slot];
    if (np.active) {
        np.health        = np.maxHealth;
        np.position      = pos;
        np.spawnPosition = pos;
        np.velocity      = {0, 0, 0};
        np.yaw           = yaw;
        np.invulnTimer   = 1.5f;
        np.isDead        = false;
        np.lastHitByPlayerSlot = 0xFF;
    }
    if (slot < m_splitPlayerCount) {
        Player& lane = m_localPlayers[slot];
        lane.health        = lane.maxHealth;
        lane.position      = pos;
        lane.velocity      = {0, 0, 0};
        lane.yaw           = yaw;
        lane.pitch         = 0.0f;
        lane.invulnTimer   = 1.5f;
        lane.hurtVignette  = 0.0f;
        lane.damageFlashTimer = 0.0f;
        lane.lastHitByPlayerSlot = 0xFF;
        m_playerDead[slot] = false;
        // Outside-swap rule: if this lane is the current alias, refresh it too.
        if (slot == m_localPlayerIndex) m_localPlayer = lane;
    }
}

// Per-tick arena bookkeeping, every role. Called from update() right after the PvP window
// closes. Authority ticks respawn clocks; every peer ticks the match-over banner and tears
// itself down when it expires (each peer was told via ARENA_OVER — nobody waits on anybody).
void Engine::arenaTick(f32 dt) {
    if (!m_level.inArena || m_gameState != GameState::IN_GAME) return;

    // Kill-feed TTLs decay everywhere (render-side data, engine-side clock).
    for (u32 i = 0; i < ARENA_FEED_LINES; i++)
        if (m_arenaFeed[i].ttl > 0.0f) m_arenaFeed[i].ttl -= dt;

    if (m_arenaOverTimer > 0.0f) {
        m_arenaOverTimer -= dt;
        if (m_arenaOverTimer <= 0.0f) arenaLeaveToMenu();
        return;   // match decided: no respawns, scores frozen
    }

    // --- THE LOOT ESCALATION (WB-297): the thing the players fight over. -------------------
    // Authority-side only (host/SP): every LOOT_INTERVAL a single item drops on one of the
    // map's anchors (WB-304, never the same twice in a row), and every wave rolls stronger
    // (Arena::lootItemLevel ramp; late waves force legendary — the pure rules are pinned in
    // test_arena.cpp). Replication is free: world items already ride the snapshot, and the
    // pickup is the ordinary server-validated CL_PICKUP path.
    if (m_netRole != NetRole::CLIENT && m_arenaLootAnchorCount > 0) {
        m_arenaLootTimer += dt;
        if (m_arenaLootTimer >= Arena::LOOT_INTERVAL) {
            m_arenaLootTimer -= Arena::LOOT_INTERVAL;
            const u32 wave = m_arenaLootWave++;
            const u32 pick = Arena::nextLootAnchor(static_cast<u32>(std::rand()),
                                                   m_arenaLootLast, m_arenaLootAnchorCount);
            m_arenaLootLast = static_cast<s8>(pick);
            const u8 ilvl = Arena::lootItemLevel(wave);
            const Rarity floorR = Arena::lootWaveForcesLegendary(wave) ? Rarity::LEGENDARY
                                                                       : Rarity::COMMON;
            ItemInstance it = ItemGen::rollItem(ilvl, m_itemDefs, m_itemDefCount,
                                                m_affixDefs, m_affixDefCount, floorR);
            if (!isItemEmpty(it)) {
                Vec3 pos = m_arenaLootAnchors[pick];
                pos.y += 0.4f;
                if (WorldItemSystem::spawn(m_worldItems, it, pos, &m_level.grid) != 0xFFFF) {
                    const ItemDef& d = m_itemDefs[it.defId];
                    char line[96];
                    std::snprintf(line, sizeof(line), "Loot wave %u: %s (ilvl %u)",
                                  wave + 1, d.name, ilvl);
                    addChatMessage(nullptr, line, {1.0f, 0.85f, 0.3f});   // gold: loot news
                    LOG_INFO("[ARENA] loot: wave=%u anchor=%u ilvl=%u rarity=%u name=%s",
                             wave + 1, pick, ilvl, static_cast<u32>(it.rarity), d.name);
                }
            }
        }
    }

    // --- MONSTER INTERLUDES (WB-298): the second loot source. ------------------------------
    // A monster crawls out at an anchor every MONSTER_INTERVAL, scaled to the wave, and drops
    // equipment on death (handleFirstKillDrop's arena branch — guaranteed, and it EARLY-OUTS
    // the whole PvE loot/kill-tracking chain, so the progression firewall stays sealed:
    // no lifetime kills, no XP-adjacent passives, no floor loot table). Entity replication
    // is the ordinary snapshot; a monster death never records an arena kill (recordKill is
    // only wired to PLAYER deaths).
    if (m_netRole != NetRole::CLIENT && m_arenaLootAnchorCount > 0) {
        m_arenaMonsterTimer += dt;
        if (m_arenaMonsterTimer >= Arena::MONSTER_INTERVAL) {
            m_arenaMonsterTimer -= Arena::MONSTER_INTERVAL;
            // Cap the pack: unkilled monsters ACCUMULATE (measured: nine of them farming
            // four wedged bots), and a match with more monsters than players is a PvE mode.
            u32 aliveMonsters = 0;
            for (u32 i = 0; i < MAX_ENTITIES; i++)
                if (m_entities.entities[i].flags & ENT_ACTIVE) aliveMonsters++;
            static const char* kMonsters[4] = {"Revenant", "Ghoul", "Bone Archer", "Tomb Wraith"};
            const char* want = kMonsters[m_arenaLootWave % 4];
            if (aliveMonsters >= 3) want = nullptr;   // pack full: skip this interval
            s32 defIdx = -1;
            if (want)
            for (u32 i = 0; i < m_enemyDefs.count; i++)
                if (std::strcmp(m_enemyDefs.defs[i].name, want) == 0) { defIdx = (s32)i; break; }
            if (defIdx >= 0) {
                const EnemyDef& d = m_enemyDefs.defs[defIdx];
                const u32 pick = Arena::nextLootAnchor(static_cast<u32>(std::rand()),
                                                       m_arenaMonsterLast, m_arenaLootAnchorCount);
                m_arenaMonsterLast = static_cast<s8>(pick);
                const u32 howMany = Arena::monsterCountForWave(m_arenaLootWave);
                const f32 hpMult  = Arena::monsterHealthMult(m_arenaLootWave);
                for (u32 k = 0; k < howMany; k++) {
                    Vec3 pos = m_arenaLootAnchors[pick];
                    pos.x += (k == 0) ? 0.0f : 1.2f;
                    pos.y += 0.5f;
                    EntityHandle h = EntitySystem::spawn(m_entities, pos, d.halfExtents, d.flying,
                                                         d.health * hpMult, d.moveSpeed,
                                                         d.detectionRange, d.attackRange,
                                                         d.attackCooldown, d.damage);
                    Entity* e = handleGet(m_entities, h);
                    if (!e) break;
                    e->meshId       = d.meshId;
                    e->materialId   = d.materialId;
                    e->enemyType    = d.enemyType;
                    e->enemyRole    = d.role;
                    e->aiPreference = d.aiPreference;
                    e->enemyDefIdx  = static_cast<u8>(defIdx);
                    e->baseMoveSpeed      = e->moveSpeed;
                    e->baseAttackCooldown = e->attackCooldown;
                }
                char line[96];
                std::snprintf(line, sizeof(line), "%s%s crawls out at the loot ground!",
                              want, howMany > 1 ? "s" : "");
                addChatMessage(nullptr, line, {1.0f, 0.5f, 0.35f});
                LOG_INFO("[ARENA] monster: %s x%u anchor=%u hpMult=%.2f",
                         want, howMany, pick, hpMult);
            }
        }
    }

    if (m_netRole == NetRole::CLIENT) {
        // Cosmetic countdown for OUR OWN death(s) (the server's clock is authoritative; ours just
        // feeds the "Respawning in N" overlay). arenaTick runs ONCE after the per-lane loop, so
        // m_localPlayerIndex is stuck at the last lane — iterate every local lane (couch client has
        // two) and drive each by its OWN net slot, or couch-P1's countdown never advances.
        for (u8 lane = 0; lane < m_splitPlayerCount && lane < MAX_LOCAL_PLAYERS; lane++) {
            u8 slot = m_clientNetSlot[lane];
            if (m_playerDead[lane]) {
                if (m_arenaRespawn[slot] <= 0.0f) m_arenaRespawn[slot] = Arena::RESPAWN_DELAY;
                m_arenaRespawn[slot] -= dt;
            } else {
                m_arenaRespawn[slot] = 0.0f;
            }
        }
        return;
    }

    // AUTHORITY: tick every dead combatant's clock and revive at zero.
    for (u32 pi = 0; pi < MAX_PLAYERS; pi++) {
        if (m_arenaRespawn[pi] <= 0.0f) continue;
        bool exists = (pi < m_splitPlayerCount) || m_players[pi].active;
        if (!exists) { m_arenaRespawn[pi] = 0.0f; continue; }   // combatant left mid-death
        m_arenaRespawn[pi] -= dt;
        if (m_arenaRespawn[pi] <= 0.0f) arenaRespawnSlot(static_cast<u8>(pi));
    }
}

// Match decided. Broadcast FIRST, then flip local state — the CREDITS ordering rule: a
// host-local flip that stops the world before the packet leaves is how clients hang.
void Engine::beginArenaOver(u8 winner) {
    LOG_INFO("[ARENA] over: winner=%u scores=%u/%u/%u/%u",
             winner, m_arenaScore.kills[0], m_arenaScore.kills[1],
             m_arenaScore.kills[2], m_arenaScore.kills[3]);
    if (m_netRole == NetRole::SERVER) {
        u8 buf[sizeof(PacketHeader) + 2 + MAX_PLAYERS];
        PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf);
        hdr->type = NetPacketType::SV_EVENT;
        hdr->flags = 0;
        hdr->seq = 0;
        u32 off = sizeof(PacketHeader);
        buf[off++] = static_cast<u8>(NetEventType::ARENA_OVER);
        buf[off++] = winner;
        for (u32 i = 0; i < MAX_PLAYERS; i++)
            buf[off++] = static_cast<u8>(m_arenaScore.kills[i] > 255 ? 255 : m_arenaScore.kills[i]);
        Net::broadcastReliable(buf, off);
    }
    m_arenaWinner    = winner;
    m_arenaOverTimer = 8.0f;
    LOG_INFO("Arena: match over — player %u wins.", winner);
}

// Tear down to the main menu (the pause-quit path MINUS the save — the arena never saves;
// a character leaves exactly as it entered). Runs independently on every peer.
void Engine::arenaLeaveToMenu() {
    m_menu.confirmQuit      = false;
    m_menu.optionsFromPause = false;
    if (m_netRole != NetRole::NONE) {
        Net::disconnect();
        m_netRole = NetRole::NONE;
    }
    m_level.inArena    = false;
    m_menu.arena       = false;
    m_splitPlayerCount = 1;
    Input::setSplitScreen(false);
    m_gameState = GameState::MENU;
    m_menu.subState    = 0;
    m_menu.selection   = 0;
    AudioSystem::stopMusic();
    Input::setRelativeMouseMode(false);
}
