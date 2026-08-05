// Spatial grid — the invariant that every active entity stays a collision candidate.
//
// Projectile collision queries this grid instead of scanning the pool, so an entity the grid does
// not return is an entity that CANNOT BE SHOT. It used to lose them two ways, both silent in a
// release build: a cell that filled past SGRID_PER_CELL simply discarded the rest, and an entity
// outside the grid's ±128 m span was skipped entirely. A 3 h soak measured 5643 overflow events
// across 7 of 9 classes, up to 15 entities at a time — enemies standing in a tight pack were
// intermittently immune to every projectile in the game, and nothing said so outside a debug log.
//
// The fix is an overflow list that every query appends. These tests pin the property rather than
// the mechanism: pack a cell, stand outside the world, do both at once — and require the entity
// back out of a query. They fail on the old code by construction.

#include <doctest/doctest.h>
#include "world/spatial_grid.h"

#include <set>

namespace {

// Minimal live entity at a position. The grid only reads position and the DEAD flag.
void addEntity(EntityPool& pool, u32 idx, Vec3 pos) {
    pool.entities[idx] = Entity{};
    pool.entities[idx].position = pos;
    pool.entities[idx].flags = ENT_ACTIVE;
    pool.activeList[pool.activeCount++] = static_cast<u16>(idx);
}

// Every index a query at `pos` hands back.
std::set<u16> queryAt(const SpatialGrid& grid, Vec3 pos) {
    u16 out[SGRID_QUERY_MAX];
    const u32 n = SpatialGridSystem::queryNeighbors(grid, pos, out, SGRID_QUERY_MAX);
    return std::set<u16>(out, out + n);
}

} // namespace

TEST_CASE("a cell packed past its cap still yields every entity") {
    static EntityPool pool;
    pool = EntityPool{};

    // Twice the per-cell cap, all inside ONE 4 m cell so they cannot spill into a neighbour.
    const u32 n = SGRID_PER_CELL * 2;
    REQUIRE(n <= MAX_ENTITIES);
    const Vec3 centre = {0.5f, 0.0f, 0.5f};
    for (u32 i = 0; i < n; i++)
        addEntity(pool, i, {centre.x + 0.01f * static_cast<f32>(i), 0.0f, centre.z});

    static SpatialGrid grid;
    SpatialGridSystem::rebuild(grid, pool);

    // The cap still bounds the CELL — that is what keeps the array small.
    const s32 cx = static_cast<s32>((centre.x + SGRID_OFFSET) / SGRID_CELL);
    const s32 cz = static_cast<s32>((centre.z + SGRID_OFFSET) / SGRID_CELL);
    CHECK(grid.count[cz][cx] == SGRID_PER_CELL);
    CHECK(grid.overflowCount == n - SGRID_PER_CELL);   // the rest are parked, not discarded

    // …but the QUERY must still see all of them. This is the property that matters: on the old
    // code exactly SGRID_PER_CELL came back and the other half were unhittable.
    const std::set<u16> got = queryAt(grid, centre);
    for (u16 i = 0; i < static_cast<u16>(n); i++) {
        CAPTURE(i);
        CHECK(got.count(i) == 1);
    }
}

TEST_CASE("an entity outside the grid span is still a candidate") {
    static EntityPool pool;
    pool = EntityPool{};

    // Well past the ±128 m the grid covers — the old rebuild `continue`d on these, so they were
    // invisible to projectiles no matter how few of them there were.
    addEntity(pool, 0, {900.0f, 0.0f, 900.0f});
    addEntity(pool, 1, {-900.0f, 0.0f, 0.0f});
    addEntity(pool, 2, {0.0f, 0.0f, 0.0f});          // one ordinary in-grid entity for contrast

    static SpatialGrid grid;
    SpatialGridSystem::rebuild(grid, pool);

    CHECK(grid.overflowCount == 2);
    const std::set<u16> got = queryAt(grid, {0.0f, 0.0f, 0.0f});
    CHECK(got.count(0) == 1);
    CHECK(got.count(1) == 1);
    CHECK(got.count(2) == 1);
}

TEST_CASE("the query bound covers a full 3x3 block plus the whole overflow list") {
    // SGRID_QUERY_MAX is what the projectile buffers are sized from. If it were ever smaller than
    // what one query can produce, queryNeighbors would truncate — which is the ORIGINAL bug in its
    // second form: the buffers were a hand-typed 72 while a 3x3 block could hold 144.
    CHECK(SGRID_QUERY_MAX >= 9 * SGRID_PER_CELL);
    CHECK(SGRID_QUERY_MAX >= MAX_ENTITIES);

    // A pool that is entirely overflow (every entity stacked on one spot) must come back whole.
    static EntityPool pool;
    pool = EntityPool{};
    for (u32 i = 0; i < MAX_ENTITIES; i++)
        addEntity(pool, i, {0.5f, 0.0f, 0.5f});

    static SpatialGrid grid;
    SpatialGridSystem::rebuild(grid, pool);
    CHECK(grid.overflowCount == MAX_ENTITIES - SGRID_PER_CELL);
    CHECK(queryAt(grid, {0.5f, 0.0f, 0.5f}).size() == MAX_ENTITIES);
}

TEST_CASE("dead entities are never candidates, packed or not") {
    static EntityPool pool;
    pool = EntityPool{};
    for (u32 i = 0; i < SGRID_PER_CELL * 2; i++) {
        addEntity(pool, i, {0.5f, 0.0f, 0.5f});
        if (i % 2 == 0) pool.entities[i].flags |= ENT_DEAD;
    }

    static SpatialGrid grid;
    SpatialGridSystem::rebuild(grid, pool);

    // The overflow list must honour the DEAD skip exactly as the cells do — a corpse that became a
    // permanent projectile magnet would be a worse bug than the one being fixed.
    const std::set<u16> got = queryAt(grid, {0.5f, 0.0f, 0.5f});
    for (u16 i = 0; i < static_cast<u16>(SGRID_PER_CELL * 2); i++) {
        CAPTURE(i);
        CHECK(got.count(i) == (i % 2 == 0 ? 0u : 1u));
    }
}
