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
            if (xOverlap && zOverlap)
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

const char* LevelGen::styleName(LayoutStyle style) {
    switch (style) {
        case LayoutStyle::BSP_ROOMS: return "rooms";
        case LayoutStyle::CAVERN:    return "cavern";
        case LayoutStyle::GAUNTLET:  return "gauntlet";
        case LayoutStyle::HUB:       return "hub";
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

    // Per-tier weights [BSP, CAVERN, GAUNTLET, HUB]. Styles echo the tier's
    // fiction: caves peak in the Spider Caverns, gauntlets in the Hellforge,
    // vault hubs in the Catacombs. Classic stays the most common style overall
    // so the structural floors keep reading as events, not the norm.
    u8 tier = floor >= 41 ? 4 : floor >= 31 ? 3 : floor >= 21 ? 2 : floor >= 11 ? 1 : 0;
    static constexpr u8 kWeights[5][4] = {
        {55, 15, 15, 15},   // 4-10  Stone Dungeon
        {35, 15, 20, 30},   // 11-20 Catacombs
        {25, 45, 10, 20},   // 21-30 Spider Caverns
        {25, 15, 40, 20},   // 31-40 Hellforge
        {25, 25, 20, 30},   // 41-50 Void
    };
    u32 acc = 0;
    for (u32 s = 0; s < 4; s++) {
        acc += kWeights[tier][s];
        if (roll < acc) return static_cast<LayoutStyle>(s);
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
