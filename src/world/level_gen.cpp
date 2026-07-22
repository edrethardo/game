// Procedural dungeon generation — four structural layout styles behind one contract.
// LevelGen::generate(grid, seed, w, d, style) initializes the grid, carves the floor
// with the requested style (classic BSP rooms, cellular-automata cavern, serpentine
// gauntlet, or hub-and-spokes), writes cell flags + floor/ceiling heights + material
// IDs into the grid, and returns the player spawn plus the room list (used by
// Engine::startGame to place enemies/chests/bosses/lights). Every style emits the
// same DungeonResult shape — rectangular rooms with corridor adjacency and a
// reachable spawn→exit pair — so downstream consumers never know which style ran.
// RNG is a local LCG (GenRNG) seeded by the caller — independent of the item-roll
// RNG so loot is not coupled to map layout, and free of transcendental float calls
// so host and client carve bit-identical grids from the shared run seed.

#include "world/level_gen.h"
#include "core/log.h"
#include <cmath>
#include <cstring>

// --- shared generation plumbing ---

// Simple LCG RNG for generation (independent from item RNG)
struct GenRNG {
    u32 state;
    u32 next() { state = state * 1664525u + 1013904223u; return state; }
    u32 range(u32 min, u32 max) {
        if (min >= max) return min;
        return min + (next() >> 8) % (max - min);
    }
    f32 f01() { return static_cast<f32>(next() >> 8) / static_cast<f32>(1u << 24); }
};

struct BSPNode {
    u32 x, z, w, d;          // partition bounds in grid cells
    s32 left  = -1;          // child indices (-1 = leaf)
    s32 right = -1;
    s32 roomIndex = -1;      // index into DungeonResult::rooms (-1 = no room)
};

static constexpr u32 MAX_BSP_NODES = 128;
static constexpr u32 MIN_PARTITION_SIZE = 7;  // minimum partition dimension
static constexpr u32 MIN_ROOM_SIZE = 4;       // minimum room dimension
static constexpr u32 ROOM_MARGIN = 1;         // margin between room and partition edge

// Scratch-buffer bound for the cavern automata. Grids are ≤48×48 today; this
// static_asserts on use so a future grid bump can't silently overrun.
static constexpr u32 GEN_MAX_CELLS = 64 * 64;

// Reset every cell to solid rock — each generator carves from this.
static void fillSolid(LevelGrid& grid) {
    for (u32 z = 0; z < grid.depth; z++) {
        for (u32 x = 0; x < grid.width; x++) {
            GridCell& c = LevelGridSystem::getCell(grid, x, z);
            c.flags           = CELL_SOLID;
            c.floorHeight     = 0;
            c.ceilingHeight   = 0;
            c.wallMaterialId  = 0;
            c.floorMaterialId = 0;
            c.ceilMaterialId  = 0;
        }
    }
}

// Carve a rectangular room into the grid
static void carveArea(LevelGrid& grid, u32 rx, u32 rz, u32 rw, u32 rd,
                      f32 floorH, f32 ceilH,
                      u8 wallMat, u8 floorMat, u8 ceilMat)
{
    u8 floorEnc = static_cast<u8>(floorH / 0.25f);
    u8 ceilEnc  = static_cast<u8>(ceilH  / 0.25f);

    for (u32 z = rz; z < rz + rd; z++) {
        for (u32 x = rx; x < rx + rw; x++) {
            if (!LevelGridSystem::isInBounds(grid, x, z)) continue;
            GridCell& cell = LevelGridSystem::getCell(grid, x, z);
            cell.flags           = CELL_FLOOR | CELL_CEILING;
            cell.floorHeight     = floorEnc;
            cell.ceilingHeight   = ceilEnc;
            cell.wallMaterialId  = wallMat;
            cell.floorMaterialId = floorMat;
            cell.ceilMaterialId  = ceilMat;
        }
    }
}

// Carve a 2-wide L-shaped corridor between two points
static void carveLCorridor(LevelGrid& grid, u32 x0, u32 z0, u32 x1, u32 z1,
                           f32 floorH, f32 ceilH)
{
    u8 floorEnc = static_cast<u8>(floorH / 0.25f);
    u8 ceilEnc  = static_cast<u8>(ceilH  / 0.25f);

    auto carveCell = [&](u32 x, u32 z) {
        if (!LevelGridSystem::isInBounds(grid, x, z)) return;
        GridCell& cell = LevelGridSystem::getCell(grid, x, z);
        if (cell.flags & CELL_SOLID) {
            cell.flags           = CELL_FLOOR | CELL_CEILING;
            cell.floorHeight     = floorEnc;
            cell.ceilingHeight   = ceilEnc;
            cell.wallMaterialId  = 0;
            cell.floorMaterialId = 1;
            cell.ceilMaterialId  = 2;
        }
    };

    // Horizontal leg — 2 cells wide in Z
    u32 xMin = (x0 < x1) ? x0 : x1;
    u32 xMax = (x0 > x1) ? x0 : x1;
    for (u32 x = xMin; x <= xMax; x++) {
        for (s32 dz = -1; dz <= 0; dz++) {
            s32 cz = static_cast<s32>(z0) + dz;
            if (cz < 0) continue;
            carveCell(x, static_cast<u32>(cz));
        }
    }

    // Vertical leg — 2 cells wide in X
    u32 zMin = (z0 < z1) ? z0 : z1;
    u32 zMax = (z0 > z1) ? z0 : z1;
    for (u32 z = zMin; z <= zMax; z++) {
        for (s32 dx = -1; dx <= 0; dx++) {
            s32 cx = static_cast<s32>(x1) + dx;
            if (cx < 0) continue;
            carveCell(static_cast<u32>(cx), z);
        }
    }
}

// Recursively split BSP
static void splitBSP(BSPNode* nodes, u32& nodeCount, u32 nodeIdx, GenRNG& rng, u32 depth) {
    BSPNode& node = nodes[nodeIdx];

    // Stop if too small or too deep
    if (depth > 5 || node.w < MIN_PARTITION_SIZE * 2 || node.d < MIN_PARTITION_SIZE * 2) {
        // Try splitting on the larger axis only
        if (node.w >= MIN_PARTITION_SIZE * 2 && depth <= 5) {
            // Can still split horizontally
        } else if (node.d >= MIN_PARTITION_SIZE * 2 && depth <= 5) {
            // Can still split vertically
        } else {
            return; // Leaf node
        }
    }

    if (nodeCount + 2 >= MAX_BSP_NODES) return;

    // Decide split direction: prefer splitting the longer axis
    bool splitHorizontal;
    if (node.w >= MIN_PARTITION_SIZE * 2 && node.d >= MIN_PARTITION_SIZE * 2) {
        // Both axes large enough, prefer longer axis with some randomness
        if (node.w > node.d + 2) splitHorizontal = true;
        else if (node.d > node.w + 2) splitHorizontal = false;
        else splitHorizontal = (rng.next() & 1) == 0;
    } else if (node.w >= MIN_PARTITION_SIZE * 2) {
        splitHorizontal = true;
    } else if (node.d >= MIN_PARTITION_SIZE * 2) {
        splitHorizontal = false;
    } else {
        return; // Can't split
    }

    u32 leftIdx  = nodeCount++;
    u32 rightIdx = nodeCount++;
    node.left  = static_cast<s32>(leftIdx);
    node.right = static_cast<s32>(rightIdx);

    if (splitHorizontal) {
        // Split along X axis
        u32 splitX = rng.range(node.x + MIN_PARTITION_SIZE, node.x + node.w - MIN_PARTITION_SIZE);
        nodes[leftIdx]  = {node.x, node.z, splitX - node.x, node.d, -1, -1, -1};
        nodes[rightIdx] = {splitX, node.z, node.x + node.w - splitX, node.d, -1, -1, -1};
    } else {
        // Split along Z axis
        u32 splitZ = rng.range(node.z + MIN_PARTITION_SIZE, node.z + node.d - MIN_PARTITION_SIZE);
        nodes[leftIdx]  = {node.x, node.z, node.w, splitZ - node.z, -1, -1, -1};
        nodes[rightIdx] = {node.x, splitZ, node.w, node.z + node.d - splitZ, -1, -1, -1};
    }

    splitBSP(nodes, nodeCount, leftIdx, rng, depth + 1);
    splitBSP(nodes, nodeCount, rightIdx, rng, depth + 1);
}

// Record a bidirectional room adjacency, de-duplicated and capped at 4 slots.
// Used for both true corridor links and bounding-box-touching rooms.
static void addAdjacency(DungeonRoom& a, u16 aIdx, DungeonRoom& b, u16 bIdx) {
    bool hasB = false;
    for (u8 k = 0; k < a.adjacentCount; k++) if (a.adjacentRooms[k] == bIdx) hasB = true;
    if (!hasB && a.adjacentCount < 4) a.adjacentRooms[a.adjacentCount++] = bIdx;
    bool hasA = false;
    for (u8 k = 0; k < b.adjacentCount; k++) if (b.adjacentRooms[k] == aIdx) hasA = true;
    if (!hasA && b.adjacentCount < 4) b.adjacentRooms[b.adjacentCount++] = aIdx;
}

// Find a room center in a subtree (for corridor connections). Also reports the
// representative room's index so callers can record the true corridor link.
static bool findRoomCenter(const BSPNode* nodes, s32 nodeIdx, const DungeonRoom* rooms,
                           u32& outCX, u32& outCZ, s32& outRoomIdx) {
    if (nodeIdx < 0) return false;
    const BSPNode& n = nodes[nodeIdx];
    if (n.roomIndex >= 0) {
        const DungeonRoom& r = rooms[n.roomIndex];
        outCX = r.x + r.w / 2;
        outCZ = r.z + r.d / 2;
        outRoomIdx = n.roomIndex;
        return true;
    }
    // Try left subtree first, then right
    if (findRoomCenter(nodes, n.left, rooms, outCX, outCZ, outRoomIdx)) return true;
    return findRoomCenter(nodes, n.right, rooms, outCX, outCZ, outRoomIdx);
}

// ---------------------------------------------------------------------------
// finalizeDungeon — the shared tail every style runs: supplement adjacency with
// bounding-box-touching rooms, pick spawn + exit, compute the world spawn pos.
// forcedSpawn/forcedExit (-1 = auto) let a style with a strong identity (the
// gauntlet's start→end run) override the generic dead-end/farthest heuristics.
// ---------------------------------------------------------------------------
static void finalizeDungeon(LevelGrid& grid, DungeonResult& result, const char* styleTag,
                            s32 forcedSpawn = -1, s32 forcedExit = -1)
{
    // Supplement corridor links (recorded by the carvers) with bounding-box-touching
    // rooms: two rooms also count as adjacent when their boxes, expanded by 3 cells,
    // overlap in both axes. addAdjacency de-dups, so corridor links are never doubled.
    // This adjacency drives the spawn/room exclusion and the squad alert system.
    for (u32 i = 0; i < result.roomCount; i++) {
        DungeonRoom& ri = result.rooms[i];
        for (u32 j = i + 1; j < result.roomCount; j++) {
            DungeonRoom& rj = result.rooms[j];
            bool xOverlap = (ri.x < rj.x + rj.w + 3) && (rj.x < ri.x + ri.w + 3);
            bool zOverlap = (ri.z < rj.z + rj.d + 3) && (rj.z < ri.z + ri.d + 3);
            // Rooms on different STORIES (FOUR_STORY stacks four same-XZ room sets 3 m apart) are
            // never neighbours — else all four mark mutually adjacent and spawnFloorEnemies (which
            // skips the spawn room plus its adjacent and 2-hop rooms) seeds ZERO enemies on the whole
            // floor. No-op for the flat styles, whose raised floors are all 0.5 m.
            if (xOverlap && zOverlap && fabsf(ri.floorHeight - rj.floorHeight) < 1.5f)
                addAdjacency(ri, static_cast<u16>(i), rj, static_cast<u16>(j));
        }
    }

    // --- Pick spawn and exit rooms — always succeeds ---
    // Spawn = room with fewest corridor exits (prefer dead-ends).
    // Exit  = farthest room from spawn.
    result.spawnRoomIdx = 0;
    result.exitRoomIdx  = 0;

    if (result.roomCount < 2) {
        if (result.roomCount > 0) {
            const DungeonRoom& r = result.rooms[0];
            result.spawnPos.x = (r.x + r.w * 0.5f) * grid.cellSize;
            result.spawnPos.y = r.floorHeight;
            result.spawnPos.z = (r.z + r.d * 0.5f) * grid.cellSize;
        }
        return;
    }

    // Count distinct corridor exits per room by scanning grid cells just
    // outside each wall. A "corridor exit" = contiguous run of floor cells.
    auto countCorridorExits = [&](const DungeonRoom& room) -> u32 {
        u32 exits = 0;
        auto scanEdge = [&](u32 fixedAxis, u32 start, u32 end, bool fixIsX) {
            bool inCorridor = false;
            for (u32 v = start; v < end; v++) {
                u32 gx = fixIsX ? v : fixedAxis;
                u32 gz = fixIsX ? fixedAxis : v;
                bool isFloor = LevelGridSystem::isInBounds(grid, gx, gz) &&
                               !LevelGridSystem::isSolid(grid, gx, gz);
                if (isFloor && !inCorridor) { exits++; inCorridor = true; }
                else if (!isFloor) inCorridor = false;
            }
        };
        if (room.z > 0)                scanEdge(room.z - 1,     room.x, room.x + room.w, true);  // north
        if (room.z + room.d < grid.depth) scanEdge(room.z + room.d, room.x, room.x + room.w, true);  // south
        if (room.x > 0)                scanEdge(room.x - 1,     room.z, room.z + room.d, false); // west
        if (room.x + room.w < grid.width) scanEdge(room.x + room.w, room.z, room.z + room.d, false); // east
        return exits;
    };

    u32 spawnIdx = 0;
    u32 fewestExits = 0xFFFFFFFF;
    if (forcedSpawn >= 0 && static_cast<u32>(forcedSpawn) < result.roomCount) {
        spawnIdx = static_cast<u32>(forcedSpawn);
        fewestExits = countCorridorExits(result.rooms[spawnIdx]);
    } else {
        // Pick spawn = room with fewest corridor exits (1 = dead-end ideal)
        for (u32 i = 0; i < result.roomCount; i++) {
            u32 ex = countCorridorExits(result.rooms[i]);
            if (ex < fewestExits) { fewestExits = ex; spawnIdx = i; }
        }
    }

    // Exit = room with greatest Euclidean distance from spawn center.
    // Euclidean is more reliable than BFS hops because the bounding-box
    // adjacency heuristic marks most rooms as 1 hop on small grids, making
    // BFS useless for distance ranking.
    f32 spawnCX = result.rooms[spawnIdx].x + result.rooms[spawnIdx].w * 0.5f;
    f32 spawnCZ = result.rooms[spawnIdx].z + result.rooms[spawnIdx].d * 0.5f;

    u32 exitIdx = (spawnIdx == 0) ? 1 : 0;
    f32 bestDistSq = 0.0f;
    if (forcedExit >= 0 && static_cast<u32>(forcedExit) < result.roomCount &&
        static_cast<u32>(forcedExit) != spawnIdx) {
        exitIdx = static_cast<u32>(forcedExit);
    } else {
        for (u32 i = 0; i < result.roomCount; i++) {
            if (i == spawnIdx) continue;
            f32 cx = result.rooms[i].x + result.rooms[i].w * 0.5f;
            f32 cz = result.rooms[i].z + result.rooms[i].d * 0.5f;
            f32 dx = cx - spawnCX, dz = cz - spawnCZ;
            f32 distSq = dx * dx + dz * dz;
            if (distSq > bestDistSq) {
                bestDistSq = distSq;
                exitIdx = i;
            }
        }
    }

    result.spawnRoomIdx = spawnIdx;
    result.exitRoomIdx  = exitIdx;

    const DungeonRoom& spawnRoom = result.rooms[spawnIdx];
    result.spawnPos.x = (spawnRoom.x + spawnRoom.w * 0.5f) * grid.cellSize;
    result.spawnPos.y = spawnRoom.floorHeight;
    result.spawnPos.z = (spawnRoom.z + spawnRoom.d * 0.5f) * grid.cellSize;

    LOG_INFO("LevelGen[%s]: %u rooms, spawn %u (%u exits), exit %u",
             styleTag, result.roomCount, spawnIdx, fewestExits, exitIdx);
}

// ---------------------------------------------------------------------------
// Style: BSP_ROOMS — the original recursive-partition rooms + L-corridors.
// ---------------------------------------------------------------------------
static void carveRoomsBSP(LevelGrid& grid, GenRNG& rng, DungeonResult& result)
{
    constexpr f32 CEIL = 3.0f;

    // Build BSP tree
    BSPNode nodes[MAX_BSP_NODES] = {};
    u32 nodeCount = 1;
    // Root node covers the usable area (1-cell border around edges)
    nodes[0] = {1, 1, grid.width - 2, grid.depth - 2, -1, -1, -1};

    splitBSP(nodes, nodeCount, 0, rng, 0);

    // Place rooms in leaf nodes
    for (u32 i = 0; i < nodeCount; i++) {
        BSPNode& node = nodes[i];
        if (node.left >= 0 || node.right >= 0) continue; // Not a leaf
        if (result.roomCount >= MAX_DUNGEON_ROOMS) break;

        // Random room size within partition (with margin)
        u32 maxW = node.w - ROOM_MARGIN * 2;
        u32 maxD = node.d - ROOM_MARGIN * 2;
        if (maxW < MIN_ROOM_SIZE || maxD < MIN_ROOM_SIZE) continue;

        u32 rw = rng.range(MIN_ROOM_SIZE, maxW + 1);
        u32 rd = rng.range(MIN_ROOM_SIZE, maxD + 1);
        u32 rx = rng.range(node.x + ROOM_MARGIN, node.x + node.w - ROOM_MARGIN - rw + 1);
        u32 rz = rng.range(node.z + ROOM_MARGIN, node.z + node.d - ROOM_MARGIN - rd + 1);

        // ~20% chance of raised floor, ~30% chance of brick walls
        f32 floorH = (rng.f01() < 0.2f) ? 0.5f : 0.0f;
        u8 wallMat = (rng.f01() < 0.3f) ? 3 : 0;

        carveArea(grid, rx, rz, rw, rd, floorH, CEIL, wallMat, 1, 2);

        DungeonRoom& room = result.rooms[result.roomCount];
        room.x = rx;
        room.z = rz;
        room.w = rw;
        room.d = rd;
        room.floorHeight = floorH;
        room.wallMat = wallMat;

        node.roomIndex = static_cast<s32>(result.roomCount);
        result.roomCount++;
    }

    // Connect sibling rooms with corridors
    for (u32 i = 0; i < nodeCount; i++) {
        BSPNode& node = nodes[i];
        if (node.left < 0 || node.right < 0) continue;

        u32 lx, lz, rx, rz; s32 lRoom = -1, rRoom = -1;
        if (findRoomCenter(nodes, node.left, result.rooms, lx, lz, lRoom) &&
            findRoomCenter(nodes, node.right, result.rooms, rx, rz, rRoom)) {
            carveLCorridor(grid, lx, lz, rx, rz, 0.0f, CEIL);
            // Record the TRUE corridor link between these two rooms (done before the
            // bbox pass in finalizeDungeon so real connections always claim the 4 slots).
            if (lRoom >= 0 && rRoom >= 0)
                addAdjacency(result.rooms[lRoom], static_cast<u16>(lRoom),
                             result.rooms[rRoom], static_cast<u16>(rRoom));
        }
    }
}

// ---------------------------------------------------------------------------
// Style: CAVERN — cellular-automata cave with rectangular clearings as rooms.
// The automata paints organic open space; the clearings guarantee the rect-room
// contract (spawners/chests/bosses need flat rects); monotone "drunken" tunnels
// chain every clearing so the floor is always traversable; a final flood fill
// closes any automata pocket the tunnels didn't reach (no strand-able space).
// ---------------------------------------------------------------------------
static void carveCavern(LevelGrid& grid, GenRNG& rng, DungeonResult& result)
{
    const u32 W = grid.width, D = grid.depth;
    if (W * D > GEN_MAX_CELLS) return; // scratch bound — falls back to BSP upstream

    static_assert(GEN_MAX_CELLS >= 48 * 48, "cavern scratch must cover the largest grid");
    u8 open[GEN_MAX_CELLS];
    u8 tmp[GEN_MAX_CELLS];
    auto at = [&](u8* buf, u32 x, u32 z) -> u8& { return buf[z * W + x]; };

    // 1) Noise seed: interior cells open at 55%, border always rock.
    for (u32 z = 0; z < D; z++)
        for (u32 x = 0; x < W; x++)
            at(open, x, z) = (x >= 1 && x < W - 1 && z >= 1 && z < D - 1 && rng.f01() < 0.55f) ? 1 : 0;

    // 2) Four smoothing passes of the classic 4-5 rule: a cell stays open when
    // its 3×3 neighborhood (self included) holds ≥5 open cells. This clumps the
    // noise into blobby chambers and necks.
    for (u32 pass = 0; pass < 4; pass++) {
        for (u32 z = 0; z < D; z++) {
            for (u32 x = 0; x < W; x++) {
                if (x < 1 || x >= W - 1 || z < 1 || z >= D - 1) { at(tmp, x, z) = 0; continue; }
                u32 n = 0;
                for (s32 dz = -1; dz <= 1; dz++)
                    for (s32 dx = -1; dx <= 1; dx++)
                        n += at(open, x + static_cast<u32>(dx), z + static_cast<u32>(dz));
                at(tmp, x, z) = (n >= 5) ? 1 : 0;
            }
        }
        std::memcpy(open, tmp, static_cast<size_t>(W) * D);
    }

    // 3) Stamp rectangular clearings — these ARE the rooms. Placed one per zone
    // of a jittered grid so they spread across the whole cave.
    u32 target = (W * D) / 180;                 // 48→12, 40→8, 32→5, 24→3
    if (target < 5) target = 5;
    if (target > MAX_DUNGEON_ROOMS) target = MAX_DUNGEON_ROOMS;
    u32 cols = 1; while (cols * cols < target) cols++;
    u32 rows = (target + cols - 1) / cols;

    for (u32 i = 0; i < target; i++) {
        u32 zoneX = i % cols, zoneZ = i / cols;
        u32 zoneW = (W - 2) / cols, zoneD = (D - 2) / rows;
        u32 rw = rng.range(5, 9), rd = rng.range(5, 9);
        // Jitter inside the zone, then clamp the rect fully inside the border.
        u32 cx = 1 + zoneX * zoneW + rng.range(0, zoneW > 0 ? zoneW : 1);
        u32 cz = 1 + zoneZ * zoneD + rng.range(0, zoneD > 0 ? zoneD : 1);
        u32 rx = (cx > rw / 2 + 1) ? cx - rw / 2 : 1;
        u32 rz = (cz > rd / 2 + 1) ? cz - rd / 2 : 1;
        if (rx + rw > W - 1) rx = W - 1 - rw;
        if (rz + rd > D - 1) rz = D - 1 - rd;

        for (u32 z = rz; z < rz + rd; z++)
            for (u32 x = rx; x < rx + rw; x++)
                at(open, x, z) = 1;

        DungeonRoom& room = result.rooms[result.roomCount];
        room = DungeonRoom{};
        room.x = rx; room.z = rz; room.w = rw; room.d = rd;
        room.floorHeight = 0.0f;                          // caves stay flat — no ledges
        room.wallMat = (rng.f01() < 0.3f) ? 3 : 0;        // same brick-variant ratio as BSP
        result.roomCount++;
    }

    // 4) Chain the clearings with 2-wide monotone tunnels (greedy nearest-unvisited
    // order). Each step moves one cell toward the target with a biased axis pick,
    // plus a rare sideways wiggle — winding, but guaranteed to arrive.
    bool visited[MAX_DUNGEON_ROOMS] = {};
    visited[0] = true;
    u32 current = 0;
    for (u32 linked = 1; linked < result.roomCount; linked++) {
        u32 best = 0xFFFFFFFF; u32 bestDist = 0xFFFFFFFF;
        const DungeonRoom& cur = result.rooms[current];
        s32 cx = static_cast<s32>(cur.x + cur.w / 2), cz = static_cast<s32>(cur.z + cur.d / 2);
        for (u32 j = 0; j < result.roomCount; j++) {
            if (visited[j]) continue;
            const DungeonRoom& rj = result.rooms[j];
            s32 dx = static_cast<s32>(rj.x + rj.w / 2) - cx;
            s32 dz = static_cast<s32>(rj.z + rj.d / 2) - cz;
            u32 dist = static_cast<u32>(dx * dx + dz * dz);
            if (dist < bestDist) { bestDist = dist; best = j; }
        }
        if (best == 0xFFFFFFFF) break;

        const DungeonRoom& to = result.rooms[best];
        s32 tx = static_cast<s32>(to.x + to.w / 2), tz = static_cast<s32>(to.z + to.d / 2);
        s32 x = cx, z = cz;
        u32 guard = 6 * static_cast<u32>((tx > x ? tx - x : x - tx) + (tz > z ? tz - z : z - tz)) + 32;
        while ((x != tx || z != tz) && guard-- > 0) {
            // Stamp a 2×2 footprint so the tunnel is walkable for full-width bodies.
            for (s32 sz = 0; sz <= 1; sz++)
                for (s32 sx = 0; sx <= 1; sx++) {
                    s32 px = x + sx, pz = z + sz;
                    if (px >= 1 && pz >= 1 && px < static_cast<s32>(W) - 1 && pz < static_cast<s32>(D) - 1)
                        at(open, static_cast<u32>(px), static_cast<u32>(pz)) = 1;
                }
            s32 dx = tx - x, dz = tz - z;
            f32 roll = rng.f01();
            if (roll < 0.15f && guard > 8) {
                // sideways wiggle — bounded by the guard, so termination holds
                if (dx == 0 || (dz != 0 && (rng.next() & 1))) x += (rng.next() & 1) ? 1 : -1;
                else                                          z += (rng.next() & 1) ? 1 : -1;
                if (x < 1) x = 1; if (x > static_cast<s32>(W) - 3) x = static_cast<s32>(W) - 3;
                if (z < 1) z = 1; if (z > static_cast<s32>(D) - 3) z = static_cast<s32>(D) - 3;
            } else {
                // biased toward the axis with more distance left
                bool stepX;
                if      (dx == 0) stepX = false;
                else if (dz == 0) stepX = true;
                else stepX = (roll < 0.65f) == ((dx > 0 ? dx : -dx) >= (dz > 0 ? dz : -dz));
                if (stepX) x += (dx > 0) ? 1 : -1;
                else       z += (dz > 0) ? 1 : -1;
            }
        }
        // Guard exhausted (extreme wiggle luck): finish with straight legs in the buffer.
        while (x != tx) { x += (tx > x) ? 1 : -1; at(open, static_cast<u32>(x), static_cast<u32>(z)) = 1; at(open, static_cast<u32>(x), static_cast<u32>(z + 1 < static_cast<s32>(D) - 1 ? z + 1 : z)) = 1; }
        while (z != tz) { z += (tz > z) ? 1 : -1; at(open, static_cast<u32>(x), static_cast<u32>(z)) = 1; at(open, static_cast<u32>(x + 1 < static_cast<s32>(W) - 1 ? x + 1 : x), static_cast<u32>(z)) = 1; }

        addAdjacency(result.rooms[current], static_cast<u16>(current),
                     result.rooms[best], static_cast<u16>(best));
        visited[best] = true;
        current = best;
    }

    // 5) Close unreachable automata pockets: flood from room 0 over the buffer
    // (4-connected, matching movement) and turn everything unmarked back to rock.
    // Prevents enemies/props/flow-field targets from landing in sealed bubbles.
    {
        std::memset(tmp, 0, static_cast<size_t>(W) * D);
        u32 stack[GEN_MAX_CELLS];
        u32 top = 0;
        u32 sx = result.rooms[0].x + result.rooms[0].w / 2;
        u32 sz = result.rooms[0].z + result.rooms[0].d / 2;
        stack[top++] = sz * W + sx;
        at(tmp, sx, sz) = 1;
        while (top > 0) {
            u32 c = stack[--top];
            u32 x = c % W, z = c / W;
            const s32 nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (u32 k = 0; k < 4; k++) {
                s32 nx = static_cast<s32>(x) + nb[k][0];
                s32 nz = static_cast<s32>(z) + nb[k][1];
                if (nx < 0 || nz < 0 || nx >= static_cast<s32>(W) || nz >= static_cast<s32>(D)) continue;
                u32 ux = static_cast<u32>(nx), uz = static_cast<u32>(nz);
                if (!at(open, ux, uz) || at(tmp, ux, uz)) continue;
                at(tmp, ux, uz) = 1;
                stack[top++] = uz * W + ux;
            }
        }
        for (u32 z = 0; z < D; z++)
            for (u32 x = 0; x < W; x++)
                if (at(open, x, z) && !at(tmp, x, z)) at(open, x, z) = 0;
    }

    // 6) Write the buffer into the real grid. Ceiling height steps between
    // 3.25-3.75 m in coarse patches for an uneven cave roof (the mesher renders
    // wall strips at height changes — reads as rock formations). One RNG draw
    // seeds the patch phase so different floors get different rooflines.
    const u32 roofPhase = rng.next();
    for (u32 z = 0; z < D; z++) {
        for (u32 x = 0; x < W; x++) {
            if (!at(open, x, z)) continue;
            GridCell& cell = LevelGridSystem::getCell(grid, x, z);
            cell.flags           = CELL_FLOOR | CELL_CEILING;
            cell.floorHeight     = 0;
            cell.ceilingHeight   = static_cast<u8>(13 + ((x / 3 + z / 3 + roofPhase) % 3));
            cell.wallMaterialId  = 0;
            cell.floorMaterialId = 1;
            cell.ceilMaterialId  = 2;
        }
    }
    // Clearings marked brick get their variant wall (retheme maps mat 3 onto the
    // tier's variant tile) — restamp their cells' wall material.
    for (u32 r = 0; r < result.roomCount; r++) {
        const DungeonRoom& room = result.rooms[r];
        if (room.wallMat == 0) continue;
        for (u32 z = room.z; z < room.z + room.d; z++)
            for (u32 x = room.x; x < room.x + room.w; x++)
                LevelGridSystem::getCell(grid, x, z).wallMaterialId = room.wallMat;
    }
}

// ---------------------------------------------------------------------------
// Style: GAUNTLET — a serpentine chain of arenas: one long fight to the exit.
// Rooms alternate connector→arena along a boustrophedon path across the grid;
// big arenas may grow a dead-end side pocket (the chest system rewards them).
// Spawn is forced to the chain's first room and exit to its last, so the floor
// always plays start-to-finish.
// ---------------------------------------------------------------------------
static void carveGauntlet(LevelGrid& grid, GenRNG& rng, DungeonResult& result,
                          s32& forcedSpawn, s32& forcedExit)
{
    const u32 W = grid.width, D = grid.depth;
    constexpr f32 CEIL = 3.0f;

    const u32 lanes   = (D >= 36) ? 3 : 2;   // serpentine rows
    const u32 perLane = (W >= 36) ? 3 : 2;   // arenas per row
    const u32 laneH   = (D - 2) / lanes;
    const u32 slotW   = (W - 2) / perLane;

    s32 prevIdx = -1;
    u32 lastMainIdx = 0;
    for (u32 lane = 0; lane < lanes; lane++) {
        for (u32 k = 0; k < perLane; k++) {
            if (result.roomCount >= MAX_DUNGEON_ROOMS) break;
            u32 slot = lane * perLane + k;
            // Boustrophedon: even lanes run left→right, odd lanes right→left,
            // so consecutive rooms are always physically near each other.
            u32 kk = (lane % 2 == 0) ? k : (perLane - 1 - k);

            // Alternate small connector rooms and big fight arenas along the chain.
            bool bigArena = (slot % 2 == 1);
            u32 rw = bigArena ? rng.range(8, 12) : rng.range(5, 8);
            u32 rd = bigArena ? rng.range(8, 12) : rng.range(5, 8);
            if (rd > laneH - 1) rd = laneH - 1;
            if (rw > slotW + 2) rw = slotW + 2;   // arenas may spill a bit into the next slot

            u32 cx = 1 + kk * slotW + slotW / 2 + rng.range(0, 3);
            u32 cz = 1 + lane * laneH + laneH / 2 + rng.range(0, 3);
            u32 rx = (cx > rw / 2 + 1) ? cx - rw / 2 : 1;
            u32 rz = (cz > rd / 2 + 1) ? cz - rd / 2 : 1;
            if (rx + rw > W - 1) rx = W - 1 - rw;
            if (rz + rd > D - 1) rz = D - 1 - rd;

            // Raised arenas read as fight platforms; connectors stay at 0 so the
            // chain never stacks two steps.
            f32 floorH = (bigArena && rng.f01() < 0.25f) ? 0.5f : 0.0f;
            u8  wallMat = (rng.f01() < 0.3f) ? 3 : 0;
            carveArea(grid, rx, rz, rw, rd, floorH, CEIL, wallMat, 1, 2);

            DungeonRoom& room = result.rooms[result.roomCount];
            room = DungeonRoom{};
            room.x = rx; room.z = rz; room.w = rw; room.d = rd;
            room.floorHeight = floorH;
            room.wallMat = wallMat;
            u32 thisIdx = result.roomCount++;
            lastMainIdx = thisIdx;

            if (prevIdx >= 0) {
                const DungeonRoom& prev = result.rooms[prevIdx];
                carveLCorridor(grid, prev.x + prev.w / 2, prev.z + prev.d / 2,
                               rx + rw / 2, rz + rd / 2, 0.0f, CEIL);
                addAdjacency(result.rooms[prevIdx], static_cast<u16>(prevIdx),
                             result.rooms[thisIdx], static_cast<u16>(thisIdx));
            }
            prevIdx = static_cast<s32>(thisIdx);
        }
    }

    // Side pockets: dead-end 5×5 treasure nooks off the big arenas, pushed toward
    // whichever Z edge has room. 40% per arena, but at least one per floor — on
    // the 2×2 small-grid chain that single pocket is also what keeps the room
    // count above the degenerate-carve fallback threshold in generate().
    u32 mainCount = result.roomCount;
    auto tryCarvePocket = [&](u32 r) -> bool {
        if (result.roomCount >= MAX_DUNGEON_ROOMS) return false;
        const DungeonRoom main = result.rooms[r];
        if (main.w < 8) return false;                // arenas only

        u32 pw = 5, pd = 5;
        u32 px = main.x + main.w / 2 - pw / 2;
        bool north = main.z > pd + 3;
        u32 pz = north ? main.z - pd - 2 : main.z + main.d + 2;
        if (!north && pz + pd > D - 1) return false; // no room on either side
        if (px + pw > W - 1) px = W - 1 - pw;
        if (px < 1) px = 1;

        carveArea(grid, px, pz, pw, pd, 0.0f, CEIL, (rng.f01() < 0.3f) ? 3 : 0, 1, 2);
        DungeonRoom& pocket = result.rooms[result.roomCount];
        pocket = DungeonRoom{};
        pocket.x = px; pocket.z = pz; pocket.w = pw; pocket.d = pd;
        pocket.floorHeight = 0.0f;
        pocket.wallMat = LevelGridSystem::getCell(grid, px, pz).wallMaterialId;
        u32 pocketIdx = result.roomCount++;

        carveLCorridor(grid, main.x + main.w / 2, main.z + main.d / 2,
                       px + pw / 2, pz + pd / 2, 0.0f, CEIL);
        addAdjacency(result.rooms[r], static_cast<u16>(r),
                     result.rooms[pocketIdx], static_cast<u16>(pocketIdx));
        return true;
    };
    u32 pockets = 0;
    for (u32 r = 0; r < mainCount; r++)
        if (rng.f01() < 0.4f && tryCarvePocket(r)) pockets++;
    if (pockets == 0)
        for (u32 r = 0; r < mainCount && pockets == 0; r++)
            if (tryCarvePocket(r)) pockets++;

    // The gauntlet's identity: enter at the chain head, exit past the final arena.
    forcedSpawn = 0;
    forcedExit  = static_cast<s32>(lastMainIdx);
}

// ---------------------------------------------------------------------------
// Style: HUB — one grand central chamber with spoke corridors out to perimeter
// vaults, plus a ring connecting neighboring vaults (real loops — the only style
// with multiple routes to everywhere). Pillars inside the hub give cover.
// Vault directions use integer compass offsets, NOT cos/sin — libm results are
// not bit-identical across platforms and this must match on host and client.
// ---------------------------------------------------------------------------
static void carveHub(LevelGrid& grid, GenRNG& rng, DungeonResult& result)
{
    const u32 W = grid.width, D = grid.depth;
    constexpr f32 CEIL = 3.0f;

    const u32 cx = W / 2, cz = D / 2;
    u32 hubSize = (W < D ? W : D) / 4;
    if (hubSize < 8)  hubSize = 8;
    if (hubSize > 13) hubSize = 13;
    const u32 hx = cx - hubSize / 2, hz = cz - hubSize / 2;

    // The hub gets a tall 4 m ceiling and a 50% shot at the variant wall — it's
    // the floor's centerpiece.
    u8 hubWall = (rng.f01() < 0.5f) ? 3 : 0;
    carveArea(grid, hx, hz, hubSize, hubSize, 0.0f, 4.0f, hubWall, 1, 2);
    {
        DungeonRoom& hub = result.rooms[result.roomCount];
        hub = DungeonRoom{};
        hub.x = hx; hub.z = hz; hub.w = hubSize; hub.d = hubSize;
        hub.floorHeight = 0.0f;
        hub.wallMat = hubWall;
        result.roomCount++;
    }

    // Compass directions as integer offsets; 181/256 ≈ 0.707 for the diagonals so
    // diagonal vaults sit at the same radius as cardinal ones.
    const s32 kDirs[8][2] = {{256, 0}, {181, 181}, {0, 256}, {-181, 181},
                             {-256, 0}, {-181, -181}, {0, -256}, {181, -181}};
    const u32 vaultCount = (W >= 40) ? 8 : 6;
    // 6-vault floors skip one diagonal per side so the layout stays roughly even.
    const u32 kSix[6] = {0, 1, 2, 4, 5, 6};

    const s32 radius = static_cast<s32>(((W < D ? W : D) / 2) - 6);
    for (u32 i = 0; i < vaultCount && result.roomCount < MAX_DUNGEON_ROOMS; i++) {
        u32 dir = (vaultCount == 8) ? i : kSix[i];
        u32 vw = rng.range(5, 8), vd = rng.range(5, 8);
        s32 vcx = static_cast<s32>(cx) + (kDirs[dir][0] * radius) / 256;
        s32 vcz = static_cast<s32>(cz) + (kDirs[dir][1] * radius) / 256;
        // Small jitter keeps repeated hub floors from feeling stamped.
        vcx += static_cast<s32>(rng.range(0, 3)) - 1;
        vcz += static_cast<s32>(rng.range(0, 3)) - 1;

        s32 rx = vcx - static_cast<s32>(vw) / 2;
        s32 rz = vcz - static_cast<s32>(vd) / 2;
        if (rx < 1) rx = 1;
        if (rz < 1) rz = 1;
        if (rx + static_cast<s32>(vw) > static_cast<s32>(W) - 1) rx = static_cast<s32>(W) - 1 - static_cast<s32>(vw);
        if (rz + static_cast<s32>(vd) > static_cast<s32>(D) - 1) rz = static_cast<s32>(D) - 1 - static_cast<s32>(vd);

        // 30% of vaults sit on a raised dais (spokes carve at 0 — one step up).
        f32 floorH = (rng.f01() < 0.3f) ? 0.5f : 0.0f;
        u8  wallMat = (rng.f01() < 0.3f) ? 3 : 0;
        carveArea(grid, static_cast<u32>(rx), static_cast<u32>(rz), vw, vd, floorH, CEIL, wallMat, 1, 2);

        DungeonRoom& vault = result.rooms[result.roomCount];
        vault = DungeonRoom{};
        vault.x = static_cast<u32>(rx); vault.z = static_cast<u32>(rz);
        vault.w = vw; vault.d = vd;
        vault.floorHeight = floorH;
        vault.wallMat = wallMat;
        u32 vaultIdx = result.roomCount++;

        // Spoke: hub center → vault center (L-corridor only carves solid cells,
        // so it passes over the already-open hub interior untouched).
        carveLCorridor(grid, cx, cz,
                       vault.x + vault.w / 2, vault.z + vault.d / 2, 0.0f, CEIL);
        addAdjacency(result.rooms[0], 0, result.rooms[vaultIdx], static_cast<u16>(vaultIdx));
    }

    // Ring: connect each vault to the next, creating loops (flanking routes the
    // tree-shaped BSP floors never have).
    for (u32 i = 1; i < result.roomCount; i++) {
        u32 j = (i == result.roomCount - 1) ? 1 : i + 1;
        const DungeonRoom& a = result.rooms[i];
        const DungeonRoom& b = result.rooms[j];
        carveLCorridor(grid, a.x + a.w / 2, a.z + a.d / 2,
                       b.x + b.w / 2, b.z + b.d / 2, 0.0f, CEIL);
        addAdjacency(result.rooms[i], static_cast<u16>(i), result.rooms[j], static_cast<u16>(j));
    }

    // Cover pillars inside the hub, inset from the corners. They're small solid
    // islands in an ≥8-wide chamber, so they can never seal a path (walkers just
    // round them), and they keep clear of the hub's center rows where the exit
    // portal / boss land. Stamped LAST so no corridor re-opens them.
    {
        const u32 inset = hubSize / 4;
        const u32 pSize = (hubSize >= 11) ? 2 : 1;
        const u32 lo = inset, hi = hubSize - inset - pSize;
        const u32 corners[4][2] = {{lo, lo}, {hi, lo}, {lo, hi}, {hi, hi}};
        for (u32 p = 0; p < 4; p++) {
            for (u32 dz = 0; dz < pSize; dz++)
                for (u32 dx = 0; dx < pSize; dx++) {
                    GridCell& cell = LevelGridSystem::getCell(grid, hx + corners[p][0] + dx,
                                                               hz + corners[p][1] + dz);
                    cell.flags = CELL_SOLID;
                }
        }
    }
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Style: VERTICAL_HALL — "The Stacked Loop". NOT a single arena — a Quake *location-based* topology: a
// LOOP of nine distinct areas laid out 3×3, stacked across two stories and circled by a route that
// spirals up and down. The four CORNERS are ground rooms (floor 0, cover pillars); the four MID-SIDES
// are BALCONIES (`CELL_PLATFORM` slabs @ 3 m — walk ON top, walk UNDER the arcade beneath); the CENTRE
// is an open VOID (ground, no balcony) that every balcony overlooks and drops into (the Quake sightline
// + a cross-level shortcut). A PINWHEEL of four RAMPS climbs each corner up to the next balcony (the
// enemies' chase-up route and yours after a drop, recorded as StoryPortals that also seat the snipers);
// two CATWALKS cross the void @ 3 m linking opposite balconies (one INTACT, one BROKEN — a jump gap),
// so the UPPER story is its own connected loop too. Circle the ring and you continuously ASCEND (corner
// →ramp→balcony) and DESCEND (balcony→drop→void/corner); the exit sits on the FAR side AND the opposite
// STORY, so every floor forces a full traversal and a level change. Enemies spawn on the ground story
// (corners + void + the arcades under the balconies) and chase up the ramps; snipers hold the balconies
// and plunge-fire into the void. The ground story is one fully-connected floor (trivially reachable);
// the upper story hangs off it via ramps + catwalks. Two full stories, seed-built ⇒ replicates in co-op
// with no wire change. Slab/ramp primitives pinned by test_platform; loop invariants by test_vertical_hall.
// ---------------------------------------------------------------------------
static constexpr u8  VH_SLAB_Q = 12;   // balcony floor: 12 qu = 3.0 m (2.5 m underside clears a body)
static constexpr f32 VH_CEIL   = 8.0f; // tall hall: generous headroom above the 3 m balconies
static constexpr u32 VH_RAMP   = 12;   // ramp length (cells): climbs 0→3 m at 0.25 m/step (walkable by bodies)

static void carveVerticalHall(LevelGrid& grid, GenRNG& rng, DungeonResult& result,
                              s32& forcedSpawn, s32& forcedExit) {
    const u32 W = grid.width, D = grid.depth;
    if (W < 40 || D < 40) return;   // needs a big grid → else BSP fallback

    const u32 hx = 1, hz = 1, hw = W - 2, hd = D - 2;
    const f32 cs = grid.cellSize;
    const u8  wallMat = (rng.f01() < 0.3f) ? 3 : 0;

    // 1) LOWER story: the ENTIRE interior at floor 0, tall ceiling. One fully-connected ground floor —
    //    the corners, the void, and the arcades under the balconies are all the same walkable story, so
    //    reachability is guaranteed no matter what hangs above.
    carveArea(grid, hx, hz, hw, hd, 0.0f, VH_CEIL, wallMat, 1, 2);
    const u8 floorMat = LevelGridSystem::getCell(grid, hx + 1, hz + 1).floorMaterialId;

    auto slab = [&](s32 x, s32 z, u8 q) {
        if (x < 0 || z < 0 || !LevelGridSystem::isInBounds(grid, (u32)x, (u32)z)) return;
        GridCell& c = LevelGridSystem::getCell(grid, (u32)x, (u32)z);
        LevelGridSystem::setPlatform(c, q, floorMat);   // REPLACE-to-single (was: |=CELL_PLATFORM; platHeight=q; platMaterialId=floorMat)
    };
    auto slabRect = [&](u32 x0, u32 x1, u32 z0, u32 z1, u8 q) {
        for (u32 z = z0; z <= z1; z++) for (u32 x = x0; x <= x1; x++) slab((s32)x, (s32)z, q);
    };

    // 3×3 band boundaries (thirds of the interior). Corners + mid-sides + centre void.
    const u32 ax = hw / 3, az = hd / 3;
    const u32 wX0 = hx,          wX1 = hx + ax - 1;           // west band  [1..14]
    const u32 cX0 = hx + ax,     cX1 = hx + 2 * ax - 1;       // centre     [15..28]
    const u32 eX0 = hx + 2 * ax, eX1 = hx + hw - 1;           // east band  [29..42]
    const u32 nZ0 = hz,          nZ1 = hz + az - 1;           // north band
    const u32 cZ0 = hz + az,     cZ1 = hz + 2 * az - 1;       // centre
    const u32 sZ0 = hz + 2 * az, sZ1 = hz + hd - 1;           // south band

    // 2) The four MID-SIDE BALCONIES @ 3 m (each fills its whole band region → its inner edge meets the
    //    void, its outer edge the wall; the ground floor survives beneath as the walk-under arcade).
    slabRect(cX0, cX1, nZ0, nZ1, VH_SLAB_Q);   // N balcony (north of the void)
    slabRect(cX0, cX1, sZ0, sZ1, VH_SLAB_Q);   // S balcony
    slabRect(wX0, wX1, cZ0, cZ1, VH_SLAB_Q);   // W balcony
    slabRect(eX0, eX1, cZ0, cZ1, VH_SLAB_Q);   // E balcony

    // 3) PINWHEEL RAMPS: each climbs from a corner floor up to the adjacent balcony edge (top cell abuts
    //    the balcony @ h12; foot @ h1 abuts the corner floor). 2-wide, recorded as StoryPortals (the
    //    cross-story chase + the sniper seats). The four together rotate the same way → an asymmetric spin.
    auto ramp = [&](s32 xf, s32 zf, s32 dx, s32 dz, s32 px, s32 pz) {
        s32 tx = xf, tz = zf;
        for (u32 i = 0; i < VH_RAMP; i++) {
            u8 h = (u8)(i + 1); if (h > VH_SLAB_Q) h = VH_SLAB_Q;
            tx = xf + dx * (s32)i; tz = zf + dz * (s32)i;
            slab(tx, tz, h); slab(tx + px, tz + pz, h);   // 2-wide across the climb
        }
        Vec3 foot = { (xf + 0.5f) * cs, 0.0f,             (zf + 0.5f) * cs };
        Vec3 top  = { (tx + 0.5f) * cs, VH_SLAB_Q * 0.25f, (tz + 0.5f) * cs };
        if (result.portalCount < MAX_STORY_PORTALS) result.portals[result.portalCount++] = { foot, top };
    };
    ramp((s32)wX0 + 2, (s32)nZ0 + 6, +1,  0, 0, 1);   // NW corner → N balcony (climb east)
    ramp((s32)eX1 - 6, (s32)nZ0 + 2,  0, +1, 1, 0);   // NE corner → E balcony (climb south)
    ramp((s32)eX1 - 2, (s32)sZ1 - 6, -1,  0, 0, 1);   // SE corner → S balcony (climb west)
    ramp((s32)wX0 + 5, (s32)sZ1 - 2,  0, -1, 1, 0);   // SW corner → W balcony (climb north)

    // 4) CATWALKS across the void @ 3 m linking OPPOSITE balconies, so the upper story is its own loop.
    //    N↔S is INTACT (the reliable high road); W↔E is BROKEN with a 2-cell JUMP gap (miss → fall to the
    //    void). They cross at the centre, so all four balconies interconnect up top.
    const u32 mx = (cX0 + cX1) / 2, mz = (cZ0 + cZ1) / 2;
    slabRect(mx, mx + 1, nZ1, sZ0, VH_SLAB_Q);                       // N↔S catwalk (intact)
    for (u32 x = wX1; x <= eX0; x++)                                 // W↔E catwalk (broken jump gap)
        if (x < cX0 + 1 || x > cX0 + 2) { slab((s32)x, (s32)mz, VH_SLAB_Q); slab((s32)x, (s32)mz + 1, VH_SLAB_Q); }

    // 5) COVER PILLARS — floor-to-ceiling solid columns in the four corner rooms and the void, breaking
    //    line-of-sight for the ground fight. Only on BARE floor (never a slab), so a balcony/ramp/catwalk
    //    is never punched through; 2×2 and sparse, so a corner or the void can never be sealed.
    auto pillars = [&](u32 rx0, u32 rx1, u32 rz0, u32 rz1, u32 n) {
        for (u32 k = 0; k < n; k++) {
            u32 px = rng.range(rx0, rx1), pz = rng.range(rz0, rz1);
            for (u32 dz = 0; dz < 2; dz++) for (u32 dx = 0; dx < 2; dx++) {
                if (!LevelGridSystem::isInBounds(grid, px + dx, pz + dz)) continue;
                GridCell& c = LevelGridSystem::getCell(grid, px + dx, pz + dz);
                if (!(c.flags & CELL_PLATFORM)) c.flags = CELL_SOLID;
            }
        }
    };
    pillars(wX0 + 1, wX1 - 2, nZ0 + 1, nZ1 - 2, 2);   // NW room
    pillars(eX0 + 1, eX1 - 2, nZ0 + 1, nZ1 - 2, 2);   // NE room
    pillars(eX0 + 1, eX1 - 2, sZ0 + 1, sZ1 - 2, 2);   // SE room
    pillars(wX0 + 1, wX1 - 2, sZ0 + 1, sZ1 - 2, 2);   // SW room
    pillars(cX0 + 2, cX1 - 3, cZ0 + 2, cZ1 - 3, 3);   // the void

    // 6) ONE player jump-pad in the void, on the SPAWN side (placed after the endpoints are chosen,
    //    below). Two pads under the catwalk crossing were the floor's biggest shortcut: pad →
    //    air-steer onto a catwalk → exit balcony in seconds, skipping the loop entirely. A single
    //    spawn-side pad keeps the recovery role — it puts you ON the upper ring near the start —
    //    while the ring itself (balcony → catwalk → balcony) still has to be walked.

    // 7) Rooms for enemy/loot placement — the nine areas, all at floor 0 (enemies seed on the ground
    //    story; snipers get lifted onto the balconies by spawnFloorNests via the ramp portals).
    auto addRoom = [&](u32 rx, u32 rz, u32 rw, u32 rd) -> u32 {
        DungeonRoom& r = result.rooms[result.roomCount];
        r = DungeonRoom{}; r.x = rx; r.z = rz; r.w = rw; r.d = rd; r.floorHeight = 0.0f; r.wallMat = wallMat;
        return result.roomCount++;
    };
    const u32 rNW = addRoom(wX0, nZ0, ax, az), rNE = addRoom(eX0, nZ0, ax, az);
    const u32 rSE = addRoom(eX0, sZ0, ax, az), rSW = addRoom(wX0, sZ0, ax, az);
    (void) addRoom(cX0, cZ0, ax, az);                                   // the void (index 4)
    const u32 rMN = addRoom(cX0, nZ0, ax, az), rME = addRoom(eX0, cZ0, ax, az);
    const u32 rMS = addRoom(cX0, sZ0, ax, az), rMW = addRoom(wX0, cZ0, ax, az);

    // Endpoints: a GROUND corner and a BALCONY mid on the FAR side (a mid this corner does NOT touch), so
    // spawn and exit are always on opposite sides AND opposite stories. A coin-flip picks whether you
    // ASCEND (spawn in the corner, exit up on the balcony) or DESCEND (spawn on the balcony, exit down in
    // the corner). startGame applies spawnBalconyPos/exitBalconyPos verbatim (upper = y 3 m, ground = y 0).
    const Vec3 cornerPos[4] = {   // NW, NE, SE, SW — inset from the true corner, clear of the ramps
        { (wX0 + 3.5f) * cs, 0.0f, (nZ0 + 3.5f) * cs }, { (eX1 - 2.5f) * cs, 0.0f, (nZ0 + 3.5f) * cs },
        { (eX1 - 2.5f) * cs, 0.0f, (sZ1 - 2.5f) * cs }, { (wX0 + 3.5f) * cs, 0.0f, (sZ1 - 2.5f) * cs } };
    const Vec3 midPos[4] = {       // N, E, S, W balcony centres (y = 3 m)
        { (cX0 + ax * 0.5f) * cs, VH_SLAB_Q * 0.25f, (nZ0 + az * 0.5f) * cs },
        { (eX0 + ax * 0.5f) * cs, VH_SLAB_Q * 0.25f, (cZ0 + az * 0.5f) * cs },
        { (cX0 + ax * 0.5f) * cs, VH_SLAB_Q * 0.25f, (sZ0 + az * 0.5f) * cs },
        { (wX0 + ax * 0.5f) * cs, VH_SLAB_Q * 0.25f, (cZ0 + az * 0.5f) * cs } };
    const u32 cornerRoom[4] = { rNW, rNE, rSE, rSW };
    const u32 midRoom[4]    = { rMN, rME, rMS, rMW };
    const u32 cSel   = rng.range(0, 4);
    // The pinwheel serves balcony i from corner i (NW→N, NE→E, SE→S, SW→W), so the mid must be the
    // one served by the corner DIAGONAL to the endpoint corner: (cSel+2)%4. The old "either far mid"
    // roll could pick a balcony whose ramp foot was ONE band from spawn — run a band, ramp up, done:
    // Aaron's "5 second walk". Diagonal service forces the route the design promised — cross the
    // whole floor (through the pillared void, under the sniper balconies) to the far corner, THEN
    // climb; or take the spawn-side pad up and walk the upper ring with its catwalk jump. Both are
    // the loop. (The mid (cSel+2)%4 never touches corner cSel, so opposite-side is preserved.)
    const u32 mSel   = (cSel + 2) % 4;
    const bool exitUp = rng.f01() < 0.5f;                            // true → ascend to the exit

    // The single pad: void quadrant nearest the GROUND start of the journey (ascend: the spawn
    // corner; descend: the drop zone under the spawn balcony = the diagonal corner's quadrant).
    {
        const u32 padCorner = exitUp ? cSel : (cSel + 2) % 4;
        const u32 pqx = (padCorner == 1 || padCorner == 2) ? cX1 - 2 : cX0 + 2;
        const u32 pqz = (padCorner >= 2)                   ? cZ1 - 2 : cZ0 + 2;
        // Deterministic 3x3 scan: the ideal cell can hold a void pillar, and a silently-skipped pad
        // would strand a fallen player with only the ramps. First open cell wins.
        for (s32 dz = 0; dz < 3; dz++) {
            for (s32 dx = 0; dx < 3; dx++) {
                const u32 x = pqx + (u32)dx, z = pqz + (u32)dz;
                if (!LevelGridSystem::isInBounds(grid, x, z)) continue;
                GridCell& c = LevelGridSystem::getCell(grid, x, z);
                if (c.flags & (CELL_PLATFORM | CELL_LEDGE | CELL_SOLID)) continue;
                c.flags |= CELL_JUMPPAD;
                dz = 3; break;                             // placed — stop both loops
            }
        }
    }
    result.spawnBalconyPos = exitUp ? cornerPos[cSel] : midPos[mSel];
    result.exitBalconyPos  = exitUp ? midPos[mSel]    : cornerPos[cSel];
    result.spawnOnUpper    = !exitUp;
    forcedSpawn = (s32)(exitUp ? cornerRoom[cSel] : midRoom[mSel]);
    forcedExit  = (s32)(exitUp ? midRoom[mSel]    : cornerRoom[cSel]);

    // A body must never start inside a pillar: open a 2-cell pad around the GROUND endpoint (the balcony
    // endpoint is a slab, which pillars skip). Clears pillars AND any stray jump-pad.
    auto clearPad = [&](Vec3 p) {
        if (p.y > 1.0f) return;
        u32 gx, gz; if (!LevelGridSystem::worldToGrid(grid, p, gx, gz)) return;
        for (s32 dz = -2; dz <= 2; dz++) for (s32 dx = -2; dx <= 2; dx++) {
            s32 x = (s32)gx + dx, z = (s32)gz + dz;
            if (x < 0 || z < 0 || !LevelGridSystem::isInBounds(grid, (u32)x, (u32)z)) continue;
            GridCell& c = LevelGridSystem::getCell(grid, (u32)x, (u32)z);
            if (c.flags & (CELL_SOLID | CELL_JUMPPAD)) {
                c.flags = CELL_FLOOR | CELL_CEILING; c.floorHeight = 0;
                c.ceilingHeight = (u8)(VH_CEIL / 0.25f); c.floorMaterialId = floorMat;
            }
        }
    };
    clearPad(result.spawnBalconyPos);
    clearPad(result.exitBalconyPos);

    // A room's CENTRE cell must never be solid: consumers place lights, the exit portal and loot
    // there, and the generic reachability contract floods to it. The cover pillars above are scattered
    // by RNG inside the room rects, so one can land exactly on a centre — clear any that did. (One
    // pillar out of two per room; the cover pattern is unaffected.)
    for (u32 i = 0; i < result.roomCount; i++) {
        const DungeonRoom& r = result.rooms[i];
        const u32 cx = r.x + r.w / 2, cz = r.z + r.d / 2;
        if (!LevelGridSystem::isInBounds(grid, cx, cz)) continue;
        GridCell& c = LevelGridSystem::getCell(grid, cx, cz);
        if (!(c.flags & CELL_SOLID)) continue;
        c.flags           = CELL_FLOOR | CELL_CEILING;
        c.floorHeight     = (u8)(r.floorHeight / 0.25f);
        c.ceilingHeight   = (u8)(VH_CEIL / 0.25f);
        c.floorMaterialId = floorMat;
    }
}

// ---------------------------------------------------------------------------
// Style: FOUR_STORY — "The Descent". A four-story MAZE stacked on ONE footprint: the L0 ground plus
// three CELL_PLATFORM slab stories at 3/6/9 m, all sharing a single full-height wall skeleton (a
// braided recursive-backtracker maze of 3-wide corridors). Every story is the same labyrinth walked
// at a different height, so you re-read a space you already know from a new altitude. You enter on L3
// in one corner and must reach the L0 exit DIAGONALLY OPPOSITE, and the only way down is through the
// floor:
//   * DROP HOLES  — >= 2 cells across. At the 6 m/s base speed a jump reaches 2.4 m and a 0.6 m body
//                   needs gap+0.6 of clearance, so a >= 2 m gap CANNOT be cleared: it is a committed
//                   descent of exactly one story.
//   * JUMP GAPS   — exactly 1 cell across (needs 1.6 m of the 2.4 m reach). Clear them to keep your
//                   height, or blow the timing and lose a story. This is the maze's own hazard.
//   * JUMP PADS   — CELL_JUMPPAD cells in dead-end nodes, authored via GridCell::jumpPadQ at ~2
//                   stories of lift, so a bad fall is recoverable and the maze is not a punish.
//                   A pad cell is a pad on every story it carries — a vertical landmark column.
// There are no ramps or stairs: portalCount stays 0 and descent is one-way by design.
//
// Express shafts are impossible BY CONSTRUCTION: a hole is punched at level L only where the slab at
// L+1 is still intact, so no column is ever open through two stories and a fall always lands exactly
// one story down. The grid itself is the ledger — a holed cell simply has no slab at that height — so
// the rule costs no memory and cannot drift out of sync with the geometry.
//
// GenRNG + integer quarter-units only => byte-identical host/client grids, so the whole floor
// replicates in co-op with NO wire or save change. Contract pinned by tests/world/test_four_story.cpp.
// ---------------------------------------------------------------------------
static constexpr u8  FS_L1_Q   = 12;   // slab tops: L1 @ 3 m
static constexpr u8  FS_L2_Q   = 24;   //            L2 @ 6 m
static constexpr u8  FS_L3_Q   = 36;   //            L3 @ 9 m
static constexpr u8  FS_CEIL_Q = 48;   // 12 m ceiling — clears L3 @ 9 m plus a 1.8 m body
static constexpr u32 FS_CORR   = 3;    // corridor width in cells (a 0.6 m body plus combat spacing)
static constexpr u32 FS_PERIOD = FS_CORR + 1;   // corridor + its 1-cell wall
static constexpr u32 FS_MAX_NODES = 16;         // lattice cap (a 48-grid uses 11x11)
static constexpr u8  FS_PAD_Q  = 92;   // 23 m/s -> 6.6 m apex: a pad lifts ~two stories

// True if this cell still carries a slab whose top is exactly `q` quarter-units.
static bool fsHasSlab(const GridCell& c, u8 q) {
    for (u8 i = 0; i < c.platCount; i++)
        if (c.platHeight[i] == q) return true;
    return false;
}

static void carveFourStory(LevelGrid& grid, GenRNG& rng, DungeonResult& result,
                           s32& forcedSpawn, s32& forcedExit) {
    const u32 W = grid.width, D = grid.depth;
    if (W < 40 || D < 40) return;   // too small -> generate() sees roomCount==0 (<5) and re-carves BSP

    const f32 cs = grid.cellSize;
    const u8  wallMat = (rng.f01() < 0.3f) ? 3 : 0;

    // 1) Open the whole interior at ground level; the maze walls are stamped back in as SOLID below.
    carveArea(grid, 1, 1, W - 2, D - 2, 0.0f, FS_CEIL_Q * 0.25f, wallMat, 1, 2);
    const u8 floorMat = LevelGridSystem::getCell(grid, 2, 2).floorMaterialId;

    u32 nx = (W - 2) / FS_PERIOD, nz = (D - 2) / FS_PERIOD;
    if (nx > FS_MAX_NODES) nx = FS_MAX_NODES;
    if (nz > FS_MAX_NODES) nz = FS_MAX_NODES;

    // The lattice rarely divides the grid evenly; CENTRE it so the remainder splits between the two
    // sides as wall bulk instead of piling into one fat dead band on the +x/+z edges.
    const u32 offX = 1 + ((W - 2) - (nx * FS_PERIOD - 1)) / 2;
    const u32 offZ = 1 + ((D - 2) - (nz * FS_PERIOD - 1)) / 2;
    // Node (i,j) owns the FS_CORR-square corridor block at (offX+i*FS_PERIOD, offZ+j*FS_PERIOD); the
    // cells one past it are the walls it shares with its neighbours.
    auto nodeX0 = [&](u32 i) -> u32 { return offX + i * FS_PERIOD; };
    auto nodeZ0 = [&](u32 j) -> u32 { return offZ + j * FS_PERIOD; };

    // Everything interior starts SOLID; node blocks and knocked-down walls carve it back open. Cells
    // past the last node (the lattice rarely divides the grid evenly) stay solid as outer wall bulk.
    for (u32 z = 1; z < D - 1; z++)
        for (u32 x = 1; x < W - 1; x++)
            LevelGridSystem::getCell(grid, x, z).flags = CELL_SOLID;

    auto openRect = [&](u32 x0, u32 z0, u32 w, u32 d) {
        for (u32 z = z0; z < z0 + d; z++)
            for (u32 x = x0; x < x0 + w; x++)
                if (LevelGridSystem::isInBounds(grid, x, z))
                    LevelGridSystem::getCell(grid, x, z).flags = CELL_FLOOR | CELL_CEILING;
    };
    for (u32 j = 0; j < nz; j++)
        for (u32 i = 0; i < nx; i++)
            openRect(nodeX0(i), nodeZ0(j), FS_CORR, FS_CORR);

    // 2) Maze over the node lattice: iterative recursive-backtracker (an explicit stack, so no
    //    recursion and no heap), then BRAIDED by reopening a few extra walls. A perfect maze has
    //    exactly one route between any two points, which reads as tedious and gives combat nowhere to
    //    flow; the braids add loops so you can circle an enemy and pick between routes.
    const s32 dxk[4] = {1, -1, 0, 0}, dzk[4] = {0, 0, 1, -1};
    auto openWall = [&](u32 i, u32 j, u32 k) {
        if      (k == 0) openRect(nodeX0(i) + FS_CORR, nodeZ0(j), 1, FS_CORR);
        else if (k == 1) openRect(nodeX0(i) - 1,       nodeZ0(j), 1, FS_CORR);
        else if (k == 2) openRect(nodeX0(i), nodeZ0(j) + FS_CORR, FS_CORR, 1);
        else             openRect(nodeX0(i), nodeZ0(j) - 1,       FS_CORR, 1);
    };
    u8  visited[FS_MAX_NODES * FS_MAX_NODES] = {};
    u16 stk[FS_MAX_NODES * FS_MAX_NODES];
    u32 top = 0;
    auto nidx = [&](u32 i, u32 j) -> u32 { return j * nx + i; };
    visited[nidx(0, 0)] = 1;
    stk[top++] = 0;
    while (top > 0) {
        const u32 cur = stk[top - 1];
        const u32 ci = cur % nx, cj = cur / nx;
        u32 cand[4], nc = 0;
        for (u32 k = 0; k < 4; k++) {
            const s32 ni = (s32)ci + dxk[k], nj = (s32)cj + dzk[k];
            if (ni < 0 || nj < 0 || ni >= (s32)nx || nj >= (s32)nz) continue;
            if (visited[nidx((u32)ni, (u32)nj)]) continue;
            cand[nc++] = k;
        }
        if (nc == 0) { top--; continue; }               // dead end -> backtrack
        const u32 k = cand[rng.range(0, nc)];
        openWall(ci, cj, k);
        const u32 ni = (u32)((s32)ci + dxk[k]), nj = (u32)((s32)cj + dzk[k]);
        visited[nidx(ni, nj)] = 1;
        stk[top++] = (u16)nidx(ni, nj);
    }
    for (u32 j = 0; j < nz; j++)                        // braid: ~1 in 5 remaining walls
        for (u32 i = 0; i < nx; i++)
            for (u32 k = 0; k < 3; k += 2) {            // +x and +z only, so each wall is offered once
                if (i + (k == 0 ? 1u : 0u) >= nx || j + (k == 2 ? 1u : 0u) >= nz) continue;
                if (rng.range(0, 100) >= 20) continue;
                openWall(i, j, k);
            }

    // 3) Three slabs over every OPEN interior cell -> the maze exists identically on all four stories.
    for (u32 z = 1; z < D - 1; z++)
        for (u32 x = 1; x < W - 1; x++) {
            GridCell& c = LevelGridSystem::getCell(grid, x, z);
            if (c.flags & CELL_SOLID) continue;
            LevelGridSystem::addPlatform(c, FS_L1_Q, floorMat);
            LevelGridSystem::addPlatform(c, FS_L2_Q, floorMat);
            LevelGridSystem::addPlatform(c, FS_L3_Q, floorMat);
        }

    // 4) Endpoints, chosen BEFORE the holes so nothing is punched out from under them: spawn on L3 in
    //    one corner node, exit on L0 in the diagonally opposite one — the longest traverse the
    //    footprint allows, and a full four-story descent.
    const u32 spawnI = nx - 1, spawnJ = 0;      // NE node
    const u32 exitI  = 0,      exitJ  = nz - 1; // SW node
    const u32 spawnCX = nodeX0(spawnI) + FS_CORR / 2, spawnCZ = nodeZ0(spawnJ) + FS_CORR / 2;
    const u32 exitCX  = nodeX0(exitI)  + FS_CORR / 2, exitCZ  = nodeZ0(exitJ)  + FS_CORR / 2;

    // 5) Punch the descent, TOP-DOWN. `qAbove` is the slab that must still be intact for a cell to be
    //    eligible, which is what makes a two-story express shaft unrepresentable.
    auto rectEligible = [&](u32 x0, u32 z0, u32 w, u32 d, u8 qAbove) -> bool {
        for (u32 z = z0; z < z0 + d; z++)
            for (u32 x = x0; x < x0 + w; x++) {
                if (!LevelGridSystem::isInBounds(grid, x, z)) return false;
                const GridCell& c = LevelGridSystem::getCell(grid, x, z);
                if (c.flags & CELL_SOLID) return false;
                if (qAbove && !fsHasSlab(c, qAbove)) return false;
            }
        return true;
    };
    auto punchRect = [&](u32 x0, u32 z0, u32 w, u32 d, u8 q) {
        for (u32 z = z0; z < z0 + d; z++)
            for (u32 x = x0; x < x0 + w; x++)
                LevelGridSystem::removePlatform(LevelGridSystem::getCell(grid, x, z), q);
    };
    auto recordHole = [&](u32 x0, u32 z0, u32 w, u32 d, u8 q) {
        if (result.dropHoleCount >= DungeonResult::MAX_DROP_HOLES) return;
        DropHole& dh = result.dropHoles[result.dropHoleCount++];
        dh.pos      = { (x0 + w * 0.5f) * cs, q * 0.25f, (z0 + d * 0.5f) * cs };
        dh.surfaceY = q * 0.25f;
    };

    const u8 qLevels[3]  = { FS_L3_Q, FS_L2_Q, FS_L1_Q };
    const u8 qAboveOf[3] = { 0,       FS_L3_Q, FS_L2_Q };
    // Hole density THINS with depth. The top story hands out ways down freely; the last one makes you
    // hunt for it. Without the gradient every descent reads the same and the floor has no shape.
    const u32 holePct[3] = { 18, 12, 7 };
    for (u32 lv = 0; lv < 3; lv++) {
        const u8 q = qLevels[lv], qAbove = qAboveOf[lv];
        u32 made = 0;
        for (u32 j = 0; j < nz; j++)
            for (u32 i = 0; i < nx; i++) {
                // Never punch the spawn node's own story out from under the player.
                if (q == FS_L3_Q && i == spawnI && j == spawnJ) continue;
                if (rng.range(0, 100) >= holePct[lv]) continue;
                // A 2-cell hole in a 3-cell corridor leaves an L-shaped ledge you can edge around —
                // a choice. A full 3-cell hole spans the corridor and is a committed drop.
                const u32 sz = (rng.range(0, 100) < 35) ? FS_CORR : 2;
                const u32 off = (sz == FS_CORR) ? 0 : rng.range(0, 2);
                const u32 hx = nodeX0(i) + off, hz = nodeZ0(j) + off;
                if (!rectEligible(hx, hz, sz, sz, qAbove)) continue;
                punchRect(hx, hz, sz, sz, q);
                recordHole(hx, hz, sz, sz, q);
                // RETURN PADS: mark the hole's footprint as a jump pad, so the surface you land on
                // one story down flings you back up through the hole you just fell through. A pad
                // cell is a pad on every story it carries, and the hole itself has no slab at this
                // level, so the pad can only fire from BELOW — exactly the lift we want.
                //
                // Only some holes get one, and that is deliberate rather than shy: a pad fires the
                // instant you are grounded on it, so a pad under EVERY hole would bounce you
                // straight back up the moment you dropped and descending would become a fight with
                // the level. One in three leaves clean holes to descend through and marks the rest
                // as two-way lifts. Raise PAD_HOLE_ONE_IN to 1 for a pad under every hole.
                constexpr u32 PAD_HOLE_ONE_IN = 3;
                if (rng.range(0, PAD_HOLE_ONE_IN) == 0) {
                    if (result.jumpPadCount < MAX_JUMP_PADS)
                        result.jumpPads[result.jumpPadCount++] =
                            { (hx + sz * 0.5f) * cs, (q * 0.25f) - 3.0f, (hz + sz * 0.5f) * cs };
                    for (u32 pz = hz; pz < hz + sz; pz++)
                        for (u32 px = hx; px < hx + sz; px++) {
                            GridCell& pc = LevelGridSystem::getCell(grid, px, pz);
                            pc.flags     = static_cast<u8>(pc.flags | CELL_JUMPPAD);
                            pc.jumpPadQ  = FS_PAD_Q;
                        }
                }
                made++;
            }
        // Every story MUST offer at least one way down, or the descent dead-ends. Walk the lattice
        // for the first eligible node rather than trusting the rolls.
        if (made == 0)
            for (u32 j = 0; j < nz && made == 0; j++)
                for (u32 i = 0; i < nx && made == 0; i++) {
                    if (q == FS_L3_Q && i == spawnI && j == spawnJ) continue;
                    const u32 hx = nodeX0(i), hz = nodeZ0(j);
                    if (!rectEligible(hx, hz, 2, 2, qAbove)) continue;
                    punchRect(hx, hz, 2, 2, q);
                    recordHole(hx, hz, 2, 2, q);
                    made++;
                }

        // JUMP GAPS: knock the slab out of a doorway (1 cell thick, corridor-wide). Clearable at base
        // speed — a rhythm break and a risk, not a descent you chose.
        for (u32 j = 0; j < nz; j++)
            for (u32 i = 0; i < nx; i++)
                for (u32 k = 0; k < 3; k += 2) {
                    if (i + (k == 0 ? 1u : 0u) >= nx || j + (k == 2 ? 1u : 0u) >= nz) continue;
                    if (rng.range(0, 100) >= 14) continue;
                    const u32 gx = (k == 0) ? nodeX0(i) + FS_CORR : nodeX0(i);
                    const u32 gz = (k == 0) ? nodeZ0(j)           : nodeZ0(j) + FS_CORR;
                    const u32 gw = (k == 0) ? 1u : FS_CORR, gd = (k == 0) ? FS_CORR : 1u;
                    if (LevelGridSystem::getCell(grid, gx, gz).flags & CELL_SOLID) continue;  // shut door
                    if (!rectEligible(gx, gz, gw, gd, qAbove)) continue;
                    punchRect(gx, gz, gw, gd, q);   // NOT recorded: dropHoles[] means "a way down"
                }
    }

    // 6) JUMP PADS in dead-end nodes (exactly one open doorway): the maze's reward for exploring a
    //    spur, and the recovery route after a missed jump. Authored at ~two stories of lift so the
    //    climb is worth the detour; a pad cell is a pad on every story it carries.
    for (u32 j = 0; j < nz; j++)
        for (u32 i = 0; i < nx; i++) {
            if (i == spawnI && j == spawnJ) continue;
            if (i == exitI  && j == exitJ)  continue;
            u32 doors = 0;
            for (u32 k = 0; k < 4; k++) {
                const s32 ni = (s32)i + dxk[k], nj = (s32)j + dzk[k];
                if (ni < 0 || nj < 0 || ni >= (s32)nx || nj >= (s32)nz) continue;
                const u32 wx = (k == 0) ? nodeX0(i) + FS_CORR : (k == 1) ? nodeX0(i) - 1 : nodeX0(i);
                const u32 wz = (k == 2) ? nodeZ0(j) + FS_CORR : (k == 3) ? nodeZ0(j) - 1 : nodeZ0(j);
                if (!(LevelGridSystem::getCell(grid, wx, wz).flags & CELL_SOLID)) doors++;
            }
            if (doors != 1) continue;
            // The pad fills the WHOLE dead-end node, not its centre cell. A 1x1 pad in a 3-wide
            // corridor is nearly impossible to spot down a dark maze passage and easy to walk past;
            // a full 3x3 glowing floor reads as a room feature from the corridor mouth, and you
            // cannot miss stepping on it once you commit to the spur.
            if (result.jumpPadCount < MAX_JUMP_PADS)
                result.jumpPads[result.jumpPadCount++] =
                    { (nodeX0(i) + FS_CORR * 0.5f) * cs, 0.0f, (nodeZ0(j) + FS_CORR * 0.5f) * cs };
            for (u32 pz = nodeZ0(j); pz < nodeZ0(j) + FS_CORR; pz++)
                for (u32 px = nodeX0(i); px < nodeX0(i) + FS_CORR; px++) {
                    GridCell& pc = LevelGridSystem::getCell(grid, px, pz);
                    if (pc.flags & CELL_SOLID) continue;
                    pc.flags |= CELL_JUMPPAD;
                    pc.jumpPadQ = FS_PAD_Q;
                    // The pad SKIN (floor + every slab top) is applied by name in startGame's
                    // applyJumpPadSkin — level_gen has no MaterialSystem, and on a stacked floor the
                    // visible surface is the slab top, not the floor.
                }
        }

    // 7) Per-level rooms (a 2x2 tiling per story) at floorHeight 0/3/6/9 — these key the flat
    //    enemy/loot spread, so every story gets populated instead of only the ground.
    const f32 levelY[4] = { 0.0f, 3.0f, 6.0f, 9.0f };
    const u32 halfW = (W - 2) / 2, halfD = (D - 2) / 2;
    u32 l0Base = 0, l3Base = 0;
    for (u32 lv = 0; lv < 4; lv++) {
        const u32 base = result.roomCount;
        if (lv == 0) l0Base = base;
        if (lv == 3) l3Base = base;
        for (u32 rz = 0; rz < 2; rz++)
            for (u32 rx = 0; rx < 2; rx++) {
                if (result.roomCount >= MAX_DUNGEON_ROOMS) break;
                DungeonRoom& r = result.rooms[result.roomCount++];
                r = DungeonRoom{};
                r.w = halfW;          r.d = halfD;
                // A room's CENTRE is what downstream code probes — spawn placement, the exit portal,
                // and the generic reachability contract all read rooms[i] centre. On an open floor any
                // rect works, but this floor is a MAZE: a naive quadrant rect centres on a wall roughly
                // a third of the time, which reads as "room unreachable". So snap the rect so its
                // centre lands on the nearest NODE centre (always corridor) that keeps it in bounds.
                const u32 wantX = 1 + rx * halfW + halfW / 2, wantZ = 1 + rz * halfD + halfD / 2;
                auto snap = [&](u32 want, u32 half, u32 n, u32 off, u32 limit) -> u32 {
                    u32 bestC = want, bestD = 0xFFFFFFFFu;
                    for (u32 k = 0; k < n; k++) {
                        const u32 c = off + k * FS_PERIOD + FS_CORR / 2;   // node centre
                        if (c < half / 2) continue;
                        const u32 origin = c - half / 2;                   // resulting rect origin
                        if (origin < 1 || origin + half > limit) continue;
                        const u32 d = (c > want) ? c - want : want - c;
                        if (d < bestD) { bestD = d; bestC = c; }
                    }
                    return (bestD == 0xFFFFFFFFu) ? 1 : bestC - half / 2;
                };
                r.x = snap(wantX, halfW, nx, offX, W - 1);
                r.z = snap(wantZ, halfD, nz, offZ, D - 1);
                r.floorHeight = levelY[lv];
                r.wallMat = wallMat;
            }
    }

    result.spawnBalconyPos = { (spawnCX + 0.5f) * cs, FS_L3_Q * 0.25f, (spawnCZ + 0.5f) * cs };
    result.exitBalconyPos  = { (exitCX  + 0.5f) * cs, 0.0f,            (exitCZ  + 0.5f) * cs };
    result.spawnOnUpper    = true;

    auto roomAt = [&](u32 base, u32 gx, u32 gz) -> s32 {
        for (u32 i = base; i < base + 4 && i < result.roomCount; i++) {
            const DungeonRoom& r = result.rooms[i];
            if (gx >= r.x && gx < r.x + r.w && gz >= r.z && gz < r.z + r.d) return (s32)i;
        }
        return (s32)base;
    };
    forcedSpawn = roomAt(l3Base, spawnCX, spawnCZ);
    forcedExit  = roomAt(l0Base, exitCX,  exitCZ);
}

bool LevelGen::isLavaFloor(u32 levelSeed, u32 floor) {
    if (floor < 31 || floor > 40) return false;
    // Never a BOSS floor (every 5th — so 35 and 40 in this tier). spawnFloorBoss expands a room into
    // an arena and rebuilds the level mesh AFTER the theme pass has already poured lava, so the two
    // would fight over the same cells; and a milestone fight staged in a lava sea is a different
    // encounter from the one that was designed. The single source of truth for "is this floor
    // molten", so the exclusion holds for the dev door too.
    if (floor % 5 == 0) return false;
    // Integer avalanche only (no float, no libm) — same rule as the carve: host and client must
    // agree bit-for-bit or one of them melts a floor the other does not.
    u32 h = levelSeed ^ (floor * 2654435761u);
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return (h % 100u) < 30u;   // ~3 of the tier's 10 floors
}

const char* LevelGen::styleName(LayoutStyle style) {
    switch (style) {
        case LayoutStyle::BSP_ROOMS: return "rooms";
        case LayoutStyle::CAVERN:    return "cavern";
        case LayoutStyle::GAUNTLET:  return "gauntlet";
        case LayoutStyle::HUB:       return "hub";
        case LayoutStyle::VERTICAL_HALL: return "vertical";
        case LayoutStyle::FOUR_STORY:    return "descent";
        default:                     return "?";
    }
}

LevelGen::LayoutStyle LevelGen::pickLayoutStyle(u32 seed, u32 floor) {
    // Floors 1-3 run on the tiny 24-cell tutorial grid — always classic BSP.
    if (floor <= 3) return LayoutStyle::BSP_ROOMS;

    // Decorrelate from the carve RNG: generate() consumes the same seed, and an
    // LCG's first draw is weak, so burn one after the xor.
    GenRNG rng{seed ^ 0x9E3779B9u};
    rng.next();
    u32 roll = rng.range(0, 100);

    // FOUR_STORY is SCHEDULED, not rolled: every floor ending in 9 (9, 19, 29, 39, 49) is a Descent
    // maze and no other floor ever is, so the four-story floor lands as a predictable landmark in the
    // run rather than a surprise. None of those are boss floors (every 5th), so there is no clash,
    // and startGame forces the >=44 grid the style needs. Checked before the weight table, which no
    // longer carries a FOUR_STORY column at all.
    if (floor % 10 == 9) return LayoutStyle::FOUR_STORY;

    // Per-tier weights [BSP, CAVERN, GAUNTLET, HUB, VERTICAL_HALL]; each row sums to 100.
    // Styles echo the tier's fiction: caves peak in the Spider Caverns, gauntlets in the Hellforge,
    // vault hubs in the Catacombs. Classic BSP stays the most common single style overall so the
    // structural floors keep reading as events, not the norm. The two multi-story styles
    // (VERTICAL_HALL, FOUR_STORY) are NON-BOSS styles — see the remap below.
    u8 tier = floor >= 41 ? 4 : floor >= 31 ? 3 : floor >= 21 ? 2 : floor >= 11 ? 1 : 0;
    static constexpr u8 kWeights[5][5] = {
        {50, 13, 12, 12, 13},   // 4-10  Stone Dungeon
        {31, 13, 17, 24, 15},   // 11-20 Catacombs
        {22, 40,  9, 17, 12},   // 21-30 Spider Caverns
        {22, 12, 37, 17, 12},   // 31-40 Hellforge
        {22, 22, 18, 26, 12},   // 41-50 Void
    };
    u32 acc = 0;
    for (u32 s = 0; s < 5; s++) {
        acc += kWeights[tier][s];
        if (roll < acc) {
            LayoutStyle st = static_cast<LayoutStyle>(s);
            // VERTICAL_HALL is a NON-BOSS style: boss floors (every 5th — the milestone floors
            // 5,10,…,50) expand a room into a boss arena and rebuild the mesh, which would stomp the
            // balcony cells; and floors 4-5 run on the tiny tutorial grid. Fall back to classic BSP
            // there. (FOUR_STORY needs no such guard — it is scheduled onto x9 floors above, none of
            // which are boss floors.)
            if (st == LayoutStyle::VERTICAL_HALL && (floor < 6 || floor % 5 == 0))
                return LayoutStyle::BSP_ROOMS;
            return st;
        }
    }
    return LayoutStyle::BSP_ROOMS;
}

DungeonResult LevelGen::generate(LevelGrid& grid, u32 seed, u32 gridWidth, u32 gridDepth,
                                 LayoutStyle style) {
    (void)gridWidth; (void)gridDepth; // grid carries its own dims; params kept for API compat
    DungeonResult result = {};
    GenRNG rng = {seed};
    s32 forcedSpawn = -1, forcedExit = -1;

    fillSolid(grid);
    switch (style) {
        case LayoutStyle::CAVERN:   carveCavern(grid, rng, result); break;
        case LayoutStyle::GAUNTLET: carveGauntlet(grid, rng, result, forcedSpawn, forcedExit); break;
        case LayoutStyle::HUB:      carveHub(grid, rng, result); break;
        case LayoutStyle::VERTICAL_HALL:
            carveVerticalHall(grid, rng, result, forcedSpawn, forcedExit); break;
        case LayoutStyle::FOUR_STORY:
            carveFourStory(grid, rng, result, forcedSpawn, forcedExit); break;
        case LayoutStyle::BSP_ROOMS:
        default:                    carveRoomsBSP(grid, rng, result); break;
    }

    // Degenerate carve (tiny grid + unlucky seed, or a scratch-bound bail) —
    // deterministically fall back to the classic generator rather than ship a
    // floor the boss/enemy placement can't populate (they need ≥3-5 rooms).
    if (style != LayoutStyle::BSP_ROOMS && result.roomCount < 5) {
        LOG_WARN("LevelGen[%s]: degenerate carve (%u rooms) — falling back to rooms",
                 styleName(style), result.roomCount);
        result = {};
        forcedSpawn = forcedExit = -1;
        GenRNG rng2 = {seed ^ 0x51ED270Bu};
        fillSolid(grid);
        carveRoomsBSP(grid, rng2, result);
        style = LayoutStyle::BSP_ROOMS;
    }

    finalizeDungeon(grid, result, styleName(style), forcedSpawn, forcedExit);
    return result;
}

// --- Legacy test dungeon (kept for fallback) ---

// Hardcoded rooms (x, z, w, d in cells) — works for a 32×32 grid
struct Room { u32 x, z, w, d; };

static constexpr Room k_rooms[] = {
    { 2,  2,  8,  6},  // room 0 — spawn room
    {13,  2,  7,  7},  // room 1
    {22,  2,  8,  6},  // room 2
    { 2, 12,  6,  8},  // room 3
    {14, 13,  8,  6},  // room 4 — raised floor
};
static constexpr u32 k_roomCount = 5;

// Carve a rectangular room into the grid
static void carveRoom(LevelGrid& grid, const Room& r,
                      f32 floorH, f32 ceilH,
                      u8 wallMat = 0, u8 floorMat = 1, u8 ceilMat = 2)
{
    u8 floorEnc = static_cast<u8>(floorH / 0.25f);
    u8 ceilEnc  = static_cast<u8>(ceilH  / 0.25f);

    for (u32 z = r.z; z < r.z + r.d; z++) {
        for (u32 x = r.x; x < r.x + r.w; x++) {
            if (!LevelGridSystem::isInBounds(grid, x, z)) continue;
            GridCell& cell = LevelGridSystem::getCell(grid, x, z);
            cell.flags           = CELL_FLOOR | CELL_CEILING;
            cell.floorHeight     = floorEnc;
            cell.ceilingHeight   = ceilEnc;
            cell.wallMaterialId  = wallMat;
            cell.floorMaterialId = floorMat;
            cell.ceilMaterialId  = ceilMat;
        }
    }
}

// Carve an L-shaped corridor between two room centre points
// 2-wide corridor variant used by BSP connector
static void carveCorridor(LevelGrid& grid,
                           u32 x0, u32 z0, u32 x1, u32 z1,
                           f32 floorH, f32 ceilH)
{
    u8 floorEnc = static_cast<u8>(floorH / 0.25f);
    u8 ceilEnc  = static_cast<u8>(ceilH  / 0.25f);

    auto carveCell = [&](u32 x, u32 z) {
        if (!LevelGridSystem::isInBounds(grid, x, z)) return;
        GridCell& cell = LevelGridSystem::getCell(grid, x, z);
        if (cell.flags & CELL_SOLID) {
            cell.flags           = CELL_FLOOR | CELL_CEILING;
            cell.floorHeight     = floorEnc;
            cell.ceilingHeight   = ceilEnc;
            cell.wallMaterialId  = 0;
            cell.floorMaterialId = 1;
            cell.ceilMaterialId  = 2;
        }
    };

    // Horizontal leg — 2 cells wide in Z
    u32 xMin = (x0 < x1) ? x0 : x1;
    u32 xMax = (x0 > x1) ? x0 : x1;
    for (u32 x = xMin; x <= xMax; x++) {
        for (s32 dz = -1; dz <= 0; dz++) {
            s32 cz = (s32)z0 + dz;
            if (cz < 0) continue;
            carveCell(x, (u32)cz);
        }
    }

    // Vertical leg — 2 cells wide in X
    u32 zMin = (z0 < z1) ? z0 : z1;
    u32 zMax = (z0 > z1) ? z0 : z1;
    for (u32 z = zMin; z <= zMax; z++) {
        for (s32 dx = -1; dx <= 0; dx++) {
            s32 cx = (s32)x1 + dx;
            if (cx < 0) continue;
            carveCell((u32)cx, z);
        }
    }
}

Vec3 LevelGen::generateTestDungeon(LevelGrid& grid) {
    // Fill everything as solid
    fillSolid(grid);

    constexpr f32 FLOOR  = 0.0f;
    constexpr f32 CEIL   = 3.0f;   // 3m ceiling (12 quarter-units)

    // Carve rooms (room 4 has raised floor + brick walls)
    for (u32 i = 0; i < k_roomCount; i++) {
        f32 floorH = (i == 4) ? 0.5f : FLOOR;
        u8 wallMat  = (i == 4) ? 3 : 0; // brick_wall for room 4
        u8 floorMat = 1;
        u8 ceilMat  = 2;
        carveRoom(grid, k_rooms[i], floorH, CEIL, wallMat, floorMat, ceilMat);
    }

    // Connect rooms with L-corridors (centre-to-centre)
    auto centre = [](const Room& r, u32& ox, u32& oz) {
        ox = r.x + r.w / 2;
        oz = r.z + r.d / 2;
    };

    // Connect 0→1, 1→2, 0→3, 3→4, 1→4
    constexpr u32 kPairs[][2] = {{0,1},{1,2},{0,3},{3,4},{1,4}};
    for (auto& p : kPairs) {
        u32 ax, az, bx, bz;
        centre(k_rooms[p[0]], ax, az);
        centre(k_rooms[p[1]], bx, bz);
        carveCorridor(grid, ax, az, bx, bz, FLOOR, CEIL);
    }

    // Player spawns in the centre of room 0
    const Room& spawn = k_rooms[0];
    f32 sx = (spawn.x + spawn.w * 0.5f) * grid.cellSize;
    f32 sz = (spawn.z + spawn.d * 0.5f) * grid.cellSize;

    LOG_INFO("LevelGen: dungeon generated (%ux%u), spawn (%.1f, %.1f)",
             grid.width, grid.depth, sx, sz);

    return {sx, 0.0f, sz};
}
