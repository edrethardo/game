// test_platform.cpp — CELL_PLATFORM: real two-story cells (a walk-under slab over a normal floor).
//
// The balcony contract, pinned from three sides: the GRID picks which story a body interacts with
// from its feet height; COLLISION lands bodies on the slab top, lets them walk beneath, and bonks
// a rising head on the underside (a 17 m/s jump-pad launch under a balcony must never tunnel up
// through the walkway); RAYCAST treats the slab as solid from every side (top, underside, rim)
// while letting shots pass cleanly under and over it — the sniper-balcony sightlines.

#include <doctest/doctest.h>
#include "world/collision.h"
#include "world/level_grid.h"
#include "world/raycast.h"
#include "game/player.h"
#include <algorithm>

namespace {

// A 12x12 open room (1 m cells, solid border, floor y=0, walls 5 m) with a BALCONY: a 2-cell-deep
// platform band along the north wall (z=1..2, x=1..10), slab top 3.0 m (12 qu, underside 2.5 m) —
// the arena's exact shape in miniature. Cell (gx,gz) spans [gx,gx+1)x[gz,gz+1); centres at +0.5.
struct BalconyRoom {
    LevelGrid grid;
    BalconyRoom() {
        LevelGridSystem::init(grid, 12, 12, 1.0f);
        for (u32 z = 0; z < 12; z++)
            for (u32 x = 0; x < 12; x++) {
                GridCell& c = grid.cells[z * 12 + x];
                const bool border = (x == 0 || z == 0 || x == 11 || z == 11);
                c.flags         = border ? CELL_SOLID : CELL_FLOOR;
                c.floorHeight   = 0;
                c.ceilingHeight = 20;
            }
        for (u32 z = 1; z <= 2; z++)
            for (u32 x = 1; x <= 10; x++) setPlat(x, z, 12);
    }
    ~BalconyRoom() { LevelGridSystem::shutdown(grid); }

    void setPlat(u32 x, u32 z, u8 topQ) {
        GridCell& c = grid.cells[z * 12 + x];
        c.flags     = static_cast<u8>(CELL_FLOOR);
        LevelGridSystem::setPlatform(c, topQ, 0);
    }
};

// Step a body through moveAndSlide with a held horizontal velocity, the way the movement code
// drives it (velocity.x/z re-asserted every tick; Y left to gravity/impulses).
void walk(Player& p, const LevelGrid& g, f32 vx, f32 vz, int ticks) {
    for (int i = 0; i < ticks; i++) {
        p.velocity.x = vx;
        p.velocity.z = vz;
        Collision::moveAndSlide(p, g, 1.0f / 60.0f);
    }
}

// A body at REST has its onGround flag alternate true/false every tick: resting means vy==0, so
// delta.y==0 skips the landing branch that sets the flag, and the -0.05 m ground probe does not
// register a body sitting exactly on floorHeight. (Pre-existing and single-slab — reproduces in
// BalconyRoom too; the 17 m/s case above only passes because it lands on the lucky parity.) So
// "is it resting on the ground" is asked over a 2-tick window, never one arbitrary tick.
bool settledOnGround(Player& p, const LevelGrid& g) {
    bool grounded = p.onGround;
    Collision::moveAndSlide(p, g, 1.0f / 60.0f);
    return grounded || p.onGround;
}

// A 12x12 open room (1 m cells, solid border, floor y=0) where every interior cell is a full
// FOUR-STORY Descent stack: slabs at 12/24/36 qu (tops 3/6/9 m) via addPlatform, so a cell carries
// platCount==3 {12,24,36} (undersides 2.5/5.5/8.5). Holes are punched per level with removePlatform.
struct StackedRoom {
    LevelGrid grid;
    StackedRoom() {
        LevelGridSystem::init(grid, 12, 12, 1.0f);
        for (u32 z = 0; z < 12; z++)
            for (u32 x = 0; x < 12; x++) {
                GridCell& c = grid.cells[z * 12 + x];
                const bool border = (x == 0 || z == 0 || x == 11 || z == 11);
                c.flags         = border ? CELL_SOLID : CELL_FLOOR;
                c.floorHeight   = 0;
                c.ceilingHeight = 48;   // FS_CEIL_Q — clears L3 @ 9 m + a 1.8 m body
                if (!border) {
                    LevelGridSystem::addPlatform(c, 12, 1);   // L1 top 3 m
                    LevelGridSystem::addPlatform(c, 24, 1);   // L2 top 6 m
                    LevelGridSystem::addPlatform(c, 36, 1);   // L3 top 9 m
                }
            }
    }
    ~StackedRoom() { LevelGridSystem::shutdown(grid); }

    // Punch a hole at slab-top `topQ` over the inclusive cell rect [x0,x1]x[z0,z1].
    void punch(u32 x0, u32 z0, u32 x1, u32 z1, u8 topQ) {
        for (u32 z = z0; z <= z1; z++)
            for (u32 x = x0; x <= x1; x++)
                LevelGridSystem::removePlatform(grid.cells[z * 12 + x], topQ);
    }
};

} // namespace

TEST_CASE("Platform grid: helpers expose top/underside and pick the story by feet height") {
    BalconyRoom room;
    CHECK(LevelGridSystem::hasPlatform(room.grid, 5, 1));
    CHECK_FALSE(LevelGridSystem::hasPlatform(room.grid, 5, 5));
    CHECK(LevelGridSystem::getPlatformTop(room.grid, 5, 1) == doctest::Approx(3.0f));
    CHECK(LevelGridSystem::getPlatformUnderside(room.grid, 5, 1) == doctest::Approx(2.5f));

    // Story selection: feet at/near the top walk the slab; feet below keep the ground floor.
    CHECK(LevelGridSystem::effectiveFloorHeight(room.grid, 5, 1, 3.0f) == doctest::Approx(3.0f));
    CHECK(LevelGridSystem::effectiveFloorHeight(room.grid, 5, 1, 2.7f) == doctest::Approx(3.0f)); // within step tolerance
    CHECK(LevelGridSystem::effectiveFloorHeight(room.grid, 5, 1, 2.5f) == doctest::Approx(0.0f)); // below tolerance
    CHECK(LevelGridSystem::effectiveFloorHeight(room.grid, 5, 1, 0.0f) == doctest::Approx(0.0f));
    CHECK(LevelGridSystem::effectiveFloorHeight(room.grid, 5, 5, 9.0f) == doctest::Approx(0.0f)); // no platform

    // A slab too thin for under-space clamps its underside to the base floor (low stair steps).
    room.setPlat(8, 5, 1);
    CHECK(LevelGridSystem::getPlatformUnderside(room.grid, 8, 5) == doctest::Approx(0.0f));
}

TEST_CASE("Platform collision: a ground body walks UNDER the balcony unobstructed") {
    BalconyRoom room;
    Player p;
    p.position = {5.5f, 0.0f, 4.5f};
    p.velocity = {0, 0, 0};
    p.onGround = true;
    walk(p, room.grid, 0.0f, -3.0f, 90);            // stroll north into the band
    CHECK(p.position.z < 2.0f);                     // deep in the arcade under the slab
    CHECK(p.position.y == doctest::Approx(0.0f));   // never lifted onto the slab
    CHECK(p.onGround);
}

TEST_CASE("Platform collision: a falling body lands ON the slab top") {
    BalconyRoom room;
    Player p;
    p.position = {5.5f, 3.6f, 1.5f};                // over the band, above the top
    p.velocity = {0, 0, 0};
    p.onGround = false;
    for (int i = 0; i < 60 && !p.onGround; i++) Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
    CHECK(p.onGround);
    CHECK(p.position.y == doctest::Approx(3.0f));
}

TEST_CASE("Platform collision: rising under the balcony bonks the underside, never tunnels up") {
    BalconyRoom room;
    Player p;
    p.position = {5.5f, 0.0f, 1.5f};                // in the arcade
    p.velocity = {0, 17.0f, 0};                     // jump-pad-scale launch (the worst case)
    p.onGround = false;
    f32 maxHead = 0.0f;
    for (int i = 0; i < 120; i++) {
        Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
        maxHead = std::max(maxHead, p.position.y + PLAYER_HEIGHT);
    }
    CHECK(maxHead <= 2.5f + 0.001f);                // head stopped at the underside
    CHECK(p.position.y == doctest::Approx(0.0f));   // and came back down to the ground story
    CHECK(p.onGround);
}

TEST_CASE("Platform collision: a slab band the body would clip blocks XZ like a wall") {
    BalconyRoom room;
    room.setPlat(5, 6, 6);                          // lone 1.5 m slab (underside 1.0) in the open
    Player p;
    p.position = {5.5f, 0.0f, 8.0f};
    p.velocity = {0, 0, 0};
    p.onGround = true;
    walk(p, room.grid, 0.0f, -3.0f, 60);            // walk north into it: head would clip the band
    CHECK(p.position.z >= 7.0f + PLAYER_HALF_WIDTH - 0.01f);   // stopped at the cell edge
    CHECK(p.position.y == doctest::Approx(0.0f));   // and was NOT hoisted onto it
}

TEST_CASE("Platform collision: graduated slab stairs climb like stairs") {
    BalconyRoom room;
    // 6 steps against the west wall: x1 top 1.5 m ... x6 top 0.25 m (0.25 m per step).
    for (u32 i = 0; i < 6; i++) room.setPlat(1 + i, 5, static_cast<u8>(6 - i));
    Player p;
    p.position = {8.5f, 0.0f, 5.5f};
    p.velocity = {0, 0, 0};
    p.onGround = true;
    walk(p, room.grid, -3.0f, 0.0f, 180);           // west, up the steps, into the wall
    CHECK(p.position.y == doctest::Approx(1.5f));   // standing on the top step
    CHECK(p.onGround);
}

TEST_CASE("Platform raycast: the slab is solid from every side, transparent past it") {
    BalconyRoom room;

    SUBCASE("downward ray hits the slab TOP, not the ground floor beneath it") {
        RayHit h = Raycast::cast(room.grid, {5.5f, 4.5f, 1.5f}, {0.0f, -1.0f, 0.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.position.y == doctest::Approx(3.0f));
        CHECK(h.normal.y == doctest::Approx(1.0f));
    }
    SUBCASE("upward ray from the arcade hits the UNDERSIDE") {
        RayHit h = Raycast::cast(room.grid, {5.5f, 0.5f, 1.5f}, {0.0f, 1.0f, 0.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.position.y == doctest::Approx(2.5f));
        CHECK(h.normal.y == doctest::Approx(-1.0f));
    }
    SUBCASE("horizontal ray at slab height hits the RIM") {
        RayHit h = Raycast::cast(room.grid, {5.5f, 2.75f, 5.5f}, {0.0f, 0.0f, -1.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.position.z == doctest::Approx(3.0f));    // front edge of the band (cells z=1..2)
        CHECK(h.normal.z == doctest::Approx(1.0f));
    }
    SUBCASE("horizontal ray UNDER the slab passes beneath, hits the far wall") {
        RayHit h = Raycast::cast(room.grid, {5.5f, 1.5f, 5.5f}, {0.0f, 0.0f, -1.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.position.z == doctest::Approx(1.0f));    // the north border wall, 2 cells past the band edge
    }
    SUBCASE("horizontal ray ABOVE the slab passes over, hits the far wall") {
        RayHit h = Raycast::cast(room.grid, {5.5f, 3.5f, 5.5f}, {0.0f, 0.0f, -1.0f}, 10.0f);
        REQUIRE(h.hit);
        CHECK(h.position.z == doctest::Approx(1.0f));
    }
    SUBCASE("balcony sniper: a down-angled shot over the edge reaches the pit floor") {
        // Eye above the band firing south-down into the room — must clear its own slab edge.
        Vec3 d = normalize(Vec3{0.0f, -0.8f, 1.0f});
        RayHit h = Raycast::cast(room.grid, {5.5f, 4.6f, 2.5f}, d, 20.0f);
        REQUIRE(h.hit);
        CHECK(h.normal.y == doctest::Approx(1.0f));
        CHECK(h.position.y == doctest::Approx(0.0f));    // ground story, out in the pit
        CHECK(h.position.z > 3.0f);
    }
}

// A 3 m solid CAUSEWAY (CELL_LEDGE @ 12 qu) is the Stacked-Hall bridge that makes the upper story the
// ONLY crossing between two split lower pits: walkable on TOP from an adjacent 3 m gallery, a WALL to
// a body in the pit below (splits the ground), and solid beneath (you can't walk under it). This test
// pins that primitive before the generator builds on it.
TEST_CASE("3 m CELL_LEDGE causeway: cross on top, blocked from the pit below") {
    LevelGrid g;
    LevelGridSystem::init(g, 12, 5, 1.0f);
    for (u32 z = 0; z < 5; z++)
        for (u32 x = 0; x < 12; x++) {
            GridCell& c = g.cells[z * 12 + x];
            const bool border = (x == 0 || z == 0 || x == 11 || z == 4);
            c.flags = border ? CELL_SOLID : CELL_FLOOR;
            c.floorHeight = 0; c.ceilingHeight = 20;
        }
    // gallery-left (platform @3 m) x=2..3, causeway (ledge @3 m) x=4..7, gallery-right x=8..9, z=1..3.
    auto plat  = [&](u32 x){ for (u32 z=1; z<=3; z++){ GridCell& c=g.cells[z*12+x]; c.flags=static_cast<u8>(CELL_FLOOR); LevelGridSystem::setPlatform(c, 12, 0); } };
    auto ledge = [&](u32 x){ for (u32 z=1; z<=3; z++){ GridCell& c=g.cells[z*12+x]; c.flags=static_cast<u8>(CELL_FLOOR|CELL_LEDGE);    c.floorHeight=12; } };
    plat(2); plat(3); ledge(4); ledge(5); ledge(6); ledge(7); plat(8); plat(9);

    SUBCASE("on the gallery: walk ACROSS the causeway, staying at 3 m") {
        Player p{}; p.position = {2.5f, 3.0f, 2.5f};   // on gallery-left, feet at 3 m
        // 120 ticks * 3 m/s = 6 m → reaches the gallery-RIGHT (x~8.5) without overshooting its edge
        // at x=9 (past which is open pit at 0 m, where it would correctly fall).
        for (int i = 0; i < 120; i++) { p.velocity.x = 3.0f; p.velocity.z = 0; Collision::moveAndSlide(p, g, 1.0f/60.0f); }
        CHECK(p.position.x > 7.5f);                    // crossed the causeway onto the right gallery
        CHECK(p.position.x < 9.5f);                    // still ON the gallery, not off its far edge
        CHECK(p.position.y == doctest::Approx(3.0f));  // never fell into the pit
    }
    SUBCASE("in the pit: walk UNDER the gallery but get BLOCKED by the solid causeway") {
        Player p{}; p.position = {1.5f, 0.0f, 2.5f};    // in the pit, feet at 0
        for (int i = 0; i < 240; i++) { p.velocity.x = 3.0f; p.velocity.z = 0; Collision::moveAndSlide(p, g, 1.0f/60.0f); }
        CHECK(p.position.y == doctest::Approx(0.0f));   // stayed on the ground (walked under the slab)
        CHECK(p.position.x < 4.0f);                     // stopped at the causeway wall — never crossed
    }
    LevelGridSystem::shutdown(g);
}

// Two-story ENTITY floor-snap (Collision::snapEntityToFloor — the Vec3 twin the AI/ensureNotInWall
// use, story-identical to enemy_ai.cpp's Entity variant). Entities are CENTRE-based, so feet =
// position.y - halfExtents.y. This is the ONE change that lets enemies climb ramps, stand on a
// balcony, walk under one, and drop off its edge — the foundation of two-story chase.
TEST_CASE("Collision::snapEntityToFloor is story-aware over a balcony") {
    BalconyRoom room;                       // slab band z=1..2, top 3.0 m
    const Vec3 half = {0.35f, 0.5f, 0.35f}; // a small ground enemy

    SUBCASE("under the balcony → stays on the GROUND story") {
        Vec3 pos = {5.5f, 0.5f, 1.5f};      // feet ~0 under the z=1..2 slab
        Collision::snapEntityToFloor(pos, half, room.grid);
        CHECK(pos.y == doctest::Approx(0.5f));   // ground floor 0 + halfY
    }
    SUBCASE("feet near the slab top → stands ON the balcony") {
        Vec3 pos = {5.5f, 3.5f, 1.5f};      // feet 3.0 == slab top, on a slab cell
        Collision::snapEntityToFloor(pos, half, room.grid);
        CHECK(pos.y == doctest::Approx(3.5f));   // slab top 3.0 + halfY
    }
    SUBCASE("stepping off the edge (feet high, cell has no slab) → drops to the ground story") {
        Vec3 pos = {5.5f, 3.5f, 5.5f};      // was on the balcony, now over open floor at z=5
        Collision::snapEntityToFloor(pos, half, room.grid);
        CHECK(pos.y == doctest::Approx(0.5f));
    }
}

// --- Phase 1: multi-slab (FOUR_STORY Descent) collision ------------------------------------
// A Descent cell carries up to 3 slabs, so every band test must run PER SLAB. The single-slab
// reads (highest top + lowest underside) describe a phantom full-height band that walls off the
// legal standing space between two slabs.
TEST_CASE("Descent band: overlapsPlatformBand tests every slab, not a phantom full-height band") {
    StackedRoom room;               // interior cells all {12,24,36}: tops 3/6/9, undersides 2.5/5.5/8.5
    const f32 hw = PLAYER_HALF_WIDTH;
    // In the arcade (head 1.8 clears the L1 underside 2.5) → fits fully beneath, no clip
    CHECK_FALSE(Collision::overlapsPlatformBand({6.0f, 0.0f, 6.0f}, hw, room.grid));
    // Feet 1.0 (head 2.8) pokes into the L1 band [2.5,3.0] → blocked
    CHECK(Collision::overlapsPlatformBand({6.0f, 1.0f, 6.0f}, hw, room.grid));
    // Feet 3.0 standing ON L1, head 4.8 clear of the L2 underside 5.5 → must PASS. The old single-slab
    // read (highest top 9.0 + lowest underside 2.5) was a phantom full-height band that wrongly blocked
    // this — the discriminator for the multi-slab loop.
    CHECK_FALSE(Collision::overlapsPlatformBand({6.0f, 3.0f, 6.0f}, hw, room.grid));
    // Feet 4.0 (head 5.8) pokes into the L2 band [5.5,6.0] → blocked
    CHECK(Collision::overlapsPlatformBand({6.0f, 4.0f, 6.0f}, hw, room.grid));
}

TEST_CASE("Descent head-clamp: running-min under a 3-slab stack (grid overload)") {
    StackedRoom room;   // interior cells all {12,24,36}: undersides 2.5 / 5.5 / 8.5

    SUBCASE("from the arcade → bonks the L1 underside (2.5), never pops onto L2") {
        Player p;
        p.position = {6.0f, 0.0f, 6.0f};   // feet on the ground floor, under L1
        p.velocity = {0.0f, 30.0f, 0.0f};  // over-strong launch: proves running-min, not last-wins
        p.onGround = false;
        f32 maxHead = 0.0f;
        for (int i = 0; i < 120; i++) {
            Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
            maxHead = std::max(maxHead, p.position.y + PLAYER_HEIGHT);
        }
        CHECK(maxHead <= 2.5f + 0.001f);                // clamped at the LOWEST underside (L1)
        CHECK(p.position.y == doctest::Approx(0.0f));   // fell back to the ground story
        CHECK(settledOnGround(p, room.grid));
    }
    SUBCASE("standing on L1 → bonks the L2 underside (5.5), never pops onto L3") {
        Player p;
        p.position = {6.0f, 3.0f, 6.0f};   // feet on L1 (top 3.0), under L2 (underside 5.5)
        p.velocity = {0.0f, 30.0f, 0.0f};
        p.onGround = false;
        f32 maxHead = 0.0f;
        for (int i = 0; i < 120; i++) {
            Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
            maxHead = std::max(maxHead, p.position.y + PLAYER_HEIGHT);
        }
        CHECK(maxHead <= 5.5f + 0.001f);                // bonked L2, never popped onto L3 (8.5)
        CHECK(p.position.y == doctest::Approx(3.0f));   // fell back onto L1
        CHECK(settledOnGround(p, room.grid));
    }
}

TEST_CASE("Descent head-clamp: running-min under a 3-slab stack (entity-obstacle overload)") {
    StackedRoom room;
    Player p;
    p.position = {6.0f, 3.0f, 6.0f};   // feet on L1, under L2 (underside 5.5)
    p.velocity = {0.0f, 30.0f, 0.0f};
    p.onGround = false;
    f32 maxHead = 0.0f;
    for (int i = 0; i < 120; i++) {
        Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f, nullptr, 0);  // the 5-arg entity overload
        maxHead = std::max(maxHead, p.position.y + PLAYER_HEIGHT);
    }
    CHECK(maxHead <= 5.5f + 0.001f);                // same clamp holds in the entity-obstacle overload
    CHECK(p.position.y == doctest::Approx(3.0f));
    CHECK(settledOnGround(p, room.grid));
}

TEST_CASE("Descent fall: free-fall through a 2x2 L3 hole lands one story down on L2") {
    StackedRoom room;
    room.punch(5, 5, 6, 6, 36);        // 2x2 L3 hole → cells (5..6,5..6) now carry {12,24}
    Player p;
    p.position = {6.0f, 9.5f, 6.0f};   // centred on the hole (AABB stays over holed cells), above L3
    p.velocity = {0, 0, 0};
    p.onGround = false;
    f32 minY = p.position.y;
    for (int i = 0; i < 120; i++) {
        Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
        minY = std::min(minY, p.position.y);
    }
    CHECK(settledOnGround(p, room.grid));
    CHECK(p.position.y == doctest::Approx(6.0f));   // caught on the next intact slab, L2
    CHECK(minY >= 6.0f - 0.001f);                    // the per-tick snap never let it dip below L2
}

TEST_CASE("Descent fall: a column holed at L3 and L2 drops through to L1") {
    StackedRoom room;
    room.punch(5, 5, 6, 6, 36);        // L3 hole
    room.punch(5, 5, 6, 6, 24);        // + L2 hole → those cells carry only {12}
    Player p;
    p.position = {6.0f, 9.5f, 6.0f};
    p.velocity = {0, 0, 0};
    p.onGround = false;
    for (int i = 0; i < 120 && !p.onGround; i++) Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);
    CHECK(p.onGround);
    CHECK(p.position.y == doctest::Approx(3.0f));   // fell past the L2 hole onto L1
}

TEST_CASE("Descent fall: walking into a 2x2 hole completes the fall (no sticky lip)") {
    StackedRoom room;
    room.punch(5, 5, 6, 6, 36);        // 2x2 L3 hole (cells x5..6, z5..6)
    Player p;
    p.position = {6.0f, 9.0f, 7.6f};   // on L3 (feet 9.0), just north of the hole, x on the cell-5/6 seam
    p.velocity = {0, 0, 0};
    p.onGround = true;
    // Modest speed so horizontal drift during the fall stays over the 2x2 hole. Once the 0.6 m AABB
    // fully clears the intact lip it must fall — a 1-cell hole could not clear it, hence >=2 wide.
    walk(p, room.grid, 0.0f, -1.5f, 120);
    CHECK(p.position.y == doctest::Approx(6.0f));   // the lip did NOT re-grab it onto L3 — it fell to L2
    CHECK(settledOnGround(p, room.grid));
}

TEST_CASE("Descent fall: a high-velocity single tick still lands on the top slab") {
    StackedRoom room;                  // full stacks, no hole
    Player p;
    p.position = {6.0f, 9.3f, 6.0f};   // just above the L3 top (9.0), within step tolerance
    p.velocity = {0.0f, -30.0f, 0.0f}; // 0.5 m in one 1/60 s tick — a velocity-gated snap would tunnel
    p.onGround = false;
    Collision::moveAndSlide(p, room.grid, 1.0f / 60.0f);   // exactly one tick
    CHECK(p.onGround);
    CHECK(p.position.y == doctest::Approx(9.0f));   // caught on L3 despite the 30 m/s descent
}
